#include "AssetPackReader.h"
#include "SnPakFormat.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <cstring>

namespace SnAPI::AssetPipeline
{

  // Forward declarations from Compression.cpp
  namespace Pack
  {
    std::vector<uint8_t> Decompress(const uint8_t* Data, size_t CompressedSize, size_t UncompressedSize, ESnPakCompression Mode);
  }

  struct AssetPackReader::Impl
  {
      std::string FilePath;
      mutable std::ifstream File; // Used only during Open() for initialization

      Pack::SnPakHeaderV1 Header;
      std::vector<std::string> StringTable;
      std::vector<Pack::SnPakIndexEntryV1> IndexEntries;
      std::vector<Pack::SnPakBulkEntryV1> BulkEntries;

      std::unordered_map<AssetId, uint32_t, UuidHash> AssetIdToIndex;
      std::unordered_map<uint64_t, std::vector<uint32_t>> NameHashToIndices;

      bool bOpen = false;

      // Validated file size (minimum of actual file size and header-reported size)
      uint64_t ValidatedFileSize = 0;

      // Maximum sane limits for allocations (protect against corrupted files)
      static constexpr size_t kMaxStringCount = 10'000'000;     // 10 million strings
      static constexpr size_t kMaxBlockSize = 1'000'000'000;    // 1 GB
      static constexpr size_t kMaxEntryCount = 10'000'000;      // 10 million assets
      static constexpr size_t kMaxBulkEntryCount = 100'000'000; // 100 million bulk entries

      // ─────────────────────────────────────────────────────────────────────────
      // FIX #1: CheckRange - Enforce file bounds on every read/seek
      // ─────────────────────────────────────────────────────────────────────────
      bool CheckRange(uint64_t Offset, uint64_t Size) const
      {
        // Check for overflow
        if (Size > ValidatedFileSize)
        {
          return false;
        }
        if (Offset > ValidatedFileSize - Size)
        {
          return false;
        }
        return true;
      }

      // ─────────────────────────────────────────────────────────────────────────
      // FIX #2: ReadExact - Check every read() succeeded (short read detection)
      // ─────────────────────────────────────────────────────────────────────────
      bool ReadExact(void* Dst, size_t Bytes) const
      {
        File.read(reinterpret_cast<char*>(Dst), static_cast<std::streamsize>(Bytes));
        return File.good() && static_cast<size_t>(File.gcount()) == Bytes;
      }

      bool SeekAndReadExact(uint64_t Offset, void* Dst, size_t Bytes) const
      {
        if (!CheckRange(Offset, Bytes))
        {
          return false;
        }
        File.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
        if (!File.good())
        {
          return false;
        }
        return ReadExact(Dst, Bytes);
      }

