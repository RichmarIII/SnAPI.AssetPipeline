#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Export.h"
#include "Uuid.h"
#include "IPayloadSerializer.h"

namespace SnAPI::AssetPipeline
{

class SNAPI_ASSETPIPELINE_API IPipelineContext
{
public:
    virtual ~IPipelineContext() = default;

    // Logging
    virtual void LogInfo(const char* Fmt, ...) = 0;
    virtual void LogWarn(const char* Fmt, ...) = 0;
    virtual void LogError(const char* Fmt, ...) = 0;

    // File I/O
    virtual bool ReadAllBytes(const std::string& Uri, std::vector<uint8_t>& Out) = 0;

    // Hashing
    virtual uint64_t HashBytes64(const void* Data, std::size_t Size) = 0;
    virtual void HashBytes128(const void* Data, std::size_t Size, uint64_t& OutHi, uint64_t& OutLo) = 0;

    // Asset ID generation
    virtual AssetId MakeDeterministicAssetId(std::string_view LogicalName, std::string_view VariantKey) = 0;

    // Serialization
    virtual const IPayloadSerializer* FindSerializer(TypeId Id) const = 0;

    // Build options
    virtual std::string GetOption(std::string_view Key, std::string_view Default = {}) const = 0;
};

} // namespace AssetPipeline
