#include "PayloadRegistry.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <stdexcept>

namespace SnAPI::AssetPipeline
{

  struct PayloadRegistry::Impl
  {
      std::shared_mutex Mutex;
      std::atomic<bool> bFrozen{false};

      std::unordered_map<TypeId, std::unique_ptr<IPayloadSerializer>, UuidHash> SerializersByType;
      std::unordered_map<std::string, IPayloadSerializer*> SerializersByName;
      std::vector<IPayloadSerializer*> AllSerializers;
  };

  PayloadRegistry::PayloadRegistry() : m_Impl(std::make_unique<Impl>()) {}

  PayloadRegistry::~PayloadRegistry() = default;

  void PayloadRegistry::Register(std::unique_ptr<IPayloadSerializer> Serializer)
  {
    if (m_Impl->bFrozen.load(std::memory_order_acquire))
    {
      throw std::runtime_error("PayloadRegistry: Cannot register after freeze");
    }

    std::unique_lock Lock(m_Impl->Mutex);

    auto TypeId = Serializer->GetTypeId();
    auto TypeName = Serializer->GetTypeName();

    if (m_Impl->SerializersByType.contains(TypeId))
    {
      throw std::runtime_error("PayloadRegistry: TypeId already registered: " + TypeId.ToString());
    }

    if (m_Impl->SerializersByName.contains(TypeName))
    {
      throw std::runtime_error("PayloadRegistry: TypeName already registered: " + std::string(TypeName));
    }

    auto* RawPtr = Serializer.get();
    m_Impl->SerializersByType[TypeId] = std::move(Serializer);
    m_Impl->SerializersByName[TypeName] = RawPtr;
    m_Impl->AllSerializers.push_back(RawPtr);
  }

  void PayloadRegistry::Freeze()
  {
    std::unique_lock Lock(m_Impl->Mutex);
    m_Impl->bFrozen.store(true, std::memory_order_release);
  }

  bool PayloadRegistry::IsFrozen() const
  {
    return m_Impl->bFrozen.load(std::memory_order_acquire);
  }

  const IPayloadSerializer* PayloadRegistry::Find(TypeId Id) const
  {
    if (m_Impl->bFrozen.load(std::memory_order_acquire))
    {
      // Lock-free read after freeze
      auto It = m_Impl->SerializersByType.find(Id);
      return It != m_Impl->SerializersByType.end() ? It->second.get() : nullptr;
    }
    else
    {
      std::shared_lock Lock(m_Impl->Mutex);
      auto It = m_Impl->SerializersByType.find(Id);
      return It != m_Impl->SerializersByType.end() ? It->second.get() : nullptr;
    }
  }

  const IPayloadSerializer* PayloadRegistry::FindByName(const char* TypeName) const
  {
    if (m_Impl->bFrozen.load(std::memory_order_acquire))
    {
      auto It = m_Impl->SerializersByName.find(TypeName);
      return It != m_Impl->SerializersByName.end() ? It->second : nullptr;
    }
    else
    {
      std::shared_lock Lock(m_Impl->Mutex);
      auto It = m_Impl->SerializersByName.find(TypeName);
      return It != m_Impl->SerializersByName.end() ? It->second : nullptr;
    }
  }

  const std::vector<IPayloadSerializer*>& PayloadRegistry::GetAll() const
  {
    return m_Impl->AllSerializers;
  }

} // namespace SnAPI::AssetPipeline
