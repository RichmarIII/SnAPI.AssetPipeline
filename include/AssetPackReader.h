#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "Export.h"
#include "Uuid.h"
#include "TypedPayload.h"
#include "IAssetCooker.h"

namespace SnAPI::AssetPipeline
{

struct SNAPI_ASSETPIPELINE_API AssetInfo
{
    AssetId Id;
    TypeId AssetKind;
    TypeId CookedPayloadType;
    uint32_t SchemaVersion;
    std::string Name;
    std::string VariantKey;
    uint32_t BulkChunkCount;
};

class SNAPI_ASSETPIPELINE_API AssetPackReader
{
public:
    AssetPackReader();
    ~AssetPackReader();

    // Open a .snpak file for reading
    std::expected<void, std::string> Open(const std::string& Path);

    // Close the pack
    void Close();

    // Check if a pack is open
    bool IsOpen() const;

    // Get the number of assets in the pack
    uint32_t GetAssetCount() const;

    // Get info about an asset by index
    std::expected<AssetInfo, std::string> GetAssetInfo(uint32_t Index) const;

    // Find an asset by ID
    std::expected<AssetInfo, std::string> FindAsset(AssetId Id) const;

    // Find assets by name (may return multiple if variants exist)
    std::vector<AssetInfo> FindAssetsByName(const std::string& Name) const;

    // Load the cooked payload for an asset
    std::expected<TypedPayload, std::string> LoadCookedPayload(AssetId Id) const;

    // Load a bulk chunk for an asset
    std::expected<std::vector<uint8_t>, std::string> LoadBulkChunk(AssetId Id, uint32_t BulkIndex) const;

    // Get bulk chunk info
    struct BulkChunkInfo
    {
        EBulkSemantic Semantic;
        uint32_t SubIndex;
        uint64_t UncompressedSize;
    };
    std::expected<BulkChunkInfo, std::string> GetBulkChunkInfo(AssetId Id, uint32_t BulkIndex) const;

    // Template helper to deserialize a typed payload
    template <typename T>
    std::expected<T, std::string> LoadCookedAs(AssetId Id) const;

    // Non-copyable
    AssetPackReader(const AssetPackReader&) = delete;
    AssetPackReader& operator=(const AssetPackReader&) = delete;

    // Movable
    AssetPackReader(AssetPackReader&&) noexcept;
    AssetPackReader& operator=(AssetPackReader&&) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace SnAPI::AssetPipeline
