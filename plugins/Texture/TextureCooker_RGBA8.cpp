#include "TexturePluginIds.h"
#include "TexturePluginPayloads.h"
#include "IAssetCooker.h"
#include "IPipelineContext.h"

#include <memory>

using namespace SnAPI::AssetPipeline;

namespace TexturePlugin
{

class TextureCooker_RGBA8 final : public IAssetCooker
{
public:
    const char* GetName() const override
    {
        return "TexturePlugin.TextureCooker.RGBA8";
    }

    bool CanCook(TypeId AssetKind, TypeId IntermediatePayloadType) const override
    {
        return AssetKind == AssetKind_Texture &&
               IntermediatePayloadType == Payload_ImageIntermediate;
    }

    bool Cook(const CookRequest& Req, CookResult& Out, IPipelineContext& Ctx) override
    {
        // Get serializers
        const IPayloadSerializer* IntermediateSer = Ctx.FindSerializer(Payload_ImageIntermediate);
        const IPayloadSerializer* CookedSer = Ctx.FindSerializer(Payload_TextureCookedInfo);

        if (!IntermediateSer)
        {
            Ctx.LogError("Missing serializer for ImageIntermediate");
            return false;
        }

        if (!CookedSer)
        {
            Ctx.LogError("Missing serializer for TextureCookedInfo");
            return false;
        }

        // Deserialize intermediate
        ImageIntermediate Img;
        if (!IntermediateSer->DeserializeFromBytes(&Img, Req.Intermediate.Bytes.data(), Req.Intermediate.Bytes.size()))
        {
            Ctx.LogError("Failed to deserialize ImageIntermediate");
            return false;
        }

        // Create cooked info
        TextureCookedInfo Info;
        Info.Width = Img.Width;
        Info.Height = Img.Height;
        Info.MipCount = 1; // No mip generation in this basic cooker
        Info.Format = ETextureFormat::RGBA8;

        // Serialize cooked info
        Out.Cooked.PayloadType = Payload_TextureCookedInfo;
        Out.Cooked.SchemaVersion = CookedSer->GetSchemaVersion();
        CookedSer->SerializeToBytes(&Info, Out.Cooked.Bytes);

        // Create bulk chunk with pixel data
        BulkChunk Mip0;
        Mip0.Semantic = EBulkSemantic::Reserved_Level;
        Mip0.SubIndex = 0; // Mip level 0
        Mip0.bCompress = true;
        Mip0.Bytes = std::move(Img.Pixels);

        Out.Bulk.push_back(std::move(Mip0));

        // Carry forward dependencies
        Out.Dependencies = Req.Dependencies;

        // Add tags
        Out.Tags["Texture.Width"] = std::to_string(Info.Width);
        Out.Tags["Texture.Height"] = std::to_string(Info.Height);
        Out.Tags["Texture.Format"] = "RGBA8";
        Out.Tags["Texture.MipCount"] = std::to_string(Info.MipCount);

        Ctx.LogInfo("Cooked texture: %s (%ux%u RGBA8)",
                    Req.LogicalName.c_str(), Info.Width, Info.Height);

        return true;
    }
};

// Factory function
std::unique_ptr<IAssetCooker> CreateTextureCooker_RGBA8()
{
    return std::make_unique<TextureCooker_RGBA8>();
}

} // namespace TexturePlugin
