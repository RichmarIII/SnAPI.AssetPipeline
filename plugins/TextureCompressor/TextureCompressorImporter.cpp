#include "TextureCompressorIds.h"
#include "TextureCompressorPayloads.h"
#include "IAssetImporter.h"
#include "IPipelineContext.h"

#include <algorithm>
#include <cctype>
#include <memory>

#if HAS_FREEIMAGE
#include <FreeImage.h>
#endif

using namespace SnAPI::AssetPipeline;

namespace TextureCompressorPlugin
{

class TextureCompressorImporter final : public IAssetImporter
{
public:
  const char* GetName() const override
  {
    return "TextureCompressor.Importer";
  }

  bool CanImport(const SourceRef& Source) const override
  {
    auto DotPos = Source.Uri.find_last_of('.');
    if (DotPos == std::string::npos)
      return false;

    std::string Ext = Source.Uri.substr(DotPos + 1);
    std::transform(Ext.begin(), Ext.end(), Ext.begin(),
                   [](unsigned char C) { return std::tolower(C); });

    return Ext == "png" || Ext == "jpg" || Ext == "jpeg" ||
           Ext == "tga" || Ext == "bmp" || Ext == "gif" ||
           Ext == "tiff" || Ext == "tif" || Ext == "exr" ||
           Ext == "hdr" || Ext == "psd" || Ext == "dds" ||
           Ext == "pbm" || Ext == "pgm" || Ext == "ppm";
  }

