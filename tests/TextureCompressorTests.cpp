#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "TextureCompressorIds.h"
#include "TextureCompressorPayloads.h"
#include "TextureCompressorPayloadSerializers.h"
#include "TextureFormatSelector.h"
#include "MipGenerator.h"
#include "CompressorBackendBCn.h"
#include "CompressorBackendASTC.h"
#include "IAssetCooker.h"
#include "IPipelineContext.h"
#include "AssetManager.h"

#include <any>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace SnAPI::AssetPipeline;
using namespace TextureCompressorPlugin;

// ========== Test Helpers ==========

namespace
{

// Create a solid-color RGBA8 test image
std::vector<uint8_t> CreateTestPixels(uint32_t Width, uint32_t Height,
                                      uint8_t R = 128, uint8_t G = 64, uint8_t B = 32, uint8_t A = 255)
{
  std::vector<uint8_t> Pixels(static_cast<size_t>(Width) * Height * 4);
  for (size_t I = 0; I < static_cast<size_t>(Width) * Height; ++I)
  {
    Pixels[I * 4 + 0] = R;
    Pixels[I * 4 + 1] = G;
    Pixels[I * 4 + 2] = B;
    Pixels[I * 4 + 3] = A;
  }
  return Pixels;
}

// Create an ImageIntermediate with given properties
ImageIntermediate CreateTestIntermediate(uint32_t Width, uint32_t Height,
                                         const std::string& Filename = "test.png",
                                         uint32_t Channels = 4,
                                         bool bIsFloat = false,
                                         bool bHasAlpha = false,
                                         bool bSRGB = true,
                                         uint32_t BitsPerChannel = 8)
{
  ImageIntermediate Img;
  Img.Width = Width;
  Img.Height = Height;
  Img.Channels = Channels;
  Img.BitsPerChannel = BitsPerChannel;
  Img.bIsFloat = bIsFloat;
  Img.bHasNonTrivialAlpha = bHasAlpha;
  Img.bSRGB = bSRGB;
  Img.SourceFilename = Filename;
  Img.Pixels = CreateTestPixels(Width, Height, 128, 64, 32, bHasAlpha ? 200 : 255);
  return Img;
}

// Mock PipelineContext for cooker tests
class MockPipelineContext : public IPipelineContext
{
public:
  std::vector<std::string> InfoMessages;
  std::vector<std::string> ErrorMessages;
  std::vector<const IPayloadSerializer*> Serializers;

  void RegisterSerializer(const IPayloadSerializer* Ser)
  {
    Serializers.push_back(Ser);
  }

  void LogInfo(const char* Fmt, ...) override
  {
    char Buf[1024];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buf, sizeof(Buf), Fmt, Args);
    va_end(Args);
    InfoMessages.emplace_back(Buf);
  }

  void LogWarn(const char* Fmt, ...) override {}

  void LogError(const char* Fmt, ...) override
  {
    char Buf[1024];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buf, sizeof(Buf), Fmt, Args);
    va_end(Args);
    ErrorMessages.emplace_back(Buf);
  }

  bool ReadAllBytes(const std::string& Uri, std::vector<uint8_t>& Out) override
  {
    return false;
  }

  uint64_t HashBytes64(const void* Data, std::size_t Size) override
  {
    return 0;
  }

  void HashBytes128(const void* Data, std::size_t Size, uint64_t& OutHi, uint64_t& OutLo) override
  {
    OutHi = 0;
    OutLo = 0;
  }

  AssetId MakeDeterministicAssetId(std::string_view LogicalName, std::string_view VariantKey) override
  {
    return Uuid::Generate();
  }

  const IPayloadSerializer* FindSerializer(TypeId Id) const override
  {
    for (const auto* Ser : Serializers)
    {
      if (Ser->GetTypeId() == Id)
        return Ser;
    }
    return nullptr;
  }

  std::string GetOption(std::string_view Key, std::string_view Default = {}) const override
  {
    return std::string(Default);
  }
};

} // namespace

// Factory function declaration from TextureCompressorCooker.cpp
namespace TextureCompressorPlugin
{
std::unique_ptr<SnAPI::AssetPipeline::IAssetCooker> CreateTextureCompressorCooker();
} // namespace TextureCompressorPlugin

// ========== Format Selection Tests ==========

TEST_CASE("FormatSelector: opaque RGB defaults to BC7 sRGB", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "diffuse.png");
  std::unordered_map<std::string, std::string> Opts;

  FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);

  REQUIRE(Sel.Format == ECompressedFormat::BC7);
  REQUIRE(Sel.bSRGB == true);
}

TEST_CASE("FormatSelector: normal map filename selects BC5 linear", "[texcomp][format]")
{
  SECTION("_normal suffix")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "brick_normal.png");
    FormatSelection Sel = TextureFormatSelector::Select(Img, {});
    REQUIRE(Sel.Format == ECompressedFormat::BC5);
    REQUIRE(Sel.bSRGB == false);
  }

  SECTION("_n. suffix")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "brick_n.png");
    FormatSelection Sel = TextureFormatSelector::Select(Img, {});
    REQUIRE(Sel.Format == ECompressedFormat::BC5);
    REQUIRE(Sel.bSRGB == false);
  }

  SECTION("_nrm suffix")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "wall_nrm.tga");
    FormatSelection Sel = TextureFormatSelector::Select(Img, {});
    REQUIRE(Sel.Format == ECompressedFormat::BC5);
    REQUIRE(Sel.bSRGB == false);
  }

  SECTION("_bump suffix")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "ground_bump.jpg");
    FormatSelection Sel = TextureFormatSelector::Select(Img, {});
    REQUIRE(Sel.Format == ECompressedFormat::BC5);
    REQUIRE(Sel.bSRGB == false);
  }

  SECTION("normalmap in name")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "normalmap_brick.png");
    FormatSelection Sel = TextureFormatSelector::Select(Img, {});
    REQUIRE(Sel.Format == ECompressedFormat::BC5);
    REQUIRE(Sel.bSRGB == false);
  }
}

TEST_CASE("FormatSelector: alpha present selects BC7", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "foliage.png", 4, false, true);
  FormatSelection Sel = TextureFormatSelector::Select(Img, {});

  REQUIRE(Sel.Format == ECompressedFormat::BC7);
  REQUIRE(Sel.bSRGB == true);
}

TEST_CASE("FormatSelector: single channel selects BC4", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "roughness.png", 1);
  Img.Pixels = std::vector<uint8_t>(256 * 256 * 1, 128); // 1-channel
  FormatSelection Sel = TextureFormatSelector::Select(Img, {});

  REQUIRE(Sel.Format == ECompressedFormat::BC4);
}

TEST_CASE("FormatSelector: two channels selects BC5", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "occlusion_roughness.png", 2);
  Img.Pixels = std::vector<uint8_t>(256 * 256 * 2, 128);
  FormatSelection Sel = TextureFormatSelector::Select(Img, {});

  REQUIRE(Sel.Format == ECompressedFormat::BC5);
  REQUIRE(Sel.bSRGB == false);
}

