#include "AssetPackWriter.h"
#include "SnPakFormat.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <cstring>

namespace SnAPI::AssetPipeline
{

  // Forward declarations from Compression.cpp
  namespace Pack
  {


  } // namespace Pack

  struct AssetPackWriter::Impl
  {
      std::vector<AssetPackEntry> Assets;
      Pack::ESnPakCompression Compression = Pack::ESnPakCompression::Zstd;
      Pack::ESnPakCompressionLevel CompressionLevel = Pack::ESnPakCompressionLevel::Default;

      static Pack::ESnPakCompression ToInternalCompression(const EPackCompression Mode)
      {
        switch (Mode)
        {
          case EPackCompression::None:
            return Pack::ESnPakCompression::None;
          case EPackCompression::LZ4:
            return Pack::ESnPakCompression::LZ4;
          case EPackCompression::LZ4HC:
            return Pack::ESnPakCompression::LZ4HC;
          case EPackCompression::Zstd:
            return Pack::ESnPakCompression::Zstd;
          case EPackCompression::ZstdFast:
            return Pack::ESnPakCompression::ZstdFast;
          default:
            return Pack::ESnPakCompression::Zstd;
        }
      }

      static Pack::ESnPakCompressionLevel ToInternalCompressionLevel(const EPackCompressionLevel Level)
      {
        switch (Level)
        {
          case EPackCompressionLevel::Fast:
            return Pack::ESnPakCompressionLevel::Fast;
          case EPackCompressionLevel::High:
            return Pack::ESnPakCompressionLevel::High;
          case EPackCompressionLevel::Max:
            return Pack::ESnPakCompressionLevel::Max;
          case EPackCompressionLevel::Default:
          default:
            return Pack::ESnPakCompressionLevel::Default;
        }
      }

      std::vector<uint8_t> CompressData(const uint8_t* Data, size_t Size) const
      {
        return Pack::Compress(Data, Size, Compression, CompressionLevel);
      }

      static std::vector<uint8_t> BuildStringTableBlock(const std::vector<std::string>& Strings)
      {
        std::vector<uint8_t> Result;

        std::vector<uint32_t> Offsets;
        size_t StringDataSize = 0;
        for (const auto& Str : Strings)
        {
          Offsets.push_back(static_cast<uint32_t>(StringDataSize));
          StringDataSize += Str.size() + 1;
        }

        Pack::SnPakStrBlockHeaderV1 Header = {};
        std::memcpy(Header.Magic, Pack::kStringMagic, 4);
        Header.Version = 1;
        Header.StringCount = static_cast<uint32_t>(Strings.size());
        Header.BlockSize = sizeof(Header) + Offsets.size() * sizeof(uint32_t) + StringDataSize;

        std::vector<uint8_t> StringData;
        StringData.reserve(StringDataSize);
        for (const auto& Str : Strings)
        {
          StringData.insert(StringData.end(), Str.begin(), Str.end());
          StringData.push_back(0);
        }

        XXH128_hash_t Hash = XXH3_128bits(StringData.data(), StringData.size());
        Header.HashHi = Hash.high64;
        Header.HashLo = Hash.low64;

        Result.resize(sizeof(Header) + Offsets.size() * sizeof(uint32_t) + StringData.size());
        std::memcpy(Result.data(), &Header, sizeof(Header));
        std::memcpy(Result.data() + sizeof(Header), Offsets.data(), Offsets.size() * sizeof(uint32_t));
        std::memcpy(Result.data() + sizeof(Header) + Offsets.size() * sizeof(uint32_t), StringData.data(), StringData.size());

        return Result;
      }

