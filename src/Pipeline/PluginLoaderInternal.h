#pragma once

#include "IPluginRegistrar.h"
#include "IAssetImporter.h"
#include "IAssetCooker.h"
#include "IPayloadSerializer.h"
#include "PayloadRegistry.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include <memory>
#include <string>
#include <vector>

namespace SnAPI::AssetPipeline
{

struct LoadedPlugin
{
    std::string Name;
    std::string Version;
    std::string Path;

    void* Handle = nullptr;

    std::vector<std::unique_ptr<IAssetImporter>> Importers;
    std::vector<std::unique_ptr<IAssetCooker>> Cookers;
    std::vector<std::unique_ptr<IPayloadSerializer>> Serializers;
};

class PluginRegistrarImpl : public IPluginRegistrar
{
  public:
    explicit PluginRegistrarImpl(LoadedPlugin& Plugin) : m_Plugin(Plugin) {}

    void RegisterImporter(std::unique_ptr<IAssetImporter> Importer) override
    {
      m_Plugin.Importers.push_back(std::move(Importer));
    }

    void RegisterCooker(std::unique_ptr<IAssetCooker> Cooker) override
    {
      m_Plugin.Cookers.push_back(std::move(Cooker));
    }

    void RegisterPayloadSerializer(std::unique_ptr<IPayloadSerializer> Serializer) override
    {
      m_Plugin.Serializers.push_back(std::move(Serializer));
    }

    void RegisterPluginInfo(const char* Name, const char* VersionString) override
    {
      m_Plugin.Name = Name ? Name : "";
      m_Plugin.Version = VersionString ? VersionString : "";
    }

  private:
    LoadedPlugin& m_Plugin;
};

class PluginLoaderInternal
{
  public:
    ~PluginLoaderInternal()
    {
      UnloadAll();
    }

    bool LoadPlugin(const std::string& Path);
    void UnloadAll();

    void TransferSerializers(PayloadRegistry& Registry)
    {
      for (auto& Plugin : m_Plugins)
      {
        for (auto& Serializer : Plugin.Serializers)
        {
          Registry.Register(std::move(Serializer));
        }
        Plugin.Serializers.clear();
      }
    }

    const std::vector<LoadedPlugin>& GetPlugins() const
    {
      return m_Plugins;
    }

    std::vector<IAssetImporter*> GetAllImporters() const
    {
      std::vector<IAssetImporter*> Result;
      for (const auto& Plugin : m_Plugins)
      {
        for (const auto& Importer : Plugin.Importers)
        {
          Result.push_back(Importer.get());
        }
      }
      return Result;
    }

    std::vector<IAssetCooker*> GetAllCookers() const
    {
      std::vector<IAssetCooker*> Result;
      for (const auto& Plugin : m_Plugins)
      {
        for (const auto& Cooker : Plugin.Cookers)
        {
          Result.push_back(Cooker.get());
        }
      }
      return Result;
    }

    IAssetImporter* FindImporter(const SourceRef& Source) const
    {
      for (const auto& Plugin : m_Plugins)
      {
        for (const auto& Importer : Plugin.Importers)
        {
          if (Importer->CanImport(Source))
          {
            return Importer.get();
          }
        }
      }
      return nullptr;
    }

    IAssetCooker* FindCooker(TypeId AssetKind, TypeId IntermediateType) const
    {
      for (const auto& Plugin : m_Plugins)
      {
        for (const auto& Cooker : Plugin.Cookers)
        {
          if (Cooker->CanCook(AssetKind, IntermediateType))
          {
            return Cooker.get();
          }
        }
      }
      return nullptr;
    }

    // Direct registration (no DLL needed) - for testing and embedded use
    void RegisterImporter(std::unique_ptr<IAssetImporter> Importer)
    {
      EnsureInlinePlugin();
      m_Plugins.back().Importers.push_back(std::move(Importer));
    }

    void RegisterCooker(std::unique_ptr<IAssetCooker> Cooker)
    {
      EnsureInlinePlugin();
      m_Plugins.back().Cookers.push_back(std::move(Cooker));
    }

    void RegisterSerializer(std::unique_ptr<IPayloadSerializer> Serializer)
    {
      EnsureInlinePlugin();
      m_Plugins.back().Serializers.push_back(std::move(Serializer));
    }

  private:
    void EnsureInlinePlugin()
    {
      if (m_Plugins.empty() || m_Plugins.back().Handle != nullptr)
      {
        LoadedPlugin InlinePlugin;
        InlinePlugin.Name = "Inline";
        InlinePlugin.Version = "1.0";
        m_Plugins.push_back(std::move(InlinePlugin));
      }
    }

    std::vector<LoadedPlugin> m_Plugins;
};

} // namespace SnAPI::AssetPipeline