TEST_CASE("FormatSelector: HDR source selects BC6H linear", "[texcomp][format]")
{
  SECTION("float flag set")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "sky.exr", 4, true);
    Img.BitsPerChannel = 32;
    FormatSelection Sel = TextureFormatSelector::Select(Img, {});
    REQUIRE(Sel.Format == ECompressedFormat::BC6H);
    REQUIRE(Sel.bSRGB == false);
  }

  SECTION("16-bit source")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "scene.png", 4, false, false, true, 16);
    FormatSelection Sel = TextureFormatSelector::Select(Img, {});
    REQUIRE(Sel.Format == ECompressedFormat::BC6H);
    REQUIRE(Sel.bSRGB == false);
  }

  SECTION("HDR filename extension")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "environment.hdr");
    FormatSelection Sel = TextureFormatSelector::Select(Img, {});
    REQUIRE(Sel.Format == ECompressedFormat::BC6H);
    REQUIRE(Sel.bSRGB == false);
  }
}

TEST_CASE("FormatSelector: explicit format override", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256);
  std::unordered_map<std::string, std::string> Opts = {{"texture.format", "BC1"}};

  FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
  REQUIRE(Sel.Format == ECompressedFormat::BC1);
}

TEST_CASE("FormatSelector: ASTC target standard color defaults to ASTC_6x6", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "diffuse.png");
  std::unordered_map<std::string, std::string> Opts = {{"texture.target", "astc"}};

  FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
  REQUIRE(Sel.Format == ECompressedFormat::ASTC_6x6);
  REQUIRE(Sel.bSRGB == true);
}

TEST_CASE("FormatSelector: ASTC target normal map selects ASTC_4x4 linear", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "wall_normal.png");
  std::unordered_map<std::string, std::string> Opts = {{"texture.target", "astc"}};

  FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
  REQUIRE(Sel.Format == ECompressedFormat::ASTC_4x4);
  REQUIRE(Sel.bSRGB == false);
}

TEST_CASE("FormatSelector: ASTC target HDR selects ASTC_6x6_HDR", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "sky.exr", 4, true);
  Img.BitsPerChannel = 32;
  std::unordered_map<std::string, std::string> Opts = {{"texture.target", "astc"}};

  FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
  REQUIRE(Sel.Format == ECompressedFormat::ASTC_6x6_HDR);
  REQUIRE(Sel.bSRGB == false);
}

TEST_CASE("FormatSelector: ASTC block size override", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "diffuse.png");

  SECTION("4x4")
  {
    std::unordered_map<std::string, std::string> Opts = {
        {"texture.target", "astc"}, {"texture.astc_block", "4x4"}};
    FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
    REQUIRE(Sel.Format == ECompressedFormat::ASTC_4x4);
  }

  SECTION("8x8")
  {
    std::unordered_map<std::string, std::string> Opts = {
        {"texture.target", "astc"}, {"texture.astc_block", "8x8"}};
    FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
    REQUIRE(Sel.Format == ECompressedFormat::ASTC_8x8);
  }

  SECTION("12x12")
  {
    std::unordered_map<std::string, std::string> Opts = {
        {"texture.target", "astc"}, {"texture.astc_block", "12x12"}};
    FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
    REQUIRE(Sel.Format == ECompressedFormat::ASTC_12x12);
  }
}

TEST_CASE("FormatSelector: quality option is parsed", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256);
  std::unordered_map<std::string, std::string> Opts = {{"texture.quality", "0.9"}};

  FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
  REQUIRE(Sel.Quality == Catch::Approx(0.9f));
}

TEST_CASE("FormatSelector: sRGB override", "[texcomp][format]")
{
  SECTION("force sRGB on linear data")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "data.png", 1);
    std::unordered_map<std::string, std::string> Opts = {{"texture.srgb", "true"}};
    FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
    REQUIRE(Sel.bSRGB == true);
  }

  SECTION("force linear on color data")
  {
    ImageIntermediate Img = CreateTestIntermediate(256, 256, "diffuse.png");
    std::unordered_map<std::string, std::string> Opts = {{"texture.srgb", "false"}};
    FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
    REQUIRE(Sel.bSRGB == false);
  }
}

TEST_CASE("FormatSelector: normal_map option forces BC5", "[texcomp][format]")
{
  ImageIntermediate Img = CreateTestIntermediate(256, 256, "generic_texture.png");
  std::unordered_map<std::string, std::string> Opts = {{"texture.normal_map", "true"}};

  FormatSelection Sel = TextureFormatSelector::Select(Img, Opts);
  REQUIRE(Sel.Format == ECompressedFormat::BC5);
  REQUIRE(Sel.bSRGB == false);
}

// ========== Mip Generation Tests ==========

TEST_CASE("MipGenerator: CalculateMipCount for power-of-two", "[texcomp][mip]")
{
  REQUIRE(MipGenerator::CalculateMipCount(1, 1) == 1);
  REQUIRE(MipGenerator::CalculateMipCount(2, 2) == 2);
  REQUIRE(MipGenerator::CalculateMipCount(4, 4) == 3);
  REQUIRE(MipGenerator::CalculateMipCount(8, 8) == 4);
  REQUIRE(MipGenerator::CalculateMipCount(256, 256) == 9);
  REQUIRE(MipGenerator::CalculateMipCount(1024, 1024) == 11);
}

TEST_CASE("MipGenerator: CalculateMipCount for NPOT", "[texcomp][mip]")
{
  // 3x3 -> 1x1 = 2 mips (3/2 = 1, clamped to 1)
  REQUIRE(MipGenerator::CalculateMipCount(3, 3) == 2);
  // 5x5 -> 2x2 -> 1x1 = 3 mips
  REQUIRE(MipGenerator::CalculateMipCount(5, 5) == 3);
  // 7x7 -> 3x3 -> 1x1 = 3 mips
  REQUIRE(MipGenerator::CalculateMipCount(7, 7) == 3);
  // 6x6 -> 3x3 -> 1x1 = 3 mips
  REQUIRE(MipGenerator::CalculateMipCount(6, 6) == 3);
}

TEST_CASE("MipGenerator: CalculateMipCount for non-square", "[texcomp][mip]")
{
  // 8x2 -> 4x1 -> 2x1 -> 1x1 = 4 mips
  REQUIRE(MipGenerator::CalculateMipCount(8, 2) == 4);
  // 2x8 -> 1x4 -> 1x2 -> 1x1 = 4 mips
  REQUIRE(MipGenerator::CalculateMipCount(2, 8) == 4);
  // 16x4 -> 8x2 -> 4x1 -> 2x1 -> 1x1 = 5 mips
  REQUIRE(MipGenerator::CalculateMipCount(16, 4) == 5);
}

TEST_CASE("MipGenerator: Generate produces correct mip count", "[texcomp][mip]")
{
  auto Pixels = CreateTestPixels(4, 4);
  auto Chain = MipGenerator::Generate(Pixels.data(), 4, 4, 4, true);

  // 4x4 -> 2x2 -> 1x1 = 3 mips
  REQUIRE(Chain.size() == 3);
  REQUIRE(Chain[0].Width == 4);
  REQUIRE(Chain[0].Height == 4);
  REQUIRE(Chain[1].Width == 2);
  REQUIRE(Chain[1].Height == 2);
  REQUIRE(Chain[2].Width == 1);
  REQUIRE(Chain[2].Height == 1);
}

