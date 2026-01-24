#pragma once

#include <memory>

#include "Export.h"
#include "IPayloadSerializer.h"
#include "IAssetImporter.h"
#include "IAssetCooker.h"

namespace SnAPI::AssetPipeline
{

class SNAPI_ASSETPIPELINE_API IPluginRegistrar
{
public:
    virtual ~IPluginRegistrar() = default;

    virtual void RegisterImporter(std::unique_ptr<IAssetImporter> Importer) = 0;
    virtual void RegisterCooker(std::unique_ptr<IAssetCooker> Cooker) = 0;
    virtual void RegisterPayloadSerializer(std::unique_ptr<IPayloadSerializer> Serializer) = 0;

    virtual void RegisterPluginInfo(const char* Name, const char* VersionString) = 0;
};

// Plugin entry point signature
// Each plugin DLL/SO must export this function
using PluginRegisterFunc = void(*)(IPluginRegistrar& Registrar);

#define SNAPI_PLUGIN_ENTRY_NAME "SnAPI_RegisterAssetPipelinePlugin"

} // namespace SnAPI::AssetPipeline

// Macro to define plugin entry point
#define SNAPI_DEFINE_PLUGIN(RegisterFunction) \
    extern "C" SNAPI_ASSETPIPELINE_PLUGIN_API void SnAPI_RegisterAssetPipelinePlugin(::SnAPI::AssetPipeline::IPluginRegistrar& Registrar) \
    { \
        RegisterFunction(Registrar); \
    }
