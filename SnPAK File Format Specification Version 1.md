# SnPAK File Format Specification

**Version:** 1.0
**Status:** Production
**Extension:** `.snpak`
**MIME Type:** `application/x-snpak`
**Created:** 2026
**Author:** SnAPI Asset Pipeline Team

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Use Cases](#2-use-cases)
3. [Design Goals](#3-design-goals)
4. [Notation and Conventions](#4-notation-and-conventions)
5. [File Structure Overview](#5-file-structure-overview)
6. [Magic Numbers and Constants](#6-magic-numbers-and-constants)
7. [Main Header (SnPakHeaderV1)](#7-main-header-snpakheaderv1)
8. [String Table Block](#8-string-table-block)
9. [Chunk Structure](#9-chunk-structure)
10. [Index Block](#10-index-block)
11. [Asset Index Entry](#11-asset-index-entry)
12. [Bulk Entry](#12-bulk-entry)
13. [UUIDs and Type Identifiers](#13-uuids-and-type-identifiers)
14. [Compression](#14-compression)
15. [Data Integrity and Hashing](#15-data-integrity-and-hashing)
16. [Append-Update Mode](#16-append-update-mode)
17. [Sanity Limits and Security](#17-sanity-limits-and-security)
18. [Complete File Layout Diagram](#18-complete-file-layout-diagram)
19. [Reading the Format](#19-reading-the-format)
20. [Writing the Format](#20-writing-the-format)
21. [Revision History](#21-revision-history)

---

## 1. Introduction

### 1.1 What is SnPAK?

SnPAK (pronounced "snap-pack") is a binary container format designed for packaging and distributing cooked game assets. It serves as the primary distribution format for the SnAPI Asset Pipeline, providing a single-file solution for bundling textures, models, audio, and other game resources that have been processed ("cooked") for runtime consumption.

### 1.2 Purpose

The SnPAK format exists to solve several critical problems in game asset distribution:

1. **Asset Aggregation**: Combines thousands of individual asset files into a single distributable package, reducing file system overhead and improving loading times.

2. **Data Integrity**: Provides cryptographic hash verification (XXH3-128) for all data blocks, ensuring assets are not corrupted during transmission or storage.

3. **Compression**: Supports multiple compression algorithms (LZ4, Zstd) to reduce storage requirements while maintaining fast decompression speeds suitable for real-time loading.

4. **Streaming Support**: Designed with random access in mind, allowing individual assets to be loaded without reading the entire file.

5. **Incremental Updates**: Supports append-update mode for adding new assets to existing packs without full rebuilds.

6. **Type Safety**: Each asset carries type information (UUIDs) enabling runtime validation and proper deserialization.

### 1.3 Format Philosophy

The SnPAK format prioritizes:

- **Read Performance**: The format is optimized for fast reading during game runtime. The index is located at a known offset, enabling quick enumeration of contents.
- **Simplicity**: The format uses fixed-size structures with clear layouts, making implementation straightforward.
- **Forward Compatibility**: Reserved fields and version numbers allow future extensions without breaking existing readers.
- **Atomicity**: Write operations use temporary files and atomic renames to prevent corruption.

### 1.4 What SnPAK is NOT

- **Not Encrypted**: SnPAK does not provide encryption. If asset protection is required, encryption must be applied at the application level or by wrapping the SnPAK file.
- **Not a Source Format**: SnPAK stores cooked/processed assets, not source data. Original assets should be retained separately.
- **Not a General Archive**: Unlike ZIP or TAR, SnPAK is specifically designed for game assets with typed payloads and bulk data support.

---

## 2. Use Cases

### 2.1 Game Distribution

The primary use case is packaging game assets for distribution:

```
game_textures.snpak    (500 MB - all game textures)
game_audio.snpak       (200 MB - music and sound effects)
game_models.snpak      (150 MB - 3D models and animations)
```

### 2.2 DLC and Updates

New content can be distributed as additional SnPAK files:

```
dlc_chapter2.snpak     (new chapter assets)
patch_v1.2.snpak       (updated assets)
```

### 2.3 Streaming Levels

Large games can use per-level or per-region packs:

```
level_forest.snpak
level_desert.snpak
level_caves.snpak
```

### 2.4 Platform-Specific Assets

Different packs for different platforms:

```
textures_pc_high.snpak
textures_console.snpak
textures_mobile.snpak
```

### 2.5 Mod Distribution

Modders can package their content using the same format:

```
community_mod_skins.snpak
```

---

## 3. Design Goals

### 3.1 Goals

| Goal | Description |
|------|-------------|
| **Fast Random Access** | Any asset can be loaded independently without reading other assets |
| **Minimal Seek Operations** | Related data is stored contiguously to minimize disk seeks |
| **Compact Index** | The asset index is small enough to load entirely into memory |
| **Hash Verification** | All data can be verified for integrity using XXH3-128 hashes |
| **Compression Flexibility** | Per-chunk compression allows optimal settings for each asset type |
| **Bulk Data Separation** | Large data (mipmaps, LODs) is stored separately from metadata |
| **Append-Only Updates** | New assets can be added without rewriting the entire file |

### 3.2 Non-Goals

| Non-Goal | Rationale |
|----------|-----------|
| Encryption | Security should be handled at the application or transport layer |
| Delta Updates | Would add complexity; full replacement is preferred |
| In-Place Modification | Append-only design is simpler and safer |
| Cross-Platform Endianness | Little-endian only; big-endian systems are rare in gaming |

---

## 4. Notation and Conventions

### 4.1 Data Types

| Type | Size | Description |
|------|------|-------------|
| `uint8_t` | 1 byte | Unsigned 8-bit integer |
| `uint16_t` | 2 bytes | Unsigned 16-bit integer, little-endian |
| `uint32_t` | 4 bytes | Unsigned 32-bit integer, little-endian |
| `uint64_t` | 8 bytes | Unsigned 64-bit integer, little-endian |
| `uint8_t[N]` | N bytes | Fixed-size byte array |
| `UUID` | 16 bytes | Universally Unique Identifier (RFC 4122) |

### 4.2 Byte Order

**All multi-byte integers are stored in little-endian byte order.**

Example: The value `0x01020304` is stored as bytes `04 03 02 01`.

The `EndianMarker` field in the header can be used to verify endianness at read time.

### 4.3 Alignment

**All structures are byte-packed with no padding.**

The format uses `#pragma pack(push, 1)` semantics. Readers and writers must ensure structures are read/written without compiler-inserted padding.

### 4.4 String Encoding

All strings are **UTF-8 encoded** and **null-terminated**.

### 4.5 Offset Notation

All offsets in this document are:
- **Absolute**: Measured from the beginning of the file (offset 0x00)
- **Hexadecimal**: Prefixed with `0x`

---

## 5. File Structure Overview

A SnPAK file consists of the following sections, in order:

```
+--------------------------------------------------+
|                  MAIN HEADER                     |  Fixed: 180 bytes
|              (SnPakHeaderV1)                     |  Offset: 0x00
+--------------------------------------------------+
|              STRING TABLE BLOCK                  |  Variable size
|          (SnPakStrBlockHeaderV1 + data)          |  Offset: Header.StringTableOffset
+--------------------------------------------------+
|                                                  |
|              PAYLOAD CHUNKS                      |  Variable size
|   (SnPakChunkHeaderV1 + compressed data) × N     |  (Asset main payloads)
|                                                  |
+--------------------------------------------------+
|                                                  |
|               BULK CHUNKS                        |  Variable size
|   (SnPakChunkHeaderV1 + compressed data) × M     |  (Mips, LODs, aux data)
|                                                  |
+--------------------------------------------------+
|                INDEX BLOCK                       |  Variable size
|   (SnPakIndexHeaderV1 + entries + bulk entries)  |  Offset: Header.IndexOffset
+--------------------------------------------------+
```

**Important Notes:**

1. The **String Table** is written immediately after the header in standard writes.
2. **Payload Chunks** and **Bulk Chunks** may be interleaved (all chunks for one asset together) or grouped.
3. The **Index Block** is typically written last, allowing the header to be updated with its location.
4. In **Append-Update Mode**, additional String Tables and Index Blocks may exist at the end of the file.

---

## 6. Magic Numbers and Constants

### 6.1 File Magic Signature

| Constant | Value (Hex) | Value (ASCII) | Size |
|----------|-------------|---------------|------|
| `kSnPakMagic` | `53 4E 50 41 4B 00 00 00` | `SNPAK\0\0\0` | 8 bytes |

The file magic identifies a file as SnPAK format. It must appear at offset 0x00.

### 6.2 Block Magic Signatures

| Constant | Value (Hex) | Value (ASCII) | Purpose |
|----------|-------------|---------------|---------|
| `kChunkMagic` | `43 48 4E 4B` | `CHNK` | Marks payload/bulk data chunks |
| `kIndexMagic` | `49 4E 44 58` | `INDX` | Marks the index block |
| `kStringMagic` | `53 54 52 53` | `STRS` | Marks the string table block |

### 6.3 Version Number

| Constant | Value | Description |
|----------|-------|-------------|
| `kSnPakVersion` | `1` | Current format version |

### 6.4 Endian Marker

| Constant | Value | Purpose |
|----------|-------|---------|
| `kEndianMarker` | `0x01020304` | Endianness detection |

When read as bytes on a little-endian system: `04 03 02 01`
When read as bytes on a big-endian system: `01 02 03 04`

Readers should compare the stored marker against the expected value to detect endianness mismatch.

---

## 7. Main Header (SnPakHeaderV1)

The main header is always located at **offset 0x00** and is exactly **180 bytes**.

### 7.1 Structure Definition

```c
struct SnPakHeaderV1 {
    uint8_t  Magic[8];              // Offset 0x00, 8 bytes
    uint32_t Version;               // Offset 0x08, 4 bytes
    uint32_t HeaderSize;            // Offset 0x0C, 4 bytes
    uint32_t EndianMarker;          // Offset 0x10, 4 bytes
    uint64_t FileSize;              // Offset 0x14, 8 bytes
    uint64_t IndexOffset;           // Offset 0x1C, 8 bytes
    uint64_t IndexSize;             // Offset 0x24, 8 bytes
    uint64_t StringTableOffset;     // Offset 0x2C, 8 bytes
    uint64_t StringTableSize;       // Offset 0x34, 8 bytes
    uint64_t TypeTableOffset;       // Offset 0x3C, 8 bytes
    uint64_t TypeTableSize;         // Offset 0x44, 8 bytes
    uint64_t IndexHashHi;           // Offset 0x4C, 8 bytes
    uint64_t IndexHashLo;           // Offset 0x54, 8 bytes
    uint32_t Flags;                 // Offset 0x5C, 4 bytes
    uint32_t Reserved0;             // Offset 0x60, 4 bytes
    uint64_t PreviousIndexOffset;   // Offset 0x64, 8 bytes
    uint64_t PreviousIndexSize;     // Offset 0x6C, 8 bytes
    uint8_t  Reserved[64];          // Offset 0x74, 64 bytes
};                                  // Total: 180 bytes
```

### 7.2 Field Descriptions

#### 7.2.1 Magic (Offset 0x00, 8 bytes)

**Type:** `uint8_t[8]`
**Value:** `{'S', 'N', 'P', 'A', 'K', 0, 0, 0}` (hex: `53 4E 50 41 4B 00 00 00`)

The file signature that identifies this file as a SnPAK archive. This must be the first 8 bytes of any valid SnPAK file.

**Validation:** Readers MUST check this field first. If it does not match, the file is not a valid SnPAK.

#### 7.2.2 Version (Offset 0x08, 4 bytes)

**Type:** `uint32_t`
**Current Value:** `1`

The format version number. This document describes version 1.

**Forward Compatibility:** Readers encountering a higher version number should either:
- Attempt to read if they support that version
- Reject the file with an appropriate error message

**Backward Compatibility:** Future versions should maintain backward compatibility where possible.

#### 7.2.3 HeaderSize (Offset 0x0C, 4 bytes)

**Type:** `uint32_t`
**Value:** `180` (for version 1)

The size of this header structure in bytes. This field enables forward compatibility:
- If a future version extends the header, old readers can skip unknown fields
- Readers should use this value to determine where header data ends

#### 7.2.4 EndianMarker (Offset 0x10, 4 bytes)

**Type:** `uint32_t`
**Value:** `0x01020304`

A marker for detecting byte order mismatches. When read correctly on a little-endian system, this value equals `0x01020304`.

**Usage:**
```c
if (header.EndianMarker != 0x01020304) {
    // Error: File was written on a different-endian system
    // SnPAK currently only supports little-endian
}
```

#### 7.2.5 FileSize (Offset 0x14, 8 bytes)

**Type:** `uint64_t`

The total size of the SnPAK file in bytes.

**Usage:**
- Verify file integrity by comparing against actual file size
- Pre-allocate buffers for file operations
- Detect truncated files

#### 7.2.6 IndexOffset (Offset 0x1C, 8 bytes)

**Type:** `uint64_t`

The absolute byte offset from the start of the file to the **current active** Index Block.

**Usage:**
```c
file.seek(header.IndexOffset);
// Now at the start of the SnPakIndexHeaderV1
```

#### 7.2.7 IndexSize (Offset 0x24, 8 bytes)

**Type:** `uint64_t`

The total size of the Index Block in bytes, including the index header and all entries.

**Usage:**
- Pre-allocate buffer for reading the entire index
- Validate that `IndexOffset + IndexSize <= FileSize`

#### 7.2.8 StringTableOffset (Offset 0x2C, 8 bytes)

**Type:** `uint64_t`

The absolute byte offset to the String Table Block.

In standard files, this immediately follows the header at offset `0xB4` (180).

#### 7.2.9 StringTableSize (Offset 0x34, 8 bytes)

**Type:** `uint64_t`

The total size of the String Table Block in bytes, including its header.

#### 7.2.10 TypeTableOffset (Offset 0x3C, 8 bytes)

**Type:** `uint64_t`
**Current Value:** `0` (reserved)

Reserved for a future Type Table feature that would provide type metadata.

When this field is `0`, no Type Table is present.

#### 7.2.11 TypeTableSize (Offset 0x44, 8 bytes)

**Type:** `uint64_t`
**Current Value:** `0` (reserved)

Size of the Type Table. When `0`, no Type Table exists.

#### 7.2.12 IndexHashHi (Offset 0x4C, 8 bytes)

**Type:** `uint64_t`

The high 64 bits of the XXH3-128 hash of the entire Index Block (including header).

#### 7.2.13 IndexHashLo (Offset 0x54, 8 bytes)

**Type:** `uint64_t`

The low 64 bits of the XXH3-128 hash of the entire Index Block.

**Verification:**
```c
XXH128_hash_t computed = XXH3_128bits(indexBlockData, indexBlockSize);
if (computed.high64 != header.IndexHashHi ||
    computed.low64 != header.IndexHashLo) {
    // Index block is corrupted
}
```

#### 7.2.14 Flags (Offset 0x5C, 4 bytes)

**Type:** `uint32_t` (bitfield)

Pack-level flags indicating special features or states:

| Flag | Value | Description |
|------|-------|-------------|
| `SnPakFlag_None` | `0x00000000` | No flags set |
| `SnPakFlag_HasTrailingIndex` | `0x00000001` | Pack has been updated via append mode |
| `SnPakFlag_HasTypeTable` | `0x00000002` | Pack contains a type table (reserved) |

**HasTrailingIndex:** When set, indicates this file has been modified using append-update mode. The `PreviousIndexOffset` and `PreviousIndexSize` fields point to the previous index.

#### 7.2.15 Reserved0 (Offset 0x60, 4 bytes)

**Type:** `uint32_t`
**Value:** `0`

Reserved for future use. Writers MUST set this to `0`. Readers MUST ignore this field.

#### 7.2.16 PreviousIndexOffset (Offset 0x64, 8 bytes)

**Type:** `uint64_t`
**Default:** `0`

When `SnPakFlag_HasTrailingIndex` is set, this points to the location of the previous index block before the last append operation.

When `0`, there is no previous index.

#### 7.2.17 PreviousIndexSize (Offset 0x6C, 8 bytes)

**Type:** `uint64_t`
**Default:** `0`

Size of the previous index block in bytes.

#### 7.2.18 Reserved (Offset 0x74, 64 bytes)

**Type:** `uint8_t[64]`
**Value:** All zeros

Reserved space for future header extensions. Writers MUST fill this with zeros. Readers MUST ignore this field.

---

## 8. String Table Block

The String Table Block stores all human-readable strings used in the pack, primarily asset names and variant keys. Storing strings centrally enables deduplication and reduces index size.

### 8.1 Block Header (SnPakStrBlockHeaderV1)

**Size:** 40 bytes

```c
struct SnPakStrBlockHeaderV1 {
    uint8_t  Magic[4];      // Offset 0x00, 4 bytes: "STRS"
    uint32_t Version;       // Offset 0x04, 4 bytes: 1
    uint64_t BlockSize;     // Offset 0x08, 8 bytes
    uint32_t StringCount;   // Offset 0x10, 4 bytes
    uint32_t Reserved0;     // Offset 0x14, 4 bytes
    uint64_t HashHi;        // Offset 0x18, 8 bytes
    uint64_t HashLo;        // Offset 0x20, 8 bytes
};                          // Total: 40 bytes
```

### 8.2 Field Descriptions

#### 8.2.1 Magic (Offset 0x00, 4 bytes)

**Value:** `{'S', 'T', 'R', 'S'}` (hex: `53 54 52 53`)

Identifies this block as a String Table.

#### 8.2.2 Version (Offset 0x04, 4 bytes)

**Value:** `1`

Version of the string table format.

#### 8.2.3 BlockSize (Offset 0x08, 8 bytes)

Total size of this block in bytes, including:
- This header (40 bytes)
- The offsets array
- The string data

#### 8.2.4 StringCount (Offset 0x10, 4 bytes)

Number of strings in the table. Valid string IDs are `0` through `StringCount - 1`.

#### 8.2.5 Reserved0 (Offset 0x14, 4 bytes)

Reserved. Must be `0`.

#### 8.2.6 HashHi / HashLo (Offset 0x18, 16 bytes)

XXH3-128 hash of the string data (NOT including the header or offsets array).

### 8.3 Block Layout

```
+------------------------------------------+
|        SnPakStrBlockHeaderV1             |  40 bytes
+------------------------------------------+
|        String Offsets Array              |  StringCount × 4 bytes
|        [uint32_t offset0]                |
|        [uint32_t offset1]                |
|        ...                               |
|        [uint32_t offsetN-1]              |
+------------------------------------------+
|        String Data                       |  Variable size
|        "string0\0"                       |
|        "string1\0"                       |
|        ...                               |
|        "stringN-1\0"                     |
+------------------------------------------+
```

### 8.4 String Lookup

To retrieve string with ID `i`:

1. Read `offsets[i]` from the offsets array
2. Read null-terminated string starting at `StringData + offsets[i]`

```c
// Pseudocode
const char* GetString(uint32_t id) {
    if (id >= header.StringCount) return nullptr;
    uint32_t offset = offsets[id];
    return (const char*)(stringData + offset);
}
```

### 8.5 Special String ID

The value `0xFFFFFFFF` is reserved to indicate "no string" (e.g., for assets without a variant key).

---

## 9. Chunk Structure

Both main payloads and bulk data are stored in **Chunks**. Each chunk consists of a header followed by (optionally compressed) data.

### 9.1 Chunk Header (SnPakChunkHeaderV1)

**Size:** 80 bytes

```c
struct SnPakChunkHeaderV1 {
    uint8_t  Magic[4];           // Offset 0x00, 4 bytes: "CHNK"
    uint32_t Version;            // Offset 0x04, 4 bytes: 1
    uint8_t  AssetId[16];        // Offset 0x08, 16 bytes
    uint8_t  PayloadType[16];    // Offset 0x18, 16 bytes
    uint32_t SchemaVersion;      // Offset 0x28, 4 bytes
    uint8_t  Compression;        // Offset 0x2C, 1 byte
    uint8_t  ChunkKind;          // Offset 0x2D, 1 byte
    uint16_t Reserved0;          // Offset 0x2E, 2 bytes
    uint64_t SizeCompressed;     // Offset 0x30, 8 bytes
    uint64_t SizeUncompressed;   // Offset 0x38, 8 bytes
    uint64_t HashHi;             // Offset 0x40, 8 bytes
    uint64_t HashLo;             // Offset 0x48, 8 bytes
};                               // Total: 80 bytes
```

### 9.2 Field Descriptions

#### 9.2.1 Magic (Offset 0x00, 4 bytes)

**Value:** `{'C', 'H', 'N', 'K'}` (hex: `43 48 4E 4B`)

Identifies this as a data chunk.

#### 9.2.2 Version (Offset 0x04, 4 bytes)

**Value:** `1`

Chunk format version.

#### 9.2.3 AssetId (Offset 0x08, 16 bytes)

The UUID of the asset this chunk belongs to. This allows verification that the correct chunk was loaded.

#### 9.2.4 PayloadType (Offset 0x18, 16 bytes)

The UUID identifying the type of payload data. This enables type-safe deserialization.

Examples:
- Texture payloads might use UUID `{00000000-0000-0000-0000-000000000001}`
- Model payloads might use UUID `{00000000-0000-0000-0000-000000000002}`

#### 9.2.5 SchemaVersion (Offset 0x28, 4 bytes)

Version of the payload schema/format. Allows backward compatibility when payload formats evolve.

For bulk chunks (mipmaps, etc.), this is typically `0`.

#### 9.2.6 Compression (Offset 0x2C, 1 byte)

The compression algorithm used for this chunk's data:

| Value | Name | Description |
|-------|------|-------------|
| `0` | None | Data is stored uncompressed |
| `1` | LZ4 | LZ4 compression (fast decompression) |
| `2` | Zstd | Zstandard compression (better ratio) |

#### 9.2.7 ChunkKind (Offset 0x2D, 1 byte)

The type of data in this chunk:

| Value | Name | Description |
|-------|------|-------------|
| `0` | MainPayload | Primary cooked asset data |
| `1` | Bulk | Auxiliary data (mipmaps, LODs, etc.) |

#### 9.2.8 Reserved0 (Offset 0x2E, 2 bytes)

Reserved. Must be `0`.

#### 9.2.9 SizeCompressed (Offset 0x30, 8 bytes)

Size of the compressed data following this header, in bytes.

If `Compression == None`, this equals `SizeUncompressed`.

#### 9.2.10 SizeUncompressed (Offset 0x38, 8 bytes)

Original size of the data before compression.

#### 9.2.11 HashHi / HashLo (Offset 0x40, 16 bytes)

XXH3-128 hash of the **uncompressed** data.

After decompression, verify:
```c
XXH128_hash_t computed = XXH3_128bits(decompressedData, SizeUncompressed);
if (computed.high64 != chunk.HashHi || computed.low64 != chunk.HashLo) {
    // Data corruption detected
}
```

### 9.3 Chunk Layout

```
+------------------------------------------+
|        SnPakChunkHeaderV1                |  80 bytes
+------------------------------------------+
|        Compressed/Raw Data               |  SizeCompressed bytes
+------------------------------------------+
```

---

## 10. Index Block

The Index Block is the central directory of all assets in the pack. It contains metadata for every asset and their bulk data references.

### 10.1 Index Header (SnPakIndexHeaderV1)

**Size:** 88 bytes

```c
struct SnPakIndexHeaderV1 {
    uint8_t  Magic[4];              // Offset 0x00, 4 bytes: "INDX"
    uint32_t Version;               // Offset 0x04, 4 bytes: 1
    uint64_t BlockSize;             // Offset 0x08, 8 bytes
    uint32_t EntryCount;            // Offset 0x10, 4 bytes
    uint32_t BulkEntryCount;        // Offset 0x14, 4 bytes
    uint64_t HashHi;                // Offset 0x18, 8 bytes
    uint64_t HashLo;                // Offset 0x20, 8 bytes
    uint64_t PreviousIndexOffset;   // Offset 0x28, 8 bytes
    uint64_t PreviousIndexSize;     // Offset 0x30, 8 bytes
    uint8_t  Reserved[32];          // Offset 0x38, 32 bytes
};                                  // Total: 88 bytes
```

### 10.2 Field Descriptions

#### 10.2.1 Magic (Offset 0x00, 4 bytes)

**Value:** `{'I', 'N', 'D', 'X'}` (hex: `49 4E 44 58`)

#### 10.2.2 Version (Offset 0x04, 4 bytes)

**Value:** `1`

#### 10.2.3 BlockSize (Offset 0x08, 8 bytes)

Total size of the index block including:
- This header (88 bytes)
- All asset entries
- All bulk entries

```
BlockSize = 88 + (EntryCount × 128) + (BulkEntryCount × 56)
```

#### 10.2.4 EntryCount (Offset 0x10, 4 bytes)

Number of asset entries in this index.

#### 10.2.5 BulkEntryCount (Offset 0x14, 4 bytes)

Total number of bulk entries across all assets.

#### 10.2.6 HashHi / HashLo (Offset 0x18, 16 bytes)

XXH3-128 hash of all entries (asset entries + bulk entries concatenated), NOT including the index header itself.

#### 10.2.7 PreviousIndexOffset / PreviousIndexSize (Offset 0x28, 16 bytes)

For append-update chains, points to the previous index block. Zero if this is the original index.

#### 10.2.8 Reserved (Offset 0x38, 32 bytes)

Reserved. Must be zeros.

### 10.3 Index Block Layout

```
+------------------------------------------+
|        SnPakIndexHeaderV1                |  88 bytes
+------------------------------------------+
|        Asset Entries                     |  EntryCount × 128 bytes
|        [SnPakIndexEntryV1 × EntryCount]  |
+------------------------------------------+
|        Bulk Entries                      |  BulkEntryCount × 56 bytes
|        [SnPakBulkEntryV1 × BulkEntryCount]|
+------------------------------------------+
```

---

## 11. Asset Index Entry

Each asset in the pack has one index entry describing its location and metadata.

### 11.1 Structure (SnPakIndexEntryV1)

**Size:** 128 bytes

```c
struct SnPakIndexEntryV1 {
    uint8_t  AssetId[16];                   // Offset 0x00, 16 bytes
    uint8_t  AssetKind[16];                 // Offset 0x10, 16 bytes
    uint8_t  CookedPayloadType[16];         // Offset 0x20, 16 bytes
    uint32_t CookedSchemaVersion;           // Offset 0x30, 4 bytes
    uint32_t NameStringId;                  // Offset 0x34, 4 bytes
    uint64_t NameHash64;                    // Offset 0x38, 8 bytes
    uint32_t VariantStringId;               // Offset 0x40, 4 bytes
    uint64_t VariantHash64;                 // Offset 0x44, 8 bytes
    uint64_t PayloadChunkOffset;            // Offset 0x4C, 8 bytes
    uint64_t PayloadChunkSizeCompressed;    // Offset 0x54, 8 bytes
    uint64_t PayloadChunkSizeUncompressed;  // Offset 0x5C, 8 bytes
    uint8_t  Compression;                   // Offset 0x64, 1 byte
    uint8_t  Flags;                         // Offset 0x65, 1 byte
    uint16_t Reserved0;                     // Offset 0x66, 2 bytes
    uint32_t BulkFirstIndex;                // Offset 0x68, 4 bytes
    uint32_t BulkCount;                     // Offset 0x6C, 4 bytes
    uint64_t PayloadHashHi;                 // Offset 0x70, 8 bytes
    uint64_t PayloadHashLo;                 // Offset 0x78, 8 bytes
};                                          // Total: 128 bytes
```

### 11.2 Field Descriptions

#### 11.2.1 AssetId (Offset 0x00, 16 bytes)

A UUID that uniquely identifies this asset. Used as the primary key for asset lookup.

#### 11.2.2 AssetKind (Offset 0x10, 16 bytes)

A UUID identifying the category/type of asset (e.g., Texture2D, StaticMesh, AudioClip).

This is used by the runtime to determine which loader/deserializer to use.

#### 11.2.3 CookedPayloadType (Offset 0x20, 16 bytes)

A UUID identifying the specific cooked payload format.

Different from AssetKind: AssetKind might be "Texture2D" while CookedPayloadType might be "DXT5CompressedTexture" or "RawRGBA8Texture".

#### 11.2.4 CookedSchemaVersion (Offset 0x30, 4 bytes)

Version number for the cooked payload schema. Enables backward compatibility when payload formats evolve.

#### 11.2.5 NameStringId (Offset 0x34, 4 bytes)

Index into the String Table for the asset's logical name.

Example: If the asset name is `"textures/checkerboard"`, this ID points to that string.

#### 11.2.6 NameHash64 (Offset 0x38, 8 bytes)

XXH3-64 hash of the asset name string. Enables O(1) name-based lookups without string comparison:

```c
uint64_t hash = XXH3_64bits(name.data(), name.length());
auto& candidates = nameHashToIndices[hash];
// Then verify actual string match
```

#### 11.2.7 VariantStringId (Offset 0x40, 4 bytes)

Index into the String Table for the variant key.

**Special Value:** `0xFFFFFFFF` means "no variant" (default/only version of this asset).

Examples of variant keys: `"high"`, `"low"`, `"mobile"`, `"desktop"`

#### 11.2.8 VariantHash64 (Offset 0x44, 8 bytes)

XXH3-64 hash of the variant key string.

**Value when no variant:** `0`

#### 11.2.9 PayloadChunkOffset (Offset 0x4C, 8 bytes)

Absolute byte offset to the main payload chunk (SnPakChunkHeaderV1).

#### 11.2.10 PayloadChunkSizeCompressed (Offset 0x54, 8 bytes)

Total size of the payload chunk including header:
```
PayloadChunkSizeCompressed = sizeof(SnPakChunkHeaderV1) + compressed_data_size
                           = 80 + chunk.SizeCompressed
```

#### 11.2.11 PayloadChunkSizeUncompressed (Offset 0x5C, 8 bytes)

Size of the payload data after decompression (NOT including chunk header).

#### 11.2.12 Compression (Offset 0x64, 1 byte)

Compression algorithm used for the main payload (same values as chunk header):
- `0`: None
- `1`: LZ4
- `2`: Zstd

#### 11.2.13 Flags (Offset 0x65, 1 byte)

Entry flags:

| Flag | Value | Description |
|------|-------|-------------|
| `IndexEntryFlag_None` | `0x00` | No flags |
| `IndexEntryFlag_HasBulk` | `0x01` | Asset has bulk data chunks |

#### 11.2.14 Reserved0 (Offset 0x66, 2 bytes)

Reserved. Must be `0`.

#### 11.2.15 BulkFirstIndex (Offset 0x68, 4 bytes)

Index of the first bulk entry for this asset in the bulk entries array.

Only valid if `Flags & IndexEntryFlag_HasBulk`.

#### 11.2.16 BulkCount (Offset 0x6C, 4 bytes)

Number of bulk entries for this asset. `0` if no bulk data.

Bulk entries for this asset are at indices `BulkFirstIndex` through `BulkFirstIndex + BulkCount - 1`.

#### 11.2.17 PayloadHashHi / PayloadHashLo (Offset 0x70, 16 bytes)

XXH3-128 hash of the uncompressed payload data.

---

## 12. Bulk Entry

Bulk entries describe auxiliary data chunks like mipmaps, LOD levels, or other large data that may be loaded independently.

### 12.1 Structure (SnPakBulkEntryV1)

**Size:** 56 bytes

```c
struct SnPakBulkEntryV1 {
    uint8_t  Semantic[4];       // Offset 0x00, 4 bytes
    uint32_t SubIndex;          // Offset 0x04, 4 bytes
    uint64_t ChunkOffset;       // Offset 0x08, 8 bytes
    uint64_t SizeCompressed;    // Offset 0x10, 8 bytes
    uint64_t SizeUncompressed;  // Offset 0x18, 8 bytes
    uint8_t  Compression;       // Offset 0x20, 1 byte
    uint8_t  Reserved0[7];      // Offset 0x21, 7 bytes
    uint64_t HashHi;            // Offset 0x28, 8 bytes
    uint64_t HashLo;            // Offset 0x30, 8 bytes
};                              // Total: 56 bytes
```

### 12.2 Field Descriptions

#### 12.2.1 Semantic (Offset 0x00, 4 bytes)

A `uint32_t` (stored as 4 bytes, little-endian) indicating the semantic meaning of this bulk data:

| Value | Name | Description |
|-------|------|-------------|
| `0` | Unknown | Unclassified bulk data |
| `1` | Reserved_Level | Mipmap/LOD level data |
| `2` | Reserved_Aux | Auxiliary stream data |
| `0x10000+` | Custom | Plugin-defined semantics |

**Level Semantic:** Used for hierarchical data like texture mipmaps or mesh LODs. The `SubIndex` field indicates the level (0 = highest detail).

**Aux Semantic:** Used for supplementary data streams that don't fit other categories.

#### 12.2.2 SubIndex (Offset 0x04, 4 bytes)

Distinguishes multiple bulk entries with the same semantic.

For mipmap levels: `SubIndex = 0` is the highest resolution, `SubIndex = 1` is the next level, etc.

#### 12.2.3 ChunkOffset (Offset 0x08, 8 bytes)

Absolute byte offset to the bulk chunk (SnPakChunkHeaderV1).

#### 12.2.4 SizeCompressed (Offset 0x10, 8 bytes)

Total size of the chunk including header:
```
SizeCompressed = sizeof(SnPakChunkHeaderV1) + compressed_data_size
               = 80 + chunk.SizeCompressed
```

#### 12.2.5 SizeUncompressed (Offset 0x18, 8 bytes)

Size of the bulk data after decompression.

#### 12.2.6 Compression (Offset 0x20, 1 byte)

Compression algorithm:
- `0`: None
- `1`: LZ4
- `2`: Zstd

Bulk chunks can have different compression settings than the main payload. For example, already-compressed data (JPEG, OGG) might use `None`.

#### 12.2.7 Reserved0 (Offset 0x21, 7 bytes)

Reserved. Must be zeros.

#### 12.2.8 HashHi / HashLo (Offset 0x28, 16 bytes)

XXH3-128 hash of the uncompressed bulk data.

---

## 13. UUIDs and Type Identifiers

SnPAK uses 128-bit UUIDs (Universally Unique Identifiers) as defined in RFC 4122 for identifying assets and types.

### 13.1 UUID Structure

```c
struct Uuid {
    uint8_t Bytes[16];
};
```

The 16 bytes are stored in their binary form (not as a string).

### 13.2 UUID Aliases

| Alias | Purpose |
|-------|---------|
| `AssetId` | Uniquely identifies an asset instance |
| `TypeId` | Identifies a type (asset kind, payload type) |

### 13.3 UUID Generation

**Version 4 (Random):**
Used for generating new asset IDs. Based on random numbers.

**Version 5 (Name-Based):**
Used for generating deterministic type IDs from a namespace and name string.

```c
TypeId textureTypeId = Uuid::GenerateV5(
    namespaceUuid,
    "SnAPI.AssetPipeline.Texture2D"
);
```

### 13.4 Null UUID

A UUID with all bytes set to `0` is considered null/invalid:

```c
bool IsNull() const {
    for (int i = 0; i < 16; ++i) {
        if (Bytes[i] != 0) return false;
    }
    return true;
}
```

### 13.5 String Representation

For display purposes, UUIDs use the standard format:
```
xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```
Example: `550e8400-e29b-41d4-a716-446655440000`

---

## 14. Compression

### 14.1 Supported Algorithms

| Algorithm | ID | Description | Use Case |
|-----------|----|-------------|----------|
| **None** | `0` | No compression | Pre-compressed data (JPEG, OGG) |
| **LZ4** | `1` | Fast compression/decompression | Real-time streaming |
| **Zstd** | `2` | High compression ratio | Distribution, storage |

### 14.2 LZ4 Compression

**Library:** lz4 (https://github.com/lz4/lz4)

**Compression:**
- Standard mode: `LZ4HC_CLEVEL_DEFAULT` (level 9)
- Maximum mode: `LZ4HC_CLEVEL_MAX` (level 12)

```c
int compressedSize = LZ4_compress_HC(
    sourceData,
    destBuffer,
    sourceSize,
    maxDestSize,
    LZ4HC_CLEVEL_DEFAULT  // or LZ4HC_CLEVEL_MAX
);
```

**Decompression:**
```c
int decompressedSize = LZ4_decompress_safe(
    compressedData,
    destBuffer,
    compressedSize,
    expectedUncompressedSize
);
```

### 14.3 Zstd Compression

**Library:** zstd (https://github.com/facebook/zstd)

**Compression:**
- Standard mode: `ZSTD_defaultCLevel()` (typically level 3)
- Maximum mode: `ZSTD_maxCLevel()` (typically level 22)

```c
size_t compressedSize = ZSTD_compress(
    destBuffer,
    destCapacity,
    sourceData,
    sourceSize,
    ZSTD_defaultCLevel()  // or ZSTD_maxCLevel()
);
```

**Decompression:**
```c
size_t decompressedSize = ZSTD_decompress(
    destBuffer,
    destCapacity,
    compressedData,
    compressedSize
);
```

### 14.4 Compression Selection Guidelines

| Data Type | Recommended | Rationale |
|-----------|-------------|-----------|
| Textures (uncompressed) | Zstd | Good ratio for pixel data |
| Mesh vertices | LZ4 | Fast loading for geometry |
| Audio (PCM) | Zstd | Good ratio for samples |
| Pre-compressed (JPEG, BC7) | None | Already compressed |
| Runtime-critical | LZ4 | Fastest decompression |

### 14.5 Per-Chunk Compression

Each chunk can use a different compression setting:
- Main payload might use Zstd for best ratio
- Streaming bulk data might use LZ4 for speed
- Pre-compressed mipmaps might use None

---

## 15. Data Integrity and Hashing

### 15.1 Hash Algorithm

SnPAK uses **XXH3-128** from the xxHash library for all integrity verification.

**Properties:**
- 128-bit output (stored as two 64-bit values)
- Extremely fast (>10 GB/s on modern CPUs)
- High quality distribution
- Not cryptographically secure (not intended for security)

### 15.2 Hash Storage

All hashes are stored as two `uint64_t` fields:
- `HashHi`: High 64 bits (`XXH128_hash_t.high64`)
- `HashLo`: Low 64 bits (`XXH128_hash_t.low64`)

### 15.3 What is Hashed

| Location | Data Hashed |
|----------|-------------|
| Main Header (`IndexHash*`) | Entire index block bytes |
| String Table Header | String data only (not header, not offsets) |
| Index Header (`Hash*`) | Entry array + bulk entry array (not header) |
| Chunk Header (`Hash*`) | Uncompressed payload/bulk data |
| Index Entry (`PayloadHash*`) | Uncompressed main payload |
| Bulk Entry (`Hash*`) | Uncompressed bulk data |

### 15.4 Verification Process

```c
// Reading a chunk
std::vector<uint8_t> decompressed = Decompress(
    compressedData,
    chunk.SizeCompressed,
    chunk.SizeUncompressed,
    chunk.Compression
);

XXH128_hash_t hash = XXH3_128bits(
    decompressed.data(),
    decompressed.size()
);

if (hash.high64 != chunk.HashHi || hash.low64 != chunk.HashLo) {
    throw std::runtime_error("Data corruption detected");
}
```

### 15.5 Name Hashing

For fast name-based lookups, asset names are hashed with **XXH3-64**:

```c
uint64_t nameHash = XXH3_64bits(assetName.data(), assetName.length());
```

This enables O(1) lookup in a hash map, with string comparison only for hash collisions.

---

## 16. Append-Update Mode

Append-Update mode allows adding new assets to an existing pack without rewriting the entire file.

### 16.1 How It Works

1. **Open Existing Pack** for read/write
2. **Seek to End** of file
3. **Write New String Table** for new assets
4. **Write New Chunks** for new assets
5. **Write New Index Block** with reference to previous index
6. **Update Header** with new index location and flags

### 16.2 Index Chain

After multiple append operations, the file contains a chain of indices:

```
┌─────────────────────────────────────────────────────────────┐
│ Header                                                       │
│   IndexOffset ──────────────────────────────────────────┐    │
│   PreviousIndexOffset ──────────────────────────────┐   │    │
│   Flags: HasTrailingIndex                           │   │    │
├─────────────────────────────────────────────────────│───│────┤
│ Original String Table                               │   │    │
├─────────────────────────────────────────────────────│───│────┤
│ Original Chunks                                     │   │    │
├─────────────────────────────────────────────────────│───│────┤
│ Original Index ←────────────────────────────────────┘   │    │
│   PreviousIndexOffset: 0                                │    │
├─────────────────────────────────────────────────────────│────┤
│ Append 1: String Table                                  │    │
├─────────────────────────────────────────────────────────│────┤
│ Append 1: Chunks                                        │    │
├─────────────────────────────────────────────────────────│────┤
│ Current Index ←─────────────────────────────────────────┘    │
│   PreviousIndexOffset → Original Index                       │
└─────────────────────────────────────────────────────────────┘
```

### 16.3 Reading Appended Packs

Readers typically only need the current index. However, they can traverse the chain to find all assets:

```c
void LoadAllAssets(SnPakHeaderV1& header) {
    LoadIndex(header.IndexOffset, header.IndexSize);

    if (header.Flags & SnPakFlag_HasTrailingIndex) {
        // Optionally load previous indices
        LoadIndex(header.PreviousIndexOffset, header.PreviousIndexSize);
    }
}
```

### 16.4 Compacting Appended Packs

After multiple appends, the file may contain:
- Duplicate string tables
- Multiple index blocks
- Orphaned data (if assets were replaced)

To compact, create a new pack with only the current assets:

```c
void Compact(const std::string& path) {
    AssetPackReader reader;
    reader.Open(path);

    AssetPackWriter writer;
    for (uint32_t i = 0; i < reader.GetAssetCount(); ++i) {
        // Copy each asset to new pack
    }

    writer.Write(path + ".new");
    std::filesystem::rename(path + ".new", path);
}
```

---

## 17. Sanity Limits and Security

### 17.1 Allocation Limits

To protect against malformed or malicious files, readers enforce maximum limits:

| Limit | Value | Description |
|-------|-------|-------------|
| `kMaxStringCount` | 10,000,000 | Maximum strings in string table |
| `kMaxBlockSize` | 1,000,000,000 | Maximum size for any block (1 GB) |
| `kMaxEntryCount` | 10,000,000 | Maximum asset entries |
| `kMaxBulkEntryCount` | 100,000,000 | Maximum bulk entries |

### 17.2 Validation Checks

Readers MUST validate:

1. **Magic Numbers**: All block magic signatures
2. **Version Numbers**: Reject unsupported versions
3. **Endian Marker**: Verify byte order compatibility
4. **Size Limits**: All sizes against sanity limits
5. **Offset Bounds**: All offsets within file size
6. **Hash Verification**: All hashes after decompression

### 17.3 Security Considerations

**No Encryption:** SnPAK provides no confidentiality. Assets are visible to anyone with file access.

**No Authentication:** Hash verification detects corruption but not malicious modification.

**Path Traversal:** Asset names may contain paths. Consumers should validate names before using them in file operations.

**Decompression Bombs:** Size limits prevent zip-bomb style attacks.

---

## 18. Complete File Layout Diagram

```
Offset    Size      Content
──────────────────────────────────────────────────────────────
0x0000    180       SnPakHeaderV1
                    ├─ Magic[8]: "SNPAK\0\0\0"
                    ├─ Version: 1
                    ├─ HeaderSize: 180
                    ├─ EndianMarker: 0x01020304
                    ├─ FileSize
                    ├─ IndexOffset ──────────────────────────┐
                    ├─ IndexSize                             │
                    ├─ StringTableOffset ────┐               │
                    ├─ StringTableSize       │               │
                    ├─ TypeTableOffset: 0    │               │
                    ├─ TypeTableSize: 0      │               │
                    ├─ IndexHashHi           │               │
                    ├─ IndexHashLo           │               │
                    ├─ Flags                 │               │
                    ├─ Reserved0             │               │
                    ├─ PreviousIndexOffset   │               │
                    ├─ PreviousIndexSize     │               │
                    └─ Reserved[64]          │               │
0x00B4    var       String Table Block ◄─────┘               │
                    ├─ SnPakStrBlockHeaderV1 (40 bytes)      │
                    │   ├─ Magic: "STRS"                     │
                    │   ├─ Version: 1                        │
                    │   ├─ BlockSize                         │
                    │   ├─ StringCount                       │
                    │   ├─ Reserved0                         │
                    │   ├─ HashHi                            │
                    │   └─ HashLo                            │
                    ├─ Offsets[StringCount] (4 bytes each)   │
                    └─ String Data (null-terminated UTF-8)   │
????      var       Payload Chunks                           │
                    ├─ SnPakChunkHeaderV1 (80 bytes)         │
                    │   ├─ Magic: "CHNK"                     │
                    │   ├─ Version: 1                        │
                    │   ├─ AssetId[16]                       │
                    │   ├─ PayloadType[16]                   │
                    │   ├─ SchemaVersion                     │
                    │   ├─ Compression                       │
                    │   ├─ ChunkKind: 0 (MainPayload)        │
                    │   ├─ Reserved0                         │
                    │   ├─ SizeCompressed                    │
                    │   ├─ SizeUncompressed                  │
                    │   ├─ HashHi                            │
                    │   └─ HashLo                            │
                    └─ Compressed Data                       │
????      var       Bulk Chunks                              │
                    ├─ SnPakChunkHeaderV1 (80 bytes)         │
                    │   ├─ Magic: "CHNK"                     │
                    │   ├─ ...                               │
                    │   └─ ChunkKind: 1 (Bulk)               │
                    └─ Compressed Data                       │
????      var       Index Block ◄────────────────────────────┘
                    ├─ SnPakIndexHeaderV1 (88 bytes)
                    │   ├─ Magic: "INDX"
                    │   ├─ Version: 1
                    │   ├─ BlockSize
                    │   ├─ EntryCount
                    │   ├─ BulkEntryCount
                    │   ├─ HashHi
                    │   ├─ HashLo
                    │   ├─ PreviousIndexOffset
                    │   ├─ PreviousIndexSize
                    │   └─ Reserved[32]
                    ├─ SnPakIndexEntryV1[EntryCount] (128 bytes each)
                    │   ├─ AssetId[16]
                    │   ├─ AssetKind[16]
                    │   ├─ CookedPayloadType[16]
                    │   ├─ CookedSchemaVersion
                    │   ├─ NameStringId
                    │   ├─ NameHash64
                    │   ├─ VariantStringId
                    │   ├─ VariantHash64
                    │   ├─ PayloadChunkOffset
                    │   ├─ PayloadChunkSizeCompressed
                    │   ├─ PayloadChunkSizeUncompressed
                    │   ├─ Compression
                    │   ├─ Flags
                    │   ├─ Reserved0
                    │   ├─ BulkFirstIndex
                    │   ├─ BulkCount
                    │   ├─ PayloadHashHi
                    │   └─ PayloadHashLo
                    └─ SnPakBulkEntryV1[BulkEntryCount] (56 bytes each)
                        ├─ Semantic[4]
                        ├─ SubIndex
                        ├─ ChunkOffset
                        ├─ SizeCompressed
                        ├─ SizeUncompressed
                        ├─ Compression
                        ├─ Reserved0[7]
                        ├─ HashHi
                        └─ HashLo
EOF
```

---

## 19. Reading the Format

### 19.1 Complete C++ Reading Example

```cpp
#include <fstream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <lz4.h>
#include <zstd.h>

// ─────────────────────────────────────────────────────────────
// STEP 1: Define all structures (must be byte-packed)
// ─────────────────────────────────────────────────────────────

#pragma pack(push, 1)

constexpr uint8_t kSnPakMagic[8] = {'S', 'N', 'P', 'A', 'K', 0, 0, 0};
constexpr uint8_t kChunkMagic[4] = {'C', 'H', 'N', 'K'};
constexpr uint8_t kIndexMagic[4] = {'I', 'N', 'D', 'X'};
constexpr uint8_t kStringMagic[4] = {'S', 'T', 'R', 'S'};
constexpr uint32_t kEndianMarker = 0x01020304;
constexpr uint32_t kSnPakVersion = 1;

enum class ESnPakCompression : uint8_t {
    None = 0,
    LZ4 = 1,
    Zstd = 2,
};

struct SnPakHeaderV1 {
    uint8_t Magic[8];
    uint32_t Version;
    uint32_t HeaderSize;
    uint32_t EndianMarker;
    uint64_t FileSize;
    uint64_t IndexOffset;
    uint64_t IndexSize;
    uint64_t StringTableOffset;
    uint64_t StringTableSize;
    uint64_t TypeTableOffset;
    uint64_t TypeTableSize;
    uint64_t IndexHashHi;
    uint64_t IndexHashLo;
    uint32_t Flags;
    uint32_t Reserved0;
    uint64_t PreviousIndexOffset;
    uint64_t PreviousIndexSize;
    uint8_t Reserved[64];
};

struct SnPakStrBlockHeaderV1 {
    uint8_t Magic[4];
    uint32_t Version;
    uint64_t BlockSize;
    uint32_t StringCount;
    uint32_t Reserved0;
    uint64_t HashHi;
    uint64_t HashLo;
};

struct SnPakIndexHeaderV1 {
    uint8_t Magic[4];
    uint32_t Version;
    uint64_t BlockSize;
    uint32_t EntryCount;
    uint32_t BulkEntryCount;
    uint64_t HashHi;
    uint64_t HashLo;
    uint64_t PreviousIndexOffset;
    uint64_t PreviousIndexSize;
    uint8_t Reserved[32];
};

struct SnPakIndexEntryV1 {
    uint8_t AssetId[16];
    uint8_t AssetKind[16];
    uint8_t CookedPayloadType[16];
    uint32_t CookedSchemaVersion;
    uint32_t NameStringId;
    uint64_t NameHash64;
    uint32_t VariantStringId;
    uint64_t VariantHash64;
    uint64_t PayloadChunkOffset;
    uint64_t PayloadChunkSizeCompressed;
    uint64_t PayloadChunkSizeUncompressed;
    uint8_t Compression;
    uint8_t Flags;
    uint16_t Reserved0;
    uint32_t BulkFirstIndex;
    uint32_t BulkCount;
    uint64_t PayloadHashHi;
    uint64_t PayloadHashLo;
};

struct SnPakBulkEntryV1 {
    uint8_t Semantic[4];
    uint32_t SubIndex;
    uint64_t ChunkOffset;
    uint64_t SizeCompressed;
    uint64_t SizeUncompressed;
    uint8_t Compression;
    uint8_t Reserved0[7];
    uint64_t HashHi;
    uint64_t HashLo;
};

struct SnPakChunkHeaderV1 {
    uint8_t Magic[4];
    uint32_t Version;
    uint8_t AssetId[16];
    uint8_t PayloadType[16];
    uint32_t SchemaVersion;
    uint8_t Compression;
    uint8_t ChunkKind;
    uint16_t Reserved0;
    uint64_t SizeCompressed;
    uint64_t SizeUncompressed;
    uint64_t HashHi;
    uint64_t HashLo;
};

#pragma pack(pop)

// ─────────────────────────────────────────────────────────────
// STEP 2: Implement decompression
// ─────────────────────────────────────────────────────────────

std::vector<uint8_t> Decompress(
    const uint8_t* data,
    size_t compressedSize,
    size_t uncompressedSize,
    ESnPakCompression mode
) {
    if (mode == ESnPakCompression::None) {
        return std::vector<uint8_t>(data, data + compressedSize);
    }

    std::vector<uint8_t> result(uncompressedSize);

    if (mode == ESnPakCompression::LZ4) {
        int decompressedSize = LZ4_decompress_safe(
            reinterpret_cast<const char*>(data),
            reinterpret_cast<char*>(result.data()),
            static_cast<int>(compressedSize),
            static_cast<int>(uncompressedSize)
        );
        if (decompressedSize < 0 ||
            static_cast<size_t>(decompressedSize) != uncompressedSize) {
            throw std::runtime_error("LZ4 decompression failed");
        }
    }
    else if (mode == ESnPakCompression::Zstd) {
        size_t decompressedSize = ZSTD_decompress(
            result.data(),
            result.size(),
            data,
            compressedSize
        );
        if (ZSTD_isError(decompressedSize) ||
            decompressedSize != uncompressedSize) {
            throw std::runtime_error("Zstd decompression failed");
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────
// STEP 3: Implement the reader class
// ─────────────────────────────────────────────────────────────

class SnPakReader {
public:
    bool Open(const std::string& path) {
        file_.open(path, std::ios::binary);
        if (!file_.is_open()) {
            return false;
        }

        // Read and validate header
        file_.read(reinterpret_cast<char*>(&header_), sizeof(header_));

        if (std::memcmp(header_.Magic, kSnPakMagic, 8) != 0) {
            return false;  // Invalid magic
        }
        if (header_.Version != kSnPakVersion) {
            return false;  // Unsupported version
        }
        if (header_.EndianMarker != kEndianMarker) {
            return false;  // Wrong endianness
        }

        // Read string table
        if (!ReadStringTable()) {
            return false;
        }

        // Read index
        if (!ReadIndex()) {
            return false;
        }

        return true;
    }

    uint32_t GetAssetCount() const {
        return static_cast<uint32_t>(indexEntries_.size());
    }

    std::string GetAssetName(uint32_t index) const {
        if (index >= indexEntries_.size()) return "";
        uint32_t stringId = indexEntries_[index].NameStringId;
        if (stringId >= stringTable_.size()) return "";
        return stringTable_[stringId];
    }

    std::vector<uint8_t> LoadAssetPayload(uint32_t index) {
        if (index >= indexEntries_.size()) {
            throw std::runtime_error("Index out of range");
        }

        const auto& entry = indexEntries_[index];

        // Seek to chunk
        file_.seekg(entry.PayloadChunkOffset, std::ios::beg);

        // Read chunk header
        SnPakChunkHeaderV1 chunkHeader;
        file_.read(reinterpret_cast<char*>(&chunkHeader), sizeof(chunkHeader));

        if (std::memcmp(chunkHeader.Magic, kChunkMagic, 4) != 0) {
            throw std::runtime_error("Invalid chunk magic");
        }

        // Read compressed data
        std::vector<uint8_t> compressedData(chunkHeader.SizeCompressed);
        file_.read(
            reinterpret_cast<char*>(compressedData.data()),
            compressedData.size()
        );

        // Decompress
        auto decompressed = Decompress(
            compressedData.data(),
            chunkHeader.SizeCompressed,
            chunkHeader.SizeUncompressed,
            static_cast<ESnPakCompression>(chunkHeader.Compression)
        );

        // Verify hash
        XXH128_hash_t hash = XXH3_128bits(
            decompressed.data(),
            decompressed.size()
        );
        if (hash.high64 != chunkHeader.HashHi ||
            hash.low64 != chunkHeader.HashLo) {
            throw std::runtime_error("Hash mismatch - data corrupted");
        }

        return decompressed;
    }

private:
    bool ReadStringTable() {
        file_.seekg(header_.StringTableOffset, std::ios::beg);

        SnPakStrBlockHeaderV1 strHeader;
        file_.read(reinterpret_cast<char*>(&strHeader), sizeof(strHeader));

        if (std::memcmp(strHeader.Magic, kStringMagic, 4) != 0) {
            return false;
        }

        // Read offsets
        std::vector<uint32_t> offsets(strHeader.StringCount);
        file_.read(
            reinterpret_cast<char*>(offsets.data()),
            offsets.size() * sizeof(uint32_t)
        );

        // Read string data
        size_t offsetsSize = offsets.size() * sizeof(uint32_t);
        size_t stringDataSize = strHeader.BlockSize - sizeof(strHeader) - offsetsSize;
        std::vector<uint8_t> stringData(stringDataSize);
        file_.read(reinterpret_cast<char*>(stringData.data()), stringData.size());

        // Verify hash
        XXH128_hash_t hash = XXH3_128bits(stringData.data(), stringData.size());
        if (hash.high64 != strHeader.HashHi || hash.low64 != strHeader.HashLo) {
            return false;  // String data corrupted
        }

        // Parse strings
        stringTable_.reserve(strHeader.StringCount);
        for (uint32_t i = 0; i < strHeader.StringCount; ++i) {
            const char* str = reinterpret_cast<const char*>(
                stringData.data() + offsets[i]
            );
            stringTable_.emplace_back(str);
        }

        return true;
    }

    bool ReadIndex() {
        file_.seekg(header_.IndexOffset, std::ios::beg);

        SnPakIndexHeaderV1 idxHeader;
        file_.read(reinterpret_cast<char*>(&idxHeader), sizeof(idxHeader));

        if (std::memcmp(idxHeader.Magic, kIndexMagic, 4) != 0) {
            return false;
        }

        // Read asset entries
        indexEntries_.resize(idxHeader.EntryCount);
        if (idxHeader.EntryCount > 0) {
            file_.read(
                reinterpret_cast<char*>(indexEntries_.data()),
                indexEntries_.size() * sizeof(SnPakIndexEntryV1)
            );
        }

        // Read bulk entries
        bulkEntries_.resize(idxHeader.BulkEntryCount);
        if (idxHeader.BulkEntryCount > 0) {
            file_.read(
                reinterpret_cast<char*>(bulkEntries_.data()),
                bulkEntries_.size() * sizeof(SnPakBulkEntryV1)
            );
        }

        // Verify hash
        size_t dataSize =
            indexEntries_.size() * sizeof(SnPakIndexEntryV1) +
            bulkEntries_.size() * sizeof(SnPakBulkEntryV1);
        std::vector<uint8_t> buffer(dataSize);

        if (!indexEntries_.empty()) {
            std::memcpy(
                buffer.data(),
                indexEntries_.data(),
                indexEntries_.size() * sizeof(SnPakIndexEntryV1)
            );
        }
        if (!bulkEntries_.empty()) {
            std::memcpy(
                buffer.data() + indexEntries_.size() * sizeof(SnPakIndexEntryV1),
                bulkEntries_.data(),
                bulkEntries_.size() * sizeof(SnPakBulkEntryV1)
            );
        }

        XXH128_hash_t hash = XXH3_128bits(buffer.data(), buffer.size());
        if (hash.high64 != idxHeader.HashHi || hash.low64 != idxHeader.HashLo) {
            return false;  // Index corrupted
        }

        return true;
    }

    std::ifstream file_;
    SnPakHeaderV1 header_;
    std::vector<std::string> stringTable_;
    std::vector<SnPakIndexEntryV1> indexEntries_;
    std::vector<SnPakBulkEntryV1> bulkEntries_;
};

// ─────────────────────────────────────────────────────────────
// STEP 4: Example usage
// ─────────────────────────────────────────────────────────────

int main() {
    SnPakReader reader;

    if (!reader.Open("assets.snpak")) {
        std::cerr << "Failed to open pack\n";
        return 1;
    }

    std::cout << "Assets in pack: " << reader.GetAssetCount() << "\n";

    for (uint32_t i = 0; i < reader.GetAssetCount(); ++i) {
        std::cout << "  " << i << ": " << reader.GetAssetName(i) << "\n";
    }

    // Load first asset
    if (reader.GetAssetCount() > 0) {
        auto payload = reader.LoadAssetPayload(0);
        std::cout << "Loaded " << payload.size() << " bytes\n";
    }

    return 0;
}
```

### 19.2 Reading Step-by-Step Summary

1. **Open file** in binary mode
2. **Read header** (180 bytes from offset 0)
3. **Validate header**: magic, version, endian marker
4. **Read string table** from `StringTableOffset`
5. **Verify string table hash**
6. **Parse strings** into memory
7. **Read index** from `IndexOffset`
8. **Verify index hash**
9. **Build lookup structures** (optional: hash maps for fast lookup)
10. **Load assets on demand**: seek to chunk, read, decompress, verify

---

## 20. Writing the Format

### 20.1 Complete C++ Writing Example

```cpp
#include <fstream>
#include <vector>
#include <cstring>
#include <filesystem>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <lz4hc.h>
#include <zstd.h>

// Include structure definitions from reading example...
// (SnPakHeaderV1, SnPakChunkHeaderV1, etc.)

// ─────────────────────────────────────────────────────────────
// STEP 1: Implement compression
// ─────────────────────────────────────────────────────────────

std::vector<uint8_t> Compress(
    const uint8_t* data,
    size_t size,
    ESnPakCompression mode
) {
    if (mode == ESnPakCompression::None || size == 0) {
        return std::vector<uint8_t>(data, data + size);
    }

    std::vector<uint8_t> result;

    if (mode == ESnPakCompression::LZ4) {
        int maxSize = LZ4_compressBound(static_cast<int>(size));
        result.resize(maxSize);

        int compressedSize = LZ4_compress_HC(
            reinterpret_cast<const char*>(data),
            reinterpret_cast<char*>(result.data()),
            static_cast<int>(size),
            maxSize,
            LZ4HC_CLEVEL_DEFAULT
        );

        if (compressedSize <= 0) {
            throw std::runtime_error("LZ4 compression failed");
        }
        result.resize(compressedSize);
    }
    else if (mode == ESnPakCompression::Zstd) {
        size_t maxSize = ZSTD_compressBound(size);
        result.resize(maxSize);

        size_t compressedSize = ZSTD_compress(
            result.data(),
            result.size(),
            data,
            size,
            ZSTD_defaultCLevel()
        );

        if (ZSTD_isError(compressedSize)) {
            throw std::runtime_error("Zstd compression failed");
        }
        result.resize(compressedSize);
    }

    return result;
}

// ─────────────────────────────────────────────────────────────
// STEP 2: Define helper structures for writing
// ─────────────────────────────────────────────────────────────

struct AssetToWrite {
    uint8_t AssetId[16];
    uint8_t AssetKind[16];
    uint8_t PayloadType[16];
    uint32_t SchemaVersion;
    std::string Name;
    std::string VariantKey;
    std::vector<uint8_t> PayloadData;

    struct BulkData {
        uint32_t Semantic;
        uint32_t SubIndex;
        bool Compress;
        std::vector<uint8_t> Data;
    };
    std::vector<BulkData> Bulk;
};

// ─────────────────────────────────────────────────────────────
// STEP 3: Implement the writer class
// ─────────────────────────────────────────────────────────────

class SnPakWriter {
public:
    void SetCompression(ESnPakCompression mode) {
        compression_ = mode;
    }

    void AddAsset(AssetToWrite asset) {
        assets_.push_back(std::move(asset));
    }

    bool Write(const std::string& outputPath) {
        std::string tempPath = outputPath + ".tmp";

        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }

        // Build string table
        std::vector<std::string> strings;
        std::unordered_map<std::string, uint32_t> stringToId;

        auto addString = [&](const std::string& s) -> uint32_t {
            auto it = stringToId.find(s);
            if (it != stringToId.end()) return it->second;
            uint32_t id = static_cast<uint32_t>(strings.size());
            strings.push_back(s);
            stringToId[s] = id;
            return id;
        };

        for (const auto& asset : assets_) {
            addString(asset.Name);
            if (!asset.VariantKey.empty()) {
                addString(asset.VariantKey);
            }
        }

        // Write header placeholder
        SnPakHeaderV1 header = {};
        std::memcpy(header.Magic, kSnPakMagic, 8);
        header.Version = kSnPakVersion;
        header.HeaderSize = sizeof(SnPakHeaderV1);
        header.EndianMarker = kEndianMarker;

        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        uint64_t currentOffset = sizeof(header);

        // Write string table
        uint64_t stringTableOffset = currentOffset;
        auto stringTableData = BuildStringTable(strings);
        file.write(
            reinterpret_cast<const char*>(stringTableData.data()),
            stringTableData.size()
        );
        currentOffset += stringTableData.size();

        // Write chunks and build index entries
        std::vector<SnPakIndexEntryV1> indexEntries;
        std::vector<SnPakBulkEntryV1> bulkEntries;

        for (const auto& asset : assets_) {
            SnPakIndexEntryV1 entry = {};

            std::memcpy(entry.AssetId, asset.AssetId, 16);
            std::memcpy(entry.AssetKind, asset.AssetKind, 16);
            std::memcpy(entry.CookedPayloadType, asset.PayloadType, 16);
            entry.CookedSchemaVersion = asset.SchemaVersion;

            entry.NameStringId = addString(asset.Name);
            entry.NameHash64 = XXH3_64bits(
                asset.Name.data(),
                asset.Name.size()
            );

            if (!asset.VariantKey.empty()) {
                entry.VariantStringId = addString(asset.VariantKey);
                entry.VariantHash64 = XXH3_64bits(
                    asset.VariantKey.data(),
                    asset.VariantKey.size()
                );
            } else {
                entry.VariantStringId = 0xFFFFFFFF;
                entry.VariantHash64 = 0;
            }

            // Write main payload chunk
            auto compressedPayload = Compress(
                asset.PayloadData.data(),
                asset.PayloadData.size(),
                compression_
            );

            XXH128_hash_t payloadHash = XXH3_128bits(
                asset.PayloadData.data(),
                asset.PayloadData.size()
            );

            SnPakChunkHeaderV1 chunkHeader = {};
            std::memcpy(chunkHeader.Magic, kChunkMagic, 4);
            chunkHeader.Version = 1;
            std::memcpy(chunkHeader.AssetId, asset.AssetId, 16);
            std::memcpy(chunkHeader.PayloadType, asset.PayloadType, 16);
            chunkHeader.SchemaVersion = asset.SchemaVersion;
            chunkHeader.Compression = static_cast<uint8_t>(compression_);
            chunkHeader.ChunkKind = 0;  // MainPayload
            chunkHeader.SizeCompressed = compressedPayload.size();
            chunkHeader.SizeUncompressed = asset.PayloadData.size();
            chunkHeader.HashHi = payloadHash.high64;
            chunkHeader.HashLo = payloadHash.low64;

            entry.PayloadChunkOffset = currentOffset;
            entry.PayloadChunkSizeCompressed =
                sizeof(chunkHeader) + compressedPayload.size();
            entry.PayloadChunkSizeUncompressed = asset.PayloadData.size();
            entry.Compression = static_cast<uint8_t>(compression_);
            entry.PayloadHashHi = payloadHash.high64;
            entry.PayloadHashLo = payloadHash.low64;

            file.write(
                reinterpret_cast<const char*>(&chunkHeader),
                sizeof(chunkHeader)
            );
            file.write(
                reinterpret_cast<const char*>(compressedPayload.data()),
                compressedPayload.size()
            );
            currentOffset += sizeof(chunkHeader) + compressedPayload.size();

            // Write bulk chunks
            if (!asset.Bulk.empty()) {
                entry.Flags = 0x01;  // HasBulk
                entry.BulkFirstIndex = static_cast<uint32_t>(bulkEntries.size());
                entry.BulkCount = static_cast<uint32_t>(asset.Bulk.size());

                for (const auto& bulk : asset.Bulk) {
                    auto bulkCompression = bulk.Compress
                        ? compression_
                        : ESnPakCompression::None;

                    auto compressedBulk = Compress(
                        bulk.Data.data(),
                        bulk.Data.size(),
                        bulkCompression
                    );

                    XXH128_hash_t bulkHash = XXH3_128bits(
                        bulk.Data.data(),
                        bulk.Data.size()
                    );

                    SnPakChunkHeaderV1 bulkChunkHeader = {};
                    std::memcpy(bulkChunkHeader.Magic, kChunkMagic, 4);
                    bulkChunkHeader.Version = 1;
                    std::memcpy(bulkChunkHeader.AssetId, asset.AssetId, 16);
                    std::memcpy(bulkChunkHeader.PayloadType, asset.PayloadType, 16);
                    bulkChunkHeader.SchemaVersion = 0;
                    bulkChunkHeader.Compression =
                        static_cast<uint8_t>(bulkCompression);
                    bulkChunkHeader.ChunkKind = 1;  // Bulk
                    bulkChunkHeader.SizeCompressed = compressedBulk.size();
                    bulkChunkHeader.SizeUncompressed = bulk.Data.size();
                    bulkChunkHeader.HashHi = bulkHash.high64;
                    bulkChunkHeader.HashLo = bulkHash.low64;

                    SnPakBulkEntryV1 bulkEntry = {};
                    std::memcpy(&bulkEntry.Semantic, &bulk.Semantic, 4);
                    bulkEntry.SubIndex = bulk.SubIndex;
                    bulkEntry.ChunkOffset = currentOffset;
                    bulkEntry.SizeCompressed =
                        sizeof(bulkChunkHeader) + compressedBulk.size();
                    bulkEntry.SizeUncompressed = bulk.Data.size();
                    bulkEntry.Compression = static_cast<uint8_t>(bulkCompression);
                    bulkEntry.HashHi = bulkHash.high64;
                    bulkEntry.HashLo = bulkHash.low64;

                    bulkEntries.push_back(bulkEntry);

                    file.write(
                        reinterpret_cast<const char*>(&bulkChunkHeader),
                        sizeof(bulkChunkHeader)
                    );
                    file.write(
                        reinterpret_cast<const char*>(compressedBulk.data()),
                        compressedBulk.size()
                    );
                    currentOffset += sizeof(bulkChunkHeader) + compressedBulk.size();
                }
            }

            indexEntries.push_back(entry);
        }

        // Write index block
        uint64_t indexOffset = currentOffset;
        auto indexData = BuildIndexBlock(indexEntries, bulkEntries);
        file.write(
            reinterpret_cast<const char*>(indexData.data()),
            indexData.size()
        );
        currentOffset += indexData.size();

        // Update header with final values
        header.FileSize = currentOffset;
        header.IndexOffset = indexOffset;
        header.IndexSize = indexData.size();
        header.StringTableOffset = stringTableOffset;
        header.StringTableSize = stringTableData.size();

        XXH128_hash_t indexHash = XXH3_128bits(
            indexData.data(),
            indexData.size()
        );
        header.IndexHashHi = indexHash.high64;
        header.IndexHashLo = indexHash.low64;

        // Rewrite header
        file.seekp(0, std::ios::beg);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.close();

        // Atomic rename
        try {
            std::filesystem::rename(tempPath, outputPath);
        } catch (...) {
            return false;
        }

        return true;
    }

private:
    std::vector<uint8_t> BuildStringTable(
        const std::vector<std::string>& strings
    ) {
        std::vector<uint32_t> offsets;
        size_t dataSize = 0;

        for (const auto& s : strings) {
            offsets.push_back(static_cast<uint32_t>(dataSize));
            dataSize += s.size() + 1;  // +1 for null terminator
        }

        std::vector<uint8_t> stringData;
        stringData.reserve(dataSize);
        for (const auto& s : strings) {
            stringData.insert(stringData.end(), s.begin(), s.end());
            stringData.push_back(0);
        }

        XXH128_hash_t hash = XXH3_128bits(stringData.data(), stringData.size());

        SnPakStrBlockHeaderV1 header = {};
        std::memcpy(header.Magic, kStringMagic, 4);
        header.Version = 1;
        header.StringCount = static_cast<uint32_t>(strings.size());
        header.BlockSize = sizeof(header) +
            offsets.size() * sizeof(uint32_t) +
            stringData.size();
        header.HashHi = hash.high64;
        header.HashLo = hash.low64;

        std::vector<uint8_t> result(header.BlockSize);
        std::memcpy(result.data(), &header, sizeof(header));
        std::memcpy(
            result.data() + sizeof(header),
            offsets.data(),
            offsets.size() * sizeof(uint32_t)
        );
        std::memcpy(
            result.data() + sizeof(header) + offsets.size() * sizeof(uint32_t),
            stringData.data(),
            stringData.size()
        );

        return result;
    }

    std::vector<uint8_t> BuildIndexBlock(
        const std::vector<SnPakIndexEntryV1>& entries,
        const std::vector<SnPakBulkEntryV1>& bulkEntries
    ) {
        size_t entriesSize = entries.size() * sizeof(SnPakIndexEntryV1);
        size_t bulkSize = bulkEntries.size() * sizeof(SnPakBulkEntryV1);

        SnPakIndexHeaderV1 header = {};
        std::memcpy(header.Magic, kIndexMagic, 4);
        header.Version = 1;
        header.EntryCount = static_cast<uint32_t>(entries.size());
        header.BulkEntryCount = static_cast<uint32_t>(bulkEntries.size());
        header.BlockSize = sizeof(header) + entriesSize + bulkSize;

        std::vector<uint8_t> result(header.BlockSize);
        std::memcpy(result.data(), &header, sizeof(header));

        if (!entries.empty()) {
            std::memcpy(
                result.data() + sizeof(header),
                entries.data(),
                entriesSize
            );
        }
        if (!bulkEntries.empty()) {
            std::memcpy(
                result.data() + sizeof(header) + entriesSize,
                bulkEntries.data(),
                bulkSize
            );
        }

        // Calculate and store hash
        XXH128_hash_t hash = XXH3_128bits(
            result.data() + sizeof(header),
            entriesSize + bulkSize
        );
        auto* headerPtr = reinterpret_cast<SnPakIndexHeaderV1*>(result.data());
        headerPtr->HashHi = hash.high64;
        headerPtr->HashLo = hash.low64;

        return result;
    }

    std::vector<AssetToWrite> assets_;
    ESnPakCompression compression_ = ESnPakCompression::Zstd;
};

// ─────────────────────────────────────────────────────────────
// STEP 4: Example usage
// ─────────────────────────────────────────────────────────────

int main() {
    SnPakWriter writer;
    writer.SetCompression(ESnPakCompression::Zstd);

    // Create a test asset
    AssetToWrite asset;

    // Generate a random UUID (simplified - use proper UUID generation)
    for (int i = 0; i < 16; ++i) asset.AssetId[i] = rand() % 256;
    for (int i = 0; i < 16; ++i) asset.AssetKind[i] = 0;
    for (int i = 0; i < 16; ++i) asset.PayloadType[i] = 0;

    asset.SchemaVersion = 1;
    asset.Name = "textures/test_texture";
    asset.VariantKey = "";  // No variant

    // Test payload data
    asset.PayloadData.resize(1024);
    for (size_t i = 0; i < asset.PayloadData.size(); ++i) {
        asset.PayloadData[i] = static_cast<uint8_t>(i % 256);
    }

    // Add a bulk chunk (e.g., mipmap)
    AssetToWrite::BulkData mip0;
    mip0.Semantic = 1;  // Level
    mip0.SubIndex = 0;  // Mip level 0
    mip0.Compress = true;
    mip0.Data.resize(256);
    asset.Bulk.push_back(mip0);

    writer.AddAsset(std::move(asset));

    if (writer.Write("test_output.snpak")) {
        std::cout << "Pack written successfully!\n";
    } else {
        std::cerr << "Failed to write pack\n";
    }

    return 0;
}
```

### 20.2 Writing Step-by-Step Summary

1. **Collect all assets** to be written
2. **Build string table** with deduplicated names and variants
3. **Write header placeholder** (will be updated at end)
4. **Write string table** at current position
5. **For each asset:**
   - Compress main payload
   - Calculate payload hash
   - Write chunk header + compressed data
   - For each bulk chunk:
     - Compress bulk data
     - Calculate bulk hash
     - Write chunk header + compressed data
   - Record index entry and bulk entries
6. **Write index block** with all entries
7. **Update header** with final offsets and sizes
8. **Rewrite header** at file start
9. **Atomic rename** from temp file to final path

---

## 21. Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026 | Initial release |

---

## Appendix A: Quick Reference Tables

### Structure Sizes

| Structure | Size (bytes) |
|-----------|--------------|
| SnPakHeaderV1 | 180 |
| SnPakStrBlockHeaderV1 | 40 |
| SnPakIndexHeaderV1 | 88 |
| SnPakIndexEntryV1 | 128 |
| SnPakBulkEntryV1 | 56 |
| SnPakChunkHeaderV1 | 80 |

### Magic Signatures

| Block | Magic (ASCII) | Magic (Hex) |
|-------|---------------|-------------|
| File | `SNPAK\0\0\0` | `53 4E 50 41 4B 00 00 00` |
| Chunk | `CHNK` | `43 48 4E 4B` |
| Index | `INDX` | `49 4E 44 58` |
| String Table | `STRS` | `53 54 52 53` |

### Compression IDs

| ID | Algorithm |
|----|-----------|
| 0 | None |
| 1 | LZ4 |
| 2 | Zstd |

### Bulk Semantics

| ID | Meaning |
|----|---------|
| 0 | Unknown |
| 1 | Level (Mip/LOD) |
| 2 | Aux (Auxiliary) |
| 0x10000+ | Custom |

---

## Appendix B: Hexdump Example

Example hexdump of a minimal SnPAK file header:

```
00000000  53 4E 50 41 4B 00 00 00  01 00 00 00 B4 00 00 00  |SNPAK...........|
00000010  04 03 02 01 00 10 00 00  00 00 00 00 00 0E 00 00  |................|
00000020  00 00 00 00 40 01 00 00  00 00 00 00 B4 00 00 00  |....@...........|
00000030  00 00 00 00 4C 00 00 00  00 00 00 00 00 00 00 00  |....L...........|
00000040  00 00 00 00 00 00 00 00  00 00 00 00 AB CD EF 01  |................|
00000050  23 45 67 89 9A 78 56 34  12 00 00 00 00 00 00 00  |#Eg..xV4........|
00000060  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00000070  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
...
000000B4  53 54 52 53 01 00 00 00  4C 00 00 00 00 00 00 00  |STRS....L.......|
                       ^           ^
                       |           +-- BlockSize
                       +-- Version

```

---

*End of SnPAK File Format Specification Version 1*