TEST_CASE("MipGenerator: Generate respects MaxMipCount", "[texcomp][mip]")
{
  auto Pixels = CreateTestPixels(16, 16);

  SECTION("limit to 2 mips")
  {
    auto Chain = MipGenerator::Generate(Pixels.data(), 16, 16, 4, true, 2);
    REQUIRE(Chain.size() == 2);
    REQUIRE(Chain[0].Width == 16);
    REQUIRE(Chain[1].Width == 8);
  }

  SECTION("limit to 1 mip")
  {
    auto Chain = MipGenerator::Generate(Pixels.data(), 16, 16, 4, true, 1);
    REQUIRE(Chain.size() == 1);
    REQUIRE(Chain[0].Width == 16);
  }

  SECTION("limit larger than actual count uses all mips")
  {
    auto Chain = MipGenerator::Generate(Pixels.data(), 4, 4, 4, true, 100);
    REQUIRE(Chain.size() == 3);
  }
}

TEST_CASE("MipGenerator: Generate correct pixel data sizes", "[texcomp][mip]")
{
  auto Pixels = CreateTestPixels(8, 8);
  auto Chain = MipGenerator::Generate(Pixels.data(), 8, 8, 4, true);

  REQUIRE(Chain[0].Pixels.size() == 8 * 8 * 4);
  REQUIRE(Chain[1].Pixels.size() == 4 * 4 * 4);
  REQUIRE(Chain[2].Pixels.size() == 2 * 2 * 4);
  REQUIRE(Chain[3].Pixels.size() == 1 * 1 * 4);
}

TEST_CASE("MipGenerator: mip 0 is a copy of source", "[texcomp][mip]")
{
  auto Pixels = CreateTestPixels(4, 4, 200, 100, 50, 255);
  auto Chain = MipGenerator::Generate(Pixels.data(), 4, 4, 4, true);

  REQUIRE(Chain[0].Pixels == Pixels);
}

TEST_CASE("MipGenerator: NPOT dimensions handled correctly", "[texcomp][mip]")
{
  // 5x3 -> 2x1 -> 1x1 = 3 mips
  auto Pixels = CreateTestPixels(5, 3);
  auto Chain = MipGenerator::Generate(Pixels.data(), 5, 3, 4, true);

  REQUIRE(Chain.size() == 3);
  REQUIRE(Chain[0].Width == 5);
  REQUIRE(Chain[0].Height == 3);
  REQUIRE(Chain[1].Width == 2);
  REQUIRE(Chain[1].Height == 1);
  REQUIRE(Chain[2].Width == 1);
  REQUIRE(Chain[2].Height == 1);
}

TEST_CASE("MipGenerator: downsampling produces averaged values", "[texcomp][mip]")
{
  // Create a 2x2 image with known values (linear mode for simpler math)
  std::vector<uint8_t> Pixels = {
      255, 0, 0, 255,   // top-left: red
      0, 255, 0, 255,   // top-right: green
      0, 0, 255, 255,   // bottom-left: blue
      255, 255, 0, 255   // bottom-right: yellow
  };

  auto Chain = MipGenerator::Generate(Pixels.data(), 2, 2, 4, false); // linear mode

  REQUIRE(Chain.size() == 2);
  REQUIRE(Chain[1].Width == 1);
  REQUIRE(Chain[1].Height == 1);

  // In linear mode, 1x1 should be average of all 4 pixels
  // R: (255+0+0+255)/4 = 127.5 -> 128
  // G: (0+255+0+255)/4 = 127.5 -> 128
  // B: (0+0+255+0)/4 = 63.75 -> 64
  // A: (255+255+255+255)/4 = 255
  REQUIRE(Chain[1].Pixels[0] == 128); // R
  REQUIRE(Chain[1].Pixels[1] == 128); // G
  REQUIRE(Chain[1].Pixels[2] == 64);  // B
  REQUIRE(Chain[1].Pixels[3] == 255); // A
}

// ========== BCn Backend Tests ==========

TEST_CASE("CompressorBackendBCn: CalculateCompressedSize correct for BC1", "[texcomp][bcn]")
{
  // BC1: 8 bytes per 4x4 block
  // 4x4 = 1 block = 8 bytes
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(4, 4, ECompressedFormat::BC1) == 8);
  // 8x8 = 4 blocks = 32 bytes
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(8, 8, ECompressedFormat::BC1) == 32);
  // 16x16 = 16 blocks = 128 bytes
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(16, 16, ECompressedFormat::BC1) == 128);
}

TEST_CASE("CompressorBackendBCn: CalculateCompressedSize correct for BC7", "[texcomp][bcn]")
{
  // BC7: 16 bytes per 4x4 block
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(4, 4, ECompressedFormat::BC7) == 16);
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(8, 8, ECompressedFormat::BC7) == 64);
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(16, 16, ECompressedFormat::BC7) == 256);
}

TEST_CASE("CompressorBackendBCn: CalculateCompressedSize handles NPOT (block-aligned)", "[texcomp][bcn]")
{
  // 5x5 -> aligns to 2x2 blocks = 4 blocks
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(5, 5, ECompressedFormat::BC7) == 4 * 16);
  // 1x1 -> 1 block
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(1, 1, ECompressedFormat::BC7) == 16);
  // 3x3 -> 1 block
  REQUIRE(CompressorBackendBCn::CalculateCompressedSize(3, 3, ECompressedFormat::BC7) == 16);
}

TEST_CASE("CompressorBackendBCn: Compress produces correctly sized output", "[texcomp][bcn]")
{
  auto Pixels = CreateTestPixels(8, 8);
  auto Result = CompressorBackendBCn::Compress(Pixels.data(), 8, 8, ECompressedFormat::BC7, 0.5f);

  REQUIRE(Result.has_value());
  REQUIRE(Result->size() == CompressorBackendBCn::CalculateCompressedSize(8, 8, ECompressedFormat::BC7));
}

TEST_CASE("CompressorBackendBCn: Compress fails on zero dimensions", "[texcomp][bcn]")
{
  auto Pixels = CreateTestPixels(4, 4);
  auto Result = CompressorBackendBCn::Compress(Pixels.data(), 0, 4, ECompressedFormat::BC7, 0.5f);
  REQUIRE_FALSE(Result.has_value());
}

