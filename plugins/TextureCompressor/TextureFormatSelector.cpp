#include "TextureFormatSelector.h"

#include <algorithm>
#include <cctype>

namespace TextureCompressorPlugin
{

namespace
{

std::string ToLower(const std::string& S)
{
  std::string Result = S;
  std::transform(Result.begin(), Result.end(), Result.begin(),
                 [](unsigned char C) { return std::tolower(C); });
  return Result;
}

std::string GetOption(const std::unordered_map<std::string, std::string>& Opts,
                      const std::string& Key, const std::string& Default = "")
{
  auto It = Opts.find(Key);
  if (It != Opts.end())
    return It->second;
  return Default;
}

float GetFloatOption(const std::unordered_map<std::string, std::string>& Opts,
                     const std::string& Key, float Default = 0.5f)
{
  auto It = Opts.find(Key);
  if (It != Opts.end())
  {
    try { return std::stof(It->second); }
    catch (...) {}
  }
  return Default;
}

} // namespace

bool TextureFormatSelector::IsNormalMapFilename(const std::string& Filename)
{
  std::string Lower = ToLower(Filename);
  return Lower.find("_normal") != std::string::npos ||
         Lower.find("_n.") != std::string::npos ||
         Lower.find("_nrm") != std::string::npos ||
         Lower.find("_bump") != std::string::npos ||
         Lower.find("_nor.") != std::string::npos ||
         Lower.find("normalmap") != std::string::npos;
}

bool TextureFormatSelector::IsHDRFilename(const std::string& Filename)
{
  std::string Lower = ToLower(Filename);
  auto DotPos = Lower.find_last_of('.');
  if (DotPos == std::string::npos)
    return false;
  std::string Ext = Lower.substr(DotPos + 1);
  return Ext == "exr" || Ext == "hdr" || Ext == "rgbm";
}

std::string TextureFormatSelector::GetTarget(const std::unordered_map<std::string, std::string>& Options)
{
  return ToLower(GetOption(Options, "texture.target", "bcn"));
}

bool TextureFormatSelector::TryParseFormatOverride(
    const std::unordered_map<std::string, std::string>& Options,
    ECompressedFormat& OutFormat)
{
  std::string FmtStr = ToLower(GetOption(Options, "texture.format"));
  if (FmtStr.empty())
    return false;

  if (FmtStr == "bc1") { OutFormat = ECompressedFormat::BC1; return true; }
  if (FmtStr == "bc3") { OutFormat = ECompressedFormat::BC3; return true; }
  if (FmtStr == "bc4") { OutFormat = ECompressedFormat::BC4; return true; }
  if (FmtStr == "bc5") { OutFormat = ECompressedFormat::BC5; return true; }
  if (FmtStr == "bc6h") { OutFormat = ECompressedFormat::BC6H; return true; }
  if (FmtStr == "bc7") { OutFormat = ECompressedFormat::BC7; return true; }
  if (FmtStr == "astc_4x4") { OutFormat = ECompressedFormat::ASTC_4x4; return true; }
  if (FmtStr == "astc_5x5") { OutFormat = ECompressedFormat::ASTC_5x5; return true; }
  if (FmtStr == "astc_6x6") { OutFormat = ECompressedFormat::ASTC_6x6; return true; }
  if (FmtStr == "astc_8x8") { OutFormat = ECompressedFormat::ASTC_8x8; return true; }
  if (FmtStr == "astc_10x10") { OutFormat = ECompressedFormat::ASTC_10x10; return true; }
  if (FmtStr == "astc_12x12") { OutFormat = ECompressedFormat::ASTC_12x12; return true; }
  if (FmtStr == "astc_4x4_hdr") { OutFormat = ECompressedFormat::ASTC_4x4_HDR; return true; }
  if (FmtStr == "astc_6x6_hdr") { OutFormat = ECompressedFormat::ASTC_6x6_HDR; return true; }
  if (FmtStr == "astc_8x8_hdr") { OutFormat = ECompressedFormat::ASTC_8x8_HDR; return true; }

  return false;
}

FormatSelection TextureFormatSelector::SelectBCn(
    const ImageIntermediate& Image,
    const std::unordered_map<std::string, std::string>& Options)
{
  FormatSelection Sel;
  Sel.Quality = GetFloatOption(Options, "texture.quality", 0.5f);

  // Check sRGB override
  std::string SrgbOpt = GetOption(Options, "texture.srgb");
  bool bForceSRGB = (SrgbOpt == "true");
  bool bForceLinear = (SrgbOpt == "false");

  // Check normal map override
  std::string NormalOpt = GetOption(Options, "texture.normal_map");
  bool bIsNormal = (NormalOpt == "true") || IsNormalMapFilename(Image.SourceFilename);

  // HDR detection
  bool bIsHDR = Image.bIsFloat || Image.BitsPerChannel > 8 || IsHDRFilename(Image.SourceFilename);

  if (bIsHDR)
  {
    Sel.Format = ECompressedFormat::BC6H;
    Sel.bSRGB = false;
  }
  else if (bIsNormal)
  {
    Sel.Format = ECompressedFormat::BC5;
    Sel.bSRGB = false;
  }
  else if (Image.Channels == 1)
  {
    Sel.Format = ECompressedFormat::BC4;
    Sel.bSRGB = bForceSRGB;
  }
  else if (Image.Channels == 2)
  {
    Sel.Format = ECompressedFormat::BC5;
    Sel.bSRGB = false;
  }
  else if (Image.bHasNonTrivialAlpha)
  {
    Sel.Format = ECompressedFormat::BC7;
    Sel.bSRGB = true;
  }
  else
  {
    // Opaque RGB/RGBA
    Sel.Format = ECompressedFormat::BC7;
    Sel.bSRGB = true;
  }

  // Apply sRGB overrides
  if (bForceSRGB) Sel.bSRGB = true;
  if (bForceLinear) Sel.bSRGB = false;

  return Sel;
}

FormatSelection TextureFormatSelector::SelectASTC(
    const ImageIntermediate& Image,
    const std::unordered_map<std::string, std::string>& Options)
{
  FormatSelection Sel;
  Sel.Quality = GetFloatOption(Options, "texture.quality", 0.5f);

  // Check sRGB override
  std::string SrgbOpt = GetOption(Options, "texture.srgb");
  bool bForceSRGB = (SrgbOpt == "true");
  bool bForceLinear = (SrgbOpt == "false");

  // Check normal map override
  std::string NormalOpt = GetOption(Options, "texture.normal_map");
  bool bIsNormal = (NormalOpt == "true") || IsNormalMapFilename(Image.SourceFilename);

  // HDR detection
  bool bIsHDR = Image.bIsFloat || Image.BitsPerChannel > 8 || IsHDRFilename(Image.SourceFilename);

  // ASTC block size override
  std::string BlockOpt = GetOption(Options, "texture.astc_block");

  if (bIsHDR)
  {
    Sel.Format = ECompressedFormat::ASTC_6x6_HDR;
    Sel.bSRGB = false;

    // Block size override for HDR
    if (BlockOpt == "4x4") Sel.Format = ECompressedFormat::ASTC_4x4_HDR;
    else if (BlockOpt == "8x8") Sel.Format = ECompressedFormat::ASTC_8x8_HDR;
  }
  else if (bIsNormal)
  {
    Sel.Format = ECompressedFormat::ASTC_4x4; // Highest quality for normals
    Sel.bSRGB = false;
  }
  else
  {
    // Default: ASTC_6x6 for good quality/size balance
    Sel.Format = ECompressedFormat::ASTC_6x6;
    Sel.bSRGB = true;

    // Block size override for LDR
    if (BlockOpt == "4x4") Sel.Format = ECompressedFormat::ASTC_4x4;
    else if (BlockOpt == "5x5") Sel.Format = ECompressedFormat::ASTC_5x5;
    else if (BlockOpt == "8x8") Sel.Format = ECompressedFormat::ASTC_8x8;
    else if (BlockOpt == "10x10") Sel.Format = ECompressedFormat::ASTC_10x10;
    else if (BlockOpt == "12x12") Sel.Format = ECompressedFormat::ASTC_12x12;
  }

  // Apply sRGB overrides
  if (bForceSRGB) Sel.bSRGB = true;
  if (bForceLinear) Sel.bSRGB = false;

  return Sel;
}

FormatSelection TextureFormatSelector::Select(
    const ImageIntermediate& Image,
    const std::unordered_map<std::string, std::string>& BuildOptions)
{
  // Check for explicit format override first
  ECompressedFormat OverrideFormat;
  if (TryParseFormatOverride(BuildOptions, OverrideFormat))
  {
    FormatSelection Sel;
    Sel.Format = OverrideFormat;
    Sel.Quality = GetFloatOption(BuildOptions, "texture.quality", 0.5f);

    // Determine sRGB based on format type
    Sel.bSRGB = !IsHDRFormat(OverrideFormat);
    std::string SrgbOpt = GetOption(BuildOptions, "texture.srgb");
    if (SrgbOpt == "true") Sel.bSRGB = true;
    else if (SrgbOpt == "false") Sel.bSRGB = false;

    return Sel;
  }

  // Select based on target family
  std::string Target = GetTarget(BuildOptions);
  if (Target == "astc")
  {
    return SelectASTC(Image, BuildOptions);
  }

  return SelectBCn(Image, BuildOptions);
}

} // namespace TextureCompressorPlugin