      static std::vector<uint8_t> BuildIndexBlock(const std::vector<Pack::SnPakIndexEntryV1>& Entries,
                                                  const std::vector<Pack::SnPakBulkEntryV1>& BulkEntries, uint64_t PrevOffset = 0,
                                                  uint64_t PrevSize = 0)
      {
        Pack::SnPakIndexHeaderV1 Header = {};
        std::memcpy(Header.Magic, Pack::kIndexMagic, 4);
        Header.Version = 1;
        Header.EntryCount = static_cast<uint32_t>(Entries.size());
        Header.BulkEntryCount = static_cast<uint32_t>(BulkEntries.size());
        Header.PreviousIndexOffset = PrevOffset;
        Header.PreviousIndexSize = PrevSize;

        size_t EntriesSize = Entries.size() * sizeof(Pack::SnPakIndexEntryV1);
        size_t BulkEntriesSize = BulkEntries.size() * sizeof(Pack::SnPakBulkEntryV1);
        Header.BlockSize = sizeof(Header) + EntriesSize + BulkEntriesSize;

        std::vector<uint8_t> Result(Header.BlockSize);
        std::memcpy(Result.data(), &Header, sizeof(Header));
        if (!Entries.empty())
        {
          std::memcpy(Result.data() + sizeof(Header), Entries.data(), EntriesSize);
        }
        if (!BulkEntries.empty())
        {
          std::memcpy(Result.data() + sizeof(Header) + EntriesSize, BulkEntries.data(), BulkEntriesSize);
        }

        auto [low64, high64] = XXH3_128bits(Result.data() + sizeof(Header), EntriesSize + BulkEntriesSize);
        auto* HeaderPtr = reinterpret_cast<Pack::SnPakIndexHeaderV1*>(Result.data());
        HeaderPtr->EntriesHashHi = high64;
        HeaderPtr->EntriesHashLo = low64;

        return Result;
      }
  };

  AssetPackWriter::AssetPackWriter() : m_Impl(std::make_unique<Impl>()) {}

  AssetPackWriter::~AssetPackWriter() = default;

  AssetPackWriter::AssetPackWriter(AssetPackWriter&&) noexcept = default;
  AssetPackWriter& AssetPackWriter::operator=(AssetPackWriter&&) noexcept = default;

  void AssetPackWriter::SetCompression(const EPackCompression Mode) const
  {
    m_Impl->Compression = Impl::ToInternalCompression(Mode);
  }

  void AssetPackWriter::SetCompressionLevel(const EPackCompressionLevel Level) const
  {
    m_Impl->CompressionLevel = Impl::ToInternalCompressionLevel(Level);
  }

  void AssetPackWriter::SetMaxCompression(bool bEnable) const
  {
    m_Impl->CompressionLevel = bEnable ? Pack::ESnPakCompressionLevel::Max : Pack::ESnPakCompressionLevel::Default;
  }

  void AssetPackWriter::AddAsset(AssetPackEntry Entry) const
  {
    m_Impl->Assets.push_back(std::move(Entry));
  }

  void AssetPackWriter::AddAsset(AssetId Id, TypeId AssetKind, const std::string& Name, const std::string& VariantKey, TypedPayload Cooked,
                                 std::vector<BulkChunk> Bulk) const
  {
    AssetPackEntry Entry;
    Entry.Id = Id;
    Entry.AssetKind = AssetKind;
    Entry.Name = Name;
    Entry.VariantKey = VariantKey;
    Entry.Cooked = std::move(Cooked);
    Entry.Bulk = std::move(Bulk);
    m_Impl->Assets.push_back(std::move(Entry));
  }

  void AssetPackWriter::Clear() const
  {
    m_Impl->Assets.clear();
  }

  uint32_t AssetPackWriter::GetPendingAssetCount() const
  {
    return static_cast<uint32_t>(m_Impl->Assets.size());
  }