TEST_CASE("CompressorBackendBCn: Compress works for all BCn formats", "[texcomp][bcn]")
{
  auto Pixels = CreateTestPixels(4, 4);

  auto ResultBC1 = CompressorBackendBCn::Compress(Pixels.data(), 4, 4, ECompressedFormat::BC1, 0.5f);
  auto ResultBC3 = CompressorBackendBCn::Compress(Pixels.data(), 4, 4, ECompressedFormat::BC3, 0.5f);
  auto ResultBC4 = CompressorBackendBCn::Compress(Pixels.data(), 4, 4, ECompressedFormat::BC4, 0.5f);
  auto ResultBC5 = CompressorBackendBCn::Compress(Pixels.data(), 4, 4, ECompressedFormat::BC5, 0.5f);
  auto ResultBC6H = CompressorBackendBCn::Compress(Pixels.data(), 4, 4, ECompressedFormat::BC6H, 0.5f);
  auto ResultBC7 = CompressorBackendBCn::Compress(Pixels.data(), 4, 4, ECompressedFormat::BC7, 0.5f);

  REQUIRE(ResultBC1.has_value());
  REQUIRE(ResultBC3.has_value());
  REQUIRE(ResultBC4.has_value());
  REQUIRE(ResultBC5.has_value());
  REQUIRE(ResultBC6H.has_value());
  REQUIRE(ResultBC7.has_value());

  // BC1/BC4: 8 bytes per block
  REQUIRE(ResultBC1->size() == 8);
  REQUIRE(ResultBC4->size() == 8);

  // BC3/BC5/BC6H/BC7: 16 bytes per block
  REQUIRE(ResultBC3->size() == 16);
  REQUIRE(ResultBC5->size() == 16);
  REQUIRE(ResultBC6H->size() == 16);
  REQUIRE(ResultBC7->size() == 16);
}

// ========== ASTC Backend Tests ==========

TEST_CASE("CompressorBackendASTC: CalculateCompressedSize correct for 4x4", "[texcomp][astc]")
{
  // 4x4 blocks, 16 bytes each
  // 4x4 image = 1 block = 16 bytes
  REQUIRE(CompressorBackendASTC::CalculateCompressedSize(4, 4, 4, 4) == 16);
  // 8x8 image = 4 blocks = 64 bytes
  REQUIRE(CompressorBackendASTC::CalculateCompressedSize(8, 8, 4, 4) == 64);
}

TEST_CASE("CompressorBackendASTC: CalculateCompressedSize correct for 6x6", "[texcomp][astc]")
{
  // 6x6 image with 6x6 blocks = 1 block = 16 bytes
  REQUIRE(CompressorBackendASTC::CalculateCompressedSize(6, 6, 6, 6) == 16);
  // 12x12 image with 6x6 blocks = 4 blocks = 64 bytes
  REQUIRE(CompressorBackendASTC::CalculateCompressedSize(12, 12, 6, 6) == 64);
}

TEST_CASE("CompressorBackendASTC: CalculateCompressedSize handles non-aligned dimensions", "[texcomp][astc]")
{
  // 5x5 with 4x4 blocks = ceil(5/4)*ceil(5/4) = 2*2 = 4 blocks = 64 bytes
  REQUIRE(CompressorBackendASTC::CalculateCompressedSize(5, 5, 4, 4) == 64);
  // 7x7 with 6x6 blocks = ceil(7/6)*ceil(7/6) = 2*2 = 4 blocks = 64 bytes
  REQUIRE(CompressorBackendASTC::CalculateCompressedSize(7, 7, 6, 6) == 64);
}

TEST_CASE("CompressorBackendASTC: Compress produces correctly sized output", "[texcomp][astc]")
{
  auto Pixels = CreateTestPixels(8, 8);
  auto Result = CompressorBackendASTC::Compress(Pixels.data(), 8, 8, 4, 4, 0.5f, false);

  REQUIRE(Result.has_value());
  REQUIRE(Result->size() == CompressorBackendASTC::CalculateCompressedSize(8, 8, 4, 4));
}

TEST_CASE("CompressorBackendASTC: Compress with various block sizes", "[texcomp][astc]")
{
  auto Pixels = CreateTestPixels(12, 12);

  auto Result4x4 = CompressorBackendASTC::Compress(Pixels.data(), 12, 12, 4, 4, 0.5f, false);
  auto Result6x6 = CompressorBackendASTC::Compress(Pixels.data(), 12, 12, 6, 6, 0.5f, false);
  auto Result8x8 = CompressorBackendASTC::Compress(Pixels.data(), 12, 12, 8, 8, 0.5f, false);

  REQUIRE(Result4x4.has_value());
  REQUIRE(Result6x6.has_value());
  REQUIRE(Result8x8.has_value());

  // 4x4: 3*3 = 9 blocks = 144 bytes
  REQUIRE(Result4x4->size() == 9 * 16);
  // 6x6: 2*2 = 4 blocks = 64 bytes
  REQUIRE(Result6x6->size() == 4 * 16);
  // 8x8: 2*2 = 4 blocks (ceil(12/8)=2) = 64 bytes
  REQUIRE(Result8x8->size() == 4 * 16);
}

TEST_CASE("CompressorBackendASTC: Compress fails on zero dimensions", "[texcomp][astc]")
{
  auto Pixels = CreateTestPixels(4, 4);
  auto Result = CompressorBackendASTC::Compress(Pixels.data(), 0, 4, 4, 4, 0.5f, false);
  REQUIRE_FALSE(Result.has_value());
}

// ========== Payload Helper Tests ==========

TEST_CASE("GetBlockDimensions returns correct values", "[texcomp]")
{
  uint32_t W, H;

  GetBlockDimensions(ECompressedFormat::BC7, W, H);
  REQUIRE(W == 4);
  REQUIRE(H == 4);

  GetBlockDimensions(ECompressedFormat::ASTC_6x6, W, H);
  REQUIRE(W == 6);
  REQUIRE(H == 6);

  GetBlockDimensions(ECompressedFormat::ASTC_8x8, W, H);
  REQUIRE(W == 8);
  REQUIRE(H == 8);

  GetBlockDimensions(ECompressedFormat::ASTC_12x12, W, H);
  REQUIRE(W == 12);
  REQUIRE(H == 12);
}

TEST_CASE("IsASTCFormat distinguishes BCn from ASTC", "[texcomp]")
{
  REQUIRE_FALSE(IsASTCFormat(ECompressedFormat::BC1));
  REQUIRE_FALSE(IsASTCFormat(ECompressedFormat::BC7));
  REQUIRE_FALSE(IsASTCFormat(ECompressedFormat::BC6H));
  REQUIRE(IsASTCFormat(ECompressedFormat::ASTC_4x4));
  REQUIRE(IsASTCFormat(ECompressedFormat::ASTC_6x6));
  REQUIRE(IsASTCFormat(ECompressedFormat::ASTC_8x8_HDR));
}

TEST_CASE("IsHDRFormat identifies HDR formats", "[texcomp]")
{
  REQUIRE_FALSE(IsHDRFormat(ECompressedFormat::BC7));
  REQUIRE_FALSE(IsHDRFormat(ECompressedFormat::ASTC_4x4));
  REQUIRE(IsHDRFormat(ECompressedFormat::BC6H));
  REQUIRE(IsHDRFormat(ECompressedFormat::ASTC_4x4_HDR));
  REQUIRE(IsHDRFormat(ECompressedFormat::ASTC_6x6_HDR));
  REQUIRE(IsHDRFormat(ECompressedFormat::ASTC_8x8_HDR));
}

