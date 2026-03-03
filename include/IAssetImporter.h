#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Export.h"
#include "IAssetImportSettings.h"
#include "Uuid.h"
#include "TypedPayload.h"

namespace SnAPI::AssetPipeline
{

class IPipelineContext;

struct SNAPI_ASSETPIPELINE_API SourceRef
{
    std::string Uri;              // File path or custom scheme
    uint64_t ContentHash = 0;     // Framework computed; plugin may override

    SourceRef() = default;
    explicit SourceRef(std::string InUri, uint64_t Hash = 0)
        : Uri(std::move(InUri))
        , ContentHash(Hash)
    {
    }
};

struct SNAPI_ASSETPIPELINE_API ImportedItem
{
    AssetId Id;
    std::string LogicalName;
    TypeId AssetKind;
    std::string VariantKey;

    TypedPayload Intermediate;
    std::vector<SourceRef> Dependencies;
    AssetImportSettingsPtr ImportSettings{};

    ImportedItem() = default;
};

class SNAPI_ASSETPIPELINE_API IAssetImporter
{
public:
    virtual ~IAssetImporter() = default;

    // Returns a unique name for this importer
    virtual const char* GetName() const = 0;

    // Returns true if this importer can handle the given source
    virtual bool CanImport(const SourceRef& Source) const = 0;

    // Import the source and produce one or more ImportedItems
    virtual bool Import(const SourceRef& Source, std::vector<ImportedItem>& OutItems, IPipelineContext& Ctx) = 0;

    // Preferred entry point for typed settings-driven import paths.
    // Default implementation keeps legacy behavior and ignores settings.
    virtual bool ImportWithSettings(const SourceRef& Source,
                                    const IAssetImportSettings* Settings,
                                    std::vector<ImportedItem>& OutItems,
                                    IPipelineContext& Ctx)
    {
        (void)Settings;
        return Import(Source, OutItems, Ctx);
    }
};

} // namespace AssetPipeline
