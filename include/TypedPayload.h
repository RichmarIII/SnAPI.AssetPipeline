#pragma once

#include <cstdint>
#include <vector>

#include "Export.h"
#include "Uuid.h"

namespace SnAPI::AssetPipeline
{

struct SNAPI_ASSETPIPELINE_API TypedPayload
{
    TypeId PayloadType;
    uint32_t SchemaVersion = 0;
    std::vector<uint8_t> Bytes;

    TypedPayload() = default;

    TypedPayload(TypeId Type, uint32_t Version)
        : PayloadType(Type)
        , SchemaVersion(Version)
    {
    }

    TypedPayload(TypeId Type, uint32_t Version, std::vector<uint8_t> Data)
        : PayloadType(Type)
        , SchemaVersion(Version)
        , Bytes(std::move(Data))
    {
    }

    bool IsEmpty() const noexcept
    {
        return Bytes.empty();
    }

    void Clear()
    {
        PayloadType = {};
        SchemaVersion = 0;
        Bytes.clear();
    }
};

} // namespace AssetPipeline