  std::expected<void, std::string> AssetPackWriter::Write(const std::string& OutputPath) const
  {
    std::string TempPath = OutputPath + ".tmp";

    std::ofstream File(TempPath, std::ios::binary | std::ios::trunc);
    if (!File.is_open())
    {
      return std::unexpected("Failed to open output file: " + TempPath);
    }

    // Build string table
    std::vector<std::string> StringTable;
    std::unordered_map<std::string, uint32_t> StringToId;
    bool bStringTableFrozen = false;

    auto AddString = [&](const std::string& Str) -> uint32_t {
      auto It = StringToId.find(Str);
      if (It != StringToId.end())
      {
        return It->second;
      }
      if (bStringTableFrozen)
      {
        throw std::runtime_error("StringTable frozen: attempted to add new string '" + Str + "'");
      }
      auto Id = static_cast<uint32_t>(StringTable.size());
      StringTable.push_back(Str);
      StringToId[Str] = Id;
      return Id;
    };

    // Lookup function for use after string table is frozen
    auto GetStringId = [&](const std::string& Str) -> uint32_t {
      auto It = StringToId.find(Str);
      if (It == StringToId.end())
      {
        throw std::runtime_error("String not found in frozen table: '" + Str + "'");
      }
      return It->second;
    };

    for (auto& Asset : m_Impl->Assets)
    {
      AddString(Asset.Name);
      if (!Asset.VariantKey.empty())
      {
        AddString(Asset.VariantKey);
      }
    }

    // Write header placeholder
    Pack::SnPakHeaderV1 Header = {};
    std::memcpy(Header.Magic, Pack::kSnPakMagic, 8);
    Header.Version = Pack::kSnPakVersion;
    Header.HeaderSize = sizeof(Pack::SnPakHeaderV1);
    Header.EndianMarker = Pack::kEndianMarker;

    File.write(reinterpret_cast<const char*>(&Header), sizeof(Header));
    uint64_t CurrentOffset = sizeof(Header);

    // Write string table
    uint64_t StringTableOffset = CurrentOffset;
    std::vector<uint8_t> StringTableData = Impl::BuildStringTableBlock(StringTable);
    File.write(reinterpret_cast<const char*>(StringTableData.data()), StringTableData.size());
    CurrentOffset += StringTableData.size();

    // Freeze string table - no new strings allowed after serialization
    bStringTableFrozen = true;

    // Write chunks and build index entries
    std::vector<Pack::SnPakIndexEntryV1> IndexEntries;
    std::vector<Pack::SnPakBulkEntryV1> BulkEntries;

    for (auto& Asset : m_Impl->Assets)
    {
      Pack::SnPakIndexEntryV1 Entry = {};

      Pack::CopyUuid(Entry.AssetId, Asset.Id.Bytes);
      Pack::CopyUuid(Entry.AssetKind, Asset.AssetKind.Bytes);
      Pack::CopyUuid(Entry.CookedPayloadType, Asset.Cooked.PayloadType.Bytes);
      Entry.CookedSchemaVersion = Asset.Cooked.SchemaVersion;

      Entry.NameStringId = GetStringId(Asset.Name);
      Entry.NameHash64 = XXH3_64bits(Asset.Name.data(), Asset.Name.size());

      if (!Asset.VariantKey.empty())
      {
        Entry.VariantStringId = GetStringId(Asset.VariantKey);
        Entry.VariantHash64 = XXH3_64bits(Asset.VariantKey.data(), Asset.VariantKey.size());
      }
      else
      {
        Entry.VariantStringId = 0xFFFFFFFF;
        Entry.VariantHash64 = 0;
      }

      // Write main payload chunk
      std::vector<uint8_t> CompressedPayload = m_Impl->CompressData(Asset.Cooked.Bytes.data(), Asset.Cooked.Bytes.size());

      Pack::SnPakChunkHeaderV1 ChunkHeader = {};
      std::memcpy(ChunkHeader.Magic, Pack::kChunkMagic, 4);
      ChunkHeader.Version = 1;
      Pack::CopyUuid(ChunkHeader.AssetId, Asset.Id.Bytes);
      Pack::CopyUuid(ChunkHeader.PayloadType, Asset.Cooked.PayloadType.Bytes);
      ChunkHeader.SchemaVersion = Asset.Cooked.SchemaVersion;
      ChunkHeader.Compression = static_cast<uint8_t>(m_Impl->Compression);
      ChunkHeader.ChunkKind = static_cast<uint8_t>(Pack::ESnPakChunkKind::MainPayload);
      ChunkHeader.Reserved0 = static_cast<uint16_t>(m_Impl->CompressionLevel);
      ChunkHeader.SizeCompressed = CompressedPayload.size();
      ChunkHeader.SizeUncompressed = Asset.Cooked.Bytes.size();

      XXH128_hash_t PayloadHash = XXH3_128bits(Asset.Cooked.Bytes.data(), Asset.Cooked.Bytes.size());
      ChunkHeader.HashHi = PayloadHash.high64;
      ChunkHeader.HashLo = PayloadHash.low64;

      Entry.PayloadChunkOffset = CurrentOffset;
      Entry.PayloadChunkSizeCompressed = sizeof(ChunkHeader) + CompressedPayload.size();
      Entry.PayloadChunkSizeUncompressed = Asset.Cooked.Bytes.size();
      Entry.Compression = static_cast<uint8_t>(m_Impl->Compression);
      Entry.Reserved0 = static_cast<uint16_t>(m_Impl->CompressionLevel);
      Entry.PayloadHashHi = PayloadHash.high64;
      Entry.PayloadHashLo = PayloadHash.low64;

      File.write(reinterpret_cast<const char*>(&ChunkHeader), sizeof(ChunkHeader));
      File.write(reinterpret_cast<const char*>(CompressedPayload.data()), CompressedPayload.size());
      CurrentOffset += sizeof(ChunkHeader) + CompressedPayload.size();

      // Write bulk chunks
      if (!Asset.Bulk.empty())
      {
        Entry.Flags |= Pack::IndexEntryFlag_HasBulk;
        Entry.BulkFirstIndex = static_cast<uint32_t>(BulkEntries.size());
        Entry.BulkCount = static_cast<uint32_t>(Asset.Bulk.size());

        for (uint32_t BulkIdx = 0; BulkIdx < Asset.Bulk.size(); ++BulkIdx)
        {
          auto& Bulk = Asset.Bulk[BulkIdx];
          Pack::ESnPakCompression BulkCompression = Bulk.bCompress ? m_Impl->Compression : Pack::ESnPakCompression::None;
          Pack::ESnPakCompressionLevel BulkLevel = Bulk.bCompress ? m_Impl->CompressionLevel : Pack::ESnPakCompressionLevel::Default;
          std::vector<uint8_t> CompressedBulk = Pack::Compress(Bulk.Bytes.data(), Bulk.Bytes.size(), BulkCompression, BulkLevel);

          Pack::SnPakChunkHeaderV1 BulkChunkHeader = {};
          std::memcpy(BulkChunkHeader.Magic, Pack::kChunkMagic, 4);
          BulkChunkHeader.Version = 1;
          Pack::CopyUuid(BulkChunkHeader.AssetId, Asset.Id.Bytes);
          Pack::CopyUuid(BulkChunkHeader.PayloadType, Asset.Cooked.PayloadType.Bytes);
          BulkChunkHeader.SchemaVersion = 0;
          BulkChunkHeader.Compression = static_cast<uint8_t>(BulkCompression);
          BulkChunkHeader.ChunkKind = static_cast<uint8_t>(Pack::ESnPakChunkKind::Bulk);
          BulkChunkHeader.Reserved0 = static_cast<uint16_t>(BulkLevel);
          BulkChunkHeader.SizeCompressed = CompressedBulk.size();
          BulkChunkHeader.SizeUncompressed = Bulk.Bytes.size();

          XXH128_hash_t BulkHash = XXH3_128bits(Bulk.Bytes.data(), Bulk.Bytes.size());
          BulkChunkHeader.HashHi = BulkHash.high64;
          BulkChunkHeader.HashLo = BulkHash.low64;

          Pack::SnPakBulkEntryV1 BulkEntry = {};
          auto SemanticVal = static_cast<uint32_t>(Bulk.Semantic);
          std::memcpy(BulkEntry.Semantic, &SemanticVal, 4);
          BulkEntry.SubIndex = BulkIdx;  // Enforce SubIndex == BulkIndex (array position)
          BulkEntry.ChunkOffset = CurrentOffset;
          BulkEntry.SizeCompressed = sizeof(BulkChunkHeader) + CompressedBulk.size();
          BulkEntry.SizeUncompressed = Bulk.Bytes.size();
          BulkEntry.Compression = static_cast<uint8_t>(BulkCompression);
          BulkEntry.Reserved0[0] = static_cast<uint8_t>(BulkLevel);
          BulkEntry.HashHi = BulkHash.high64;
          BulkEntry.HashLo = BulkHash.low64;

          BulkEntries.push_back(BulkEntry);

          File.write(reinterpret_cast<const char*>(&BulkChunkHeader), sizeof(BulkChunkHeader));
          File.write(reinterpret_cast<const char*>(CompressedBulk.data()), CompressedBulk.size());
          CurrentOffset += sizeof(BulkChunkHeader) + CompressedBulk.size();
        }
      }
      else
      {
        Entry.BulkFirstIndex = 0;
        Entry.BulkCount = 0;
      }

      IndexEntries.push_back(Entry);
    }

    // Write index block
    uint64_t IndexOffset = CurrentOffset;
    std::vector<uint8_t> IndexData = Impl::BuildIndexBlock(IndexEntries, BulkEntries);
    File.write(reinterpret_cast<const char*>(IndexData.data()), IndexData.size());
    CurrentOffset += IndexData.size();

    // Update header
    Header.FileSize = CurrentOffset;
    Header.IndexOffset = IndexOffset;
    Header.IndexSize = IndexData.size();
    Header.StringTableOffset = StringTableOffset;
    Header.StringTableSize = StringTableData.size();

    XXH128_hash_t IndexHash = XXH3_128bits(IndexData.data(), IndexData.size());
    Header.IndexHashHi = IndexHash.high64;
    Header.IndexHashLo = IndexHash.low64;

    // Write updated header
    File.seekp(0, std::ios::beg);
    File.write(reinterpret_cast<const char*>(&Header), sizeof(Header));
    File.close();

    // Atomic rename
    try
    {
      std::filesystem::rename(TempPath, OutputPath);
    }
    catch (const std::exception& E)
    {
      return std::unexpected(std::string("Failed to rename temp file: ") + E.what());
    }

    return {};
  }

