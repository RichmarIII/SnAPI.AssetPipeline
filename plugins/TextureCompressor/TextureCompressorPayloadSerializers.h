#pragma once

#include "IPayloadSerializer.h"

#include <memory>

namespace TextureCompressorPlugin
{

std::unique_ptr<SnAPI::AssetPipeline::IPayloadSerializer> CreateCompressorImageIntermediateSerializer();
std::unique_ptr<SnAPI::AssetPipeline::IPayloadSerializer> CreateCompressorCookedInfoSerializer();

} // namespace TextureCompressorPlugin
