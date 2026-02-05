#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Export.h"
#include "Uuid.h"
#include "TypedPayload.h"
#include "IAssetImporter.h"
#include "PackCompression.h"

namespace SnAPI::AssetPipeline
{

class IPipelineContext;

enum class EBulkSemantic : uint32_t
{
    Unknown = 0,
    Reserved_Level = 1,     // Mip/LOD-like layers
    Reserved_Aux = 2,       // Auxiliary streams
    // Plugins may use 0x10000+ for custom semantics
};

struct SNAPI_ASSETPIPELINE_API BulkChunk
{
    EBulkSemantic Semantic = EBulkSemantic::Unknown;
    uint32_t SubIndex = 0;
    bool bCompress = true;
    std::vector<uint8_t> Bytes;
    std::optional<EPackCompression> CompressionOverride;
    std::optional<EPackCompressionLevel> CompressionLevelOverride;

    BulkChunk() = default;
    BulkChunk(EBulkSemantic InSemantic, uint32_t InSubIndex, bool InCompress = true)
        : Semantic(InSemantic)
        , SubIndex(InSubIndex)
        , bCompress(InCompress)
    {
    }
};

struct SNAPI_ASSETPIPELINE_API CookRequest
{
    AssetId Id;
    std::string LogicalName;
    TypeId AssetKind;
    std::string VariantKey;

    TypedPayload Intermediate;
    std::vector<SourceRef> Dependencies;

    std::unordered_map<std::string, std::string> BuildOptions;

    CookRequest() = default;
};

struct SNAPI_ASSETPIPELINE_API CookResult
{
    TypedPayload Cooked;
    std::vector<BulkChunk> Bulk;
    std::vector<SourceRef> Dependencies;
    std::unordered_map<std::string, std::string> Tags;

    CookResult() = default;
};

class SNAPI_ASSETPIPELINE_API IAssetCooker
{
public:
    virtual ~IAssetCooker() = default;

    // Returns a unique name for this cooker
    [[nodiscard]] virtual const char* GetName() const = 0;

    // Returns true if this cooker can handle the given asset kind and intermediate type
    virtual bool CanCook(TypeId AssetKind, TypeId IntermediatePayloadType) const = 0;

    // Cook the intermediate payload into a cooked payload
    virtual bool Cook(const CookRequest& Req, CookResult& Out, IPipelineContext& Ctx) = 0;
};

} // namespace AssetPipeline