  std::expected<void, std::string> AssetPackWriter::AppendUpdate(const std::string& PackPath) const
  {
    std::fstream File(PackPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!File.is_open())
    {
      // Pack doesn't exist, just write a new one
      return Write(PackPath);
    }

    // FIX #5: Comprehensive header validation for AppendUpdate
    // Verify the existing pack is valid and compatible before appending
    Pack::SnPakHeaderV1 OldHeader{};
    File.read(reinterpret_cast<char*>(&OldHeader), sizeof(OldHeader));

    if (!File.good())
    {
      return std::unexpected("Failed to read existing pack header");
    }

    if (std::memcmp(OldHeader.Magic, Pack::kSnPakMagic, 8) != 0)
    {
      return std::unexpected("Invalid pack file magic");
    }

    // Validate version - must be compatible
    if (OldHeader.Version != Pack::kSnPakVersion)
    {
      return std::unexpected("Pack version mismatch - expected " + std::to_string(Pack::kSnPakVersion) +
                             ", got " + std::to_string(OldHeader.Version));
    }

    // Validate header size - ensures struct layout compatibility
    if (OldHeader.HeaderSize != sizeof(Pack::SnPakHeaderV1))
    {
      return std::unexpected("Header size mismatch - pack may be from incompatible version");
    }

    // Validate endian marker - ensures byte order compatibility
    if (OldHeader.EndianMarker != Pack::kEndianMarker)
    {
      return std::unexpected("Endian mismatch - pack was created on different architecture");
    }

    // Validate file size against actual file size
    File.seekg(0, std::ios::end);
    if (auto ActualFileSize = static_cast<uint64_t>(File.tellg()); OldHeader.FileSize > ActualFileSize)
    {
      return std::unexpected("Header FileSize (" + std::to_string(OldHeader.FileSize) +
                             ") exceeds actual file size (" + std::to_string(ActualFileSize) +
                             ") - pack may be truncated");
    }

    File.seekp(0, std::ios::end);
    uint64_t CurrentOffset = File.tellp();

    // Build string table for new assets
    std::vector<std::string> StringTable;
    std::unordered_map<std::string, uint32_t> StringToId;
    bool bStringTableFrozen = false;

    auto AddString = [&](const std::string& Str) -> uint32_t {
      if (const auto It = StringToId.find(Str); It != StringToId.end())
      {
        return It->second;
      }
      if (bStringTableFrozen)
      {
        throw std::runtime_error("StringTable frozen: attempted to add new string '" + Str + "'");
      }
      auto Id = static_cast<uint32_t>(StringTable.size());
      StringTable.push_back(Str);
      StringToId[Str] = Id;
      return Id;
    };

    // Lookup function for use after string table is frozen
    auto GetStringId = [&](const std::string& Str) -> uint32_t {
      const auto It = StringToId.find(Str);
      if (It == StringToId.end())
      {
        throw std::runtime_error("String not found in frozen table: '" + Str + "'");
      }
      return It->second;
    };

    for (auto& Asset : m_Impl->Assets)
    {
      AddString(Asset.Name);
      if (!Asset.VariantKey.empty())
      {
        AddString(Asset.VariantKey);
      }
    }

    // Write new string table
    uint64_t NewStringTableOffset = CurrentOffset;
    std::vector<uint8_t> StringTableData = Impl::BuildStringTableBlock(StringTable);
    File.write(reinterpret_cast<const char*>(StringTableData.data()), StringTableData.size());
    CurrentOffset += StringTableData.size();

    // Freeze string table - no new strings allowed after serialization
    bStringTableFrozen = true;

    // Write new chunks and build index entries
    std::vector<Pack::SnPakIndexEntryV1> IndexEntries;
    std::vector<Pack::SnPakBulkEntryV1> BulkEntries;

    for (auto& Asset : m_Impl->Assets)
    {
      Pack::SnPakIndexEntryV1 Entry = {};

      Pack::CopyUuid(Entry.AssetId, Asset.Id.Bytes);
      Pack::CopyUuid(Entry.AssetKind, Asset.AssetKind.Bytes);
      Pack::CopyUuid(Entry.CookedPayloadType, Asset.Cooked.PayloadType.Bytes);
      Entry.CookedSchemaVersion = Asset.Cooked.SchemaVersion;

      Entry.NameStringId = GetStringId(Asset.Name);
      Entry.NameHash64 = XXH3_64bits(Asset.Name.data(), Asset.Name.size());

      if (!Asset.VariantKey.empty())
      {
        Entry.VariantStringId = GetStringId(Asset.VariantKey);
        Entry.VariantHash64 = XXH3_64bits(Asset.VariantKey.data(), Asset.VariantKey.size());
      }
      else
      {
        Entry.VariantStringId = 0xFFFFFFFF;
        Entry.VariantHash64 = 0;
      }

      // Write main payload
      std::vector<uint8_t> CompressedPayload = m_Impl->CompressData(Asset.Cooked.Bytes.data(), Asset.Cooked.Bytes.size());

      Pack::SnPakChunkHeaderV1 ChunkHeader = {};
      std::memcpy(ChunkHeader.Magic, Pack::kChunkMagic, 4);
      ChunkHeader.Version = 1;
      Pack::CopyUuid(ChunkHeader.AssetId, Asset.Id.Bytes);
      Pack::CopyUuid(ChunkHeader.PayloadType, Asset.Cooked.PayloadType.Bytes);
      ChunkHeader.SchemaVersion = Asset.Cooked.SchemaVersion;
      ChunkHeader.Compression = static_cast<uint8_t>(m_Impl->Compression);
      ChunkHeader.ChunkKind = static_cast<uint8_t>(Pack::ESnPakChunkKind::MainPayload);
      ChunkHeader.Reserved0 = static_cast<uint16_t>(m_Impl->CompressionLevel);
      ChunkHeader.SizeCompressed = CompressedPayload.size();
      ChunkHeader.SizeUncompressed = Asset.Cooked.Bytes.size();

      XXH128_hash_t PayloadHash = XXH3_128bits(Asset.Cooked.Bytes.data(), Asset.Cooked.Bytes.size());
      ChunkHeader.HashHi = PayloadHash.high64;
      ChunkHeader.HashLo = PayloadHash.low64;

      Entry.PayloadChunkOffset = CurrentOffset;
      Entry.PayloadChunkSizeCompressed = sizeof(ChunkHeader) + CompressedPayload.size();
      Entry.PayloadChunkSizeUncompressed = Asset.Cooked.Bytes.size();
      Entry.Compression = static_cast<uint8_t>(m_Impl->Compression);
      Entry.Reserved0 = static_cast<uint16_t>(m_Impl->CompressionLevel);
      Entry.PayloadHashHi = PayloadHash.high64;
      Entry.PayloadHashLo = PayloadHash.low64;

      File.write(reinterpret_cast<const char*>(&ChunkHeader), sizeof(ChunkHeader));
      File.write(reinterpret_cast<const char*>(CompressedPayload.data()), CompressedPayload.size());
      CurrentOffset += sizeof(ChunkHeader) + CompressedPayload.size();

      // Write bulk chunks
      if (!Asset.Bulk.empty())
      {
        Entry.Flags |= Pack::IndexEntryFlag_HasBulk;
        Entry.BulkFirstIndex = static_cast<uint32_t>(BulkEntries.size());
        Entry.BulkCount = static_cast<uint32_t>(Asset.Bulk.size());

        for (uint32_t BulkIdx = 0; BulkIdx < Asset.Bulk.size(); ++BulkIdx)
        {
          auto& Bulk = Asset.Bulk[BulkIdx];
          Pack::ESnPakCompression BulkCompression = Bulk.bCompress ? m_Impl->Compression : Pack::ESnPakCompression::None;
          Pack::ESnPakCompressionLevel BulkLevel = Bulk.bCompress ? m_Impl->CompressionLevel : Pack::ESnPakCompressionLevel::Default;
          std::vector<uint8_t> CompressedBulk = Pack::Compress(Bulk.Bytes.data(), Bulk.Bytes.size(), BulkCompression, BulkLevel);

          Pack::SnPakChunkHeaderV1 BulkChunkHeader = {};
          std::memcpy(BulkChunkHeader.Magic, Pack::kChunkMagic, 4);
          BulkChunkHeader.Version = 1;
          Pack::CopyUuid(BulkChunkHeader.AssetId, Asset.Id.Bytes);
          Pack::CopyUuid(BulkChunkHeader.PayloadType, Asset.Cooked.PayloadType.Bytes);
          BulkChunkHeader.SchemaVersion = 0;
          BulkChunkHeader.Compression = static_cast<uint8_t>(BulkCompression);
          BulkChunkHeader.ChunkKind = static_cast<uint8_t>(Pack::ESnPakChunkKind::Bulk);
          BulkChunkHeader.Reserved0 = static_cast<uint16_t>(BulkLevel);
          BulkChunkHeader.SizeCompressed = CompressedBulk.size();
          BulkChunkHeader.SizeUncompressed = Bulk.Bytes.size();

          XXH128_hash_t BulkHash = XXH3_128bits(Bulk.Bytes.data(), Bulk.Bytes.size());
          BulkChunkHeader.HashHi = BulkHash.high64;
          BulkChunkHeader.HashLo = BulkHash.low64;

          Pack::SnPakBulkEntryV1 BulkEntry = {};
          auto SemanticVal = static_cast<uint32_t>(Bulk.Semantic);
          std::memcpy(BulkEntry.Semantic, &SemanticVal, 4);
          BulkEntry.SubIndex = BulkIdx;  // Enforce SubIndex == BulkIndex (array position)
          BulkEntry.ChunkOffset = CurrentOffset;
          BulkEntry.SizeCompressed = sizeof(BulkChunkHeader) + CompressedBulk.size();
          BulkEntry.SizeUncompressed = Bulk.Bytes.size();
          BulkEntry.Compression = static_cast<uint8_t>(BulkCompression);
          BulkEntry.Reserved0[0] = static_cast<uint8_t>(BulkLevel);
          BulkEntry.HashHi = BulkHash.high64;
          BulkEntry.HashLo = BulkHash.low64;

          BulkEntries.push_back(BulkEntry);

          File.write(reinterpret_cast<const char*>(&BulkChunkHeader), sizeof(BulkChunkHeader));
          File.write(reinterpret_cast<const char*>(CompressedBulk.data()), CompressedBulk.size());
          CurrentOffset += sizeof(BulkChunkHeader) + CompressedBulk.size();
        }
      }
      else
      {
        Entry.BulkFirstIndex = 0;
        Entry.BulkCount = 0;
      }

      IndexEntries.push_back(Entry);
    }

    // Write new index block (with reference to previous)
    uint64_t NewIndexOffset = CurrentOffset;
    std::vector<uint8_t> IndexData = Impl::BuildIndexBlock(IndexEntries, BulkEntries, OldHeader.IndexOffset, OldHeader.IndexSize);
    File.write(reinterpret_cast<const char*>(IndexData.data()), IndexData.size());
    CurrentOffset += IndexData.size();

    // Update header
    Pack::SnPakHeaderV1 NewHeader = OldHeader;
    NewHeader.FileSize = CurrentOffset;
    NewHeader.IndexOffset = NewIndexOffset;
    NewHeader.IndexSize = IndexData.size();
    NewHeader.StringTableOffset = NewStringTableOffset;
    NewHeader.StringTableSize = StringTableData.size();
    NewHeader.PreviousIndexOffset = OldHeader.IndexOffset;
    NewHeader.PreviousIndexSize = OldHeader.IndexSize;
    NewHeader.Flags |= Pack::SnPakFlag_HasTrailingIndex;

    XXH128_hash_t IndexHash = XXH3_128bits(IndexData.data(), IndexData.size());
    NewHeader.IndexHashHi = IndexHash.high64;
    NewHeader.IndexHashLo = IndexHash.low64;

    File.seekp(0, std::ios::beg);
    File.write(reinterpret_cast<const char*>(&NewHeader), sizeof(NewHeader));
    File.close();

    return {};
  }

} // namespace SnAPI::AssetPipeline
