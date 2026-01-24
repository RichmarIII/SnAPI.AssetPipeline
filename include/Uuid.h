#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "Export.h"

namespace SnAPI::AssetPipeline
{

struct SNAPI_ASSETPIPELINE_API Uuid
{
    uint8_t Bytes[16] = {};

    constexpr Uuid() noexcept = default;

    constexpr Uuid(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
                   uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7,
                   uint8_t b8, uint8_t b9, uint8_t b10, uint8_t b11,
                   uint8_t b12, uint8_t b13, uint8_t b14, uint8_t b15) noexcept
        : Bytes{b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15}
    {
    }

    constexpr bool operator==(const Uuid& Other) const noexcept
    {
        for (int i = 0; i < 16; ++i)
        {
            if (Bytes[i] != Other.Bytes[i])
                return false;
        }
        return true;
    }

    constexpr bool operator!=(const Uuid& Other) const noexcept
    {
        return !(*this == Other);
    }

    constexpr bool operator<(const Uuid& Other) const noexcept
    {
        for (int i = 0; i < 16; ++i)
        {
            if (Bytes[i] < Other.Bytes[i])
                return true;
            if (Bytes[i] > Other.Bytes[i])
                return false;
        }
        return false;
    }

    constexpr bool IsNull() const noexcept
    {
        for (int i = 0; i < 16; ++i)
        {
            if (Bytes[i] != 0)
                return false;
        }
        return true;
    }

    // Convert to string representation (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
    std::string ToString() const;

    // Parse from string representation
    static Uuid FromString(const std::string& Str);

    // Generate a random UUID (v4)
    static Uuid Generate();

    // Generate a deterministic UUID (v5) from namespace and name
    static Uuid GenerateV5(const Uuid& Namespace, const std::string& Name);
};

// Convenient type aliases
using TypeId = Uuid;
using AssetId = Uuid;

// Hash functor for use in unordered containers (header-only, no dependencies)
struct UuidHash
{
    std::size_t operator()(const Uuid& Id) const noexcept
    {
        uint64_t A = 0, B = 0;
        for (int i = 0; i < 8; ++i)
        {
            A |= static_cast<uint64_t>(Id.Bytes[i]) << (i * 8);
        }
        for (int i = 0; i < 8; ++i)
        {
            B |= static_cast<uint64_t>(Id.Bytes[8 + i]) << (i * 8);
        }
        // FNV-1a inspired mixing
        return static_cast<std::size_t>(A ^ (B + 0x9e3779b97f4a7c15ULL + (A << 6) + (A >> 2)));
    }
};

// Macro for defining constexpr UUIDs
#define SNAPI_UUID(b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,b10,b11,b12,b13,b14,b15) \
    ::SnAPI::AssetPipeline::Uuid(b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,b10,b11,b12,b13,b14,b15)

} // namespace AssetPipeline