  bool Import(const SourceRef& Source, std::vector<ImportedItem>& OutItems, IPipelineContext& Ctx) override
  {
#if HAS_FREEIMAGE
    FreeImage_Initialise(FALSE);

    // Format detection: content-based first, then fallback to extension
    FREE_IMAGE_FORMAT FIFormat = FreeImage_GetFileType(Source.Uri.c_str(), 0);
    if (FIFormat == FIF_UNKNOWN)
    {
      FIFormat = FreeImage_GetFIFFromFilename(Source.Uri.c_str());
    }

    if (FIFormat == FIF_UNKNOWN)
    {
      Ctx.LogError("TextureCompressor: Unknown format: %s", Source.Uri.c_str());
      FreeImage_DeInitialise();
      return false;
    }

    if (!FreeImage_FIFSupportsReading(FIFormat))
    {
      Ctx.LogError("TextureCompressor: Format not supported for reading: %s", Source.Uri.c_str());
      FreeImage_DeInitialise();
      return false;
    }

    // Load image (first page for multi-page formats)
    FIBITMAP* Bitmap = FreeImage_Load(FIFormat, Source.Uri.c_str());
    if (!Bitmap)
    {
      Ctx.LogError("TextureCompressor: Failed to load: %s", Source.Uri.c_str());
      FreeImage_DeInitialise();
      return false;
    }

    // Detect source properties
    FREE_IMAGE_TYPE ImageType = FreeImage_GetImageType(Bitmap);
    uint32_t BPP = FreeImage_GetBPP(Bitmap);
    uint32_t OrigChannels = BPP / (ImageType == FIT_BITMAP ? 8 : 32);

    ImageIntermediate Img;
    Img.SourceFilename = Source.Uri;
    Img.Width = FreeImage_GetWidth(Bitmap);
    Img.Height = FreeImage_GetHeight(Bitmap);

    // Determine if source is floating point / HDR
    bool bSourceIsFloat = (ImageType == FIT_RGBF || ImageType == FIT_RGBAF ||
                           ImageType == FIT_FLOAT);
    bool bSourceIs16Bit = (ImageType == FIT_RGB16 || ImageType == FIT_RGBA16 ||
                           ImageType == FIT_UINT16);

    Img.bIsFloat = bSourceIsFloat;
    if (bSourceIsFloat)
      Img.BitsPerChannel = 32;
    else if (bSourceIs16Bit)
      Img.BitsPerChannel = 16;
    else
      Img.BitsPerChannel = 8;

    // sRGB heuristic: non-float non-normal-map files are assumed sRGB
    Img.bSRGB = !bSourceIsFloat && !IsNormalMapFilename(Source.Uri);

    if (bSourceIsFloat)
    {
      // HDR path: convert to RGBAF (float RGBA)
      FIBITMAP* FloatBitmap = FreeImage_ConvertToRGBAF(Bitmap);
      FreeImage_Unload(Bitmap);

      if (!FloatBitmap)
      {
        Ctx.LogError("TextureCompressor: Failed to convert to float RGBA: %s", Source.Uri.c_str());
        FreeImage_DeInitialise();
        return false;
      }

      Img.Channels = 4;
      Img.BitsPerChannel = 32;
      size_t PixelBytes = static_cast<size_t>(Img.Width) * Img.Height * 4 * sizeof(float);
      Img.Pixels.resize(PixelBytes);

      uint32_t Pitch = FreeImage_GetPitch(FloatBitmap);
      const BYTE* Bits = FreeImage_GetBits(FloatBitmap);

      // FreeImage stores bottom-up, convert to top-down RGBA float
      for (uint32_t Y = 0; Y < Img.Height; ++Y)
      {
        uint32_t SrcY = Img.Height - 1 - Y;
        const float* Row = reinterpret_cast<const float*>(Bits + static_cast<size_t>(SrcY) * Pitch);
        float* DstRow = reinterpret_cast<float*>(Img.Pixels.data()) +
                        static_cast<size_t>(Y) * Img.Width * 4;

        for (uint32_t X = 0; X < Img.Width; ++X)
        {
          // RGBAF format: R, G, B, A in order
          DstRow[X * 4 + 0] = Row[X * 4 + 0];
          DstRow[X * 4 + 1] = Row[X * 4 + 1];
          DstRow[X * 4 + 2] = Row[X * 4 + 2];
          DstRow[X * 4 + 3] = Row[X * 4 + 3];
        }
      }

      // Check for non-trivial alpha
      Img.bHasNonTrivialAlpha = DetectNonTrivialAlphaFloat(
          reinterpret_cast<const float*>(Img.Pixels.data()),
          Img.Width, Img.Height);

      FreeImage_Unload(FloatBitmap);
    }
    else
    {
      // LDR path: convert to 32-bit BGRA
      FIBITMAP* Bitmap32 = FreeImage_ConvertTo32Bits(Bitmap);
      FreeImage_Unload(Bitmap);

      if (!Bitmap32)
      {
        Ctx.LogError("TextureCompressor: Failed to convert to 32-bit: %s", Source.Uri.c_str());
        FreeImage_DeInitialise();
        return false;
      }

      Img.Channels = 4;
      Img.BitsPerChannel = 8;
      size_t PixelBytes = static_cast<size_t>(Img.Width) * Img.Height * 4;
      Img.Pixels.resize(PixelBytes);

      uint32_t Pitch = FreeImage_GetPitch(Bitmap32);
      const BYTE* Bits = FreeImage_GetBits(Bitmap32);

      // Convert bottom-up BGRA to top-down RGBA
      bool bHasAlpha = false;
      for (uint32_t Y = 0; Y < Img.Height; ++Y)
      {
        uint32_t SrcY = Img.Height - 1 - Y;
        const BYTE* Row = Bits + static_cast<size_t>(SrcY) * Pitch;

        for (uint32_t X = 0; X < Img.Width; ++X)
        {
          size_t DstIdx = (static_cast<size_t>(Y) * Img.Width + X) * 4;
          Img.Pixels[DstIdx + 0] = Row[X * 4 + FI_RGBA_RED];
          Img.Pixels[DstIdx + 1] = Row[X * 4 + FI_RGBA_GREEN];
          Img.Pixels[DstIdx + 2] = Row[X * 4 + FI_RGBA_BLUE];
          Img.Pixels[DstIdx + 3] = Row[X * 4 + FI_RGBA_ALPHA];

          if (Row[X * 4 + FI_RGBA_ALPHA] != 255)
            bHasAlpha = true;
        }
      }

      Img.bHasNonTrivialAlpha = bHasAlpha;
      // Record source channel count
      if (OrigChannels < 4)
        Img.Channels = 4; // We always output RGBA

      FreeImage_Unload(Bitmap32);
    }

    FreeImage_DeInitialise();

    // Serialize intermediate
    const IPayloadSerializer* Serializer = Ctx.FindSerializer(Payload_CompressorImageIntermediate);
    if (!Serializer)
    {
      Ctx.LogError("TextureCompressor: Missing serializer for ImageIntermediate");
      return false;
    }

    ImportedItem Item;
    Item.LogicalName = Source.Uri;
    Item.AssetKind = AssetKind_CompressedTexture;
    Item.Dependencies.push_back(Source);
    Item.Id = Ctx.MakeDeterministicAssetId(Item.LogicalName, Item.VariantKey);

    Item.Intermediate.PayloadType = Payload_CompressorImageIntermediate;
    Item.Intermediate.SchemaVersion = Serializer->GetSchemaVersion();
    Serializer->SerializeToBytes(&Img, Item.Intermediate.Bytes);

    OutItems.push_back(std::move(Item));

    Ctx.LogInfo("TextureCompressor: Imported %s (%ux%u, %u-bit%s%s)",
                Source.Uri.c_str(), Img.Width, Img.Height,
                Img.BitsPerChannel,
                Img.bIsFloat ? " float" : "",
                Img.bHasNonTrivialAlpha ? " alpha" : "");
    return true;
#else
    Ctx.LogError("FreeImage not available - cannot import: %s", Source.Uri.c_str());
    return false;
#endif
  }

private:
  static bool IsNormalMapFilename(const std::string& Filename)
  {
    std::string Lower = Filename;
    std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                   [](unsigned char C) { return std::tolower(C); });
    return Lower.find("_normal") != std::string::npos ||
           Lower.find("_n.") != std::string::npos ||
           Lower.find("_nrm") != std::string::npos ||
           Lower.find("_bump") != std::string::npos;
  }

  static bool DetectNonTrivialAlphaFloat(const float* Pixels, uint32_t Width, uint32_t Height)
  {
    for (size_t I = 0; I < static_cast<size_t>(Width) * Height; ++I)
    {
      float A = Pixels[I * 4 + 3];
      if (A < 0.99f)
        return true;
    }
    return false;
  }
};

// Factory function
std::unique_ptr<IAssetImporter> CreateTextureCompressorImporter()
{
  return std::make_unique<TextureCompressorImporter>();
}

} // namespace TextureCompressorPlugin
