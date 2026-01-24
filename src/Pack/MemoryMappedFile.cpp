#include "MemoryMappedFile.h"
#include "SnPakFormat.h"

#include <cstring>
#include <vector>
#include <stdexcept>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace SnAPI::AssetPipeline
{

  // ========== MemoryMappedFile ==========

  MemoryMappedFile::~MemoryMappedFile()
  {
    Close();
  }

  MemoryMappedFile::MemoryMappedFile(MemoryMappedFile&& Other) noexcept
      : m_Path(std::move(Other.m_Path)), m_Data(Other.m_Data), m_Size(Other.m_Size), m_Access(Other.m_Access)
#ifdef _WIN32
        ,
        m_FileHandle(Other.m_FileHandle), m_MappingHandle(Other.m_MappingHandle)
#else
        ,
        m_Fd(Other.m_Fd)
#endif
  {
    Other.m_Data = nullptr;
    Other.m_Size = 0;
#ifdef _WIN32
    Other.m_FileHandle = nullptr;
    Other.m_MappingHandle = nullptr;
#else
    Other.m_Fd = -1;
#endif
  }

  MemoryMappedFile& MemoryMappedFile::operator=(MemoryMappedFile&& Other) noexcept
  {
    if (this != &Other)
    {
      Close();
      m_Path = std::move(Other.m_Path);
      m_Data = Other.m_Data;
      m_Size = Other.m_Size;
      m_Access = Other.m_Access;
#ifdef _WIN32
      m_FileHandle = Other.m_FileHandle;
      m_MappingHandle = Other.m_MappingHandle;
      Other.m_FileHandle = nullptr;
      Other.m_MappingHandle = nullptr;
#else
      m_Fd = Other.m_Fd;
      Other.m_Fd = -1;
#endif
      Other.m_Data = nullptr;
      Other.m_Size = 0;
    }
    return *this;
  }

  std::expected<void, std::string> MemoryMappedFile::Open(const std::string& Path, EMapAccess Access)
  {
    Close();

    m_Path = Path;
    m_Access = Access;

#ifdef _WIN32
    DWORD DesiredAccess = GENERIC_READ;
    DWORD ShareMode = FILE_SHARE_READ;
    DWORD Protect = PAGE_READONLY;
    DWORD MapAccess = FILE_MAP_READ;

    if (Access == EMapAccess::ReadWrite)
    {
      DesiredAccess |= GENERIC_WRITE;
      Protect = PAGE_READWRITE;
      MapAccess = FILE_MAP_ALL_ACCESS;
    }
    else if (Access == EMapAccess::CopyOnWrite)
    {
      Protect = PAGE_WRITECOPY;
      MapAccess = FILE_MAP_COPY;
    }

    m_FileHandle = CreateFileA(Path.c_str(), DesiredAccess, ShareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (m_FileHandle == INVALID_HANDLE_VALUE)
    {
      m_FileHandle = nullptr;
      return std::unexpected("Failed to open file: " + Path);
    }

    LARGE_INTEGER FileSize;
    if (!GetFileSizeEx(m_FileHandle, &FileSize))
    {
      CloseHandle(m_FileHandle);
      m_FileHandle = nullptr;
      return std::unexpected("Failed to get file size: " + Path);
    }

    m_Size = static_cast<size_t>(FileSize.QuadPart);

    if (m_Size == 0)
    {
      // Empty file - no mapping needed
      return {};
    }

    m_MappingHandle = CreateFileMappingA(m_FileHandle, nullptr, Protect, 0, 0, nullptr);

    if (!m_MappingHandle)
    {
      CloseHandle(m_FileHandle);
      m_FileHandle = nullptr;
      return std::unexpected("Failed to create file mapping: " + Path);
    }

    m_Data = static_cast<uint8_t*>(MapViewOfFile(m_MappingHandle, MapAccess, 0, 0, 0));

    if (!m_Data)
    {
      CloseHandle(m_MappingHandle);
      CloseHandle(m_FileHandle);
      m_MappingHandle = nullptr;
      m_FileHandle = nullptr;
      return std::unexpected("Failed to map file: " + Path);
    }

#else // POSIX

    int Flags = O_RDONLY;
    int Prot = PROT_READ;
    int MapFlags = MAP_PRIVATE;

    if (Access == EMapAccess::ReadWrite)
    {
      Flags = O_RDWR;
      Prot = PROT_READ | PROT_WRITE;
      MapFlags = MAP_SHARED;
    }
    else if (Access == EMapAccess::CopyOnWrite)
    {
      Prot = PROT_READ | PROT_WRITE;
      MapFlags = MAP_PRIVATE;
    }

    m_Fd = open(Path.c_str(), Flags);
    if (m_Fd < 0)
    {
      return std::unexpected("Failed to open file: " + Path);
    }

    struct stat St;
    if (fstat(m_Fd, &St) < 0)
    {
      close(m_Fd);
      m_Fd = -1;
      return std::unexpected("Failed to stat file: " + Path);
    }

    m_Size = static_cast<size_t>(St.st_size);

    if (m_Size == 0)
    {
      // Empty file - no mapping needed
      return {};
    }

    m_Data = static_cast<uint8_t*>(mmap(nullptr, m_Size, Prot, MapFlags, m_Fd, 0));
    if (m_Data == MAP_FAILED)
    {
      m_Data = nullptr;
      close(m_Fd);
      m_Fd = -1;
      return std::unexpected("Failed to mmap file: " + Path);
    }

#endif

    return {};
  }

  void MemoryMappedFile::Close()
  {
#ifdef _WIN32
    if (m_Data)
    {
      UnmapViewOfFile(m_Data);
      m_Data = nullptr;
    }
    if (m_MappingHandle)
    {
      CloseHandle(m_MappingHandle);
      m_MappingHandle = nullptr;
    }
    if (m_FileHandle)
    {
      CloseHandle(m_FileHandle);
      m_FileHandle = nullptr;
    }
#else
    if (m_Data && m_Size > 0)
    {
      munmap(m_Data, m_Size);
      m_Data = nullptr;
    }
    if (m_Fd >= 0)
    {
      close(m_Fd);
      m_Fd = -1;
    }
#endif
    m_Size = 0;
    m_Path.clear();
  }

  std::span<const uint8_t> MemoryMappedFile::GetSpan(size_t Offset, size_t Length) const
  {
    if (!m_Data || Offset >= m_Size)
    {
      return {};
    }
    Length = std::min(Length, m_Size - Offset);
    return {m_Data + Offset, Length};
  }

  std::expected<std::span<const uint8_t>, std::string> MemoryMappedFile::Read(size_t Offset, size_t Length) const
  {
    if (!m_Data)
    {
      return std::unexpected("File not mapped");
    }
    if (Offset >= m_Size)
    {
      return std::unexpected("Offset beyond file size");
    }
    if (Offset + Length > m_Size)
    {
      return std::unexpected("Read extends beyond file size");
    }
    return std::span<const uint8_t>{m_Data + Offset, Length};
  }

  void MemoryMappedFile::Prefetch(size_t Offset, size_t Length) const
  {
    if (!m_Data || Offset >= m_Size)
    {
      return;
    }
    Length = std::min(Length, m_Size - Offset);

#ifdef _WIN32
    // Windows: Use PrefetchVirtualMemory if available (Windows 8+)
    // For simplicity, we'll just touch the pages
    volatile uint8_t Dummy = 0;
    size_t PageSize = 4096;
    for (size_t I = 0; I < Length; I += PageSize)
    {
      Dummy = m_Data[Offset + I];
    }
    (void)Dummy;
#else
    // POSIX: Use madvise
    madvise(m_Data + Offset, Length, MADV_WILLNEED);
#endif
  }

  // ========== MemoryMappedRegion ==========

  MemoryMappedRegion::~MemoryMappedRegion()
  {
    Unmap();
  }

  MemoryMappedRegion::MemoryMappedRegion(MemoryMappedRegion&& Other) noexcept
      : m_Path(std::move(Other.m_Path)), m_Data(Other.m_Data), m_AlignedBase(Other.m_AlignedBase), m_Size(Other.m_Size),
        m_AlignedSize(Other.m_AlignedSize), m_FileOffset(Other.m_FileOffset), m_Access(Other.m_Access)
#ifdef _WIN32
        ,
        m_FileHandle(Other.m_FileHandle), m_MappingHandle(Other.m_MappingHandle)
#else
        ,
        m_Fd(Other.m_Fd)
#endif
  {
    Other.m_Data = nullptr;
    Other.m_AlignedBase = nullptr;
    Other.m_Size = 0;
    Other.m_AlignedSize = 0;
#ifdef _WIN32
    Other.m_FileHandle = nullptr;
    Other.m_MappingHandle = nullptr;
#else
    Other.m_Fd = -1;
#endif
  }

  MemoryMappedRegion& MemoryMappedRegion::operator=(MemoryMappedRegion&& Other) noexcept
  {
    if (this != &Other)
    {
      Unmap();
      m_Path = std::move(Other.m_Path);
      m_Data = Other.m_Data;
      m_AlignedBase = Other.m_AlignedBase;
      m_Size = Other.m_Size;
      m_AlignedSize = Other.m_AlignedSize;
      m_FileOffset = Other.m_FileOffset;
      m_Access = Other.m_Access;
#ifdef _WIN32
      m_FileHandle = Other.m_FileHandle;
      m_MappingHandle = Other.m_MappingHandle;
      Other.m_FileHandle = nullptr;
      Other.m_MappingHandle = nullptr;
#else
      m_Fd = Other.m_Fd;
      Other.m_Fd = -1;
#endif
      Other.m_Data = nullptr;
      Other.m_AlignedBase = nullptr;
      Other.m_Size = 0;
      Other.m_AlignedSize = 0;
    }
    return *this;
  }

  std::expected<void, std::string> MemoryMappedRegion::Map(const std::string& Path, size_t Offset, size_t Length, EMapAccess Access)
  {
    Unmap();

    m_Path = Path;
    m_Access = Access;
    m_FileOffset = Offset;
    m_Size = Length;

#ifdef _WIN32
    // Get system allocation granularity
    SYSTEM_INFO SysInfo;
    GetSystemInfo(&SysInfo);
    size_t Granularity = SysInfo.dwAllocationGranularity;

    // Align offset down to granularity boundary
    size_t AlignedOffset = (Offset / Granularity) * Granularity;
    size_t OffsetDelta = Offset - AlignedOffset;
    m_AlignedSize = Length + OffsetDelta;

    DWORD DesiredAccess = GENERIC_READ;
    DWORD Protect = PAGE_READONLY;
    DWORD MapAccess = FILE_MAP_READ;

    if (Access == EMapAccess::ReadWrite)
    {
      DesiredAccess |= GENERIC_WRITE;
      Protect = PAGE_READWRITE;
      MapAccess = FILE_MAP_ALL_ACCESS;
    }
    else if (Access == EMapAccess::CopyOnWrite)
    {
      Protect = PAGE_WRITECOPY;
      MapAccess = FILE_MAP_COPY;
    }

    m_FileHandle = CreateFileA(Path.c_str(), DesiredAccess, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_FileHandle == INVALID_HANDLE_VALUE)
    {
      m_FileHandle = nullptr;
      return std::unexpected("Failed to open file: " + Path);
    }

    m_MappingHandle = CreateFileMappingA(m_FileHandle, nullptr, Protect, 0, 0, nullptr);
    if (!m_MappingHandle)
    {
      CloseHandle(m_FileHandle);
      m_FileHandle = nullptr;
      return std::unexpected("Failed to create file mapping: " + Path);
    }

    DWORD OffsetHigh = static_cast<DWORD>(AlignedOffset >> 32);
    DWORD OffsetLow = static_cast<DWORD>(AlignedOffset & 0xFFFFFFFF);

    m_AlignedBase = static_cast<uint8_t*>(MapViewOfFile(m_MappingHandle, MapAccess, OffsetHigh, OffsetLow, m_AlignedSize));
    if (!m_AlignedBase)
    {
      CloseHandle(m_MappingHandle);
      CloseHandle(m_FileHandle);
      m_MappingHandle = nullptr;
      m_FileHandle = nullptr;
      return std::unexpected("Failed to map region: " + Path);
    }

    m_Data = m_AlignedBase + OffsetDelta;

#else // POSIX

    // Get page size for alignment
    long PageSize = sysconf(_SC_PAGE_SIZE);
    if (PageSize < 0)
      PageSize = 4096;

    // Align offset down to page boundary
    size_t AlignedOffset = (Offset / static_cast<size_t>(PageSize)) * static_cast<size_t>(PageSize);
    size_t OffsetDelta = Offset - AlignedOffset;
    m_AlignedSize = Length + OffsetDelta;

    int Flags = O_RDONLY;
    int Prot = PROT_READ;
    int MapFlags = MAP_PRIVATE;

    if (Access == EMapAccess::ReadWrite)
    {
      Flags = O_RDWR;
      Prot = PROT_READ | PROT_WRITE;
      MapFlags = MAP_SHARED;
    }
    else if (Access == EMapAccess::CopyOnWrite)
    {
      Prot = PROT_READ | PROT_WRITE;
      MapFlags = MAP_PRIVATE;
    }

    m_Fd = open(Path.c_str(), Flags);
    if (m_Fd < 0)
    {
      return std::unexpected("Failed to open file: " + Path);
    }

    m_AlignedBase = static_cast<uint8_t*>(mmap(nullptr, m_AlignedSize, Prot, MapFlags, m_Fd, static_cast<off_t>(AlignedOffset)));
    if (m_AlignedBase == MAP_FAILED)
    {
      m_AlignedBase = nullptr;
      close(m_Fd);
      m_Fd = -1;
      return std::unexpected("Failed to mmap region: " + Path);
    }

    m_Data = m_AlignedBase + OffsetDelta;

#endif

    return {};
  }

  void MemoryMappedRegion::Unmap()
  {
#ifdef _WIN32
    if (m_AlignedBase)
    {
      UnmapViewOfFile(m_AlignedBase);
      m_AlignedBase = nullptr;
      m_Data = nullptr;
    }
    if (m_MappingHandle)
    {
      CloseHandle(m_MappingHandle);
      m_MappingHandle = nullptr;
    }
    if (m_FileHandle)
    {
      CloseHandle(m_FileHandle);
      m_FileHandle = nullptr;
    }
#else
    if (m_AlignedBase && m_AlignedSize > 0)
    {
      munmap(m_AlignedBase, m_AlignedSize);
      m_AlignedBase = nullptr;
      m_Data = nullptr;
    }
    if (m_Fd >= 0)
    {
      close(m_Fd);
      m_Fd = -1;
    }
#endif
    m_Size = 0;
    m_AlignedSize = 0;
    m_Path.clear();
  }

  // ========== StreamingBulkReader ==========

  std::expected<void, std::string> StreamingBulkReader::Open(const std::string& PackPath)
  {
    return m_MappedFile.Open(PackPath, EMapAccess::ReadOnly);
  }

  void StreamingBulkReader::Close()
  {
    m_MappedFile.Close();
  }

  std::expected<std::span<const uint8_t>, std::string> StreamingBulkReader::ReadChunk(size_t Offset, size_t Size) const
  {
    return m_MappedFile.Read(Offset, Size);
  }

  std::expected<std::vector<uint8_t>, std::string> StreamingBulkReader::ReadAndDecompress(size_t Offset, size_t CompressedSize,
                                                                                          size_t UncompressedSize, uint8_t CompressionType) const
  {
    auto DataResult = m_MappedFile.Read(Offset, CompressedSize);
    if (!DataResult.has_value())
    {
      return std::unexpected(DataResult.error());
    }

    auto Span = *DataResult;

    // No compression
    if (CompressionType == 0 || CompressedSize == UncompressedSize)
    {
      return std::vector<uint8_t>(Span.begin(), Span.end());
    }

    // Convert to ESnPakCompression
    Pack::ESnPakCompression Mode;
    if (CompressionType == 1)
    {
      Mode = Pack::ESnPakCompression::LZ4;
    }
    else if (CompressionType == 2)
    {
      Mode = Pack::ESnPakCompression::Zstd;
    }
    else
    {
      return std::unexpected("Unknown compression type: " + std::to_string(CompressionType));
    }

    try
    {
      return Pack::Decompress(Span.data(), Span.size(), UncompressedSize, Mode);
    }
    catch (const std::exception& E)
    {
      return std::unexpected(std::string("Decompression failed: ") + E.what());
    }
  }

  void StreamingBulkReader::PrefetchRange(size_t Offset, size_t Size) const
  {
    m_MappedFile.Prefetch(Offset, Size);
  }

  std::expected<MemoryMappedRegion, std::string> StreamingBulkReader::MapRegion(size_t Offset, size_t Size) const
  {
    MemoryMappedRegion Region;
    auto Result = Region.Map(m_MappedFile.GetPath(), Offset, Size, EMapAccess::ReadOnly);
    if (!Result.has_value())
    {
      return std::unexpected(Result.error());
    }
    return Region;
  }

} // namespace SnAPI::AssetPipeline
