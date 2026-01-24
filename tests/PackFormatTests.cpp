#include <catch2/catch_test_macros.hpp>

#include "Pack/SnPakFormat.h"
#include "Uuid.h"
#include "TypedPayload.h"
#include "IAssetCooker.h"

#include <cstring>

using namespace SnAPI::AssetPipeline;
using namespace SnAPI::AssetPipeline::Pack;

TEST_CASE("SnPakHeaderV1 size is correct", "[pack]")
{
    REQUIRE(sizeof(SnPakHeaderV1) == 180);
}

TEST_CASE("SnPakStrBlockHeaderV1 size is correct", "[pack]")
{
    REQUIRE(sizeof(SnPakStrBlockHeaderV1) == 40);
}

TEST_CASE("SnPakIndexHeaderV1 size is correct", "[pack]")
{
    REQUIRE(sizeof(SnPakIndexHeaderV1) == 88);
}

TEST_CASE("SnPakIndexEntryV1 size is correct", "[pack]")
{
    REQUIRE(sizeof(SnPakIndexEntryV1) == 128);
}

TEST_CASE("SnPakBulkEntryV1 size is correct", "[pack]")
{
    REQUIRE(sizeof(SnPakBulkEntryV1) == 56);
}

TEST_CASE("SnPakChunkHeaderV1 size is correct", "[pack]")
{
    REQUIRE(sizeof(SnPakChunkHeaderV1) == 80);
}

TEST_CASE("SnPakHeaderV1 magic is correct", "[pack]")
{
    SnPakHeaderV1 Header = {};
    std::memcpy(Header.Magic, kSnPakMagic, 8);

    REQUIRE(Header.Magic[0] == 'S');
    REQUIRE(Header.Magic[1] == 'N');
    REQUIRE(Header.Magic[2] == 'P');
    REQUIRE(Header.Magic[3] == 'A');
    REQUIRE(Header.Magic[4] == 'K');
    REQUIRE(Header.Magic[5] == 0);
    REQUIRE(Header.Magic[6] == 0);
    REQUIRE(Header.Magic[7] == 0);
}

TEST_CASE("Pack endian marker", "[pack]")
{
    REQUIRE(kEndianMarker == 0x01020304);

    // This should be the same on all platforms (we store little-endian)
    uint8_t Bytes[4];
    uint32_t Marker = kEndianMarker;
    std::memcpy(Bytes, &Marker, 4);

    // On little-endian: 04 03 02 01
    // On big-endian: 01 02 03 04
    // This test verifies the value is consistent
    REQUIRE((Bytes[0] == 0x04 || Bytes[0] == 0x01));
}

TEST_CASE("ESnPakCompression values", "[pack]")
{
    REQUIRE(static_cast<uint8_t>(ESnPakCompression::None) == 0);
    REQUIRE(static_cast<uint8_t>(ESnPakCompression::LZ4) == 1);
    REQUIRE(static_cast<uint8_t>(ESnPakCompression::Zstd) == 2);
}

TEST_CASE("CopyUuid helper", "[pack]")
{
    Uuid Source(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00);

    uint8_t Dest[16] = {};
    CopyUuid(Dest, Source.Bytes);

    for (int i = 0; i < 16; ++i)
    {
        REQUIRE(Dest[i] == Source.Bytes[i]);
    }
}

TEST_CASE("CompareUuid helper", "[pack]")
{
    uint8_t A[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t B[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t C[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    REQUIRE(CompareUuid(A, B));
    REQUIRE_FALSE(CompareUuid(A, C));
}

TEST_CASE("SnPakIndexEntryV1 flags", "[pack]")
{
    SnPakIndexEntryV1 Entry = {};

    REQUIRE(Entry.Flags == 0);
    REQUIRE((Entry.Flags & IndexEntryFlag_HasBulk) == 0);

    Entry.Flags |= IndexEntryFlag_HasBulk;
    REQUIRE((Entry.Flags & IndexEntryFlag_HasBulk) != 0);
}

TEST_CASE("SnPakHeaderV1 initialization", "[pack]")
{
    SnPakHeaderV1 Header = {};
    std::memcpy(Header.Magic, kSnPakMagic, 8);
    Header.Version = kSnPakVersion;
    Header.HeaderSize = sizeof(SnPakHeaderV1);
    Header.EndianMarker = kEndianMarker;

    REQUIRE(Header.Version == 1);
    REQUIRE(Header.HeaderSize == 180);
    REQUIRE(Header.IndexOffset == 0);
    REQUIRE(Header.IndexSize == 0);
}

TEST_CASE("Variant string ID sentinel", "[pack]")
{
    SnPakIndexEntryV1 Entry = {};
    Entry.VariantStringId = 0xFFFFFFFF;

    REQUIRE(Entry.VariantStringId == 0xFFFFFFFF);
}

TEST_CASE("EBulkSemantic values", "[pack]")
{
    REQUIRE(static_cast<uint32_t>(EBulkSemantic::Unknown) == 0);
    REQUIRE(static_cast<uint32_t>(EBulkSemantic::Reserved_Level) == 1);
    REQUIRE(static_cast<uint32_t>(EBulkSemantic::Reserved_Aux) == 2);
}
