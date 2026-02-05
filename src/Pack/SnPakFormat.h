#pragma once

#include <cstdint>
#include <vector>

// All structs are byte-packed, little-endian
#pragma pack(push, 1)

namespace SnAPI::AssetPipeline::Pack
{

  // File magic: "SNPAK\0\0\0" (8 bytes)
  constexpr uint8_t kSnPakMagic[8] = {'S', 'N', 'P', 'A', 'K', 0, 0, 0};
  constexpr uint32_t kSnPakVersion = 1;

  // Block magics
  constexpr uint8_t kChunkMagic[4] = {'C', 'H', 'N', 'K'};
  constexpr uint8_t kIndexMagic[4] = {'I', 'N', 'D', 'X'};
  constexpr uint8_t kStringMagic[4] = {'S', 'T', 'R', 'S'};

  // Endian marker: 0x01020304 in native order
  constexpr uint32_t kEndianMarker = 0x01020304;

  // Compression kinds
  enum class ESnPakCompression : uint8_t
  {
    None = 0,
    LZ4 = 1,
    Zstd = 2,
    LZ4HC = 3,
    ZstdFast = 4,
  };

  enum class ESnPakCompressionLevel : uint8_t
  {
    Default = 0,
    Fast = 1,
    High = 2,
    Max = 3,
  };

  // Header flags
  enum ESnPakFlags : uint32_t
  {
    SnPakFlag_None = 0,
    SnPakFlag_HasTrailingIndex = 1 << 0,
    SnPakFlag_HasTypeTable = 1 << 1,
  };

  // Chunk kinds
  enum class ESnPakChunkKind : uint8_t
  {
    MainPayload = 0,
    Bulk = 1,
  };

  struct SnPakHeaderV1
  {
      uint8_t Magic[8];    // "SNPAK\0\0\0"
      uint32_t Version;    // 1
      uint32_t HeaderSize; // sizeof(SnPakHeaderV1)

      uint32_t EndianMarker; // 0x01020304

      uint64_t FileSize; // total bytes

      // Offsets to current "active" blocks
      uint64_t IndexOffset;
      uint64_t IndexSize;

      uint64_t StringTableOffset;
      uint64_t StringTableSize;

      uint64_t TypeTableOffset; // 0 if none
      uint64_t TypeTableSize;   // 0 if none

      // Hash of ENTIRE index block (header + entries + bulk entries)
      // This covers the index header itself, allowing validation of the whole block.
      // NOTE: The index header also contains its own hash (HashHi/HashLo) which covers
      // only the entries + bulk entries (not the index header). This is intentional:
      // - Pack header hash: validates the complete index block including its header
      // - Index header hash: validates entry data only (belt + suspenders approach)
      uint64_t IndexHashHi;
      uint64_t IndexHashLo;

      // Flags
      uint32_t Flags;
      uint32_t Reserved0;

      // For append-update mode
      uint64_t PreviousIndexOffset; // 0 if none
      uint64_t PreviousIndexSize;   // 0 if none

      uint8_t Reserved[64]; // pad for future expansions
  };

  static_assert(sizeof(SnPakHeaderV1) == 180, "SnPakHeaderV1 size mismatch");

  struct SnPakStrBlockHeaderV1
  {
      uint8_t Magic[4];   // "STRS"
      uint32_t Version;   // 1
      uint64_t BlockSize; // includes header
      uint32_t StringCount;
      uint32_t Reserved0;
      uint64_t HashHi;
      uint64_t HashLo;
  };

  static_assert(sizeof(SnPakStrBlockHeaderV1) == 40, "SnPakStrBlockHeaderV1 size mismatch");

  struct SnPakIndexHeaderV1
  {
      uint8_t Magic[4];   // "INDX"
      uint32_t Version;   // 1
      uint64_t BlockSize; // includes header + arrays

      uint32_t EntryCount;
      uint32_t BulkEntryCount;

      // Hash of ENTRIES + BULK ENTRIES ONLY (does not include this header)
      // This allows validation of the entry data independently.
      // The pack header also stores a hash of the entire index block (including this header).
      uint64_t EntriesHashHi;
      uint64_t EntriesHashLo;

