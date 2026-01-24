#pragma once

#include <cstdint>
#include <vector>

#include "Export.h"
#include "Uuid.h"

namespace SnAPI::AssetPipeline
{

class SNAPI_ASSETPIPELINE_API IPayloadSerializer
{
public:
    virtual ~IPayloadSerializer() = default;

    // Returns the TypeId this serializer handles
    virtual TypeId GetTypeId() const = 0;

    // Returns a human-readable name for the type
    virtual const char* GetTypeName() const = 0;

    // Returns the current schema version
    virtual uint32_t GetSchemaVersion() const = 0;

    // Serialize an object to bytes
    virtual void SerializeToBytes(const void* Object, std::vector<uint8_t>& OutBytes) const = 0;

    // Deserialize bytes to an object
    virtual bool DeserializeFromBytes(void* Object, const uint8_t* Bytes, std::size_t Size) const = 0;

    // Optional: Migrate bytes from one schema version to another
    // Returns false if migration is not supported or fails
    virtual bool MigrateBytes(uint32_t FromVersion, uint32_t ToVersion, std::vector<uint8_t>& InOutBytes) const
    {
        (void)FromVersion;
        (void)ToVersion;
        (void)InOutBytes;
        return false;
    }
};

} // namespace AssetPipeline