TEST_CASE("GetBytesPerBlock returns correct values", "[texcomp]")
{
  REQUIRE(GetBytesPerBlock(ECompressedFormat::BC1) == 8);
  REQUIRE(GetBytesPerBlock(ECompressedFormat::BC4) == 8);
  REQUIRE(GetBytesPerBlock(ECompressedFormat::BC3) == 16);
  REQUIRE(GetBytesPerBlock(ECompressedFormat::BC5) == 16);
  REQUIRE(GetBytesPerBlock(ECompressedFormat::BC7) == 16);
  REQUIRE(GetBytesPerBlock(ECompressedFormat::ASTC_4x4) == 16);
  REQUIRE(GetBytesPerBlock(ECompressedFormat::ASTC_8x8) == 16);
}

// ========== Serializer Tests ==========

TEST_CASE("ImageIntermediate serializer round-trips correctly", "[texcomp][serializer]")
{
  auto Ser = CreateCompressorImageIntermediateSerializer();
  REQUIRE(Ser != nullptr);
  REQUIRE(Ser->GetTypeId() == Payload_CompressorImageIntermediate);

  ImageIntermediate Img = CreateTestIntermediate(16, 16, "test.png", 4, false, true, true, 8);

  std::vector<uint8_t> Bytes;
  Ser->SerializeToBytes(&Img, Bytes);
  REQUIRE_FALSE(Bytes.empty());

  ImageIntermediate Deserialized;
  bool bOk = Ser->DeserializeFromBytes(&Deserialized, Bytes.data(), Bytes.size());
  REQUIRE(bOk);
  REQUIRE(Deserialized.Width == 16);
  REQUIRE(Deserialized.Height == 16);
  REQUIRE(Deserialized.Channels == 4);
  REQUIRE(Deserialized.BitsPerChannel == 8);
  REQUIRE(Deserialized.bIsFloat == false);
  REQUIRE(Deserialized.bHasNonTrivialAlpha == true);
  REQUIRE(Deserialized.bSRGB == true);
  REQUIRE(Deserialized.SourceFilename == "test.png");
  REQUIRE(Deserialized.Pixels.size() == Img.Pixels.size());
}

TEST_CASE("CookedInfo serializer round-trips correctly", "[texcomp][serializer]")
{
  auto Ser = CreateCompressorCookedInfoSerializer();
  REQUIRE(Ser != nullptr);
  REQUIRE(Ser->GetTypeId() == Payload_CompressorCookedInfo);

  TextureCompressorCookedInfo Info;
  Info.BaseWidth = 512;
  Info.BaseHeight = 256;
  Info.MipCount = 3;
  Info.Format = ECompressedFormat::BC7;
  Info.bSRGB = true;
  Info.SourceChannels = 4;
  Info.BlockWidth = 4;
  Info.BlockHeight = 4;
  Info.MipLevels = {
      {512, 256, 4096},
      {256, 128, 1024},
      {128, 64, 256}};

  std::vector<uint8_t> Bytes;
  Ser->SerializeToBytes(&Info, Bytes);
  REQUIRE_FALSE(Bytes.empty());

  TextureCompressorCookedInfo Deserialized;
  bool bOk = Ser->DeserializeFromBytes(&Deserialized, Bytes.data(), Bytes.size());
  REQUIRE(bOk);
  REQUIRE(Deserialized.BaseWidth == 512);
  REQUIRE(Deserialized.BaseHeight == 256);
  REQUIRE(Deserialized.MipCount == 3);
  REQUIRE(Deserialized.Format == ECompressedFormat::BC7);
  REQUIRE(Deserialized.bSRGB == true);
  REQUIRE(Deserialized.SourceChannels == 4);
  REQUIRE(Deserialized.BlockWidth == 4);
  REQUIRE(Deserialized.BlockHeight == 4);
  REQUIRE(Deserialized.MipLevels.size() == 3);
  REQUIRE(Deserialized.MipLevels[0].Width == 512);
  REQUIRE(Deserialized.MipLevels[0].CompressedSize == 4096);
}

// ========== Full Pipeline (Cooker) Tests ==========

TEST_CASE("TextureCompressorCooker: CanCook matches correct types", "[texcomp][pipeline]")
{
  auto Cooker = CreateTextureCompressorCooker();

  REQUIRE(Cooker->CanCook(AssetKind_CompressedTexture, Payload_CompressorImageIntermediate));
  REQUIRE_FALSE(Cooker->CanCook(AssetKind_CompressedTexture, Payload_CompressorCookedInfo));

  TypeId RandomType = SNAPI_UUID(0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44,
                                  0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC);
  REQUIRE_FALSE(Cooker->CanCook(RandomType, Payload_CompressorImageIntermediate));
}

TEST_CASE("TextureCompressorCooker: Cook BCn produces correct bulk chunks", "[texcomp][pipeline][bcn]")
{
  auto Cooker = CreateTextureCompressorCooker();
  auto IntermediateSer = CreateCompressorImageIntermediateSerializer();
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  MockPipelineContext Ctx;
  Ctx.RegisterSerializer(IntermediateSer.get());
  Ctx.RegisterSerializer(CookedSer.get());

  // Create a 4x4 image intermediate
  ImageIntermediate Img = CreateTestIntermediate(4, 4, "test.png");

  CookRequest Req;
  Req.AssetKind = AssetKind_CompressedTexture;
  Req.LogicalName = "test.png";
  Req.Intermediate.PayloadType = Payload_CompressorImageIntermediate;
  Req.Intermediate.SchemaVersion = IntermediateSer->GetSchemaVersion();
  IntermediateSer->SerializeToBytes(&Img, Req.Intermediate.Bytes);

  CookResult Out;
  bool bOk = Cooker->Cook(Req, Out, Ctx);

  REQUIRE(bOk);

  // Should have mip levels as bulk chunks (4x4 -> 2x2 -> 1x1 = 3 mips)
  REQUIRE(Out.Bulk.size() == 3);

  // All bulk chunks should have bCompress = false (already compressed)
  for (const auto& Chunk : Out.Bulk)
  {
    REQUIRE(Chunk.bCompress == false);
    REQUIRE(Chunk.Semantic == EBulkSemantic::Reserved_Level);
    REQUIRE_FALSE(Chunk.Bytes.empty());
  }

  // SubIndex should be sequential
  REQUIRE(Out.Bulk[0].SubIndex == 0);
  REQUIRE(Out.Bulk[1].SubIndex == 1);
  REQUIRE(Out.Bulk[2].SubIndex == 2);

  // Cooked payload should be valid
  REQUIRE(Out.Cooked.PayloadType == Payload_CompressorCookedInfo);
  REQUIRE_FALSE(Out.Cooked.Bytes.empty());

  // Deserialize and verify cooked info
  TextureCompressorCookedInfo CookedInfo;
  REQUIRE(CookedSer->DeserializeFromBytes(&CookedInfo, Out.Cooked.Bytes.data(), Out.Cooked.Bytes.size()));
  REQUIRE(CookedInfo.BaseWidth == 4);
  REQUIRE(CookedInfo.BaseHeight == 4);
  REQUIRE(CookedInfo.MipCount == 3);
  REQUIRE(CookedInfo.Format == ECompressedFormat::BC7); // Default for opaque RGBA
  REQUIRE(CookedInfo.bSRGB == true);
  REQUIRE(CookedInfo.MipLevels.size() == 3);
}

