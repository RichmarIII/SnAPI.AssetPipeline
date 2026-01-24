#include "TexturePluginIds.h"
#include "TexturePluginPayloads.h"
#include "IAssetImporter.h"
#include "IPipelineContext.h"

#include <algorithm>
#include <cctype>
#include <memory>

#if HAS_FREEIMAGE
#include <FreeImage.h>
#endif

using namespace SnAPI::AssetPipeline;

namespace TexturePlugin
{

class TextureImporter_FreeImage final : public IAssetImporter
{
public:
    const char* GetName() const override
    {
        return "TexturePlugin.TextureImporter.FreeImage";
    }

    bool CanImport(const SourceRef& Source) const override
    {
        // Check extension
        auto DotPos = Source.Uri.find_last_of('.');
        if (DotPos == std::string::npos)
        {
            return false;
        }

        std::string Ext = Source.Uri.substr(DotPos + 1);
        std::transform(Ext.begin(), Ext.end(), Ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        return Ext == "png" || Ext == "jpg" || Ext == "jpeg" ||
               Ext == "tga" || Ext == "bmp" || Ext == "gif" ||
               Ext == "tiff" || Ext == "tif";
    }

    bool Import(const SourceRef& Source, std::vector<ImportedItem>& OutItems, IPipelineContext& Ctx) override
    {
#if HAS_FREEIMAGE
        FreeImage_Initialise(FALSE);

        // Determine format
        FREE_IMAGE_FORMAT Format = FreeImage_GetFileType(Source.Uri.c_str(), 0);
        if (Format == FIF_UNKNOWN)
        {
            Format = FreeImage_GetFIFFromFilename(Source.Uri.c_str());
        }

        if (Format == FIF_UNKNOWN)
        {
            Ctx.LogError("FreeImage: Unknown format: %s", Source.Uri.c_str());
            FreeImage_DeInitialise();
            return false;
        }

        if (!FreeImage_FIFSupportsReading(Format))
        {
            Ctx.LogError("FreeImage: Format not supported for reading: %s", Source.Uri.c_str());
            FreeImage_DeInitialise();
            return false;
        }

        // Load image
        FIBITMAP* Bitmap = FreeImage_Load(Format, Source.Uri.c_str());
        if (!Bitmap)
        {
            Ctx.LogError("FreeImage: Failed to load: %s", Source.Uri.c_str());
            FreeImage_DeInitialise();
            return false;
        }

        // Convert to 32-bit BGRA
        FIBITMAP* Bitmap32 = FreeImage_ConvertTo32Bits(Bitmap);
        FreeImage_Unload(Bitmap);

        if (!Bitmap32)
        {
            Ctx.LogError("FreeImage: Failed to convert to 32-bit: %s", Source.Uri.c_str());
            FreeImage_DeInitialise();
            return false;
        }

        // Get dimensions
        const uint32_t Width = FreeImage_GetWidth(Bitmap32);
        const uint32_t Height = FreeImage_GetHeight(Bitmap32);
        const uint32_t Pitch = FreeImage_GetPitch(Bitmap32);
        const BYTE* Bits = FreeImage_GetBits(Bitmap32);

        // Create intermediate payload
        ImageIntermediate Img;
        Img.Width = Width;
        Img.Height = Height;
        Img.Channels = 4;
        Img.Pixels.resize(static_cast<size_t>(Width) * Height * 4);

        // FreeImage stores bottom-up BGRA, convert to top-down RGBA
        for (uint32_t y = 0; y < Height; ++y)
        {
            const uint32_t SrcY = Height - 1 - y; // Flip vertically
            const BYTE* Row = Bits + static_cast<size_t>(SrcY) * Pitch;

            for (uint32_t x = 0; x < Width; ++x)
            {
                const BYTE B = Row[x * 4 + FI_RGBA_BLUE];
                const BYTE G = Row[x * 4 + FI_RGBA_GREEN];
                const BYTE R = Row[x * 4 + FI_RGBA_RED];
                const BYTE A = Row[x * 4 + FI_RGBA_ALPHA];

                const size_t DestIdx = (static_cast<size_t>(y) * Width + x) * 4;
                Img.Pixels[DestIdx + 0] = R;
                Img.Pixels[DestIdx + 1] = G;
                Img.Pixels[DestIdx + 2] = B;
                Img.Pixels[DestIdx + 3] = A;
            }
        }

        FreeImage_Unload(Bitmap32);
        FreeImage_DeInitialise();

        // Serialize intermediate
        const IPayloadSerializer* Serializer = Ctx.FindSerializer(Payload_ImageIntermediate);
        if (!Serializer)
        {
            Ctx.LogError("Missing serializer for ImageIntermediate");
            return false;
        }

        ImportedItem Item;
        Item.LogicalName = Source.Uri;
        Item.AssetKind = AssetKind_Texture;
        Item.VariantKey = "";
        Item.Dependencies.push_back(Source);
        Item.Id = Ctx.MakeDeterministicAssetId(Item.LogicalName, Item.VariantKey);

        Item.Intermediate.PayloadType = Payload_ImageIntermediate;
        Item.Intermediate.SchemaVersion = Serializer->GetSchemaVersion();
        Serializer->SerializeToBytes(&Img, Item.Intermediate.Bytes);

        OutItems.push_back(std::move(Item));

        Ctx.LogInfo("Imported texture: %s (%ux%u)", Source.Uri.c_str(), Width, Height);
        return true;
#else
        Ctx.LogError("FreeImage not available - cannot import: %s", Source.Uri.c_str());
        return false;
#endif
    }
};

// Factory function
std::unique_ptr<IAssetImporter> CreateTextureImporter_FreeImage()
{
    return std::make_unique<TextureImporter_FreeImage>();
}

} // namespace TexturePlugin
