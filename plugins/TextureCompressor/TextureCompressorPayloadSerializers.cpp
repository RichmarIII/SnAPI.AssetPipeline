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
  uint32_t GetSchemaVersion() const override { return 1; }

  void SerializeToBytes(const void* Object, std::vector<uint8_t>& OutBytes) const override
  {
    CerealSerialize(*static_cast<const TextureCompressorCookedInfo*>(Object), OutBytes);
  }

  bool DeserializeFromBytes(void* Object, const uint8_t* Bytes, std::size_t Size) const override
  {
    return CerealDeserialize(*static_cast<TextureCompressorCookedInfo*>(Object), Bytes, Size);
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