      // ─────────────────────────────────────────────────────────────────────────
      // FIX #5 & #7: Safe string table reading with bounds checking
      // ─────────────────────────────────────────────────────────────────────────
      std::expected<void, std::string> ReadStringTable()
      {
        // FIX #7: Validate string table offset/size against file bounds FIRST
        if (!CheckRange(Header.StringTableOffset, Header.StringTableSize))
        {
          return std::unexpected("String table offset/size exceeds file bounds");
        }

        // Validate minimum size for header
        if (Header.StringTableSize < sizeof(Pack::SnPakStrBlockHeaderV1))
        {
          return std::unexpected("String table size too small for header");
        }

        Pack::SnPakStrBlockHeaderV1 StrHeader;
        if (!SeekAndReadExact(Header.StringTableOffset, &StrHeader, sizeof(StrHeader)))
        {
          return std::unexpected("Failed to read string table header");
        }

        // Validate magic
        if (std::memcmp(StrHeader.Magic, Pack::kStringMagic, 4) != 0)
        {
          return std::unexpected("Invalid string table magic");
        }

        // Validate version
        if (StrHeader.Version != 1)
        {
          return std::unexpected("Unsupported string table version: " + std::to_string(StrHeader.Version));
        }

        // FIX #7: Validate BlockSize matches header
        if (StrHeader.BlockSize != Header.StringTableSize)
        {
          return std::unexpected("String table BlockSize mismatch with header");
        }

        // Sanity check string count
        if (StrHeader.StringCount > kMaxStringCount)
        {
          return std::unexpected("String count exceeds sanity limit");
        }

        // FIX #7: Compute and validate minimum expected size
        size_t OffsetsSize = static_cast<size_t>(StrHeader.StringCount) * sizeof(uint32_t);
        size_t MinExpected = sizeof(Pack::SnPakStrBlockHeaderV1) + OffsetsSize;
        if (StrHeader.BlockSize < MinExpected)
        {
          return std::unexpected("String table BlockSize too small for offset array");
        }

        // FIX #7: Validate entire block is within file bounds
        if (!CheckRange(Header.StringTableOffset, StrHeader.BlockSize))
        {
          return std::unexpected("String table block exceeds file bounds");
        }

        // Read offsets array
        std::vector<uint32_t> Offsets(StrHeader.StringCount);
        if (StrHeader.StringCount > 0)
        {
          if (!ReadExact(Offsets.data(), OffsetsSize))
          {
            return std::unexpected("Failed to read string table offsets");
          }
        }

        // Calculate and read string data
        size_t StringDataSize = StrHeader.BlockSize - sizeof(Pack::SnPakStrBlockHeaderV1) - OffsetsSize;
        std::vector<uint8_t> StringData(StringDataSize);
        if (StringDataSize > 0)
        {
          if (!ReadExact(StringData.data(), StringDataSize))
          {
            return std::unexpected("Failed to read string data");
          }
        }

        // Verify hash of string data
        XXH128_hash_t Hash = XXH3_128bits(StringData.data(), StringData.size());
        if (Hash.high64 != StrHeader.HashHi || Hash.low64 != StrHeader.HashLo)
        {
          return std::unexpected("String table hash mismatch - data corrupted");
        }

        // FIX #5: Safe string parsing with offset bounds and null terminator checks
        StringTable.clear();
        StringTable.reserve(StrHeader.StringCount);

        for (uint32_t i = 0; i < StrHeader.StringCount; ++i)
        {
          uint32_t Offset = Offsets[i];

          // FIX #5: Validate offset is within string data bounds
          if (Offset >= StringDataSize)
          {
            return std::unexpected("String offset " + std::to_string(i) + " out of bounds");
          }

          // FIX #5: Find null terminator within remaining bounds
          const uint8_t* Start = StringData.data() + Offset;
          size_t MaxLen = StringDataSize - Offset;
          const void* NullPos = std::memchr(Start, 0, MaxLen);

          if (NullPos == nullptr)
          {
            return std::unexpected("String " + std::to_string(i) + " missing null terminator");
          }

          // FIX #5: Construct string with explicit length
          size_t StrLen = static_cast<const uint8_t*>(NullPos) - Start;
          StringTable.emplace_back(reinterpret_cast<const char*>(Start), StrLen);
        }

        return {};
      }

