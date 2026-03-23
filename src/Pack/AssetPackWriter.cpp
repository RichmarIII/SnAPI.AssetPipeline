#include "AssetPackWriter.h"
#include "SnPakFormat.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fstream>
#include <filesystem>
#include <limits>
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
      Pack::ESnPakCompression Compression = Pack::ESnPakCompression::ZstdFast;
      Pack::ESnPakCompressionLevel CompressionLevel = Pack::ESnPakCompressionLevel::Fast;

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

      static Pack::ESnPakCompression ResolveCompression(const std::optional<EPackCompression>& Override,
                                                        const Pack::ESnPakCompression Fallback)
      {
        return Override ? ToInternalCompression(*Override) : Fallback;
      }

      static Pack::ESnPakCompressionLevel ResolveCompressionLevel(const std::optional<EPackCompressionLevel>& Override,
                                                                  const Pack::ESnPakCompressionLevel Fallback,
                                                                  const Pack::ESnPakCompression Compression)
      {
        if (Override)
        {
          return ToInternalCompressionLevel(*Override);
        }
        if (Compression == Pack::ESnPakCompression::None)
        {
          return Pack::ESnPakCompressionLevel::Default;
        }
        return Fallback;
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
                                                  const std::vector<Pack::SnPakBulkEntryV1>& BulkEntries,
                                                  const std::vector<Pack::SnPakDependencyOwnerV1>& DependencyOwners,
                                                  const std::vector<Pack::SnPakDependencyEntryV1>& DependencyEntries,
                                                  uint64_t PrevOffset = 0,
                                                  uint64_t PrevSize = 0)
      {
        Pack::SnPakIndexHeaderV1 Header = {};
        std::memcpy(Header.Magic, Pack::kIndexMagic, 4);
        Header.Version = 1;
        Header.EntryCount = static_cast<uint32_t>(Entries.size());
        Header.BulkEntryCount = static_cast<uint32_t>(BulkEntries.size());
        Header.PreviousIndexOffset = PrevOffset;
        Header.PreviousIndexSize = PrevSize;
        Pack::SetDependencyOwnerCount(Header, static_cast<uint32_t>(DependencyOwners.size()));
        Pack::SetDependencyEntryCount(Header, static_cast<uint32_t>(DependencyEntries.size()));

        size_t EntriesSize = Entries.size() * sizeof(Pack::SnPakIndexEntryV1);
        size_t BulkEntriesSize = BulkEntries.size() * sizeof(Pack::SnPakBulkEntryV1);
        size_t DependencyOwnersSize = DependencyOwners.size() * sizeof(Pack::SnPakDependencyOwnerV1);
        size_t DependencyEntriesSize = DependencyEntries.size() * sizeof(Pack::SnPakDependencyEntryV1);
        Header.BlockSize = sizeof(Header) + EntriesSize + BulkEntriesSize + DependencyOwnersSize + DependencyEntriesSize;

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
        if (!DependencyOwners.empty())
        {
          std::memcpy(Result.data() + sizeof(Header) + EntriesSize + BulkEntriesSize, DependencyOwners.data(), DependencyOwnersSize);
        }
        if (!DependencyEntries.empty())
        {
          std::memcpy(Result.data() + sizeof(Header) + EntriesSize + BulkEntriesSize + DependencyOwnersSize,
                      DependencyEntries.data(),
                      DependencyEntriesSize);
        }

        auto [low64, high64] =
            XXH3_128bits(Result.data() + sizeof(Header), EntriesSize + BulkEntriesSize + DependencyOwnersSize + DependencyEntriesSize);
        auto* HeaderPtr = reinterpret_cast<Pack::SnPakIndexHeaderV1*>(Result.data());
        HeaderPtr->EntriesHashHi = high64;
        HeaderPtr->EntriesHashLo = low64;

        return Result;
      }

      template <typename AddStringFn>
      static std::expected<void, std::string> AppendDependencyMetadata(const uint32_t AssetIndex,
                                                                       const std::vector<AssetDependencyRef>& Dependencies,
                                                                       std::vector<Pack::SnPakDependencyOwnerV1>& DependencyOwners,
                                                                       std::vector<Pack::SnPakDependencyEntryV1>& DependencyEntries,
                                                                       AddStringFn&& AddString)
      {
        if (Dependencies.empty())
        {
          return {};
        }

        if (DependencyOwners.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
        {
          return std::unexpected("Dependency owner table exceeds 32-bit range");
        }
        if (DependencyEntries.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - Dependencies.size())
        {
          return std::unexpected("Dependency entry table exceeds 32-bit range");
        }

        Pack::SnPakDependencyOwnerV1 Owner{};
        Owner.AssetIndex = AssetIndex;
        Owner.FirstDependencyIndex = static_cast<uint32_t>(DependencyEntries.size());
        Owner.DependencyCount = static_cast<uint32_t>(Dependencies.size());
        DependencyOwners.push_back(Owner);

        for (const AssetDependencyRef& Dependency : Dependencies)
        {
          Pack::SnPakDependencyEntryV1 Entry{};
          Pack::CopyUuid(Entry.AssetId, Dependency.Id.Bytes);
          if (!Dependency.LogicalName.empty())
          {
            auto StringIdResult = AddString(Dependency.LogicalName);
            if (!StringIdResult)
            {
              return std::unexpected(StringIdResult.error());
            }
            Entry.LogicalNameStringId = *StringIdResult;
          }
          else
          {
            Entry.LogicalNameStringId = Pack::kInvalidStringId;
          }
          Entry.Kind = static_cast<uint8_t>(Dependency.Kind);
          DependencyEntries.push_back(Entry);
        }

        return {};
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
      for (const AssetDependencyRef& Dependency : Asset.AssetDependencies)
      {
        if (!Dependency.LogicalName.empty())
        {
          AddString(Dependency.LogicalName);
        }
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
    std::vector<Pack::SnPakDependencyOwnerV1> DependencyOwners;
    std::vector<Pack::SnPakDependencyEntryV1> DependencyEntries;

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
      const auto AssetCompression = Impl::ResolveCompression(Asset.CompressionOverride, m_Impl->Compression);
      const auto AssetLevel = Impl::ResolveCompressionLevel(Asset.CompressionLevelOverride, m_Impl->CompressionLevel, AssetCompression);
      std::vector<uint8_t> CompressedPayload = Pack::Compress(Asset.Cooked.Bytes.data(), Asset.Cooked.Bytes.size(), AssetCompression, AssetLevel);

      Pack::SnPakChunkHeaderV1 ChunkHeader = {};
      std::memcpy(ChunkHeader.Magic, Pack::kChunkMagic, 4);
      ChunkHeader.Version = 1;
      Pack::CopyUuid(ChunkHeader.AssetId, Asset.Id.Bytes);
      Pack::CopyUuid(ChunkHeader.PayloadType, Asset.Cooked.PayloadType.Bytes);
      ChunkHeader.SchemaVersion = Asset.Cooked.SchemaVersion;
      ChunkHeader.Compression = static_cast<uint8_t>(AssetCompression);
      ChunkHeader.ChunkKind = static_cast<uint8_t>(Pack::ESnPakChunkKind::MainPayload);
      ChunkHeader.Reserved0 = static_cast<uint16_t>(AssetLevel);
      ChunkHeader.SizeCompressed = CompressedPayload.size();
      ChunkHeader.SizeUncompressed = Asset.Cooked.Bytes.size();

      XXH128_hash_t PayloadHash = XXH3_128bits(Asset.Cooked.Bytes.data(), Asset.Cooked.Bytes.size());
      ChunkHeader.HashHi = PayloadHash.high64;
      ChunkHeader.HashLo = PayloadHash.low64;

      Entry.PayloadChunkOffset = CurrentOffset;
      Entry.PayloadChunkSizeCompressed = sizeof(ChunkHeader) + CompressedPayload.size();
      Entry.PayloadChunkSizeUncompressed = Asset.Cooked.Bytes.size();
      Entry.Compression = static_cast<uint8_t>(AssetCompression);
      Entry.Reserved0 = static_cast<uint16_t>(AssetLevel);
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
          Pack::ESnPakCompression BulkCompression = Bulk.CompressionOverride
                                                      ? Impl::ToInternalCompression(*Bulk.CompressionOverride)
                                                      : (Bulk.bCompress ? m_Impl->Compression : Pack::ESnPakCompression::None);
          Pack::ESnPakCompressionLevel BulkLevel = Impl::ResolveCompressionLevel(Bulk.CompressionLevelOverride, m_Impl->CompressionLevel, BulkCompression);
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
          BulkEntry.SubIndex = Bulk.SubIndex;
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

      auto DependencyResult =
          Impl::AppendDependencyMetadata(static_cast<uint32_t>(IndexEntries.size() - 1),
                                         Asset.AssetDependencies,
                                         DependencyOwners,
                                         DependencyEntries,
                                         [&GetStringId](const std::string& Value) -> std::expected<uint32_t, std::string> {
                                           try
                                           {
                                             return GetStringId(Value);
                                           }
                                           catch (const std::exception& E)
                                           {
                                             return std::unexpected(E.what());
                                           }
                                         });
      if (!DependencyResult)
      {
        return std::unexpected(DependencyResult.error());
      }
    }

    // Write index block
    uint64_t IndexOffset = CurrentOffset;
    std::vector<uint8_t> IndexData = Impl::BuildIndexBlock(IndexEntries, BulkEntries, DependencyOwners, DependencyEntries);
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
    if (m_Impl->Assets.empty())
    {
      return {};
    }

    std::fstream File(PackPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!File.is_open())
    {
      return Write(PackPath);
    }

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
    if (OldHeader.Version != Pack::kSnPakVersion)
    {
      return std::unexpected("Pack version mismatch - expected " + std::to_string(Pack::kSnPakVersion) +
                             ", got " + std::to_string(OldHeader.Version));
    }
    if (OldHeader.HeaderSize != sizeof(Pack::SnPakHeaderV1))
    {
      return std::unexpected("Header size mismatch - pack may be from incompatible version");
    }
    if (OldHeader.EndianMarker != Pack::kEndianMarker)
    {
      return std::unexpected("Endian mismatch - pack was created on different architecture");
    }

    File.seekg(0, std::ios::end);
    const uint64_t ActualFileSize = static_cast<uint64_t>(File.tellg());
    if (OldHeader.FileSize > ActualFileSize)
    {
      return std::unexpected("Header FileSize (" + std::to_string(OldHeader.FileSize) +
                             ") exceeds actual file size (" + std::to_string(ActualFileSize) +
                             ") - pack may be truncated");
    }

    const auto CheckRange = [ActualFileSize](const uint64_t Offset, const uint64_t Size) {
      if (Size > ActualFileSize)
      {
        return false;
      }
      if (Offset > ActualFileSize - Size)
      {
        return false;
      }
      return true;
    };

    if (!CheckRange(OldHeader.StringTableOffset, OldHeader.StringTableSize))
    {
      return std::unexpected("String table offset/size exceeds file bounds");
    }

    File.seekg(static_cast<std::streamoff>(OldHeader.StringTableOffset), std::ios::beg);
    if (!File.good())
    {
      return std::unexpected("Failed to seek to string table");
    }

    Pack::SnPakStrBlockHeaderV1 StringHeader{};
    File.read(reinterpret_cast<char*>(&StringHeader), sizeof(StringHeader));
    if (!File.good())
    {
      return std::unexpected("Failed to read string table header");
    }
    if (std::memcmp(StringHeader.Magic, Pack::kStringMagic, 4) != 0)
    {
      return std::unexpected("Invalid string table magic");
    }
    if (StringHeader.Version != 1)
    {
      return std::unexpected("Unsupported string table version: " + std::to_string(StringHeader.Version));
    }
    if (StringHeader.BlockSize != OldHeader.StringTableSize)
    {
      return std::unexpected("String table BlockSize mismatch with header");
    }
    if (StringHeader.StringCount > (std::numeric_limits<size_t>::max() / sizeof(uint32_t)))
    {
      return std::unexpected("String table count exceeds host addressable range");
    }

    const size_t ExistingOffsetsSize = static_cast<size_t>(StringHeader.StringCount) * sizeof(uint32_t);
    if (StringHeader.BlockSize < sizeof(Pack::SnPakStrBlockHeaderV1) + ExistingOffsetsSize)
    {
      return std::unexpected("String table block size too small for offsets");
    }

    std::vector<uint32_t> ExistingStringOffsets(StringHeader.StringCount);
    if (!ExistingStringOffsets.empty())
    {
      File.read(reinterpret_cast<char*>(ExistingStringOffsets.data()), static_cast<std::streamsize>(ExistingOffsetsSize));
      if (!File.good())
      {
        return std::unexpected("Failed to read string table offsets");
      }
    }

    const size_t ExistingStringDataSize =
        static_cast<size_t>(StringHeader.BlockSize - sizeof(Pack::SnPakStrBlockHeaderV1) - ExistingOffsetsSize);
    std::vector<uint8_t> ExistingStringData(ExistingStringDataSize);
    if (!ExistingStringData.empty())
    {
      File.read(reinterpret_cast<char*>(ExistingStringData.data()), static_cast<std::streamsize>(ExistingStringDataSize));
      if (!File.good())
      {
        return std::unexpected("Failed to read string table data");
      }
    }

    std::vector<std::string> StringTable{};
    StringTable.reserve(static_cast<size_t>(StringHeader.StringCount) + m_Impl->Assets.size() * 2u);
    std::unordered_map<std::string, uint32_t> StringToId{};
    StringToId.reserve(static_cast<size_t>(StringHeader.StringCount) + m_Impl->Assets.size() * 2u);

    for (uint32_t StringIndex = 0; StringIndex < StringHeader.StringCount; ++StringIndex)
    {
      const uint32_t Offset = ExistingStringOffsets[StringIndex];
      if (Offset >= ExistingStringDataSize)
      {
        return std::unexpected("String table offset " + std::to_string(StringIndex) + " out of range");
      }

      const uint8_t* const Start = ExistingStringData.data() + Offset;
      const size_t MaxLen = ExistingStringDataSize - Offset;
      const void* const NullPos = std::memchr(Start, 0, MaxLen);
      if (NullPos == nullptr)
      {
        return std::unexpected("String table entry " + std::to_string(StringIndex) + " missing null terminator");
      }

      const size_t Length = static_cast<const uint8_t*>(NullPos) - Start;
      std::string StringValue(reinterpret_cast<const char*>(Start), Length);
      StringTable.push_back(StringValue);
      StringToId.try_emplace(StringValue, StringIndex);
    }

    if (!CheckRange(OldHeader.IndexOffset, OldHeader.IndexSize))
    {
      return std::unexpected("Index offset/size exceeds file bounds");
    }

    File.seekg(static_cast<std::streamoff>(OldHeader.IndexOffset), std::ios::beg);
    if (!File.good())
    {
      return std::unexpected("Failed to seek to index block");
    }

    Pack::SnPakIndexHeaderV1 OldIndexHeader{};
    File.read(reinterpret_cast<char*>(&OldIndexHeader), sizeof(OldIndexHeader));
    if (!File.good())
    {
      return std::unexpected("Failed to read index header");
    }
    if (std::memcmp(OldIndexHeader.Magic, Pack::kIndexMagic, 4) != 0)
    {
      return std::unexpected("Invalid index magic");
    }
    if (OldIndexHeader.Version != 1)
    {
      return std::unexpected("Unsupported index version: " + std::to_string(OldIndexHeader.Version));
    }
    if (OldIndexHeader.BlockSize != OldHeader.IndexSize)
    {
      return std::unexpected("Index BlockSize mismatch with header");
    }

    const uint64_t ExistingIndexEntriesSize = static_cast<uint64_t>(OldIndexHeader.EntryCount) * sizeof(Pack::SnPakIndexEntryV1);
    const uint64_t ExistingBulkEntriesSize = static_cast<uint64_t>(OldIndexHeader.BulkEntryCount) * sizeof(Pack::SnPakBulkEntryV1);
    const uint64_t ExistingDependencyOwnerCount = Pack::GetDependencyOwnerCount(OldIndexHeader);
    const uint64_t ExistingDependencyEntryCount = Pack::GetDependencyEntryCount(OldIndexHeader);
    const uint64_t ExistingDependencyOwnersSize =
        ExistingDependencyOwnerCount * sizeof(Pack::SnPakDependencyOwnerV1);
    const uint64_t ExistingDependencyEntriesSize =
        ExistingDependencyEntryCount * sizeof(Pack::SnPakDependencyEntryV1);
    const uint64_t ExpectedIndexBlockSize =
        static_cast<uint64_t>(sizeof(Pack::SnPakIndexHeaderV1)) + ExistingIndexEntriesSize + ExistingBulkEntriesSize +
        ExistingDependencyOwnersSize + ExistingDependencyEntriesSize;
    if (ExpectedIndexBlockSize != OldIndexHeader.BlockSize)
    {
      return std::unexpected("Index block size does not match entry counts");
    }

    std::vector<Pack::SnPakIndexEntryV1> ExistingIndexEntries(OldIndexHeader.EntryCount);
    if (!ExistingIndexEntries.empty())
    {
      File.read(reinterpret_cast<char*>(ExistingIndexEntries.data()), static_cast<std::streamsize>(ExistingIndexEntriesSize));
      if (!File.good())
      {
        return std::unexpected("Failed to read existing index entries");
      }
    }

    std::vector<Pack::SnPakBulkEntryV1> ExistingBulkEntries(OldIndexHeader.BulkEntryCount);
    if (!ExistingBulkEntries.empty())
    {
      File.read(reinterpret_cast<char*>(ExistingBulkEntries.data()), static_cast<std::streamsize>(ExistingBulkEntriesSize));
      if (!File.good())
      {
        return std::unexpected("Failed to read existing bulk entries");
      }
    }

    std::vector<Pack::SnPakDependencyOwnerV1> ExistingDependencyOwners(static_cast<size_t>(ExistingDependencyOwnerCount));
    if (!ExistingDependencyOwners.empty())
    {
      File.read(reinterpret_cast<char*>(ExistingDependencyOwners.data()), static_cast<std::streamsize>(ExistingDependencyOwnersSize));
      if (!File.good())
      {
        return std::unexpected("Failed to read existing dependency owner entries");
      }
    }

    std::vector<Pack::SnPakDependencyEntryV1> ExistingDependencyEntries(static_cast<size_t>(ExistingDependencyEntryCount));
    if (!ExistingDependencyEntries.empty())
    {
      File.read(reinterpret_cast<char*>(ExistingDependencyEntries.data()), static_cast<std::streamsize>(ExistingDependencyEntriesSize));
      if (!File.good())
      {
        return std::unexpected("Failed to read existing dependency entries");
      }
    }

    std::unordered_map<AssetId, size_t, UuidHash> ExistingAssetToIndex{};
    ExistingAssetToIndex.reserve(ExistingIndexEntries.size());
    std::vector<AssetId> ExistingAssetOrder{};
    ExistingAssetOrder.reserve(ExistingIndexEntries.size());
    for (size_t ExistingIndex = 0; ExistingIndex < ExistingIndexEntries.size(); ++ExistingIndex)
    {
      AssetId ExistingId{};
      std::memcpy(ExistingId.Bytes, ExistingIndexEntries[ExistingIndex].AssetId, sizeof(ExistingId.Bytes));

      if (auto It = ExistingAssetToIndex.find(ExistingId); It == ExistingAssetToIndex.end())
      {
        ExistingAssetOrder.push_back(ExistingId);
        ExistingAssetToIndex.emplace(ExistingId, ExistingIndex);
      }
      else
      {
        It->second = ExistingIndex;
      }
    }

    std::unordered_map<uint32_t, size_t> ExistingAssetIndexToDependencyOwner{};
    ExistingAssetIndexToDependencyOwner.reserve(ExistingDependencyOwners.size());
    for (size_t OwnerIndex = 0; OwnerIndex < ExistingDependencyOwners.size(); ++OwnerIndex)
    {
      const auto& Owner = ExistingDependencyOwners[OwnerIndex];
      if (Owner.AssetIndex >= ExistingIndexEntries.size())
      {
        return std::unexpected("Existing dependency owner references invalid asset index");
      }
      if (Owner.FirstDependencyIndex > ExistingDependencyEntries.size() ||
          Owner.DependencyCount > ExistingDependencyEntries.size() - Owner.FirstDependencyIndex)
      {
        return std::unexpected("Existing dependency owner references invalid dependency range");
      }
      if (!ExistingAssetIndexToDependencyOwner.emplace(Owner.AssetIndex, OwnerIndex).second)
      {
        return std::unexpected("Existing pack contains duplicate dependency owner for the same asset");
      }
    }

    std::unordered_map<AssetId, size_t, UuidHash> PendingById{};
    PendingById.reserve(m_Impl->Assets.size());
    for (size_t PendingIndex = 0; PendingIndex < m_Impl->Assets.size(); ++PendingIndex)
    {
      PendingById[m_Impl->Assets[PendingIndex].Id] = PendingIndex;
    }

    const auto AddString = [&StringTable, &StringToId](const std::string& Value) -> std::expected<uint32_t, std::string> {
      if (const auto It = StringToId.find(Value); It != StringToId.end())
      {
        return It->second;
      }
      if (StringTable.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
      {
        return std::unexpected("String table exceeds 32-bit string id range");
      }
      const uint32_t NewId = static_cast<uint32_t>(StringTable.size());
      StringTable.push_back(Value);
      StringToId.emplace(Value, NewId);
      return NewId;
    };

    auto CopyExistingBulkEntries =
        [&ExistingBulkEntries](const Pack::SnPakIndexEntryV1& SourceEntry, Pack::SnPakIndexEntryV1& DestEntry,
                               std::vector<Pack::SnPakBulkEntryV1>& OutBulkEntries) -> std::expected<void, std::string> {
      DestEntry.Flags = static_cast<uint8_t>(DestEntry.Flags & ~Pack::IndexEntryFlag_HasBulk);
      DestEntry.BulkFirstIndex = 0;
      DestEntry.BulkCount = 0;

      if (!(SourceEntry.Flags & Pack::IndexEntryFlag_HasBulk) || SourceEntry.BulkCount == 0)
      {
        return {};
      }
      if (SourceEntry.BulkFirstIndex > ExistingBulkEntries.size() ||
          SourceEntry.BulkCount > ExistingBulkEntries.size() - SourceEntry.BulkFirstIndex)
      {
        return std::unexpected("Existing asset has invalid bulk entry range");
      }
      if (OutBulkEntries.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - SourceEntry.BulkCount)
      {
        return std::unexpected("Bulk table exceeds 32-bit range");
      }

      DestEntry.Flags = static_cast<uint8_t>(DestEntry.Flags | Pack::IndexEntryFlag_HasBulk);
      DestEntry.BulkFirstIndex = static_cast<uint32_t>(OutBulkEntries.size());
      DestEntry.BulkCount = SourceEntry.BulkCount;
      for (uint32_t BulkIndex = 0; BulkIndex < SourceEntry.BulkCount; ++BulkIndex)
      {
        OutBulkEntries.push_back(ExistingBulkEntries[SourceEntry.BulkFirstIndex + BulkIndex]);
      }
      return {};
    };

    auto CopyExistingDependencyEntries =
        [&ExistingDependencyOwners, &ExistingDependencyEntries, &ExistingAssetIndexToDependencyOwner](
            const uint32_t SourceAssetIndex,
            const uint32_t DestAssetIndex,
            std::vector<Pack::SnPakDependencyOwnerV1>& OutDependencyOwners,
            std::vector<Pack::SnPakDependencyEntryV1>& OutDependencyEntries) -> std::expected<void, std::string> {
      const auto OwnerIt = ExistingAssetIndexToDependencyOwner.find(SourceAssetIndex);
      if (OwnerIt == ExistingAssetIndexToDependencyOwner.end())
      {
        return {};
      }

      const auto& SourceOwner = ExistingDependencyOwners[OwnerIt->second];
      if (OutDependencyOwners.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
      {
        return std::unexpected("Dependency owner table exceeds 32-bit range");
      }
      if (OutDependencyEntries.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - SourceOwner.DependencyCount)
      {
        return std::unexpected("Dependency entry table exceeds 32-bit range");
      }

      Pack::SnPakDependencyOwnerV1 DestOwner{};
      DestOwner.AssetIndex = DestAssetIndex;
      DestOwner.FirstDependencyIndex = static_cast<uint32_t>(OutDependencyEntries.size());
      DestOwner.DependencyCount = SourceOwner.DependencyCount;
      OutDependencyOwners.push_back(DestOwner);

      for (uint32_t DependencyIndex = 0; DependencyIndex < SourceOwner.DependencyCount; ++DependencyIndex)
      {
        OutDependencyEntries.push_back(ExistingDependencyEntries[SourceOwner.FirstDependencyIndex + DependencyIndex]);
      }
      return {};
    };

    File.clear();
    File.seekp(0, std::ios::end);
    uint64_t CurrentOffset = static_cast<uint64_t>(File.tellp());

    std::vector<Pack::SnPakIndexEntryV1> NewIndexEntries{};
    std::vector<Pack::SnPakBulkEntryV1> NewBulkEntries{};
    std::vector<Pack::SnPakDependencyOwnerV1> NewDependencyOwners{};
    std::vector<Pack::SnPakDependencyEntryV1> NewDependencyEntries{};
    NewIndexEntries.reserve(ExistingAssetOrder.size() + PendingById.size());

    auto BuildUpdatedEntry =
        [&](const AssetPackEntry& Asset,
            const uint32_t AssetIndex,
            const Pack::SnPakIndexEntryV1* ExistingEntryForBulk) -> std::expected<Pack::SnPakIndexEntryV1, std::string> {
      Pack::SnPakIndexEntryV1 Entry = {};

      Pack::CopyUuid(Entry.AssetId, Asset.Id.Bytes);
      Pack::CopyUuid(Entry.AssetKind, Asset.AssetKind.Bytes);
      Pack::CopyUuid(Entry.CookedPayloadType, Asset.Cooked.PayloadType.Bytes);
      Entry.CookedSchemaVersion = Asset.Cooked.SchemaVersion;

      auto NameIdResult = AddString(Asset.Name);
      if (!NameIdResult)
      {
        return std::unexpected(NameIdResult.error());
      }
      Entry.NameStringId = *NameIdResult;
      Entry.NameHash64 = XXH3_64bits(Asset.Name.data(), Asset.Name.size());

      if (!Asset.VariantKey.empty())
      {
        auto VariantIdResult = AddString(Asset.VariantKey);
        if (!VariantIdResult)
        {
          return std::unexpected(VariantIdResult.error());
        }
        Entry.VariantStringId = *VariantIdResult;
        Entry.VariantHash64 = XXH3_64bits(Asset.VariantKey.data(), Asset.VariantKey.size());
      }
      else
      {
        Entry.VariantStringId = 0xFFFFFFFF;
        Entry.VariantHash64 = 0;
      }

      const auto AssetCompression = Impl::ResolveCompression(Asset.CompressionOverride, m_Impl->Compression);
      const auto AssetLevel = Impl::ResolveCompressionLevel(Asset.CompressionLevelOverride, m_Impl->CompressionLevel, AssetCompression);
      std::vector<uint8_t> CompressedPayload = Pack::Compress(Asset.Cooked.Bytes.data(), Asset.Cooked.Bytes.size(), AssetCompression, AssetLevel);

      Pack::SnPakChunkHeaderV1 ChunkHeader = {};
      std::memcpy(ChunkHeader.Magic, Pack::kChunkMagic, 4);
      ChunkHeader.Version = 1;
      Pack::CopyUuid(ChunkHeader.AssetId, Asset.Id.Bytes);
      Pack::CopyUuid(ChunkHeader.PayloadType, Asset.Cooked.PayloadType.Bytes);
      ChunkHeader.SchemaVersion = Asset.Cooked.SchemaVersion;
      ChunkHeader.Compression = static_cast<uint8_t>(AssetCompression);
      ChunkHeader.ChunkKind = static_cast<uint8_t>(Pack::ESnPakChunkKind::MainPayload);
      ChunkHeader.Reserved0 = static_cast<uint16_t>(AssetLevel);
      ChunkHeader.SizeCompressed = CompressedPayload.size();
      ChunkHeader.SizeUncompressed = Asset.Cooked.Bytes.size();

      XXH128_hash_t PayloadHash = XXH3_128bits(Asset.Cooked.Bytes.data(), Asset.Cooked.Bytes.size());
      ChunkHeader.HashHi = PayloadHash.high64;
      ChunkHeader.HashLo = PayloadHash.low64;

      Entry.PayloadChunkOffset = CurrentOffset;
      Entry.PayloadChunkSizeCompressed = sizeof(ChunkHeader) + CompressedPayload.size();
      Entry.PayloadChunkSizeUncompressed = Asset.Cooked.Bytes.size();
      Entry.Compression = static_cast<uint8_t>(AssetCompression);
      Entry.Reserved0 = static_cast<uint16_t>(AssetLevel);
      Entry.PayloadHashHi = PayloadHash.high64;
      Entry.PayloadHashLo = PayloadHash.low64;

      File.write(reinterpret_cast<const char*>(&ChunkHeader), sizeof(ChunkHeader));
      File.write(reinterpret_cast<const char*>(CompressedPayload.data()), static_cast<std::streamsize>(CompressedPayload.size()));
      if (!File.good())
      {
        return std::unexpected("Failed to write payload chunk");
      }
      CurrentOffset += sizeof(ChunkHeader) + CompressedPayload.size();

      Entry.Flags = static_cast<uint8_t>(Entry.Flags & ~Pack::IndexEntryFlag_HasBulk);
      Entry.BulkFirstIndex = 0;
      Entry.BulkCount = 0;

      if (!Asset.Bulk.empty())
      {
        if (Asset.Bulk.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
        {
          return std::unexpected("Asset bulk chunk count exceeds 32-bit range");
        }
        if (NewBulkEntries.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - Asset.Bulk.size())
        {
          return std::unexpected("Bulk table exceeds 32-bit range");
        }

        Entry.Flags = static_cast<uint8_t>(Entry.Flags | Pack::IndexEntryFlag_HasBulk);
        Entry.BulkFirstIndex = static_cast<uint32_t>(NewBulkEntries.size());
        Entry.BulkCount = static_cast<uint32_t>(Asset.Bulk.size());

        for (const auto& Bulk : Asset.Bulk)
        {
          const Pack::ESnPakCompression BulkCompression = Bulk.CompressionOverride
                                                            ? Impl::ToInternalCompression(*Bulk.CompressionOverride)
                                                            : (Bulk.bCompress ? m_Impl->Compression : Pack::ESnPakCompression::None);
          const Pack::ESnPakCompressionLevel BulkLevel =
              Impl::ResolveCompressionLevel(Bulk.CompressionLevelOverride, m_Impl->CompressionLevel, BulkCompression);
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

          const XXH128_hash_t BulkHash = XXH3_128bits(Bulk.Bytes.data(), Bulk.Bytes.size());
          BulkChunkHeader.HashHi = BulkHash.high64;
          BulkChunkHeader.HashLo = BulkHash.low64;

          Pack::SnPakBulkEntryV1 BulkEntry = {};
          const auto SemanticVal = static_cast<uint32_t>(Bulk.Semantic);
          std::memcpy(BulkEntry.Semantic, &SemanticVal, 4);
          BulkEntry.SubIndex = Bulk.SubIndex;
          BulkEntry.ChunkOffset = CurrentOffset;
          BulkEntry.SizeCompressed = sizeof(BulkChunkHeader) + CompressedBulk.size();
          BulkEntry.SizeUncompressed = Bulk.Bytes.size();
          BulkEntry.Compression = static_cast<uint8_t>(BulkCompression);
          BulkEntry.Reserved0[0] = static_cast<uint8_t>(BulkLevel);
          BulkEntry.HashHi = BulkHash.high64;
          BulkEntry.HashLo = BulkHash.low64;

          NewBulkEntries.push_back(BulkEntry);

          File.write(reinterpret_cast<const char*>(&BulkChunkHeader), sizeof(BulkChunkHeader));
          File.write(reinterpret_cast<const char*>(CompressedBulk.data()), static_cast<std::streamsize>(CompressedBulk.size()));
          if (!File.good())
          {
            return std::unexpected("Failed to write bulk chunk");
          }
          CurrentOffset += sizeof(BulkChunkHeader) + CompressedBulk.size();
        }
      }
      else if (ExistingEntryForBulk != nullptr)
      {
        auto CopyBulkResult = CopyExistingBulkEntries(*ExistingEntryForBulk, Entry, NewBulkEntries);
        if (!CopyBulkResult)
        {
          return std::unexpected(CopyBulkResult.error());
        }
      }

      auto DependencyResult =
          Impl::AppendDependencyMetadata(AssetIndex, Asset.AssetDependencies, NewDependencyOwners, NewDependencyEntries, AddString);
      if (!DependencyResult)
      {
        return std::unexpected(DependencyResult.error());
      }

      return Entry;
    };

    for (const AssetId& ExistingId : ExistingAssetOrder)
    {
      const auto ExistingIndexIt = ExistingAssetToIndex.find(ExistingId);
      if (ExistingIndexIt == ExistingAssetToIndex.end())
      {
        return std::unexpected("Internal error: missing existing asset index");
      }

      const auto& ExistingEntry = ExistingIndexEntries[ExistingIndexIt->second];
      if (const auto PendingIt = PendingById.find(ExistingId); PendingIt != PendingById.end())
      {
        const AssetPackEntry& PendingAsset = m_Impl->Assets[PendingIt->second];
        auto NewEntryResult = BuildUpdatedEntry(PendingAsset, static_cast<uint32_t>(NewIndexEntries.size()), &ExistingEntry);
        if (!NewEntryResult)
        {
          return std::unexpected("Failed to build updated asset entry: " + NewEntryResult.error());
        }
        NewIndexEntries.push_back(*NewEntryResult);
        PendingById.erase(PendingIt);
      }
      else
      {
        Pack::SnPakIndexEntryV1 PreservedEntry = ExistingEntry;
        auto CopyBulkResult = CopyExistingBulkEntries(ExistingEntry, PreservedEntry, NewBulkEntries);
        if (!CopyBulkResult)
        {
          return std::unexpected("Failed to preserve existing bulk entries: " + CopyBulkResult.error());
        }
        auto CopyDependencyResult = CopyExistingDependencyEntries(
            static_cast<uint32_t>(ExistingIndexIt->second), static_cast<uint32_t>(NewIndexEntries.size()), NewDependencyOwners, NewDependencyEntries);
        if (!CopyDependencyResult)
        {
          return std::unexpected("Failed to preserve existing dependency entries: " + CopyDependencyResult.error());
        }
        NewIndexEntries.push_back(PreservedEntry);
      }
    }

    for (size_t PendingIndex = 0; PendingIndex < m_Impl->Assets.size(); ++PendingIndex)
    {
      const AssetPackEntry& PendingAsset = m_Impl->Assets[PendingIndex];
      const auto PendingIt = PendingById.find(PendingAsset.Id);
      if (PendingIt == PendingById.end())
      {
        continue;
      }
      if (PendingIt->second != PendingIndex)
      {
        continue;
      }

      auto NewEntryResult = BuildUpdatedEntry(PendingAsset, static_cast<uint32_t>(NewIndexEntries.size()), nullptr);
      if (!NewEntryResult)
      {
        return std::unexpected("Failed to build new asset entry: " + NewEntryResult.error());
      }
      NewIndexEntries.push_back(*NewEntryResult);
      PendingById.erase(PendingIt);
    }

    if (NewIndexEntries.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    {
      return std::unexpected("Index entry count exceeds 32-bit range");
    }
    if (NewBulkEntries.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    {
      return std::unexpected("Bulk entry count exceeds 32-bit range");
    }
    if (NewDependencyOwners.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    {
      return std::unexpected("Dependency owner count exceeds 32-bit range");
    }
    if (NewDependencyEntries.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    {
      return std::unexpected("Dependency entry count exceeds 32-bit range");
    }

    const uint64_t NewStringTableOffset = CurrentOffset;
    std::vector<uint8_t> StringTableData = Impl::BuildStringTableBlock(StringTable);
    File.write(reinterpret_cast<const char*>(StringTableData.data()), static_cast<std::streamsize>(StringTableData.size()));
    if (!File.good())
    {
      return std::unexpected("Failed to write updated string table block");
    }
    CurrentOffset += StringTableData.size();

    const uint64_t NewIndexOffset = CurrentOffset;
    std::vector<uint8_t> IndexData =
        Impl::BuildIndexBlock(NewIndexEntries, NewBulkEntries, NewDependencyOwners, NewDependencyEntries, OldHeader.IndexOffset, OldHeader.IndexSize);
    File.write(reinterpret_cast<const char*>(IndexData.data()), static_cast<std::streamsize>(IndexData.size()));
    if (!File.good())
    {
      return std::unexpected("Failed to write updated index block");
    }
    CurrentOffset += IndexData.size();

    Pack::SnPakHeaderV1 NewHeader = OldHeader;
    NewHeader.FileSize = CurrentOffset;
    NewHeader.IndexOffset = NewIndexOffset;
    NewHeader.IndexSize = IndexData.size();
    NewHeader.StringTableOffset = NewStringTableOffset;
    NewHeader.StringTableSize = StringTableData.size();
    NewHeader.PreviousIndexOffset = OldHeader.IndexOffset;
    NewHeader.PreviousIndexSize = OldHeader.IndexSize;
    NewHeader.Flags |= Pack::SnPakFlag_HasTrailingIndex;

    const XXH128_hash_t IndexHash = XXH3_128bits(IndexData.data(), IndexData.size());
    NewHeader.IndexHashHi = IndexHash.high64;
    NewHeader.IndexHashLo = IndexHash.low64;

    File.seekp(0, std::ios::beg);
    File.write(reinterpret_cast<const char*>(&NewHeader), sizeof(NewHeader));
    if (!File.good())
    {
      return std::unexpected("Failed to write updated pack header");
    }
    File.close();

    return {};
  }

} // namespace SnAPI::AssetPipeline