TEST_CASE("TextureCompressorCooker: Cook ASTC produces correct bulk chunks", "[texcomp][pipeline][astc]")
{
  auto Cooker = CreateTextureCompressorCooker();
  auto IntermediateSer = CreateCompressorImageIntermediateSerializer();
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  MockPipelineContext Ctx;
  Ctx.RegisterSerializer(IntermediateSer.get());
  Ctx.RegisterSerializer(CookedSer.get());

  ImageIntermediate Img = CreateTestIntermediate(8, 8, "test.png");

  CookRequest Req;
  Req.AssetKind = AssetKind_CompressedTexture;
  Req.LogicalName = "test.png";
  Req.BuildOptions["texture.target"] = "astc";
  Req.Intermediate.PayloadType = Payload_CompressorImageIntermediate;
  Req.Intermediate.SchemaVersion = IntermediateSer->GetSchemaVersion();
  IntermediateSer->SerializeToBytes(&Img, Req.Intermediate.Bytes);

  CookResult Out;
  bool bOk = Cooker->Cook(Req, Out, Ctx);

  REQUIRE(bOk);

  // 8x8 -> 4x4 -> 2x2 -> 1x1 = 4 mips
  REQUIRE(Out.Bulk.size() == 4);

  for (const auto& Chunk : Out.Bulk)
  {
    REQUIRE(Chunk.bCompress == false);
    REQUIRE(Chunk.Semantic == EBulkSemantic::Reserved_Level);
  }

  // Verify cooked info
  TextureCompressorCookedInfo CookedInfo;
  REQUIRE(CookedSer->DeserializeFromBytes(&CookedInfo, Out.Cooked.Bytes.data(), Out.Cooked.Bytes.size()));
  REQUIRE(CookedInfo.Format == ECompressedFormat::ASTC_6x6); // Default for ASTC target
  REQUIRE(CookedInfo.MipCount == 4);
  REQUIRE(CookedInfo.BlockWidth == 6);
  REQUIRE(CookedInfo.BlockHeight == 6);
}

TEST_CASE("TextureCompressorCooker: BCn output sizes match format expectations", "[texcomp][pipeline][bcn]")
{
  auto Cooker = CreateTextureCompressorCooker();
  auto IntermediateSer = CreateCompressorImageIntermediateSerializer();
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  MockPipelineContext Ctx;
  Ctx.RegisterSerializer(IntermediateSer.get());
  Ctx.RegisterSerializer(CookedSer.get());

  ImageIntermediate Img = CreateTestIntermediate(8, 8, "test.png");

  CookRequest Req;
  Req.AssetKind = AssetKind_CompressedTexture;
  Req.LogicalName = "test.png";
  Req.BuildOptions["texture.format"] = "BC1"; // Force BC1
  Req.Intermediate.PayloadType = Payload_CompressorImageIntermediate;
  Req.Intermediate.SchemaVersion = IntermediateSer->GetSchemaVersion();
  IntermediateSer->SerializeToBytes(&Img, Req.Intermediate.Bytes);

  CookResult Out;
  REQUIRE(Cooker->Cook(Req, Out, Ctx));

  // Mip 0: 8x8 in BC1 = 4 blocks = 8*4 = 32 bytes
  REQUIRE(Out.Bulk[0].Bytes.size() == CompressorBackendBCn::CalculateCompressedSize(8, 8, ECompressedFormat::BC1));
  // Mip 1: 4x4 in BC1 = 1 block = 8 bytes
  REQUIRE(Out.Bulk[1].Bytes.size() == CompressorBackendBCn::CalculateCompressedSize(4, 4, ECompressedFormat::BC1));
}

TEST_CASE("TextureCompressorCooker: ASTC block size in cooked info matches format", "[texcomp][pipeline][astc]")
{
  auto Cooker = CreateTextureCompressorCooker();
  auto IntermediateSer = CreateCompressorImageIntermediateSerializer();
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  MockPipelineContext Ctx;
  Ctx.RegisterSerializer(IntermediateSer.get());
  Ctx.RegisterSerializer(CookedSer.get());

  ImageIntermediate Img = CreateTestIntermediate(16, 16, "test.png");

  CookRequest Req;
  Req.AssetKind = AssetKind_CompressedTexture;
  Req.LogicalName = "test.png";
  Req.BuildOptions["texture.target"] = "astc";
  Req.BuildOptions["texture.astc_block"] = "8x8";
  Req.Intermediate.PayloadType = Payload_CompressorImageIntermediate;
  Req.Intermediate.SchemaVersion = IntermediateSer->GetSchemaVersion();
  IntermediateSer->SerializeToBytes(&Img, Req.Intermediate.Bytes);

  CookResult Out;
  REQUIRE(Cooker->Cook(Req, Out, Ctx));

  TextureCompressorCookedInfo CookedInfo;
  REQUIRE(CookedSer->DeserializeFromBytes(&CookedInfo, Out.Cooked.Bytes.data(), Out.Cooked.Bytes.size()));
  REQUIRE(CookedInfo.Format == ECompressedFormat::ASTC_8x8);
  REQUIRE(CookedInfo.BlockWidth == 8);
  REQUIRE(CookedInfo.BlockHeight == 8);
}

TEST_CASE("TextureCompressorCooker: max_mips limits mip count", "[texcomp][pipeline]")
{
  auto Cooker = CreateTextureCompressorCooker();
  auto IntermediateSer = CreateCompressorImageIntermediateSerializer();
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  MockPipelineContext Ctx;
  Ctx.RegisterSerializer(IntermediateSer.get());
  Ctx.RegisterSerializer(CookedSer.get());

  ImageIntermediate Img = CreateTestIntermediate(16, 16, "test.png");

  CookRequest Req;
  Req.AssetKind = AssetKind_CompressedTexture;
  Req.LogicalName = "test.png";
  Req.BuildOptions["texture.max_mips"] = "2";
  Req.Intermediate.PayloadType = Payload_CompressorImageIntermediate;
  Req.Intermediate.SchemaVersion = IntermediateSer->GetSchemaVersion();
  IntermediateSer->SerializeToBytes(&Img, Req.Intermediate.Bytes);

  CookResult Out;
  REQUIRE(Cooker->Cook(Req, Out, Ctx));

  REQUIRE(Out.Bulk.size() == 2);

  TextureCompressorCookedInfo CookedInfo;
  REQUIRE(CookedSer->DeserializeFromBytes(&CookedInfo, Out.Cooked.Bytes.data(), Out.Cooked.Bytes.size()));
  REQUIRE(CookedInfo.MipCount == 2);
}

