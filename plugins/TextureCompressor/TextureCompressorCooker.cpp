#include "TextureCompressorIds.h"
#include "TextureCompressorPayloads.h"
#include "TextureFormatSelector.h"
#include "MipGenerator.h"
#include "CompressorBackendBCn.h"
#include "CompressorBackendASTC.h"
#include "IAssetCooker.h"
#include "IPipelineContext.h"

#include <memory>

using namespace SnAPI::AssetPipeline;

namespace TextureCompressorPlugin
{

class TextureCompressorCooker final : public IAssetCooker
{
public:
  const char* GetName() const override
  {
    return "TextureCompressor.Cooker";
  }

  bool CanCook(TypeId AssetKind, TypeId IntermediatePayloadType) const override
  {
    return AssetKind == AssetKind_CompressedTexture &&
           IntermediatePayloadType == Payload_CompressorImageIntermediate;
  }

  bool Cook(const CookRequest& Req, CookResult& Out, IPipelineContext& Ctx) override
  {
    // Get serializers
    const IPayloadSerializer* IntermediateSer = Ctx.FindSerializer(Payload_CompressorImageIntermediate);
    const IPayloadSerializer* CookedSer = Ctx.FindSerializer(Payload_CompressorCookedInfo);

    if (!IntermediateSer)
    {
      Ctx.LogError("TextureCompressor: Missing serializer for ImageIntermediate");
      return false;
    }
    if (!CookedSer)
    {
      Ctx.LogError("TextureCompressor: Missing serializer for CookedInfo");
      return false;
    }

    // Deserialize intermediate
    ImageIntermediate Img;
    if (!IntermediateSer->DeserializeFromBytes(&Img, Req.Intermediate.Bytes.data(), Req.Intermediate.Bytes.size()))
    {
      Ctx.LogError("TextureCompressor: Failed to deserialize ImageIntermediate");
      return false;
    }

    // Select format using heuristics
    FormatSelection Selection = TextureFormatSelector::Select(Img, Req.BuildOptions);

    // Get block dimensions
    uint32_t BlockW, BlockH;
    GetBlockDimensions(Selection.Format, BlockW, BlockH);

    // Determine max mip count from build options
    int32_t MaxMipCount = -1;
    auto MaxMipIt = Req.BuildOptions.find("texture.max_mips");
    if (MaxMipIt != Req.BuildOptions.end())
    {
      try { MaxMipCount = std::stoi(MaxMipIt->second); }
      catch (...) {}
    }

    // Generate mip chain (using LDR pixels - RGBA8)
    // For HDR, we'd need float mip generation, but for now we use RGBA8 path
    std::vector<MipLevel> MipChain;
    if (Img.bIsFloat)
    {
      // For HDR/float sources, generate mips from the float data
      // For now, just use mip 0 (float mip gen would need a float downsample path)
      MipLevel Mip0;
      Mip0.Width = Img.Width;
      Mip0.Height = Img.Height;
      Mip0.Pixels = Img.Pixels; // float RGBA data
      MipChain.push_back(std::move(Mip0));
    }
    else
    {
      MipChain = MipGenerator::Generate(
          Img.Pixels.data(), Img.Width, Img.Height,
          4, // Always RGBA
          Selection.bSRGB,
          MaxMipCount);
    }

    // Build cooked info
    TextureCompressorCookedInfo CookedInfo;
    CookedInfo.BaseWidth = Img.Width;
    CookedInfo.BaseHeight = Img.Height;
    CookedInfo.MipCount = static_cast<uint32_t>(MipChain.size());
    CookedInfo.Format = Selection.Format;
    CookedInfo.bSRGB = Selection.bSRGB;
    CookedInfo.SourceChannels = Img.Channels;
    CookedInfo.BlockWidth = BlockW;
    CookedInfo.BlockHeight = BlockH;

    // Compress each mip level
    bool bIsASTC = IsASTCFormat(Selection.Format);
    bool bIsHDR = IsHDRFormat(Selection.Format);

    for (uint32_t MipIdx = 0; MipIdx < CookedInfo.MipCount; ++MipIdx)
    {
      const MipLevel& Mip = MipChain[MipIdx];

      std::expected<std::vector<uint8_t>, std::string> CompressResult;

      if (bIsASTC)
      {
        CompressResult = CompressorBackendASTC::Compress(
            Mip.Pixels.data(), Mip.Width, Mip.Height,
            BlockW, BlockH, Selection.Quality, bIsHDR);
      }
      else
      {
        CompressResult = CompressorBackendBCn::Compress(
            Mip.Pixels.data(), Mip.Width, Mip.Height,
            Selection.Format, Selection.Quality);
      }

      if (!CompressResult.has_value())
      {
        Ctx.LogError("TextureCompressor: Compression failed for mip %u: %s",
                     MipIdx, CompressResult.error().c_str());
        return false;
      }

      // Record mip level info
      MipLevelInfo MipInfo;
      MipInfo.Width = Mip.Width;
      MipInfo.Height = Mip.Height;
      MipInfo.CompressedSize = static_cast<uint32_t>(CompressResult->size());
      CookedInfo.MipLevels.push_back(MipInfo);

      // Create bulk chunk for this mip (already compressed, don't re-compress)
      BulkChunk Chunk(EBulkSemantic::Reserved_Level, MipIdx, false);
      Chunk.Bytes = std::move(*CompressResult);
      Out.Bulk.push_back(std::move(Chunk));
    }

    // Serialize cooked info
    Out.Cooked.PayloadType = Payload_CompressorCookedInfo;
    Out.Cooked.SchemaVersion = CookedSer->GetSchemaVersion();
    CookedSer->SerializeToBytes(&CookedInfo, Out.Cooked.Bytes);

    // Carry forward dependencies
    Out.Dependencies = Req.Dependencies;

    // Add tags
    Out.Tags["Texture.Width"] = std::to_string(CookedInfo.BaseWidth);
    Out.Tags["Texture.Height"] = std::to_string(CookedInfo.BaseHeight);
    Out.Tags["Texture.Format"] = GetFormatName(CookedInfo.Format);
    Out.Tags["Texture.MipCount"] = std::to_string(CookedInfo.MipCount);
    Out.Tags["Texture.sRGB"] = CookedInfo.bSRGB ? "true" : "false";
    Out.Tags["Texture.BlockSize"] = std::to_string(BlockW) + "x" + std::to_string(BlockH);

    Ctx.LogInfo("TextureCompressor: Cooked %s (%ux%u %s, %u mips%s)",
                Req.LogicalName.c_str(),
                CookedInfo.BaseWidth, CookedInfo.BaseHeight,
                GetFormatName(CookedInfo.Format),
                CookedInfo.MipCount,
                CookedInfo.bSRGB ? " sRGB" : " linear");

    return true;
  }
};

// Factory function
std::unique_ptr<IAssetCooker> CreateTextureCompressorCooker()
{
  return std::make_unique<TextureCompressorCooker>();
}

} // namespace TextureCompressorPlugin
