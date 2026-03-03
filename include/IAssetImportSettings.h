#pragma once

#include "Export.h"

#include <memory>

namespace SnAPI::AssetPipeline
{

/**
 * @brief Type-erased import settings payload passed from editor/tools into importer/cooker plugins.
 * @remarks
 * Settings objects are owned via shared pointers so a single typed settings instance can be reused
 * across all imported/cooked assets emitted from one source file.
 */
class SNAPI_ASSETPIPELINE_API IAssetImportSettings
{
public:
    virtual ~IAssetImportSettings() = default;

    /**
     * @brief Clone settings into a new heap instance.
     * @return Newly allocated deep copy.
     */
    [[nodiscard]] virtual std::unique_ptr<IAssetImportSettings> Clone() const = 0;
};

using AssetImportSettingsPtr = std::shared_ptr<const IAssetImportSettings>;

} // namespace SnAPI::AssetPipeline