TEST_CASE("TextureCompressorCooker: tags are set correctly", "[texcomp][pipeline]")
{
  auto Cooker = CreateTextureCompressorCooker();
  auto IntermediateSer = CreateCompressorImageIntermediateSerializer();
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  MockPipelineContext Ctx;
  Ctx.RegisterSerializer(IntermediateSer.get());
  Ctx.RegisterSerializer(CookedSer.get());

  ImageIntermediate Img = CreateTestIntermediate(8, 8, "test.png");

  CookRequest Req;
  Req.AssetKind = AssetKind_CompressedTexture;
  Req.LogicalName = "test.png";
  Req.Intermediate.PayloadType = Payload_CompressorImageIntermediate;
  Req.Intermediate.SchemaVersion = IntermediateSer->GetSchemaVersion();
  IntermediateSer->SerializeToBytes(&Img, Req.Intermediate.Bytes);

  CookResult Out;
  REQUIRE(Cooker->Cook(Req, Out, Ctx));

  REQUIRE(Out.Tags.count("Texture.Width") == 1);
  REQUIRE(Out.Tags.count("Texture.Height") == 1);
  REQUIRE(Out.Tags.count("Texture.Format") == 1);
  REQUIRE(Out.Tags.count("Texture.MipCount") == 1);
  REQUIRE(Out.Tags.count("Texture.sRGB") == 1);
  REQUIRE(Out.Tags.count("Texture.BlockSize") == 1);

  REQUIRE(Out.Tags["Texture.Width"] == "8");
  REQUIRE(Out.Tags["Texture.Height"] == "8");
  REQUIRE(Out.Tags["Texture.Format"] == "BC7");
  REQUIRE(Out.Tags["Texture.sRGB"] == "true");
  REQUIRE(Out.Tags["Texture.BlockSize"] == "4x4");
}

TEST_CASE("TextureCompressorCooker: missing serializer produces error", "[texcomp][pipeline]")
{
  auto Cooker = CreateTextureCompressorCooker();

  MockPipelineContext Ctx;
  // No serializers registered

  ImageIntermediate Img = CreateTestIntermediate(4, 4);

  CookRequest Req;
  Req.AssetKind = AssetKind_CompressedTexture;
  Req.LogicalName = "test.png";
  Req.Intermediate.PayloadType = Payload_CompressorImageIntermediate;
  Req.Intermediate.Bytes = {1, 2, 3}; // Dummy data

  CookResult Out;
  bool bOk = Cooker->Cook(Req, Out, Ctx);

  REQUIRE_FALSE(bOk);
  REQUIRE_FALSE(Ctx.ErrorMessages.empty());
}

// ========== Params API Tests ==========

TEST_CASE("AssetLoadContext accepts std::any Params", "[params]")
{
  // Verify the Params field exists and works with any type
  TextureCompressorLoadParams Params;
  Params.MaxMipLevel = 3;
  Params.bStreamMips = true;

  std::any AnyParams = Params;

  // Cast back
  auto* CastedParams = std::any_cast<TextureCompressorLoadParams>(&AnyParams);
  REQUIRE(CastedParams != nullptr);
  REQUIRE(CastedParams->MaxMipLevel == 3);
  REQUIRE(CastedParams->bStreamMips == true);
}

TEST_CASE("std::any_cast returns nullptr for wrong type", "[params]")
{
  std::any AnyParams = std::string("wrong type");

  auto* CastedParams = std::any_cast<TextureCompressorLoadParams>(&AnyParams);
  REQUIRE(CastedParams == nullptr);
}

TEST_CASE("Empty std::any is handled gracefully", "[params]")
{
  std::any EmptyParams;

  REQUIRE_FALSE(EmptyParams.has_value());

  auto* CastedParams = std::any_cast<TextureCompressorLoadParams>(&EmptyParams);
  REQUIRE(CastedParams == nullptr);
}

TEST_CASE("TextureCompressorLoadParams defaults", "[params]")
{
  TextureCompressorLoadParams Params;
  REQUIRE(Params.MaxMipLevel == -1);
  REQUIRE(Params.bStreamMips == false);
}

TEST_CASE("AssetManager Load accepts Params argument", "[params]")
{
  AssetManagerConfig Config;
  AssetManager Manager(Config);

  TextureCompressorLoadParams Params;
  Params.MaxMipLevel = 2;

  // This should compile and not crash (asset won't be found, but the API accepts params)
  struct DummyAsset {};
  auto Result = Manager.Load<DummyAsset>("nonexistent.asset", std::any(Params));
  REQUIRE_FALSE(Result.has_value());
}

TEST_CASE("AssetManager Get accepts Params argument", "[params]")
{
  AssetManagerConfig Config;
  AssetManager Manager(Config);

  struct DummyAsset {};
  auto Result = Manager.Get<DummyAsset>("nonexistent.asset", std::any(42));
  REQUIRE_FALSE(Result.has_value());
}

// ========== Reference Factory Test ==========

namespace
{

// A simple CompressedTexture runtime type for testing
struct CompressedTexture
{
  uint32_t Width = 0;
  uint32_t Height = 0;
  ECompressedFormat Format = ECompressedFormat::Unknown;
  bool bSRGB = false;
  uint32_t MipCount = 0;
  uint32_t BlockWidth = 4;
  uint32_t BlockHeight = 4;
  std::vector<std::vector<uint8_t>> MipData;
};

class TextureCompressorFactory : public TAssetFactory<CompressedTexture>
{
public:
  TypeId GetCookedPayloadType() const override
  {
    return Payload_CompressorCookedInfo;
  }

protected:
  std::expected<CompressedTexture, std::string> DoLoad(const AssetLoadContext& Context) override
  {
    // Deserialize cooked info
    auto InfoResult = Context.DeserializeCooked<TextureCompressorCookedInfo>();
    if (!InfoResult.has_value())
    {
      return std::unexpected(InfoResult.error());
    }

    const auto& Info = *InfoResult;

    CompressedTexture Tex;
    Tex.Width = Info.BaseWidth;
    Tex.Height = Info.BaseHeight;
    Tex.Format = Info.Format;
    Tex.bSRGB = Info.bSRGB;
    Tex.MipCount = Info.MipCount;
    Tex.BlockWidth = Info.BlockWidth;
    Tex.BlockHeight = Info.BlockHeight;

    // Determine max mip to load from params
    int32_t MaxMipLevel = -1;
    if (Context.Params.has_value())
    {
      auto* LoadParams = std::any_cast<TextureCompressorLoadParams>(&Context.Params);
      if (LoadParams)
      {
        MaxMipLevel = LoadParams->MaxMipLevel;
      }
    }

    uint32_t MipsToLoad = Info.MipCount;
    if (MaxMipLevel >= 0 && static_cast<uint32_t>(MaxMipLevel) < MipsToLoad)
    {
      MipsToLoad = static_cast<uint32_t>(MaxMipLevel);
    }

    // Load mip bulk chunks
    for (uint32_t I = 0; I < MipsToLoad; ++I)
    {
      auto BulkResult = Context.LoadBulk(I);
      if (BulkResult.has_value())
      {
        Tex.MipData.push_back(std::move(*BulkResult));
      }
    }

    return Tex;
  }
};

} // namespace