      // ─────────────────────────────────────────────────────────────────────────
      // FIX #6 & #8: Safe index reading with bounds checking and streaming hash
      // ─────────────────────────────────────────────────────────────────────────
      std::expected<void, std::string> ReadIndex()
      {
        // FIX #6: Validate index offset/size against file bounds FIRST
        if (!CheckRange(Header.IndexOffset, Header.IndexSize))
        {
          return std::unexpected("Index offset/size exceeds file bounds");
        }

        // Validate minimum size for header
        if (Header.IndexSize < sizeof(Pack::SnPakIndexHeaderV1))
        {
          return std::unexpected("Index size too small for header");
        }

        Pack::SnPakIndexHeaderV1 IdxHeader;
        if (!SeekAndReadExact(Header.IndexOffset, &IdxHeader, sizeof(IdxHeader)))
        {
          return std::unexpected("Failed to read index header");
        }

        // Validate magic
        if (std::memcmp(IdxHeader.Magic, Pack::kIndexMagic, 4) != 0)
        {
          return std::unexpected("Invalid index magic");
        }

        // Validate version
        if (IdxHeader.Version != 1)
        {
          return std::unexpected("Unsupported index version: " + std::to_string(IdxHeader.Version));
        }

        // FIX #6: Validate BlockSize matches header
        if (IdxHeader.BlockSize != Header.IndexSize)
        {
          return std::unexpected("Index BlockSize mismatch with header");
        }

        // Sanity check entry counts
        if (IdxHeader.EntryCount > kMaxEntryCount)
        {
          return std::unexpected("Entry count exceeds sanity limit");
        }
        if (IdxHeader.BulkEntryCount > kMaxBulkEntryCount)
        {
          return std::unexpected("Bulk entry count exceeds sanity limit");
        }

        // FIX #6: Compute expected size and validate strictly
        size_t EntriesSize = static_cast<size_t>(IdxHeader.EntryCount) * sizeof(Pack::SnPakIndexEntryV1);
        size_t BulkEntriesSize = static_cast<size_t>(IdxHeader.BulkEntryCount) * sizeof(Pack::SnPakBulkEntryV1);
        size_t ExpectedSize = sizeof(Pack::SnPakIndexHeaderV1) + EntriesSize + BulkEntriesSize;

        if (IdxHeader.BlockSize != ExpectedSize)
        {
          return std::unexpected("Index BlockSize does not match expected size for entry counts");
        }

        // FIX #6: Validate entire block is within file bounds
        if (!CheckRange(Header.IndexOffset, IdxHeader.BlockSize))
        {
          return std::unexpected("Index block exceeds file bounds");
        }

        // Read entries
        IndexEntries.resize(IdxHeader.EntryCount);
        if (IdxHeader.EntryCount > 0)
        {
          if (!ReadExact(IndexEntries.data(), EntriesSize))
          {
            return std::unexpected("Failed to read index entries");
          }
        }

        // Read bulk entries
        BulkEntries.resize(IdxHeader.BulkEntryCount);
        if (IdxHeader.BulkEntryCount > 0)
        {
          if (!ReadExact(BulkEntries.data(), BulkEntriesSize))
          {
            return std::unexpected("Failed to read bulk entries");
          }
        }

        // FIX #8: Use streaming xxhash API to avoid large copy buffer
        XXH3_state_t* State = XXH3_createState();
        if (State == nullptr)
        {
          return std::unexpected("Failed to create hash state");
        }

        XXH3_128bits_reset(State);

        if (!IndexEntries.empty())
        {
          XXH3_128bits_update(State, IndexEntries.data(), EntriesSize);
        }
        if (!BulkEntries.empty())
        {
          XXH3_128bits_update(State, BulkEntries.data(), BulkEntriesSize);
        }

        XXH128_hash_t Hash = XXH3_128bits_digest(State);
        XXH3_freeState(State);

        if (Hash.high64 != IdxHeader.EntriesHashHi || Hash.low64 != IdxHeader.EntriesHashLo)
        {
          return std::unexpected("Index entries hash mismatch - data corrupted");
        }

        // FIX #4: Verify the header's IndexHash against the entire index block
        // The header stores a hash of the complete index block (header + entries + bulk entries)
        // This provides an additional integrity check at a different level than EntriesHash
        {
          XXH3_state_t* BlockState = XXH3_createState();
          if (BlockState == nullptr)
          {
            return std::unexpected("Failed to create hash state for index block");
          }

          XXH3_128bits_reset(BlockState);

          // Hash the index header (includes EntriesHashHi/Lo as written)
          XXH3_128bits_update(BlockState, &IdxHeader, sizeof(IdxHeader));

          // Hash entries and bulk entries
          if (!IndexEntries.empty())
          {
            XXH3_128bits_update(BlockState, IndexEntries.data(), EntriesSize);
          }
          if (!BulkEntries.empty())
          {
            XXH3_128bits_update(BlockState, BulkEntries.data(), BulkEntriesSize);
          }

          XXH128_hash_t BlockHash = XXH3_128bits_digest(BlockState);
          XXH3_freeState(BlockState);

          if (BlockHash.high64 != Header.IndexHashHi || BlockHash.low64 != Header.IndexHashLo)
          {
            return std::unexpected("Index block hash mismatch with header - data corrupted");
          }
        }

        // Build lookup maps
        AssetIdToIndex.clear();
        NameHashToIndices.clear();

        for (uint32_t i = 0; i < IndexEntries.size(); ++i)
        {
          AssetId Id;
          std::memcpy(Id.Bytes, IndexEntries[i].AssetId, 16);
          AssetIdToIndex[Id] = i;
          NameHashToIndices[IndexEntries[i].NameHash64].push_back(i);
        }

        return {};
      }

