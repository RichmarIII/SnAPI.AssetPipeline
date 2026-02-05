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

// Compression mode for writing packs
enum class EPackCompression
{
    None,
    LZ4,
    LZ4HC,
    Zstd,
    ZstdFast,
};

enum class EPackCompressionLevel
{
    Default,
    Fast,
    High,
    Max,
};

// Entry to add to an asset pack
struct SNAPI_ASSETPIPELINE_API AssetPackEntry
{
    AssetId Id;
    TypeId AssetKind;
    std::string Name;
    std::string VariantKey;

    TypedPayload Cooked;
    std::vector<BulkChunk> Bulk;
};

class SNAPI_ASSETPIPELINE_API AssetPackWriter
{
public:
    AssetPackWriter();
    ~AssetPackWriter();

    // Set compression mode (default: Zstd)
    void SetCompression(EPackCompression Mode) const;

    // Set compression level (default: Default)
    void SetCompressionLevel(EPackCompressionLevel Level) const;

    // Enable maximum compression level (slower, smaller output)
    void SetMaxCompression(bool bEnable) const;

    // Add an asset to be written
    void AddAsset(AssetPackEntry Entry) const;

    // Add an asset with individual parameters
    void AddAsset(
        AssetId Id,
        TypeId AssetKind,
        const std::string& Name,
        const std::string& VariantKey,
        TypedPayload Cooked,
        std::vector<BulkChunk> Bulk = {}) const;

    // Write the pack to a file (atomic: writes to temp file then renames)
    std::expected<void, std::string> Write(const std::string& OutputPath) const;

    // Append assets to an existing pack (creates new index at end)
    // If the file doesn't exist, creates a new pack
    std::expected<void, std::string> AppendUpdate(const std::string& PackPath) const;

    // Clear all pending assets
    void Clear() const;

    // Get the number of pending assets
    uint32_t GetPendingAssetCount() const;

    // Non-copyable
    AssetPackWriter(const AssetPackWriter&) = delete;
    AssetPackWriter& operator=(const AssetPackWriter&) = delete;

    // Movable
    AssetPackWriter(AssetPackWriter&&) noexcept;
    AssetPackWriter& operator=(AssetPackWriter&&) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace SnAPI::AssetPipeline
