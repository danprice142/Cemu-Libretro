#include "LibretroAudioAPI.h"

std::mutex LibretroAudioAPI::s_audioMutex;
std::vector<int16_t> LibretroAudioAPI::s_audioBuffer;
bool LibretroAudioAPI::s_playing = false;

LibretroAudioAPI::LibretroAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	s_audioBuffer.reserve(samplerate * channels); // 1 second buffer
}

bool LibretroAudioAPI::NeedAdditionalBlocks() const
{
	std::lock_guard lock(s_audioMutex);
	// Accept blocks if buffer isn't too large (avoid unbounded growth)
	return s_audioBuffer.size() < m_samplerate * m_channels * 2;
}

bool LibretroAudioAPI::FeedBlock(sint16* data)
{
	if (!s_playing)
		return false;

	std::lock_guard lock(s_audioMutex);
	const uint32 sampleCount = m_samplesPerBlock * m_channels;
	s_audioBuffer.insert(s_audioBuffer.end(), data, data + sampleCount);
	return true;
}

bool LibretroAudioAPI::Play()
{
	s_playing = true;
	return true;
}

bool LibretroAudioAPI::Stop()
{
	s_playing = false;
	return true;
}

void LibretroAudioAPI::FlushAudio(retro_audio_sample_batch_t audio_batch_cb)
{
	if (!audio_batch_cb)
		return;

	std::lock_guard lock(s_audioMutex);
	if (s_audioBuffer.empty())
		return;

	// Send all accumulated audio to frontend
	size_t frames = s_audioBuffer.size() / 2;
	if (frames > 0)
		audio_batch_cb(s_audioBuffer.data(), frames);

	s_audioBuffer.clear();
}

void LibretroAudioAPI::Reset()
{
	std::lock_guard lock(s_audioMutex);
	s_audioBuffer.clear();
	s_playing = false;
}
