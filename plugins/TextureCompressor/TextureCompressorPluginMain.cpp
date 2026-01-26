#include "IPluginRegistrar.h"

#include <memory>

namespace TextureCompressorPlugin
{

// Factory function declarations
std::unique_ptr<SnAPI::AssetPipeline::IPayloadSerializer> CreateCompressorImageIntermediateSerializer();
std::unique_ptr<SnAPI::AssetPipeline::IPayloadSerializer> CreateCompressorCookedInfoSerializer();
std::unique_ptr<SnAPI::AssetPipeline::IAssetImporter> CreateTextureCompressorImporter();
std::unique_ptr<SnAPI::AssetPipeline::IAssetCooker> CreateTextureCompressorCooker();

static void RegisterPlugin(SnAPI::AssetPipeline::IPluginRegistrar& Registrar)
{
  Registrar.RegisterPluginInfo("TextureCompressorPlugin", "1.0.0");

  // Register serializers first (required by importers/cookers)
  Registrar.RegisterPayloadSerializer(CreateCompressorImageIntermediateSerializer());
  Registrar.RegisterPayloadSerializer(CreateCompressorCookedInfoSerializer());

  // Register importer
  Registrar.RegisterImporter(CreateTextureCompressorImporter());

  // Register cooker
  Registrar.RegisterCooker(CreateTextureCompressorCooker());
}

} // namespace TextureCompressorPlugin

// Plugin entry point
SNAPI_DEFINE_PLUGIN(TextureCompressorPlugin::RegisterPlugin)
