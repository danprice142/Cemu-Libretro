#include "Common/VFSFileStream.h"
#include "Common/FileStream.h"
#include <cstdarg>

#ifdef RETRO_CORE
#include "libretro/libretro.h"
#endif

struct retro_vfs_interface* VFSFileStream::s_vfs_interface = nullptr;

void VFSFileStream::SetVFSInterface(struct retro_vfs_interface* vfs_interface)
{
	s_vfs_interface = vfs_interface;
}

VFSFileStream::VFSFileStream(struct retro_vfs_file_handle* vfs_handle)
{
	m_vfsHandle = vfs_handle;
	m_useVFS = true;
	m_isValid = (vfs_handle != nullptr);
}

VFSFileStream::VFSFileStream(FileStream* native_stream)
{
	m_nativeStream = native_stream;
	m_useVFS = false;
	m_isValid = (native_stream != nullptr);
}

VFSFileStream* VFSFileStream::openFile(std::string_view path)
{
	return openFile2(fs::path(path), false);
}

VFSFileStream* VFSFileStream::openFile(const wchar_t* path, bool allowWrite)
{
	return openFile2(fs::path(path), allowWrite);
}

VFSFileStream* VFSFileStream::openFile2(const fs::path& path, bool allowWrite)
{
#ifdef RETRO_CORE
	if (s_vfs_interface && s_vfs_interface->open)
	{
		unsigned int mode = allowWrite ? (RETRO_VFS_FILE_ACCESS_READ | RETRO_VFS_FILE_ACCESS_WRITE) : RETRO_VFS_FILE_ACCESS_READ;
		struct retro_vfs_file_handle* handle = s_vfs_interface->open(path.string().c_str(), mode, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (handle)
			return new VFSFileStream(handle);
		return nullptr;
	}
#endif
	
	// Fallback to native FileStream
	FileStream* fs = FileStream::openFile2(path, allowWrite);
	if (fs)
		return new VFSFileStream(fs);
	return nullptr;
}

VFSFileStream* VFSFileStream::createFile(const wchar_t* path)
{
	return createFile2(fs::path(path));
}

VFSFileStream* VFSFileStream::createFile(std::string_view path)
{
	return createFile2(fs::path(path));
}

VFSFileStream* VFSFileStream::createFile2(const fs::path& path)
{
#ifdef RETRO_CORE
	if (s_vfs_interface && s_vfs_interface->open)
	{
		unsigned int mode = RETRO_VFS_FILE_ACCESS_READ | RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
		struct retro_vfs_file_handle* handle = s_vfs_interface->open(path.string().c_str(), mode, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (handle)
			return new VFSFileStream(handle);
		return nullptr;
	}
#endif
	
	// Fallback to native FileStream
	FileStream* fs = FileStream::createFile2(path);
	if (fs)
		return new VFSFileStream(fs);
	return nullptr;
}

std::optional<std::vector<uint8>> VFSFileStream::LoadIntoMemory(const fs::path& path)
{
	VFSFileStream* fs = openFile2(path);
	if (!fs)
		return std::nullopt;
	
	uint64 fileSize = fs->GetSize();
	if (fileSize == 0)
	{
		delete fs;
		return std::nullopt;
	}
	
	std::vector<uint8> v;
	v.resize(fileSize);
	fs->SetPosition(0);
	uint32 bytesRead = fs->readData(v.data(), (uint32)fileSize);
	delete fs;
	
	if (bytesRead != fileSize)
		return std::nullopt;
	
	return v;
}

void VFSFileStream::SetPosition(uint64 pos)
{
	if (!m_isValid)
		return;
		
#ifdef RETRO_CORE
	if (m_useVFS && s_vfs_interface && s_vfs_interface->seek)
	{
		s_vfs_interface->seek(m_vfsHandle, (int64_t)pos, RETRO_VFS_SEEK_POSITION_START);
		return;
	}
#endif
	
	if (m_nativeStream)
		m_nativeStream->SetPosition(pos);
}

uint64 VFSFileStream::GetSize()
{
	if (!m_isValid)
		return 0;
		
#ifdef RETRO_CORE
	if (m_useVFS && s_vfs_interface && s_vfs_interface->size)
	{
		return (uint64)s_vfs_interface->size(m_vfsHandle);
	}
#endif
	
	if (m_nativeStream)
		return m_nativeStream->GetSize();
	
	return 0;
}

bool VFSFileStream::SetEndOfFile()
{
	if (!m_isValid)
		return false;
		
#ifdef RETRO_CORE
	if (m_useVFS && s_vfs_interface && s_vfs_interface->truncate)
	{
		int64_t currentPos = s_vfs_interface->tell(m_vfsHandle);
		return s_vfs_interface->truncate(m_vfsHandle, currentPos) == 0;
	}
#endif
	
	if (m_nativeStream)
		return m_nativeStream->SetEndOfFile();
	
	return false;
}

void VFSFileStream::extract(std::vector<uint8>& data)
{
	if (!m_isValid)
		return;
		
	uint64 fileSize = GetSize();
	data.resize((size_t)fileSize);
	SetPosition(0);
	readData(data.data(), (uint32)fileSize);
}

void VFSFileStream::Flush()
{
#ifdef RETRO_CORE
	if (m_useVFS && s_vfs_interface && s_vfs_interface->flush)
	{
		s_vfs_interface->flush(m_vfsHandle);
		return;
	}
#endif
	
	// Native FileStream on Windows doesn't have flush, Unix does
#ifndef _WIN32
	if (m_nativeStream)
		m_nativeStream->Flush();
#endif
}

uint32 VFSFileStream::readData(void* data, uint32 length)
{
	if (!m_isValid)
		return 0;
		
#ifdef RETRO_CORE
	if (m_useVFS && s_vfs_interface && s_vfs_interface->read)
	{
		return (uint32)s_vfs_interface->read(m_vfsHandle, data, (uint64_t)length);
	}
#endif
	
	if (m_nativeStream)
		return m_nativeStream->readData(data, length);
	
	return 0;
}

bool VFSFileStream::readU64(uint64& v)
{
	return readData(&v, sizeof(uint64)) == sizeof(uint64);
}

bool VFSFileStream::readU32(uint32& v)
{
	return readData(&v, sizeof(uint32)) == sizeof(uint32);
}

bool VFSFileStream::readU16(uint16& v)
{
	return readData(&v, sizeof(uint16)) == sizeof(uint16);
}

bool VFSFileStream::readU8(uint8& v)
{
	return readData(&v, sizeof(uint8)) == sizeof(uint8);
}

bool VFSFileStream::readLine(std::string& line)
{
	line.clear();
	uint8 c;
	bool isEOF = true;
	while (readU8(c))
	{
		isEOF = false;
		if (c == '\r' || c == '\n')
			break;
		line.push_back((char)c);
	}
	return !isEOF;
}

sint32 VFSFileStream::writeData(const void* data, sint32 length)
{
	if (!m_isValid)
		return 0;
		
#ifdef RETRO_CORE
	if (m_useVFS && s_vfs_interface && s_vfs_interface->write)
	{
		return (sint32)s_vfs_interface->write(m_vfsHandle, data, (uint64_t)length);
	}
#endif
	
	if (m_nativeStream)
		return m_nativeStream->writeData(data, length);
	
	return 0;
}

void VFSFileStream::writeU64(uint64 v)
{
	writeData(&v, sizeof(uint64));
}

void VFSFileStream::writeU32(uint32 v)
{
	writeData(&v, sizeof(uint32));
}

void VFSFileStream::writeU16(uint16 v)
{
	writeData(&v, sizeof(uint16));
}

void VFSFileStream::writeU8(uint8 v)
{
	writeData(&v, sizeof(uint8));
}

void VFSFileStream::writeStringFmt(const char* format, ...)
{
	char buffer[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	writeData(buffer, (sint32)strlen(buffer));
}

void VFSFileStream::writeString(const char* str)
{
	writeData(str, (sint32)strlen(str));
}

void VFSFileStream::writeLine(const char* str)
{
	writeData(str, (sint32)strlen(str));
	writeData("\r\n", 2);
}

VFSFileStream::~VFSFileStream()
{
	if (!m_isValid)
		return;
		
#ifdef RETRO_CORE
	if (m_useVFS && m_vfsHandle && s_vfs_interface && s_vfs_interface->close)
	{
		s_vfs_interface->close(m_vfsHandle);
		return;
	}
#endif
	
	if (m_nativeStream)
	{
		delete m_nativeStream;
	}
}
