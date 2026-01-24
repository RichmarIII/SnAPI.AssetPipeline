#pragma once

#include "IPayloadSerializer.h"
#include <memory>

namespace TexturePlugin
{

// Factory functions for creating payload serializers
// Games should call these and register with PayloadRegistry
std::unique_ptr<SnAPI::AssetPipeline::IPayloadSerializer> CreateImageIntermediateSerializer();
std::unique_ptr<SnAPI::AssetPipeline::IPayloadSerializer> CreateTextureCookedInfoSerializer();

} // namespace TexturePlugin