      // ─────────────────────────────────────────────────────────────────────────
      // FIX #3 & #4 & #6: LoadChunk with size validation and chunk identity checks
      // FIX #4 (mutex): Each call opens its own file stream for true parallelism
      // FIX #6: Added ExpectedBulkAssetId for bulk chunk AssetId validation
      // ─────────────────────────────────────────────────────────────────────────
      std::expected<std::vector<uint8_t>, std::string> LoadChunk(uint64_t Offset, uint64_t ExpectedTotalSize, uint64_t ExpectedUncompressedSize,
                                                                 const Pack::SnPakIndexEntryV1* ExpectedEntry = nullptr,
                                                                 const Pack::SnPakBulkEntryV1* ExpectedBulkEntry = nullptr,
                                                                 const uint8_t* ExpectedBulkAssetId = nullptr) const
      {
        // FIX #4: Open a dedicated stream for this chunk load
        // This enables true parallel reads from multiple threads without mutex contention
        std::ifstream ChunkFile(FilePath, std::ios::binary);
        if (!ChunkFile.is_open())
        {
          return std::unexpected("Failed to open file for chunk read: " + FilePath);
        }

        // Helper lambdas for local stream operations
        auto LocalReadExact = [&ChunkFile](void* Dst, size_t Bytes) -> bool {
          ChunkFile.read(reinterpret_cast<char*>(Dst), static_cast<std::streamsize>(Bytes));
          return ChunkFile.good() && static_cast<size_t>(ChunkFile.gcount()) == Bytes;
        };

        auto LocalSeekAndRead = [&, this](uint64_t Off, void* Dst, size_t Bytes) -> bool {
          if (!CheckRange(Off, Bytes))
          {
            return false;
          }
          ChunkFile.seekg(static_cast<std::streamoff>(Off), std::ios::beg);
          if (!ChunkFile.good())
          {
            return false;
          }
          return LocalReadExact(Dst, Bytes);
        };

        // FIX #3: Validate chunk location against file bounds using INDEX size
        if (!CheckRange(Offset, ExpectedTotalSize))
        {
          return std::unexpected("Chunk offset/size exceeds file bounds");
        }

        // Ensure minimum size for chunk header
        if (ExpectedTotalSize < sizeof(Pack::SnPakChunkHeaderV1))
        {
          return std::unexpected("Chunk total size too small for header");
        }

        Pack::SnPakChunkHeaderV1 ChunkHeader;
        if (!LocalSeekAndRead(Offset, &ChunkHeader, sizeof(ChunkHeader)))
        {
          return std::unexpected("Failed to read chunk header");
        }

        // Validate magic
        if (std::memcmp(ChunkHeader.Magic, Pack::kChunkMagic, 4) != 0)
        {
          return std::unexpected("Invalid chunk magic");
        }

        // Validate version
        if (ChunkHeader.Version != 1)
        {
          return std::unexpected("Unsupported chunk version: " + std::to_string(ChunkHeader.Version));
        }

        // FIX #3: Verify chunk header sizes match index expectations
        if (ChunkHeader.SizeUncompressed != ExpectedUncompressedSize)
        {
          return std::unexpected("Chunk uncompressed size mismatch with index");
        }

        uint64_t ExpectedCompressedDataSize = ExpectedTotalSize - sizeof(Pack::SnPakChunkHeaderV1);
        if (ChunkHeader.SizeCompressed != ExpectedCompressedDataSize)
        {
          return std::unexpected("Chunk compressed size mismatch with index");
        }

        // Sanity check sizes
        if (ChunkHeader.SizeCompressed > kMaxBlockSize)
        {
          return std::unexpected("Chunk compressed size exceeds sanity limit");
        }
        if (ChunkHeader.SizeUncompressed > kMaxBlockSize)
        {
          return std::unexpected("Chunk uncompressed size exceeds sanity limit");
        }

        // FIX #4: Validate chunk identity for main payloads
        if (ExpectedEntry != nullptr)
        {
          // Must be MainPayload kind
          if (ChunkHeader.ChunkKind != static_cast<uint8_t>(Pack::ESnPakChunkKind::MainPayload))
          {
            return std::unexpected("Chunk kind mismatch - expected MainPayload");
          }

          // AssetId must match
          if (!Pack::CompareUuid(ChunkHeader.AssetId, ExpectedEntry->AssetId))
          {
            return std::unexpected("Chunk AssetId mismatch with index entry");
          }

          // PayloadType must match
          if (!Pack::CompareUuid(ChunkHeader.PayloadType, ExpectedEntry->CookedPayloadType))
          {
            return std::unexpected("Chunk PayloadType mismatch with index entry");
          }

          // SchemaVersion must match
          if (ChunkHeader.SchemaVersion != ExpectedEntry->CookedSchemaVersion)
          {
            return std::unexpected("Chunk SchemaVersion mismatch with index entry");
          }

          // Compression must match
          if (ChunkHeader.Compression != ExpectedEntry->Compression)
          {
            return std::unexpected("Chunk Compression mismatch with index entry");
          }
        }

        // FIX #4 & #6: Validate chunk identity for bulk data
        if (ExpectedBulkEntry != nullptr)
        {
          // Must be Bulk kind
          if (ChunkHeader.ChunkKind != static_cast<uint8_t>(Pack::ESnPakChunkKind::Bulk))
          {
            return std::unexpected("Chunk kind mismatch - expected Bulk");
          }

          // Compression must match
          if (ChunkHeader.Compression != ExpectedBulkEntry->Compression)
          {
            return std::unexpected("Bulk chunk Compression mismatch with bulk entry");
          }

          // FIX #6: AssetId must match the parent asset
          if (ExpectedBulkAssetId != nullptr && !Pack::CompareUuid(ChunkHeader.AssetId, ExpectedBulkAssetId))
          {
            return std::unexpected("Bulk chunk AssetId mismatch - chunk belongs to different asset");
          }
        }

        // Decompress (or read directly for uncompressed data)
        auto Mode = static_cast<Pack::ESnPakCompression>(ChunkHeader.Compression);
        std::vector<uint8_t> Output;

        if (Mode == Pack::ESnPakCompression::None)
        {
          // FIX #3: For uncompressed data, read directly into output buffer
          // This avoids allocating a separate CompressedData buffer
          if (ChunkHeader.SizeCompressed != ChunkHeader.SizeUncompressed)
          {
            return std::unexpected("Uncompressed chunk has mismatched sizes");
          }
          Output.resize(ChunkHeader.SizeUncompressed);
          if (ChunkHeader.SizeUncompressed > 0)
          {
            if (!LocalReadExact(Output.data(), Output.size()))
            {
              return std::unexpected("Failed to read chunk data");
            }
          }
        }
        else
        {
          // Read compressed data, then decompress
          std::vector<uint8_t> CompressedData(ChunkHeader.SizeCompressed);
          if (ChunkHeader.SizeCompressed > 0)
          {
            if (!LocalReadExact(CompressedData.data(), CompressedData.size()))
            {
              return std::unexpected("Failed to read chunk compressed data");
            }
          }

          try
          {
            Output = Pack::Decompress(CompressedData.data(), CompressedData.size(), ChunkHeader.SizeUncompressed, Mode);
          }
          catch (const std::exception& E)
          {
            return std::unexpected(std::string("Decompression failed: ") + E.what());
          }
        }

        // Verify hash of decompressed/output data
        XXH128_hash_t Hash = XXH3_128bits(Output.data(), Output.size());
        if (Hash.high64 != ChunkHeader.HashHi || Hash.low64 != ChunkHeader.HashLo)
        {
          return std::unexpected("Chunk hash mismatch - data corrupted");
        }

        return Output;
      }
  };

