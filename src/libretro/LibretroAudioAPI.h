#pragma once

#include "audio/IAudioAPI.h"
#include "libretro.h"
#include <vector>
#include <mutex>

class LibretroAudioAPI : public IAudioAPI
{
public:
	class LibretroDeviceDescription : public DeviceDescription
	{
	public:
		LibretroDeviceDescription()
			: DeviceDescription(L"Libretro Audio") {}

		std::wstring GetIdentifier() const override { return L"libretro"; }
	};

	LibretroAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample);

	AudioAPI GetType() const override { return Cubeb; } // pretend to be Cubeb for config compatibility

	bool NeedAdditionalBlocks() const override;
	bool FeedBlock(sint16* data) override;
	bool Play() override;
	bool Stop() override;

	// Called from retro_run() to flush accumulated audio to frontend
	static void FlushAudio(retro_audio_sample_batch_t audio_batch_cb);

	// Reset accumulated audio
	static void Reset();

private:
	static std::mutex s_audioMutex;
	static std::vector<int16_t> s_audioBuffer;
	static bool s_playing;
};
