#include "AssetCache.h"

#include <algorithm>

namespace SnAPI::AssetPipeline
{

  AssetCache::AssetCache(const AssetCacheConfig& Config) : m_Config(Config)
  {
    if (m_Config.EvictionThresholdBytes == 0)
    {
      m_Config.EvictionThresholdBytes = static_cast<size_t>(m_Config.MaxMemoryBytes * 0.9);
    }
  }

  AssetCache::~AssetCache()
  {
    ClearAll();
  }

  bool AssetCache::Contains(AssetId Id, std::type_index Type) const
  {
    std::shared_lock Lock(m_CacheMutex);
    return m_Cache.find(CacheKey(Id, Type)) != m_Cache.end();
  }

  std::shared_ptr<CacheEntry> AssetCache::GetEntry(AssetId Id, std::type_index Type)
  {
    std::shared_lock Lock(m_CacheMutex);
    auto It = m_Cache.find(CacheKey(Id, Type));
    if (It != m_Cache.end())
    {
      return It->second;
    }
    return nullptr;
  }

  void AssetCache::InsertEntry(std::shared_ptr<CacheEntry> Entry)
  {
    // Evict if needed before inserting
    if (m_MemoryUsage.load() + Entry->SizeBytes > m_Config.EvictionThresholdBytes)
    {
      Evict();
    }

    std::unique_lock Lock(m_CacheMutex);

    CacheKey Key(Entry->Id, Entry->Type);

    // Remove existing entry if present
    auto It = m_Cache.find(Key);
    if (It != m_Cache.end())
    {
      m_MemoryUsage.fetch_sub(It->second->SizeBytes);

      // Remove from LRU
      auto LruIt = m_LruMap.find(It->second.get());
      if (LruIt != m_LruMap.end())
      {
        m_LruList.erase(LruIt->second);
        m_LruMap.erase(LruIt);
      }
    }

    // Insert new entry
    m_Cache[Key] = Entry;
    m_MemoryUsage.fetch_add(Entry->SizeBytes);

    // Add to LRU front
    m_LruList.push_front(Entry);
    m_LruMap[Entry.get()] = m_LruList.begin();
  }

  void AssetCache::TouchLRU(const std::shared_ptr<CacheEntry>& Entry)
  {
    std::unique_lock Lock(m_CacheMutex);

    auto It = m_LruMap.find(Entry.get());
    if (It != m_LruMap.end())
    {
      m_LruList.erase(It->second);
      m_LruList.push_front(Entry);
      It->second = m_LruList.begin();
    }
  }

  bool AssetCache::Remove(AssetId Id, std::type_index Type)
  {
    std::unique_lock Lock(m_CacheMutex);

    CacheKey Key(Id, Type);
    auto It = m_Cache.find(Key);
    if (It == m_Cache.end())
    {
      return false;
    }

    // Only remove if not referenced
    if (It->second->RefCount.load() > 0)
    {
      return false;
    }

    m_MemoryUsage.fetch_sub(It->second->SizeBytes);

    // Remove from LRU
    auto LruIt = m_LruMap.find(It->second.get());
    if (LruIt != m_LruMap.end())
    {
      m_LruList.erase(LruIt->second);
      m_LruMap.erase(LruIt);
    }

    m_Cache.erase(It);
    return true;
  }

  void AssetCache::ForceRemove(AssetId Id, std::type_index Type)
  {
    std::unique_lock Lock(m_CacheMutex);

    CacheKey Key(Id, Type);
    auto It = m_Cache.find(Key);
    if (It == m_Cache.end())
    {
      return;
    }

    m_MemoryUsage.fetch_sub(It->second->SizeBytes);

    // Remove from LRU
    auto LruIt = m_LruMap.find(It->second.get());
    if (LruIt != m_LruMap.end())
    {
      m_LruList.erase(LruIt->second);
      m_LruMap.erase(LruIt);
    }

    m_Cache.erase(It);
  }

  size_t AssetCache::ClearUnreferenced()
  {
    std::unique_lock Lock(m_CacheMutex);

    size_t RemovedCount = 0;
    auto It = m_Cache.begin();
    while (It != m_Cache.end())
    {
      if (It->second->RefCount.load() == 0)
      {
        m_MemoryUsage.fetch_sub(It->second->SizeBytes);

        // Remove from LRU
        auto LruIt = m_LruMap.find(It->second.get());
        if (LruIt != m_LruMap.end())
        {
          m_LruList.erase(LruIt->second);
          m_LruMap.erase(LruIt);
        }

        It = m_Cache.erase(It);
        ++RemovedCount;
      }
      else
      {
        ++It;
      }
    }

    return RemovedCount;
  }

  void AssetCache::ClearAll()
  {
    std::unique_lock Lock(m_CacheMutex);

    m_LruList.clear();
    m_LruMap.clear();
    m_Cache.clear();
    m_MemoryUsage.store(0);
  }

  size_t AssetCache::Evict()
  {
    if (m_MemoryUsage.load() < m_Config.EvictionThresholdBytes)
    {
      return 0;
    }

    std::unique_lock Lock(m_CacheMutex);

    size_t EvictedCount = 0;
    size_t TargetUsage = static_cast<size_t>(m_Config.MaxMemoryBytes * 0.7); // Evict down to 70%
    auto Now = std::chrono::steady_clock::now();

    // FIX #1: Iterate from back to front using a reverse iterator instead of pop_back
    // This avoids corrupting m_LruMap when skipping referenced entries
    auto It = m_LruList.end();
    while (m_MemoryUsage.load() > TargetUsage && It != m_LruList.begin())
    {
      --It;
      auto& Entry = *It;

      // Skip if referenced and we're configured to only evict unreferenced
      if (m_Config.bEvictOnlyUnreferenced && Entry->RefCount.load() > 0)
      {
        // Don't modify the list - just skip this entry and continue scanning
        continue;
      }

      // Skip if too recently loaded
      auto Age = std::chrono::duration_cast<std::chrono::seconds>(Now - Entry->LastAccess);
      if (Age < m_Config.MinAgeBeforeEviction)
      {
        // Stop evicting - remaining entries toward the front are newer
        break;
      }

      // Evict this entry
      CacheKey Key(Entry->Id, Entry->Type);
      m_MemoryUsage.fetch_sub(Entry->SizeBytes);

      m_LruMap.erase(Entry.get());
      m_Cache.erase(Key);
      It = m_LruList.erase(It); // Erase returns iterator to next element (toward begin)

      ++EvictedCount;
    }

    return EvictedCount;
  }

  size_t AssetCache::GetCachedCount() const
  {
    std::shared_lock Lock(m_CacheMutex);
    return m_Cache.size();
  }

  size_t AssetCache::GetMemoryUsage() const
  {
    return m_MemoryUsage.load();
  }

  size_t AssetCache::GetReferencedCount() const
  {
    std::shared_lock Lock(m_CacheMutex);
    size_t Count = 0;
    for (const auto& [Key, Entry] : m_Cache)
    {
      if (Entry->RefCount.load() > 0)
      {
        ++Count;
      }
    }
    return Count;
  }

  void AssetCache::SetConfig(const AssetCacheConfig& Config)
  {
    m_Config = Config;
    if (m_Config.EvictionThresholdBytes == 0)
    {
      m_Config.EvictionThresholdBytes = static_cast<size_t>(m_Config.MaxMemoryBytes * 0.9);
    }

    // Trigger eviction if needed with new config
    Evict();
  }

} // namespace SnAPI::AssetPipeline