  AssetPackReader::AssetPackReader() : m_Impl(std::make_unique<Impl>()) {}

  AssetPackReader::~AssetPackReader() = default;

  AssetPackReader::AssetPackReader(AssetPackReader&&) noexcept = default;
  AssetPackReader& AssetPackReader::operator=(AssetPackReader&&) noexcept = default;

  std::expected<void, std::string> AssetPackReader::Open(const std::string& Path)
  {
    m_Impl->FilePath = Path;
    m_Impl->File.open(Path, std::ios::binary);

    if (!m_Impl->File.is_open())
    {
      return std::unexpected("Failed to open file: " + Path);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // FIX #1: Get actual file size for bounds validation
    // ─────────────────────────────────────────────────────────────────────────
    std::error_code EC;
    auto ActualFileSize = std::filesystem::file_size(Path, EC);
    if (EC)
    {
      return std::unexpected("Failed to get file size: " + EC.message());
    }

    // Must be at least large enough for the header
    if (ActualFileSize < sizeof(Pack::SnPakHeaderV1))
    {
      return std::unexpected("File too small to contain header");
    }

    // Temporarily set validated size to read header
    m_Impl->ValidatedFileSize = ActualFileSize;

    // Read header
    if (!m_Impl->ReadExact(&m_Impl->Header, sizeof(Pack::SnPakHeaderV1)))
    {
      return std::unexpected("Failed to read pack header");
    }

    // Validate magic
    if (std::memcmp(m_Impl->Header.Magic, Pack::kSnPakMagic, 8) != 0)
    {
      return std::unexpected("Invalid pack file magic");
    }

    // Validate version
    if (m_Impl->Header.Version != Pack::kSnPakVersion)
    {
      return std::unexpected("Unsupported pack version: " + std::to_string(m_Impl->Header.Version));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // FIX #9: Validate Header.HeaderSize
    // ─────────────────────────────────────────────────────────────────────────
    if (m_Impl->Header.HeaderSize != sizeof(Pack::SnPakHeaderV1))
    {
      return std::unexpected("Header size mismatch - expected " + std::to_string(sizeof(Pack::SnPakHeaderV1)) + ", got " +
                             std::to_string(m_Impl->Header.HeaderSize));
    }

    // Validate endian marker
    if (m_Impl->Header.EndianMarker != Pack::kEndianMarker)
    {
      return std::unexpected("Endian mismatch - pack was created on different architecture");
    }

    // FIX #1: Use minimum of header FileSize and actual file size
    // This prevents issues with truncated files or oversized headers
    if (m_Impl->Header.FileSize > ActualFileSize)
    {
      return std::unexpected("Header FileSize (" + std::to_string(m_Impl->Header.FileSize) + ") exceeds actual file size (" +
                             std::to_string(ActualFileSize) + ")");
    }
    m_Impl->ValidatedFileSize = m_Impl->Header.FileSize;

    // Read string table
    auto StrResult = m_Impl->ReadStringTable();
    if (!StrResult.has_value())
    {
      return std::unexpected("Failed to read string table: " + StrResult.error());
    }

    // Read index
    auto IdxResult = m_Impl->ReadIndex();
    if (!IdxResult.has_value())
    {
      return std::unexpected("Failed to read index: " + IdxResult.error());
    }

    m_Impl->bOpen = true;
    return {};
  }

  void AssetPackReader::Close()
  {
    if (m_Impl->File.is_open())
    {
      m_Impl->File.close();
    }
    m_Impl->bOpen = false;
    m_Impl->ValidatedFileSize = 0;
    m_Impl->StringTable.clear();
    m_Impl->IndexEntries.clear();
    m_Impl->BulkEntries.clear();
    m_Impl->AssetIdToIndex.clear();
    m_Impl->NameHashToIndices.clear();
  }

  bool AssetPackReader::IsOpen() const
  {
    return m_Impl->bOpen;
  }

  uint32_t AssetPackReader::GetAssetCount() const
  {
    return static_cast<uint32_t>(m_Impl->IndexEntries.size());
  }

  std::expected<AssetInfo, std::string> AssetPackReader::GetAssetInfo(uint32_t Index) const
  {
    if (Index >= m_Impl->IndexEntries.size())
    {
      return std::unexpected("Index out of range");
    }

    const auto& Entry = m_Impl->IndexEntries[Index];

    AssetInfo Info;
    std::memcpy(Info.Id.Bytes, Entry.AssetId, 16);
    std::memcpy(Info.AssetKind.Bytes, Entry.AssetKind, 16);
    std::memcpy(Info.CookedPayloadType.Bytes, Entry.CookedPayloadType, 16);
    Info.SchemaVersion = Entry.CookedSchemaVersion;

    if (Entry.NameStringId < m_Impl->StringTable.size())
    {
      Info.Name = m_Impl->StringTable[Entry.NameStringId];
    }

    if (Entry.VariantStringId != 0xFFFFFFFF && Entry.VariantStringId < m_Impl->StringTable.size())
    {
      Info.VariantKey = m_Impl->StringTable[Entry.VariantStringId];
    }

    Info.BulkChunkCount = Entry.BulkCount;

    return Info;
  }

  std::expected<AssetInfo, std::string> AssetPackReader::FindAsset(AssetId Id) const
  {
    auto It = m_Impl->AssetIdToIndex.find(Id);
    if (It == m_Impl->AssetIdToIndex.end())
    {
      return std::unexpected("Asset not found: " + Id.ToString());
    }

    return GetAssetInfo(It->second);
  }

  std::vector<AssetInfo> AssetPackReader::FindAssetsByName(const std::string& Name) const
  {
    std::vector<AssetInfo> Results;

    uint64_t NameHash = XXH3_64bits(Name.data(), Name.size());
    auto It = m_Impl->NameHashToIndices.find(NameHash);
    if (It != m_Impl->NameHashToIndices.end())
    {
      for (uint32_t Index : It->second)
      {
        auto Info = GetAssetInfo(Index);
        if (Info.has_value() && Info->Name == Name)
        {
          Results.push_back(*Info);
        }
      }
    }

    return Results;
  }

  std::expected<TypedPayload, std::string> AssetPackReader::LoadCookedPayload(AssetId Id) const
  {
    auto It = m_Impl->AssetIdToIndex.find(Id);
    if (It == m_Impl->AssetIdToIndex.end())
    {
      return std::unexpected("Asset not found: " + Id.ToString());
    }

    const auto& Entry = m_Impl->IndexEntries[It->second];

    // FIX #3 & #4: Pass entry for size and identity validation
    auto ChunkResult = m_Impl->LoadChunk(Entry.PayloadChunkOffset, Entry.PayloadChunkSizeCompressed, Entry.PayloadChunkSizeUncompressed,
                                         &Entry,   // For identity validation
                                         nullptr); // Not a bulk chunk

    if (!ChunkResult.has_value())
    {
      return std::unexpected(ChunkResult.error());
    }

    TypedPayload Payload;
    std::memcpy(Payload.PayloadType.Bytes, Entry.CookedPayloadType, 16);
    Payload.SchemaVersion = Entry.CookedSchemaVersion;
    Payload.Bytes = std::move(*ChunkResult);

    return Payload;
  }

  std::expected<std::vector<uint8_t>, std::string> AssetPackReader::LoadBulkChunk(AssetId Id, uint32_t BulkIndex) const
  {
    auto It = m_Impl->AssetIdToIndex.find(Id);
    if (It == m_Impl->AssetIdToIndex.end())
    {
      return std::unexpected("Asset not found: " + Id.ToString());
    }

    const auto& Entry = m_Impl->IndexEntries[It->second];

    if (!(Entry.Flags & Pack::IndexEntryFlag_HasBulk) || BulkIndex >= Entry.BulkCount)
    {
      return std::unexpected("Bulk chunk index out of range");
    }

    uint32_t GlobalBulkIndex = Entry.BulkFirstIndex + BulkIndex;
    if (GlobalBulkIndex >= m_Impl->BulkEntries.size())
    {
      return std::unexpected("Invalid bulk entry index");
    }

    const auto& BulkEntry = m_Impl->BulkEntries[GlobalBulkIndex];

    // FIX #6: Validate SubIndex matches the expected bulk index position
    // This ensures the bulk entry was written with correct SubIndex == array position
    if (BulkEntry.SubIndex != BulkIndex)
    {
      return std::unexpected("Bulk SubIndex mismatch (expected " + std::to_string(BulkIndex) + ", got " + std::to_string(BulkEntry.SubIndex) +
                             ") - corrupt or wrong pack");
    }

    // FIX #3 & #4 & #6: Pass bulk entry for size and identity validation
    // FIX #6: Also pass the parent asset's AssetId for chunk identity verification
    return m_Impl->LoadChunk(BulkEntry.ChunkOffset, BulkEntry.SizeCompressed, BulkEntry.SizeUncompressed,
                             nullptr,           // Not validating against main entry
                             &BulkEntry,        // For bulk identity validation
                             Entry.AssetId);    // FIX #6: For AssetId validation
  }

  std::expected<AssetPackReader::BulkChunkInfo, std::string> AssetPackReader::GetBulkChunkInfo(AssetId Id, uint32_t BulkIndex) const
  {
    auto It = m_Impl->AssetIdToIndex.find(Id);
    if (It == m_Impl->AssetIdToIndex.end())
    {
      return std::unexpected("Asset not found: " + Id.ToString());
    }

    const auto& Entry = m_Impl->IndexEntries[It->second];

    if (!(Entry.Flags & Pack::IndexEntryFlag_HasBulk) || BulkIndex >= Entry.BulkCount)
    {
      return std::unexpected("Bulk chunk index out of range");
    }

    uint32_t GlobalBulkIndex = Entry.BulkFirstIndex + BulkIndex;
    if (GlobalBulkIndex >= m_Impl->BulkEntries.size())
    {
      return std::unexpected("Invalid bulk entry index");
    }

    const auto& BulkEntry = m_Impl->BulkEntries[GlobalBulkIndex];

    BulkChunkInfo Info;
    uint32_t SemanticVal;
    std::memcpy(&SemanticVal, BulkEntry.Semantic, 4);
    Info.Semantic = static_cast<EBulkSemantic>(SemanticVal);
    Info.SubIndex = BulkEntry.SubIndex;
    Info.UncompressedSize = BulkEntry.SizeUncompressed;

    return Info;
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // FIX #10: Append-update mode documentation
  // ─────────────────────────────────────────────────────────────────────────────
  // DESIGN NOTE: This reader intentionally loads only the "current active" index
  // pointed to by Header.IndexOffset. The PreviousIndexOffset/PreviousIndexSize
  // fields in both the header and index blocks exist for:
  //   1. Tooling that needs to inspect the history of append operations
  //   2. Potential future merge/compaction tools
  //   3. Rollback scenarios
  //
  // The current design philosophy is "latest index wins" - when a pack is
  // updated via AppendUpdate(), the new index contains all assets the writer
  // wants to be active. Previous indices are retained for history but are NOT
  // automatically merged.
  //
  // If you need to access assets from previous indices, you would need to:
  //   1. Read Header.PreviousIndexOffset/Size
  //   2. Manually load and parse that index block
  //   3. Merge the entries yourself (handling duplicates by AssetId)
  //
  // This keeps the reader simple and fast for the common case.
  // ─────────────────────────────────────────────────────────────────────────────

} // namespace SnAPI::AssetPipeline
