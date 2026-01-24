#include "TexturePluginIds.h"
#include "TexturePluginPayloads.h"
#include "IPayloadSerializer.h"

#include <sstream>
#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>

using namespace SnAPI::AssetPipeline;

namespace TexturePlugin
{

// cereal serialization functions
template <class Archive>
void serialize(Archive& Ar, ImageIntermediate& V)
{
    Ar(V.Width, V.Height, V.Channels, V.Pixels);
}

template <class Archive>
void serialize(Archive& Ar, TextureCookedInfo& V)
{
    Ar(V.Width, V.Height, V.MipCount, V.Format);
}

// Helper functions
template <typename T>
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

template <typename T>
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
class ImageIntermediateSerializer final : public IPayloadSerializer
{
public:
    TypeId GetTypeId() const override
    {
        return Payload_ImageIntermediate;
    }

    const char* GetTypeName() const override
    {
        return "TexturePlugin.ImageIntermediate";
    }

    uint32_t GetSchemaVersion() const override
    {
        return 1;
    }

    void SerializeToBytes(const void* Object, std::vector<uint8_t>& OutBytes) const override
    {
        CerealSerialize(*static_cast<const ImageIntermediate*>(Object), OutBytes);
    }

    bool DeserializeFromBytes(void* Object, const uint8_t* Bytes, std::size_t Size) const override
    {
        return CerealDeserialize(*static_cast<ImageIntermediate*>(Object), Bytes, Size);
    }
};

// TextureCookedInfo serializer
class TextureCookedInfoSerializer final : public IPayloadSerializer
{
public:
    TypeId GetTypeId() const override
    {
        return Payload_TextureCookedInfo;
    }

    const char* GetTypeName() const override
    {
        return "TexturePlugin.TextureCookedInfo";
    }

    uint32_t GetSchemaVersion() const override
    {
        return 1;
    }

    void SerializeToBytes(const void* Object, std::vector<uint8_t>& OutBytes) const override
    {
        CerealSerialize(*static_cast<const TextureCookedInfo*>(Object), OutBytes);
    }

    bool DeserializeFromBytes(void* Object, const uint8_t* Bytes, std::size_t Size) const override
    {
        return CerealDeserialize(*static_cast<TextureCookedInfo*>(Object), Bytes, Size);
    }
};

// Factory functions
std::unique_ptr<IPayloadSerializer> CreateImageIntermediateSerializer()
{
    return std::make_unique<ImageIntermediateSerializer>();
}

std::unique_ptr<IPayloadSerializer> CreateTextureCookedInfoSerializer()
{
    return std::make_unique<TextureCookedInfoSerializer>();
}

} // namespace TexturePlugin
