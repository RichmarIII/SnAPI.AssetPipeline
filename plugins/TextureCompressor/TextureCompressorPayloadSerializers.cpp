#include "TextureCompressorIds.h"
#include "TextureCompressorPayloads.h"
#include "TextureCompressorPayloadSerializers.h"
#include "IPayloadSerializer.h"

#include <sstream>
#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

using namespace SnAPI::AssetPipeline;

namespace TextureCompressorPlugin
{

// cereal serialization functions
template<class Archive>
void serialize(Archive& Ar, ImageIntermediate& V)
{
  Ar(V.Width, V.Height, V.Channels, V.BitsPerChannel,
     V.bIsFloat, V.bHasNonTrivialAlpha, V.bSRGB,
     V.SourceFilename, V.Pixels);
}

template<class Archive>
void serialize(Archive& Ar, MipLevelInfo& V)
{
  Ar(V.Width, V.Height, V.CompressedSize);
}

template<class Archive>
void serialize(Archive& Ar, TextureCompressorCookedInfo& V)
{
  Ar(V.BaseWidth, V.BaseHeight, V.MipCount, V.Format,
     V.RequestedTarget, V.RequestedFormat, V.RequestedQuality,
     V.bSRGB, V.SourceChannels, V.BlockWidth, V.BlockHeight,
     V.MipLevels);
}

// Helper functions
template<typename T>
void CerealSerialize(const T& Obj, std::vector<uint8_t>& Out)
{
  std::ostringstream Os(std::ios::binary);
  {
    cereal::BinaryOutputArchive Ar(Os);
    Ar(Obj);
  }
  auto Str = Os.str();
  Out.assign(Str.begin(), Str.end());
}

template<typename T>
bool CerealDeserialize(T& Obj, const uint8_t* Bytes, std::size_t Size)
{
  try
  {
    std::string Str(reinterpret_cast<const char*>(Bytes), Size);
    std::istringstream Is(Str, std::ios::binary);
    cereal::BinaryInputArchive Ar(Is);
    Ar(Obj);
    return true;
  }
  catch (...)
  {
    return false;
  }
}

// ImageIntermediate serializer
class CompressorImageIntermediateSerializer final : public IPayloadSerializer
{
public:
  TypeId GetTypeId() const override { return Payload_CompressorImageIntermediate; }
  const char* GetTypeName() const override { return "TextureCompressor.ImageIntermediate"; }
  uint32_t GetSchemaVersion() const override { return 1; }

  void SerializeToBytes(const void* Object, std::vector<uint8_t>& OutBytes) const override
  {
    CerealSerialize(*static_cast<const ImageIntermediate*>(Object), OutBytes);
  }

  bool DeserializeFromBytes(void* Object, const uint8_t* Bytes, std::size_t Size) const override
  {
    return CerealDeserialize(*static_cast<ImageIntermediate*>(Object), Bytes, Size);
  }
};

// TextureCompressorCookedInfo serializer
class CompressorCookedInfoSerializer final : public IPayloadSerializer
{
public:
  TypeId GetTypeId() const override { return Payload_CompressorCookedInfo; }
  const char* GetTypeName() const override { return "TextureCompressor.CookedInfo"; }
  uint32_t GetSchemaVersion() const override { return 2; }

  void SerializeToBytes(const void* Object, std::vector<uint8_t>& OutBytes) const override
  {
    CerealSerialize(*static_cast<const TextureCompressorCookedInfo*>(Object), OutBytes);
  }

  bool DeserializeFromBytes(void* Object, const uint8_t* Bytes, std::size_t Size) const override
  {
    return CerealDeserialize(*static_cast<TextureCompressorCookedInfo*>(Object), Bytes, Size);
  }

  bool MigrateBytes(const uint32_t FromVersion, const uint32_t ToVersion, std::vector<uint8_t>& InOutBytes) const override
  {
    if (FromVersion == ToVersion)
    {
      return true;
    }
    if (FromVersion != 1u || ToVersion != 2u)
    {
      return false;
    }

    struct LegacyCookedInfoV1
    {
      uint32_t BaseWidth = 0;
      uint32_t BaseHeight = 0;
      uint32_t MipCount = 0;
      ECompressedFormat Format = ECompressedFormat::Unknown;
      bool bSRGB = true;
      uint32_t SourceChannels = 4;
      uint32_t BlockWidth = 4;
      uint32_t BlockHeight = 4;
      std::vector<MipLevelInfo> MipLevels{};
    };

    auto DeserializeLegacy = [](LegacyCookedInfoV1& Out, const uint8_t* Bytes, std::size_t Size) -> bool {
      try
      {
        std::string Str(reinterpret_cast<const char*>(Bytes), Size);
        std::istringstream Is(Str, std::ios::binary);
        cereal::BinaryInputArchive Ar(Is);
        Ar(Out.BaseWidth, Out.BaseHeight, Out.MipCount, Out.Format,
           Out.bSRGB, Out.SourceChannels, Out.BlockWidth, Out.BlockHeight,
           Out.MipLevels);
        return true;
      }
      catch (...)
      {
        return false;
      }
    };

    LegacyCookedInfoV1 Legacy{};
    if (!DeserializeLegacy(Legacy, InOutBytes.data(), InOutBytes.size()))
    {
      return false;
    }

    TextureCompressorCookedInfo Upgraded{};
    Upgraded.BaseWidth = Legacy.BaseWidth;
    Upgraded.BaseHeight = Legacy.BaseHeight;
    Upgraded.MipCount = Legacy.MipCount;
    Upgraded.Format = Legacy.Format;
    Upgraded.RequestedTarget = IsASTCFormat(Legacy.Format) ? ECompressionTarget::ASTC : ECompressionTarget::BCn;
    Upgraded.RequestedFormat = Legacy.Format;
    Upgraded.RequestedQuality = 0.5f;
    Upgraded.bSRGB = Legacy.bSRGB;
    Upgraded.SourceChannels = Legacy.SourceChannels;
    Upgraded.BlockWidth = Legacy.BlockWidth;
    Upgraded.BlockHeight = Legacy.BlockHeight;
    Upgraded.MipLevels = std::move(Legacy.MipLevels);

    CerealSerialize(Upgraded, InOutBytes);
    return true;
  }
};

// Factory functions
std::unique_ptr<IPayloadSerializer> CreateCompressorImageIntermediateSerializer()
{
  return std::make_unique<CompressorImageIntermediateSerializer>();
}

std::unique_ptr<IPayloadSerializer> CreateCompressorCookedInfoSerializer()
{
  return std::make_unique<CompressorCookedInfoSerializer>();
}

} // namespace TextureCompressorPlugin