      // For append-update chains
      uint64_t PreviousIndexOffset;
      uint64_t PreviousIndexSize;

      uint8_t Reserved[32];
  };

  static_assert(sizeof(SnPakIndexHeaderV1) == 88, "SnPakIndexHeaderV1 size mismatch");

  struct SnPakIndexEntryV1
  {
      uint8_t AssetId[16];

      uint8_t AssetKind[16]; // UUID

      uint8_t CookedPayloadType[16]; // UUID
      uint32_t CookedSchemaVersion;

      uint32_t NameStringId; // string table ID
      uint64_t NameHash64;

      uint32_t VariantStringId; // 0xFFFFFFFF if none
      uint64_t VariantHash64;

      // Main payload chunk location
      uint64_t PayloadChunkOffset;
      uint64_t PayloadChunkSizeCompressed;
      uint64_t PayloadChunkSizeUncompressed;

      uint8_t Compression; // ESnPakCompression
      uint8_t Flags;       // bit0=HasBulk
      uint16_t Reserved0;  // Compression level (low byte), reserved (high byte)

      uint32_t BulkFirstIndex; // into bulk array
      uint32_t BulkCount;

      uint64_t PayloadHashHi;
      uint64_t PayloadHashLo;
  };

  static_assert(sizeof(SnPakIndexEntryV1) == 128, "SnPakIndexEntryV1 size mismatch");

  // Index entry flags
  enum ESnPakIndexEntryFlags : uint8_t
  {
    IndexEntryFlag_None = 0,
    IndexEntryFlag_HasBulk = 1 << 0,
  };

  struct SnPakBulkEntryV1
  {
      uint8_t Semantic[4]; // EBulkSemantic as u32 LE
      uint32_t SubIndex;

      uint64_t ChunkOffset;
      uint64_t SizeCompressed;
      uint64_t SizeUncompressed;

      uint8_t Compression; // ESnPakCompression
      uint8_t Reserved0[7]; // Compression level in Reserved0[0]

      uint64_t HashHi;
      uint64_t HashLo;
  };

  static_assert(sizeof(SnPakBulkEntryV1) == 56, "SnPakBulkEntryV1 size mismatch");

  struct SnPakChunkHeaderV1
  {
      uint8_t Magic[4]; // "CHNK"
      uint32_t Version; // 1

      uint8_t AssetId[16];
      uint8_t PayloadType[16]; // cooked payload type UUID
      uint32_t SchemaVersion;  // for payloads; 0 for bulk

      uint8_t Compression; // ESnPakCompression
      uint8_t ChunkKind;   // ESnPakChunkKind
      uint16_t Reserved0;  // Compression level (low byte), reserved (high byte)

      uint64_t SizeCompressed;
      uint64_t SizeUncompressed;

      uint64_t HashHi;
      uint64_t HashLo;
  };

  static_assert(sizeof(SnPakChunkHeaderV1) == 80, "SnPakChunkHeaderV1 size mismatch");

} // namespace SnAPI::AssetPipeline::Pack

#pragma pack(pop)

namespace SnAPI::AssetPipeline::Pack
{

  // Helper to copy UUID bytes
  inline void CopyUuid(uint8_t* Dest, const uint8_t* Src)
  {
    for (int i = 0; i < 16; ++i)
    {
      Dest[i] = Src[i];
    }
  }

  inline bool CompareUuid(const uint8_t* A, const uint8_t* B)
  {
    for (int i = 0; i < 16; ++i)
    {
      if (A[i] != B[i])
        return false;
    }
    return true;
  }

  // Compression/decompression functions
  std::vector<uint8_t> Compress(const uint8_t* Data, size_t Size, ESnPakCompression Mode, ESnPakCompressionLevel Level);
  std::vector<uint8_t> Compress(const uint8_t* Data, size_t Size, ESnPakCompression Mode);
  std::vector<uint8_t> CompressMax(const uint8_t* Data, size_t Size, ESnPakCompression Mode);
  std::vector<uint8_t> Decompress(const uint8_t* Data, size_t CompressedSize, size_t UncompressedSize, ESnPakCompression Mode);

} // namespace SnAPI::AssetPipeline::Pack
