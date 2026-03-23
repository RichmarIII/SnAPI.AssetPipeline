#include <catch2/catch_test_macros.hpp>

#include "AssetPackReader.h"
#include "AssetPackWriter.h"
#include "Uuid.h"

#include <chrono>
#include <filesystem>

using namespace SnAPI::AssetPipeline;

namespace
{
    std::filesystem::path MakeUniqueTempDir()
    {
        const auto Base = std::filesystem::temp_directory_path();
        const auto Stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto Dir = Base / ("snapi_assetpipeline_packdeps_" + std::to_string(Stamp));
        std::filesystem::create_directories(Dir);
        return Dir;
    }

    AssetPackEntry MakeAsset(const AssetId Id, const TypeId AssetKind, const TypeId PayloadType, const std::string& Name)
    {
        AssetPackEntry Entry{};
        Entry.Id = Id;
        Entry.AssetKind = AssetKind;
        Entry.Name = Name;
        Entry.Cooked.PayloadType = PayloadType;
        Entry.Cooked.SchemaVersion = 1;
        Entry.Cooked.Bytes = {1, 2, 3, 4};
        return Entry;
    }
}

TEST_CASE("Asset packs round-trip generic asset dependency metadata", "[pack]")
{
    const auto TempDir = MakeUniqueTempDir();
    const auto PackPath = TempDir / "Deps.snpak";

    const AssetId Asset = SNAPI_UUID(0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                     0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f);
    const AssetId RequiredDep = SNAPI_UUID(0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                           0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f);
    const AssetId OptionalDep = SNAPI_UUID(0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                                           0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f);
    const TypeId AssetKind = SNAPI_UUID(0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                                        0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf);
    const TypeId PayloadType = SNAPI_UUID(0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
                                          0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf);

    AssetPackWriter Writer;
    auto Entry = MakeAsset(Asset, AssetKind, PayloadType, "Levels/Main.level");
    Entry.AssetDependencies = {
        AssetDependencyRef{RequiredDep, "Materials/Hero.material", EAssetDependencyKind::Required},
        AssetDependencyRef{OptionalDep, "UI/Hud.prefab", EAssetDependencyKind::Optional},
    };
    Writer.AddAsset(std::move(Entry));
    REQUIRE(Writer.Write(PackPath.string()).has_value());

    AssetPackReader Reader;
    REQUIRE(Reader.Open(PackPath.string()).has_value());

    auto Info = Reader.FindAsset(Asset);
    REQUIRE(Info.has_value());
    REQUIRE(Info->AssetDependencies.size() == 2);
    REQUIRE(Info->AssetDependencies[0].Id == RequiredDep);
    REQUIRE(Info->AssetDependencies[0].LogicalName == "Materials/Hero.material");
    REQUIRE(Info->AssetDependencies[0].Kind == EAssetDependencyKind::Required);
    REQUIRE(Info->AssetDependencies[1].Id == OptionalDep);
    REQUIRE(Info->AssetDependencies[1].LogicalName == "UI/Hud.prefab");
    REQUIRE(Info->AssetDependencies[1].Kind == EAssetDependencyKind::Optional);

    std::filesystem::remove_all(TempDir);
}

TEST_CASE("Asset pack append-update preserves and replaces generic dependency metadata", "[pack]")
{
    const auto TempDir = MakeUniqueTempDir();
    const auto PackPath = TempDir / "DepsAppend.snpak";

    const AssetId FirstAsset = SNAPI_UUID(0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
                                          0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f);
    const AssetId SecondAsset = SNAPI_UUID(0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                                           0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f);
    const AssetId FirstDep = SNAPI_UUID(0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
                                        0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f);
    const AssetId SecondDep = SNAPI_UUID(0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                         0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f);
    const TypeId AssetKind = SNAPI_UUID(0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                                        0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf);
    const TypeId PayloadType = SNAPI_UUID(0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                                          0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf);

    {
        AssetPackWriter Writer;
        auto First = MakeAsset(FirstAsset, AssetKind, PayloadType, "Levels/A.level");
        First.AssetDependencies = {
            AssetDependencyRef{FirstDep, "Shared/First.asset", EAssetDependencyKind::Required},
        };
        auto Second = MakeAsset(SecondAsset, AssetKind, PayloadType, "Levels/B.level");
        Writer.AddAsset(std::move(First));
        Writer.AddAsset(std::move(Second));
        REQUIRE(Writer.Write(PackPath.string()).has_value());
    }

    {
        AssetPackWriter Writer;
        auto UpdatedSecond = MakeAsset(SecondAsset, AssetKind, PayloadType, "Levels/B.level");
        UpdatedSecond.AssetDependencies = {
            AssetDependencyRef{SecondDep, "Shared/Second.asset", EAssetDependencyKind::Auxiliary},
        };
        Writer.AddAsset(std::move(UpdatedSecond));
        REQUIRE(Writer.AppendUpdate(PackPath.string()).has_value());
    }

    AssetPackReader Reader;
    REQUIRE(Reader.Open(PackPath.string()).has_value());

    auto FirstInfo = Reader.FindAsset(FirstAsset);
    REQUIRE(FirstInfo.has_value());
    REQUIRE(FirstInfo->AssetDependencies.size() == 1);
    REQUIRE(FirstInfo->AssetDependencies[0].Id == FirstDep);
    REQUIRE(FirstInfo->AssetDependencies[0].LogicalName == "Shared/First.asset");
    REQUIRE(FirstInfo->AssetDependencies[0].Kind == EAssetDependencyKind::Required);

    auto SecondInfo = Reader.FindAsset(SecondAsset);
    REQUIRE(SecondInfo.has_value());
    REQUIRE(SecondInfo->AssetDependencies.size() == 1);
    REQUIRE(SecondInfo->AssetDependencies[0].Id == SecondDep);
    REQUIRE(SecondInfo->AssetDependencies[0].LogicalName == "Shared/Second.asset");
    REQUIRE(SecondInfo->AssetDependencies[0].Kind == EAssetDependencyKind::Auxiliary);

    std::filesystem::remove_all(TempDir);
}
