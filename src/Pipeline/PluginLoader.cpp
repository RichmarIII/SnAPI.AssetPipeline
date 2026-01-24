#include "IPluginRegistrar.h"
#include "PayloadRegistry.h"

#include <vector>
#include <string>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace SnAPI::AssetPipeline
{

  struct LoadedPlugin
  {
      std::string Name;
      std::string Version;
      std::string Path;

#ifdef _WIN32
      HMODULE Handle = nullptr;
#else
      void* Handle = nullptr;
#endif

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

  class PluginLoader
  {
    public:
      ~PluginLoader()
      {
        UnloadAll();
      }

      bool LoadPlugin(const std::string& Path)
      {
        LoadedPlugin Plugin;
        Plugin.Path = Path;

#ifdef _WIN32
        Plugin.Handle = LoadLibraryA(Path.c_str());
        if (!Plugin.Handle)
        {
          return false;
        }

        auto EntryFunc = reinterpret_cast<PluginRegisterFunc>(GetProcAddress(Plugin.Handle, SNAPI_PLUGIN_ENTRY_NAME));
#else
        Plugin.Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!Plugin.Handle)
        {
          return false;
        }

        auto EntryFunc = reinterpret_cast<PluginRegisterFunc>(dlsym(Plugin.Handle, SNAPI_PLUGIN_ENTRY_NAME));
#endif

        if (!EntryFunc)
        {
#ifdef _WIN32
          FreeLibrary(Plugin.Handle);
#else
          dlclose(Plugin.Handle);
#endif
          return false;
        }

        // Call plugin registration
        PluginRegistrarImpl Registrar(Plugin);
        EntryFunc(Registrar);

        m_Plugins.push_back(std::move(Plugin));
        return true;
      }

      void UnloadAll()
      {
        for (auto& Plugin : m_Plugins)
        {
          Plugin.Importers.clear();
          Plugin.Cookers.clear();
          Plugin.Serializers.clear();

          if (Plugin.Handle)
          {
#ifdef _WIN32
            FreeLibrary(Plugin.Handle);
#else
            dlclose(Plugin.Handle);
#endif
            Plugin.Handle = nullptr;
          }
        }
        m_Plugins.clear();
      }

      // Transfer ownership of serializers to registry
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

      // Get all importers from all plugins
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

      // Get all cookers from all plugins
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

      // Find the best importer for a source
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

      // Find the best cooker for an asset kind and intermediate type
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

    private:
      std::vector<LoadedPlugin> m_Plugins;
  };

} // namespace SnAPI::AssetPipeline
