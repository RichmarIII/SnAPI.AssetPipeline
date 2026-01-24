#include "IPluginRegistrar.h"

#include <memory>

namespace TexturePlugin
{

// Factory function declarations
std::unique_ptr<SnAPI::AssetPipeline::IPayloadSerializer> CreateImageIntermediateSerializer();
std::unique_ptr<SnAPI::AssetPipeline::IPayloadSerializer> CreateTextureCookedInfoSerializer();
std::unique_ptr<SnAPI::AssetPipeline::IAssetImporter> CreateTextureImporter_FreeImage();
std::unique_ptr<SnAPI::AssetPipeline::IAssetCooker> CreateTextureCooker_RGBA8();

static void RegisterPlugin(SnAPI::AssetPipeline::IPluginRegistrar& Registrar)
{
    Registrar.RegisterPluginInfo("TexturePlugin", "1.0.0");

    // Register serializers first (required by importers/cookers)
    Registrar.RegisterPayloadSerializer(CreateImageIntermediateSerializer());
    Registrar.RegisterPayloadSerializer(CreateTextureCookedInfoSerializer());

    // Register importers
    Registrar.RegisterImporter(CreateTextureImporter_FreeImage());

    // Register cookers
    Registrar.RegisterCooker(CreateTextureCooker_RGBA8());
}

} // namespace TexturePlugin

// Plugin entry point
SNAPI_DEFINE_PLUGIN(TexturePlugin::RegisterPlugin)
