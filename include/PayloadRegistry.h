#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "Export.h"
#include "Uuid.h"
#include "IPayloadSerializer.h"

namespace SnAPI::AssetPipeline
{

class SNAPI_ASSETPIPELINE_API PayloadRegistry
{
public:
    PayloadRegistry();
    ~PayloadRegistry();

    // Register a serializer. Must be called before Freeze().
    void Register(std::unique_ptr<IPayloadSerializer> Serializer);

    // Freeze the registry for lock-free reads. No more registrations allowed.
    void Freeze();

    // Check if registry is frozen
    bool IsFrozen() const;

    // Find a serializer by TypeId. Thread-safe after Freeze().
    const IPayloadSerializer* Find(TypeId Id) const;

    // Find a serializer by type name. Thread-safe after Freeze().
    const IPayloadSerializer* FindByName(const char* TypeName) const;

    // Get all registered serializers
    const std::vector<IPayloadSerializer*>& GetAll() const;

    // Non-copyable
    PayloadRegistry(const PayloadRegistry&) = delete;
    PayloadRegistry& operator=(const PayloadRegistry&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace AssetPipeline
