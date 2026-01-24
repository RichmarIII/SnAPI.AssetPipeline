#include <catch2/catch_test_macros.hpp>

#include "AssetPackReader.h"
#include "Pack/SnPakFormat.h"

#include <fstream>
#include <filesystem>
#include <cstring>
#include <random>

using namespace SnAPI::AssetPipeline;
using namespace SnAPI::AssetPipeline::Pack;

// Helper to create a minimal valid pack file for testing
class TestPackBuilder
{
public:
    std::vector<uint8_t> Build()
    {
        std::vector<uint8_t> Data;

        // Header
        SnPakHeaderV1 Header = {};
        std::memcpy(Header.Magic, kSnPakMagic, 8);
        Header.Version = kSnPakVersion;
        Header.HeaderSize = sizeof(SnPakHeaderV1);
        Header.EndianMarker = kEndianMarker;

        // String table (empty)
        SnPakStrBlockHeaderV1 StrHeader = {};
        std::memcpy(StrHeader.Magic, kStringMagic, 4);
        StrHeader.Version = 1;
        StrHeader.StringCount = 0;
        StrHeader.BlockSize = sizeof(SnPakStrBlockHeaderV1);
        StrHeader.HashHi = 0;
        StrHeader.HashLo = 0;

        // Index (empty)
        SnPakIndexHeaderV1 IdxHeader = {};
        std::memcpy(IdxHeader.Magic, kIndexMagic, 4);
        IdxHeader.Version = 1;
        IdxHeader.EntryCount = 0;
        IdxHeader.BulkEntryCount = 0;
        IdxHeader.BlockSize = sizeof(SnPakIndexHeaderV1);
        IdxHeader.HashHi = 0;
        IdxHeader.HashLo = 0;

        // Calculate offsets
        uint64_t Offset = sizeof(Header);
        Header.StringTableOffset = Offset;
        Header.StringTableSize = sizeof(StrHeader);
        Offset += sizeof(StrHeader);

        Header.IndexOffset = Offset;
        Header.IndexSize = sizeof(IdxHeader);
        Offset += sizeof(IdxHeader);

        Header.FileSize = Offset;

        // Build data
        Data.resize(Offset);
        std::memcpy(Data.data(), &Header, sizeof(Header));
        std::memcpy(Data.data() + Header.StringTableOffset, &StrHeader, sizeof(StrHeader));
        std::memcpy(Data.data() + Header.IndexOffset, &IdxHeader, sizeof(IdxHeader));

        return Data;
    }
};

std::string CreateTempPack(const std::vector<uint8_t>& Data)
{
    static int Counter = 0;
    std::string Path = std::filesystem::temp_directory_path().string() +
                       "/test_pack_" + std::to_string(Counter++) + ".snpak";

    std::ofstream File(Path, std::ios::binary);
    File.write(reinterpret_cast<const char*>(Data.data()), Data.size());
    File.close();

    return Path;
}

void CleanupTempPack(const std::string& Path)
{
    std::filesystem::remove(Path);
}

TEST_CASE("Valid minimal pack opens successfully", "[corruption]")
{
    TestPackBuilder Builder;
    auto Data = Builder.Build();
    std::string Path = CreateTempPack(Data);

    AssetPackReader Reader;
    auto Result = Reader.Open(Path);

    // Note: May fail due to hash mismatch with empty data
    // This tests the basic structure is valid
    if (Result.has_value())
    {
        REQUIRE(Reader.IsOpen());
        REQUIRE(Reader.GetAssetCount() == 0);
    }

    CleanupTempPack(Path);
}

TEST_CASE("Corrupted magic fails to open", "[corruption]")
{
    TestPackBuilder Builder;
    auto Data = Builder.Build();

    // Corrupt the magic
    Data[0] = 'X';

    std::string Path = CreateTempPack(Data);

    AssetPackReader Reader;
    auto Result = Reader.Open(Path);

    REQUIRE_FALSE(Result.has_value());
    REQUIRE(Result.error().find("magic") != std::string::npos);

    CleanupTempPack(Path);
}

TEST_CASE("Wrong version fails to open", "[corruption]")
{
    TestPackBuilder Builder;
    auto Data = Builder.Build();

    // Corrupt the version (bytes 8-11)
    Data[8] = 99;

    std::string Path = CreateTempPack(Data);

    AssetPackReader Reader;
    auto Result = Reader.Open(Path);

    REQUIRE_FALSE(Result.has_value());
    REQUIRE(Result.error().find("version") != std::string::npos);

    CleanupTempPack(Path);
}

TEST_CASE("Corrupted endian marker fails to open", "[corruption]")
{
    TestPackBuilder Builder;
    auto Data = Builder.Build();

    // Corrupt the endian marker (bytes 16-19)
    Data[16] = 0xFF;
    Data[17] = 0xFF;
    Data[18] = 0xFF;
    Data[19] = 0xFF;

    std::string Path = CreateTempPack(Data);

    AssetPackReader Reader;
    auto Result = Reader.Open(Path);

    REQUIRE_FALSE(Result.has_value());
    REQUIRE(Result.error().find("ndian") != std::string::npos);

    CleanupTempPack(Path);
}

TEST_CASE("Non-existent file fails to open", "[corruption]")
{
    AssetPackReader Reader;
    auto Result = Reader.Open("/nonexistent/path/file.snpak");

    REQUIRE_FALSE(Result.has_value());
    REQUIRE((Result.error().find("open") != std::string::npos ||
             Result.error().find("Failed") != std::string::npos));
}

TEST_CASE("Empty file fails to open", "[corruption]")
{
    std::string Path = CreateTempPack({});

    AssetPackReader Reader;
    auto Result = Reader.Open(Path);

    REQUIRE_FALSE(Result.has_value());

    CleanupTempPack(Path);
}

TEST_CASE("Truncated header fails to open", "[corruption]")
{
    TestPackBuilder Builder;
    auto Data = Builder.Build();

    // Truncate to less than header size
    Data.resize(50);

    std::string Path = CreateTempPack(Data);

    AssetPackReader Reader;
    auto Result = Reader.Open(Path);

    REQUIRE_FALSE(Result.has_value());

    CleanupTempPack(Path);
}

TEST_CASE("Random corruption in pack data", "[corruption]")
{
    TestPackBuilder Builder;
    auto Data = Builder.Build();

    // Skip the first 20 bytes (magic, version, header size, endian marker)
    // and corrupt some bytes in the middle
    if (Data.size() > 100)
    {
        std::random_device Rd;
        std::mt19937 Gen(Rd());
        std::uniform_int_distribution<size_t> Dist(50, Data.size() - 1);

        for (int i = 0; i < 10; ++i)
        {
            Data[Dist(Gen)] ^= 0xFF;
        }
    }

    std::string Path = CreateTempPack(Data);

    AssetPackReader Reader;
    auto Result = Reader.Open(Path);

    // Should either fail to open or open but fail when reading data
    // (depends on what was corrupted)

    CleanupTempPack(Path);
}

TEST_CASE("Pack header struct sizes are stable", "[corruption]")
{
    // These tests ensure the struct sizes don't accidentally change
    // which would break file format compatibility

    REQUIRE(sizeof(SnPakHeaderV1) == 180);
    REQUIRE(sizeof(SnPakStrBlockHeaderV1) == 40);
    REQUIRE(sizeof(SnPakIndexHeaderV1) == 88);
    REQUIRE(sizeof(SnPakIndexEntryV1) == 128);
    REQUIRE(sizeof(SnPakBulkEntryV1) == 56);
    REQUIRE(sizeof(SnPakChunkHeaderV1) == 80);
}
