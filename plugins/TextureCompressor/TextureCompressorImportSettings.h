#pragma once

#include "IAssetImportSettings.h"
#include "TextureCompressorPayloads.h"

#include <cstdint>
#include <memory>

namespace TextureCompressorPlugin
{

enum class ETextureColorSpacePolicy : uint8_t
{
  Auto = 0,
  ForceSrgb,
  ForceLinear,
};

struct TextureCompressorImportSettings final : public SnAPI::AssetPipeline::IAssetImportSettings
{
  ECompressionTarget Target = ECompressionTarget::BCn;
  ECompressedFormat Format = ECompressedFormat::Unknown;
  float Quality = 0.5f;
  ETextureColorSpacePolicy ColorSpacePolicy = ETextureColorSpacePolicy::Auto;
  bool ForceNormalMap = false;
  int32_t MaxMipCount = -1;

  [[nodiscard]] std::unique_ptr<SnAPI::AssetPipeline::IAssetImportSettings> Clone() const override
  {
    return std::make_unique<TextureCompressorImportSettings>(*this);
  }
};

} // namespace TextureCompressorPlugin