TEST_CASE("TextureCompressorFactory: DoLoad parses CookedInfo", "[texcomp][factory]")
{
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  TextureCompressorCookedInfo Info;
  Info.BaseWidth = 256;
  Info.BaseHeight = 256;
  Info.MipCount = 3;
  Info.Format = ECompressedFormat::BC7;
  Info.bSRGB = true;
  Info.SourceChannels = 4;
  Info.BlockWidth = 4;
  Info.BlockHeight = 4;
  Info.MipLevels = {
      {256, 256, 1024},
      {128, 128, 256},
      {64, 64, 64}};

  TypedPayload CookedPayload;
  CookedPayload.PayloadType = Payload_CompressorCookedInfo;
  CookedPayload.SchemaVersion = CookedSer->GetSchemaVersion();
  CookedSer->SerializeToBytes(&Info, CookedPayload.Bytes);

  // Prepare bulk data
  std::vector<std::vector<uint8_t>> BulkData = {
      std::vector<uint8_t>(1024, 0xAA),
      std::vector<uint8_t>(256, 0xBB),
      std::vector<uint8_t>(64, 0xCC)};

  AssetInfo DummyInfo;
  PayloadRegistry Registry;
  Registry.Register(CreateCompressorCookedInfoSerializer());

  AssetLoadContext LoadCtx{
      .Cooked = CookedPayload,
      .Info = DummyInfo,
      .LoadBulk = [&](uint32_t Idx) -> std::expected<std::vector<uint8_t>, std::string> {
        if (Idx < BulkData.size())
          return BulkData[Idx];
        return std::unexpected("Bulk chunk not found");
      },
      .GetBulkInfo = [](uint32_t) -> std::expected<AssetPackReader::BulkChunkInfo, std::string> { return std::unexpected("not implemented"); },
      .Registry = Registry,
      .Params = {}};

  TextureCompressorFactory Factory;
  auto Result = Factory.Load(LoadCtx);

  REQUIRE(Result.has_value());

  auto* Tex = static_cast<CompressedTexture*>(Result->get());
  REQUIRE(Tex->Width == 256);
  REQUIRE(Tex->Height == 256);
  REQUIRE(Tex->Format == ECompressedFormat::BC7);
  REQUIRE(Tex->bSRGB == true);
  REQUIRE(Tex->MipCount == 3);
  REQUIRE(Tex->MipData.size() == 3);
  REQUIRE(Tex->MipData[0].size() == 1024);
  REQUIRE(Tex->MipData[1].size() == 256);
  REQUIRE(Tex->MipData[2].size() == 64);
}

TEST_CASE("TextureCompressorFactory: MaxMipLevel param limits loaded mips", "[texcomp][factory][params]")
{
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  TextureCompressorCookedInfo Info;
  Info.BaseWidth = 64;
  Info.BaseHeight = 64;
  Info.MipCount = 4;
  Info.Format = ECompressedFormat::ASTC_6x6;
  Info.bSRGB = true;
  Info.BlockWidth = 6;
  Info.BlockHeight = 6;
  Info.MipLevels = {
      {64, 64, 512},
      {32, 32, 128},
      {16, 16, 32},
      {8, 8, 16}};

  TypedPayload CookedPayload;
  CookedPayload.PayloadType = Payload_CompressorCookedInfo;
  CookedPayload.SchemaVersion = CookedSer->GetSchemaVersion();
  CookedSer->SerializeToBytes(&Info, CookedPayload.Bytes);

  std::vector<std::vector<uint8_t>> BulkData = {
      std::vector<uint8_t>(512, 0x11),
      std::vector<uint8_t>(128, 0x22),
      std::vector<uint8_t>(32, 0x33),
      std::vector<uint8_t>(16, 0x44)};

  AssetInfo DummyInfo;
  PayloadRegistry Registry;
  Registry.Register(CreateCompressorCookedInfoSerializer());

  TextureCompressorLoadParams Params;
  Params.MaxMipLevel = 2; // Only load first 2 mips

  AssetLoadContext LoadCtx{
      .Cooked = CookedPayload,
      .Info = DummyInfo,
      .LoadBulk = [&](uint32_t Idx) -> std::expected<std::vector<uint8_t>, std::string> {
        if (Idx < BulkData.size())
          return BulkData[Idx];
        return std::unexpected("Bulk chunk not found");
      },
      .GetBulkInfo = [](uint32_t) -> std::expected<AssetPackReader::BulkChunkInfo, std::string> { return std::unexpected("not implemented"); },
      .Registry = Registry,
      .Params = std::any(Params)};

  TextureCompressorFactory Factory;
  auto Result = Factory.Load(LoadCtx);

  REQUIRE(Result.has_value());

  auto* Tex = static_cast<CompressedTexture*>(Result->get());
  REQUIRE(Tex->MipCount == 4); // Info still shows 4
  REQUIRE(Tex->MipData.size() == 2); // But only 2 loaded
  REQUIRE(Tex->MipData[0].size() == 512);
  REQUIRE(Tex->MipData[1].size() == 128);
}

TEST_CASE("TextureCompressorFactory: wrong Params type is ignored gracefully", "[texcomp][factory][params]")
{
  auto CookedSer = CreateCompressorCookedInfoSerializer();

  TextureCompressorCookedInfo Info;
  Info.BaseWidth = 32;
  Info.BaseHeight = 32;
  Info.MipCount = 2;
  Info.Format = ECompressedFormat::BC7;
  Info.bSRGB = true;
  Info.BlockWidth = 4;
  Info.BlockHeight = 4;
  Info.MipLevels = {{32, 32, 256}, {16, 16, 64}};

  TypedPayload CookedPayload;
  CookedPayload.PayloadType = Payload_CompressorCookedInfo;
  CookedPayload.SchemaVersion = CookedSer->GetSchemaVersion();
  CookedSer->SerializeToBytes(&Info, CookedPayload.Bytes);

  std::vector<std::vector<uint8_t>> BulkData = {
      std::vector<uint8_t>(256, 0xAA),
      std::vector<uint8_t>(64, 0xBB)};

  AssetInfo DummyInfo;
  PayloadRegistry Registry;
  Registry.Register(CreateCompressorCookedInfoSerializer());

  // Pass wrong type as params - should be ignored, all mips loaded
  AssetLoadContext LoadCtx{
      .Cooked = CookedPayload,
      .Info = DummyInfo,
      .LoadBulk = [&](uint32_t Idx) -> std::expected<std::vector<uint8_t>, std::string> {
        if (Idx < BulkData.size())
          return BulkData[Idx];
        return std::unexpected("Bulk chunk not found");
      },
      .GetBulkInfo = [](uint32_t) -> std::expected<AssetPackReader::BulkChunkInfo, std::string> { return std::unexpected("not implemented"); },
      .Registry = Registry,
      .Params = std::any(std::string("wrong type"))};

  TextureCompressorFactory Factory;
  auto Result = Factory.Load(LoadCtx);

  REQUIRE(Result.has_value());

  auto* Tex = static_cast<CompressedTexture*>(Result->get());
  REQUIRE(Tex->MipData.size() == 2); // All mips loaded despite wrong param type
}
