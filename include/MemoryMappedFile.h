#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Export.h"

namespace SnAPI::AssetPipeline
{

// Memory access mode
enum class EMapAccess
{
    ReadOnly,
    ReadWrite,
    CopyOnWrite,  // Changes are private to this mapping
};

// Memory-mapped file for efficient streaming access
class SNAPI_ASSETPIPELINE_API MemoryMappedFile
{
public:
    MemoryMappedFile() = default;
    ~MemoryMappedFile();

    // Open a file for memory-mapped access
    // Returns error string on failure
    std::expected<void, std::string> Open(const std::string& Path, EMapAccess Access = EMapAccess::ReadOnly);

    // Close the mapping
    void Close();

    // Check if the file is open
    bool IsOpen() const { return m_Data != nullptr; }

    // Get the total file size
    size_t GetSize() const { return m_Size; }

    // Get the full mapped data
    const uint8_t* GetData() const { return m_Data; }

    // Get a span of the mapped data
    std::span<const uint8_t> GetSpan() const { return {m_Data, m_Size}; }

    // Get a span of a region
    std::span<const uint8_t> GetSpan(size_t Offset, size_t Length) const;

    // Read bytes at an offset (safe version with bounds checking)
    std::expected<std::span<const uint8_t>, std::string> Read(size_t Offset, size_t Length) const;

    // Prefetch a region into memory (hint to OS)
    void Prefetch(size_t Offset, size_t Length) const;

    // Get the file path
    const std::string& GetPath() const { return m_Path; }

    // Non-copyable
    MemoryMappedFile(const MemoryMappedFile&) = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;

    // Movable
    MemoryMappedFile(MemoryMappedFile&& Other) noexcept;
    MemoryMappedFile& operator=(MemoryMappedFile&& Other) noexcept;

private:
    std::string m_Path;
    uint8_t* m_Data = nullptr;
    size_t m_Size = 0;
    EMapAccess m_Access = EMapAccess::ReadOnly;

#ifdef _WIN32
    void* m_FileHandle = nullptr;
    void* m_MappingHandle = nullptr;
#else
    int m_Fd = -1;
#endif
};

// Memory-mapped region within a file (for partial mapping of large files)
class SNAPI_ASSETPIPELINE_API MemoryMappedRegion
{
public:
    MemoryMappedRegion() = default;
    ~MemoryMappedRegion();

    // Map a region of a file
    std::expected<void, std::string> Map(const std::string& Path, size_t Offset, size_t Length,
                                          EMapAccess Access = EMapAccess::ReadOnly);

    // Unmap the region
    void Unmap();

    // Check if mapped
    bool IsMapped() const { return m_Data != nullptr; }

    // Get the mapped data
    const uint8_t* GetData() const { return m_Data; }
    size_t GetSize() const { return m_Size; }

    // Get as span
    std::span<const uint8_t> GetSpan() const { return {m_Data, m_Size}; }

    // Get the file offset this region starts at
    size_t GetFileOffset() const { return m_FileOffset; }

    // Non-copyable
    MemoryMappedRegion(const MemoryMappedRegion&) = delete;
    MemoryMappedRegion& operator=(const MemoryMappedRegion&) = delete;

    // Movable
    MemoryMappedRegion(MemoryMappedRegion&& Other) noexcept;
    MemoryMappedRegion& operator=(MemoryMappedRegion&& Other) noexcept;

private:
    std::string m_Path;
    uint8_t* m_Data = nullptr;
    uint8_t* m_AlignedBase = nullptr;  // Actual mmap base (may differ due to alignment)
    size_t m_Size = 0;
    size_t m_AlignedSize = 0;
    size_t m_FileOffset = 0;
    EMapAccess m_Access = EMapAccess::ReadOnly;

#ifdef _WIN32
    void* m_FileHandle = nullptr;
    void* m_MappingHandle = nullptr;
#else
    int m_Fd = -1;
#endif
};

// Streaming bulk chunk reader that uses memory mapping
class SNAPI_ASSETPIPELINE_API StreamingBulkReader
{
public:
    StreamingBulkReader() = default;
    ~StreamingBulkReader() = default;

    // Open a pack file for streaming access
    std::expected<void, std::string> Open(const std::string& PackPath);

    // Close the reader
    void Close();

    // Check if open
    bool IsOpen() const { return m_MappedFile.IsOpen(); }

    // Get the pack file size
    size_t GetPackSize() const { return m_MappedFile.GetSize(); }

    // Read a chunk at a specific offset (zero-copy if possible)
    // Returns a span pointing directly into the memory-mapped region
    std::expected<std::span<const uint8_t>, std::string> ReadChunk(size_t Offset, size_t Size) const;

    // Read and decompress a chunk
    // This allocates memory for the decompressed data
    std::expected<std::vector<uint8_t>, std::string> ReadAndDecompress(
        size_t Offset, size_t CompressedSize, size_t UncompressedSize, uint8_t CompressionType) const;

    // Prefetch chunks for upcoming reads (async hint to OS)
    void PrefetchRange(size_t Offset, size_t Size) const;

    // Map a specific region for extended access
    std::expected<MemoryMappedRegion, std::string> MapRegion(size_t Offset, size_t Size) const;

private:
    MemoryMappedFile m_MappedFile;
};

} // namespace SnAPI::AssetPipeline
