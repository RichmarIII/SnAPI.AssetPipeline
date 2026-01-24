#include "Uuid.h"
#include "PipelineBuildConfig.h"

#include <sqlite3.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace SnAPI::AssetPipeline
{

  // Forward declaration
  uint64_t HashFile(const std::string& Path);

  struct CacheEntry
  {
      AssetId Id;
      std::string LogicalName;
      std::string VariantKey;

      uint64_t SourceHash = 0;
      uint64_t DependenciesHash = 0;
      uint64_t IntermediatePayloadHash = 0;
      uint64_t CookedPayloadHash = 0;
      uint64_t BuildOptionsHash = 0;

      std::string ImporterName;
      std::string ImporterPluginVersion;
      std::string CookerName;
      std::string CookerPluginVersion;

      bool bValid = false;
  };

  // Dependency information
  struct DependencyInfo
  {
      std::string FilePath;
      uint64_t FileHash = 0;
      std::filesystem::file_time_type LastModified;
  };

  class IncrementalCache
  {
    public:
      IncrementalCache() = default;

      ~IncrementalCache()
      {
        Close();
      }

      bool Open(const std::string& DbPath)
      {
        int Result = sqlite3_open(DbPath.c_str(), &m_Db);
        if (Result != SQLITE_OK)
        {
          return false;
        }

        // Enable WAL mode for better concurrency
        sqlite3_exec(m_Db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);

        // Create tables if not exists
        const char* CreateTablesSql = R"(
            CREATE TABLE IF NOT EXISTS cache_entries (
                asset_id BLOB PRIMARY KEY,
                logical_name TEXT NOT NULL,
                variant_key TEXT,
                source_hash INTEGER,
                dependencies_hash INTEGER,
                intermediate_hash INTEGER,
                cooked_hash INTEGER,
                build_options_hash INTEGER,
                importer_name TEXT,
                importer_plugin_version TEXT,
                cooker_name TEXT,
                cooker_plugin_version TEXT,
                timestamp INTEGER DEFAULT (strftime('%s', 'now'))
            );
            CREATE INDEX IF NOT EXISTS idx_logical_name ON cache_entries(logical_name);

            CREATE TABLE IF NOT EXISTS dependencies (
                asset_id BLOB NOT NULL,
                dependency_path TEXT NOT NULL,
                file_hash INTEGER,
                last_modified INTEGER,
                dependency_type TEXT DEFAULT 'file',
                PRIMARY KEY (asset_id, dependency_path),
                FOREIGN KEY (asset_id) REFERENCES cache_entries(asset_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_dep_path ON dependencies(dependency_path);

            CREATE TABLE IF NOT EXISTS reverse_dependencies (
                dependency_path TEXT NOT NULL,
                dependent_asset_id BLOB NOT NULL,
                PRIMARY KEY (dependency_path, dependent_asset_id)
            );
            CREATE INDEX IF NOT EXISTS idx_rev_dep_asset ON reverse_dependencies(dependent_asset_id);

            CREATE TABLE IF NOT EXISTS file_hashes (
                file_path TEXT PRIMARY KEY,
                file_hash INTEGER,
                last_modified INTEGER
            );
        )";

        char* ErrMsg = nullptr;
        Result = sqlite3_exec(m_Db, CreateTablesSql, nullptr, nullptr, &ErrMsg);
        if (Result != SQLITE_OK)
        {
          sqlite3_free(ErrMsg);
          return false;
        }

        PrepareStatements();
        return true;
      }

      void Close()
      {
        FinalizeStatements();
        if (m_Db)
        {
          sqlite3_close(m_Db);
          m_Db = nullptr;
        }
      }

      CacheEntry Get(const AssetId& Id)
      {
        CacheEntry Entry;
        Entry.Id = Id;

        if (!m_StmtSelect || !m_Db)
        {
          return Entry;
        }

        sqlite3_reset(m_StmtSelect);
        sqlite3_bind_blob(m_StmtSelect, 1, Id.Bytes, 16, SQLITE_STATIC);

        if (sqlite3_step(m_StmtSelect) == SQLITE_ROW)
        {
          Entry.LogicalName = reinterpret_cast<const char*>(sqlite3_column_text(m_StmtSelect, 0));

          const char* Variant = reinterpret_cast<const char*>(sqlite3_column_text(m_StmtSelect, 1));
          Entry.VariantKey = Variant ? Variant : "";

          Entry.SourceHash = static_cast<uint64_t>(sqlite3_column_int64(m_StmtSelect, 2));
          Entry.DependenciesHash = static_cast<uint64_t>(sqlite3_column_int64(m_StmtSelect, 3));
          Entry.IntermediatePayloadHash = static_cast<uint64_t>(sqlite3_column_int64(m_StmtSelect, 4));
          Entry.CookedPayloadHash = static_cast<uint64_t>(sqlite3_column_int64(m_StmtSelect, 5));
          Entry.BuildOptionsHash = static_cast<uint64_t>(sqlite3_column_int64(m_StmtSelect, 6));

          Entry.ImporterName = reinterpret_cast<const char*>(sqlite3_column_text(m_StmtSelect, 7));
          Entry.ImporterPluginVersion = reinterpret_cast<const char*>(sqlite3_column_text(m_StmtSelect, 8));
          Entry.CookerName = reinterpret_cast<const char*>(sqlite3_column_text(m_StmtSelect, 9));
          Entry.CookerPluginVersion = reinterpret_cast<const char*>(sqlite3_column_text(m_StmtSelect, 10));

          Entry.bValid = true;
        }

        return Entry;
      }

      bool Put(const CacheEntry& Entry)
      {
        if (!m_StmtInsert || !m_Db)
        {
          return false;
        }

        sqlite3_reset(m_StmtInsert);

        sqlite3_bind_blob(m_StmtInsert, 1, Entry.Id.Bytes, 16, SQLITE_STATIC);
        sqlite3_bind_text(m_StmtInsert, 2, Entry.LogicalName.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(m_StmtInsert, 3, Entry.VariantKey.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(m_StmtInsert, 4, static_cast<int64_t>(Entry.SourceHash));
        sqlite3_bind_int64(m_StmtInsert, 5, static_cast<int64_t>(Entry.DependenciesHash));
        sqlite3_bind_int64(m_StmtInsert, 6, static_cast<int64_t>(Entry.IntermediatePayloadHash));
        sqlite3_bind_int64(m_StmtInsert, 7, static_cast<int64_t>(Entry.CookedPayloadHash));
        sqlite3_bind_int64(m_StmtInsert, 8, static_cast<int64_t>(Entry.BuildOptionsHash));
        sqlite3_bind_text(m_StmtInsert, 9, Entry.ImporterName.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(m_StmtInsert, 10, Entry.ImporterPluginVersion.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(m_StmtInsert, 11, Entry.CookerName.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(m_StmtInsert, 12, Entry.CookerPluginVersion.c_str(), -1, SQLITE_STATIC);

        return sqlite3_step(m_StmtInsert) == SQLITE_DONE;
      }

      bool Remove(const AssetId& Id)
      {
        if (!m_StmtDelete || !m_Db)
        {
          return false;
        }

        // Remove from reverse dependencies first
        RemoveDependencies(Id);

        sqlite3_reset(m_StmtDelete);
        sqlite3_bind_blob(m_StmtDelete, 1, Id.Bytes, 16, SQLITE_STATIC);

        return sqlite3_step(m_StmtDelete) == SQLITE_DONE;
      }

      // ========== Dependency Tracking ==========

      // Add a dependency for an asset
      bool AddDependency(const AssetId& Id, const std::string& DependencyPath, const std::string& Type = "file")
      {
        if (!m_StmtAddDep || !m_Db)
        {
          return false;
        }

        // Get current file hash and mod time
        uint64_t FileHash = 0;
        int64_t ModTime = 0;

        try
        {
          if (std::filesystem::exists(DependencyPath))
          {
            FileHash = HashFile(DependencyPath);
            auto LastWrite = std::filesystem::last_write_time(DependencyPath);
            ModTime = LastWrite.time_since_epoch().count();
          }
        }
        catch (...)
        {
          // Ignore filesystem errors
        }

        sqlite3_reset(m_StmtAddDep);
        sqlite3_bind_blob(m_StmtAddDep, 1, Id.Bytes, 16, SQLITE_STATIC);
        sqlite3_bind_text(m_StmtAddDep, 2, DependencyPath.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(m_StmtAddDep, 3, static_cast<int64_t>(FileHash));
        sqlite3_bind_int64(m_StmtAddDep, 4, ModTime);
        sqlite3_bind_text(m_StmtAddDep, 5, Type.c_str(), -1, SQLITE_STATIC);

        bool Success = sqlite3_step(m_StmtAddDep) == SQLITE_DONE;

        // Also add reverse dependency
        if (Success)
        {
          AddReverseDependency(DependencyPath, Id);
        }

        return Success;
      }

      // Set all dependencies for an asset (replaces existing)
      bool SetDependencies(const AssetId& Id, const std::vector<std::string>& Dependencies, const std::string& Type = "file")
      {
        RemoveDependencies(Id);

        for (const auto& Dep : Dependencies)
        {
          if (!AddDependency(Id, Dep, Type))
          {
            return false;
          }
        }

        return true;
      }

      // Get all dependencies for an asset
      std::vector<DependencyInfo> GetDependencies(const AssetId& Id)
      {
        std::vector<DependencyInfo> Dependencies;

        if (!m_StmtGetDeps || !m_Db)
        {
          return Dependencies;
        }

        sqlite3_reset(m_StmtGetDeps);
        sqlite3_bind_blob(m_StmtGetDeps, 1, Id.Bytes, 16, SQLITE_STATIC);

        while (sqlite3_step(m_StmtGetDeps) == SQLITE_ROW)
        {
          DependencyInfo Info;
          Info.FilePath = reinterpret_cast<const char*>(sqlite3_column_text(m_StmtGetDeps, 0));
          Info.FileHash = static_cast<uint64_t>(sqlite3_column_int64(m_StmtGetDeps, 1));
          // LastModified not directly convertible but we can skip for now
          Dependencies.push_back(Info);
        }

        return Dependencies;
      }

      // Remove all dependencies for an asset
      void RemoveDependencies(const AssetId& Id)
      {
        if (!m_StmtRemoveDeps || !m_Db)
        {
          return;
        }

        // Get existing dependencies first for reverse cleanup
        auto ExistingDeps = GetDependencies(Id);
        for (const auto& Dep : ExistingDeps)
        {
          RemoveReverseDependency(Dep.FilePath, Id);
        }

        sqlite3_reset(m_StmtRemoveDeps);
        sqlite3_bind_blob(m_StmtRemoveDeps, 1, Id.Bytes, 16, SQLITE_STATIC);
        sqlite3_step(m_StmtRemoveDeps);
      }

      // Get all assets that depend on a given file path
      std::vector<AssetId> GetDependentAssets(const std::string& FilePath)
      {
        std::vector<AssetId> Dependents;

        if (!m_StmtGetReverseDeps || !m_Db)
        {
          return Dependents;
        }

        sqlite3_reset(m_StmtGetReverseDeps);
        sqlite3_bind_text(m_StmtGetReverseDeps, 1, FilePath.c_str(), -1, SQLITE_STATIC);

        while (sqlite3_step(m_StmtGetReverseDeps) == SQLITE_ROW)
        {
          AssetId Id;
          const void* Blob = sqlite3_column_blob(m_StmtGetReverseDeps, 0);
          if (Blob)
          {
            std::memcpy(Id.Bytes, Blob, 16);
          }
          Dependents.push_back(Id);
        }

        return Dependents;
      }

      // Check if any dependency of an asset has changed
      bool HasDependencyChanged(const AssetId& Id)
      {
        auto Dependencies = GetDependencies(Id);

        for (const auto& Dep : Dependencies)
        {
          try
          {
            if (!std::filesystem::exists(Dep.FilePath))
            {
              // Dependency deleted
              return true;
            }

            uint64_t CurrentHash = HashFile(Dep.FilePath);
            if (CurrentHash != Dep.FileHash)
            {
              return true;
            }
          }
          catch (...)
          {
            // Error reading file - assume changed
            return true;
          }
        }

        return false;
      }

      // Compute a combined hash of all dependencies
      uint64_t ComputeDependenciesHash(const AssetId& Id)
      {
        auto Dependencies = GetDependencies(Id);

        if (Dependencies.empty())
        {
          return 0;
        }

        // Sort for deterministic ordering
        std::sort(Dependencies.begin(), Dependencies.end(), [](const DependencyInfo& A, const DependencyInfo& B) { return A.FilePath < B.FilePath; });

        // XOR combine all hashes (simple approach)
        uint64_t CombinedHash = 0;
        for (const auto& Dep : Dependencies)
        {
          CombinedHash ^= Dep.FileHash;
          CombinedHash = (CombinedHash << 7) | (CombinedHash >> 57); // Rotate
        }

        return CombinedHash;
      }

      // Refresh dependencies hash for an asset
      uint64_t RefreshDependenciesHash(const AssetId& Id)
      {
        auto Dependencies = GetDependencies(Id);

        if (Dependencies.empty())
        {
          return 0;
        }

        // Sort for deterministic ordering
        std::sort(Dependencies.begin(), Dependencies.end(), [](const DependencyInfo& A, const DependencyInfo& B) { return A.FilePath < B.FilePath; });

        // Compute current hashes
        uint64_t CombinedHash = 0;
        for (auto& Dep : Dependencies)
        {
          try
          {
            Dep.FileHash = HashFile(Dep.FilePath);
          }
          catch (...)
          {
            // Keep existing hash
          }
          CombinedHash ^= Dep.FileHash;
          CombinedHash = (CombinedHash << 7) | (CombinedHash >> 57);
        }

        return CombinedHash;
      }

      // ========== File Hash Cache ==========

      uint64_t GetCachedFileHash(const std::string& FilePath)
      {
        if (!m_StmtGetFileHash || !m_Db)
        {
          return HashFile(FilePath);
        }

        sqlite3_reset(m_StmtGetFileHash);
        sqlite3_bind_text(m_StmtGetFileHash, 1, FilePath.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(m_StmtGetFileHash) == SQLITE_ROW)
        {
          int64_t CachedModTime = sqlite3_column_int64(m_StmtGetFileHash, 1);

          try
          {
            auto CurrentModTime = std::filesystem::last_write_time(FilePath);
            int64_t CurrentModTimeInt = CurrentModTime.time_since_epoch().count();

            if (CurrentModTimeInt == CachedModTime)
            {
              // File hasn't changed, return cached hash
              return static_cast<uint64_t>(sqlite3_column_int64(m_StmtGetFileHash, 0));
            }
          }
          catch (...)
          {
            // Fall through to recompute
          }
        }

        // Compute and cache new hash
        uint64_t Hash = HashFile(FilePath);
        CacheFileHash(FilePath, Hash);
        return Hash;
      }

      void CacheFileHash(const std::string& FilePath, uint64_t Hash)
      {
        if (!m_StmtSetFileHash || !m_Db)
        {
          return;
        }

        int64_t ModTime = 0;
        try
        {
          auto LastWrite = std::filesystem::last_write_time(FilePath);
          ModTime = LastWrite.time_since_epoch().count();
        }
        catch (...)
        {
          // Ignore
        }

        sqlite3_reset(m_StmtSetFileHash);
        sqlite3_bind_text(m_StmtSetFileHash, 1, FilePath.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(m_StmtSetFileHash, 2, static_cast<int64_t>(Hash));
        sqlite3_bind_int64(m_StmtSetFileHash, 3, ModTime);
        sqlite3_step(m_StmtSetFileHash);
      }

      // ========== Rebuild Detection ==========

      // Check if an asset needs rebuilding
      bool NeedsRebuild(const CacheEntry& NewEntry, const CacheEntry& OldEntry)
      {
        if (!OldEntry.bValid)
        {
          return true;
        }

        if (NewEntry.SourceHash != OldEntry.SourceHash)
        {
          return true;
        }

        if (NewEntry.DependenciesHash != OldEntry.DependenciesHash)
        {
          return true;
        }

        if (NewEntry.BuildOptionsHash != OldEntry.BuildOptionsHash)
        {
          return true;
        }

        if (NewEntry.ImporterName != OldEntry.ImporterName || NewEntry.ImporterPluginVersion != OldEntry.ImporterPluginVersion)
        {
          return true;
        }

        if (NewEntry.CookerName != OldEntry.CookerName || NewEntry.CookerPluginVersion != OldEntry.CookerPluginVersion)
        {
          return true;
        }

        return false;
      }

      // Check if an asset needs rebuilding (comprehensive check including dependencies)
      bool NeedsRebuildWithDependencies(const AssetId& Id, uint64_t CurrentSourceHash, uint64_t CurrentBuildOptionsHash,
                                        const std::string& ImporterName, const std::string& ImporterVersion, const std::string& CookerName,
                                        const std::string& CookerVersion)
      {
        CacheEntry OldEntry = Get(Id);

        if (!OldEntry.bValid)
        {
          return true;
        }

        if (CurrentSourceHash != OldEntry.SourceHash)
        {
          return true;
        }

        if (CurrentBuildOptionsHash != OldEntry.BuildOptionsHash)
        {
          return true;
        }

        if (ImporterName != OldEntry.ImporterName || ImporterVersion != OldEntry.ImporterPluginVersion)
        {
          return true;
        }

        if (CookerName != OldEntry.CookerName || CookerVersion != OldEntry.CookerPluginVersion)
        {
          return true;
        }

        // Check if any dependency has changed
        if (HasDependencyChanged(Id))
        {
          return true;
        }

        return false;
      }

      // ========== Transactions ==========

      void BeginTransaction()
      {
        if (m_Db)
        {
          sqlite3_exec(m_Db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
        }
      }

      void CommitTransaction()
      {
        if (m_Db)
        {
          sqlite3_exec(m_Db, "COMMIT", nullptr, nullptr, nullptr);
        }
      }

      void RollbackTransaction()
      {
        if (m_Db)
        {
          sqlite3_exec(m_Db, "ROLLBACK", nullptr, nullptr, nullptr);
        }
      }

      // ========== Statistics ==========

      size_t GetCachedEntryCount()
      {
        if (!m_Db)
        {
          return 0;
        }

        sqlite3_stmt* Stmt = nullptr;
        sqlite3_prepare_v2(m_Db, "SELECT COUNT(*) FROM cache_entries", -1, &Stmt, nullptr);

        size_t Count = 0;
        if (sqlite3_step(Stmt) == SQLITE_ROW)
        {
          Count = static_cast<size_t>(sqlite3_column_int64(Stmt, 0));
        }

        sqlite3_finalize(Stmt);
        return Count;
      }

      size_t GetDependencyCount()
      {
        if (!m_Db)
        {
          return 0;
        }

        sqlite3_stmt* Stmt = nullptr;
        sqlite3_prepare_v2(m_Db, "SELECT COUNT(*) FROM dependencies", -1, &Stmt, nullptr);

        size_t Count = 0;
        if (sqlite3_step(Stmt) == SQLITE_ROW)
        {
          Count = static_cast<size_t>(sqlite3_column_int64(Stmt, 0));
        }

        sqlite3_finalize(Stmt);
        return Count;
      }

      // Clear all stale entries (assets no longer in source)
      size_t PruneStaleEntries(const std::vector<AssetId>& ValidAssetIds)
      {
        // This is expensive - only do periodically
        if (!m_Db)
        {
          return 0;
        }

        // Build a set for quick lookup
        std::unordered_map<std::string, bool> ValidIdSet;
        for (const auto& Id : ValidAssetIds)
        {
          ValidIdSet[std::string(reinterpret_cast<const char*>(Id.Bytes), 16)] = true;
        }

        // Query all existing entries
        sqlite3_stmt* Stmt = nullptr;
        sqlite3_prepare_v2(m_Db, "SELECT asset_id FROM cache_entries", -1, &Stmt, nullptr);

        std::vector<AssetId> ToRemove;
        while (sqlite3_step(Stmt) == SQLITE_ROW)
        {
          const void* Blob = sqlite3_column_blob(Stmt, 0);
          if (Blob)
          {
            AssetId Id;
            std::memcpy(Id.Bytes, Blob, 16);

            std::string IdStr(reinterpret_cast<const char*>(Id.Bytes), 16);
            if (ValidIdSet.find(IdStr) == ValidIdSet.end())
            {
              ToRemove.push_back(Id);
            }
          }
        }
        sqlite3_finalize(Stmt);

        // Remove stale entries
        for (const auto& Id : ToRemove)
        {
          Remove(Id);
        }

        return ToRemove.size();
      }

    private:
      void AddReverseDependency(const std::string& FilePath, const AssetId& DependentId)
      {
        if (!m_StmtAddRevDep || !m_Db)
        {
          return;
        }

        sqlite3_reset(m_StmtAddRevDep);
        sqlite3_bind_text(m_StmtAddRevDep, 1, FilePath.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_blob(m_StmtAddRevDep, 2, DependentId.Bytes, 16, SQLITE_STATIC);
        sqlite3_step(m_StmtAddRevDep);
      }

      void RemoveReverseDependency(const std::string& FilePath, const AssetId& DependentId)
      {
        if (!m_StmtRemoveRevDep || !m_Db)
        {
          return;
        }

        sqlite3_reset(m_StmtRemoveRevDep);
        sqlite3_bind_text(m_StmtRemoveRevDep, 1, FilePath.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_blob(m_StmtRemoveRevDep, 2, DependentId.Bytes, 16, SQLITE_STATIC);
        sqlite3_step(m_StmtRemoveRevDep);
      }

      void PrepareStatements()
      {
        const char* SelectSql = R"(
            SELECT logical_name, variant_key, source_hash, dependencies_hash,
                   intermediate_hash, cooked_hash, build_options_hash,
                   importer_name, importer_plugin_version,
                   cooker_name, cooker_plugin_version
            FROM cache_entries WHERE asset_id = ?
        )";

        const char* InsertSql = R"(
            INSERT OR REPLACE INTO cache_entries (
                asset_id, logical_name, variant_key, source_hash,
                dependencies_hash, intermediate_hash, cooked_hash,
                build_options_hash, importer_name, importer_plugin_version,
                cooker_name, cooker_plugin_version
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        const char* DeleteSql = "DELETE FROM cache_entries WHERE asset_id = ?";

        const char* AddDepSql = R"(
            INSERT OR REPLACE INTO dependencies (asset_id, dependency_path, file_hash, last_modified, dependency_type)
            VALUES (?, ?, ?, ?, ?)
        )";

        const char* GetDepsSql = R"(
            SELECT dependency_path, file_hash, last_modified FROM dependencies WHERE asset_id = ?
        )";

        const char* RemoveDepsSql = "DELETE FROM dependencies WHERE asset_id = ?";

        const char* AddRevDepSql = R"(
            INSERT OR IGNORE INTO reverse_dependencies (dependency_path, dependent_asset_id) VALUES (?, ?)
        )";

        const char* RemoveRevDepSql = R"(
            DELETE FROM reverse_dependencies WHERE dependency_path = ? AND dependent_asset_id = ?
        )";

        const char* GetRevDepsSql = R"(
            SELECT dependent_asset_id FROM reverse_dependencies WHERE dependency_path = ?
        )";

        const char* GetFileHashSql = R"(
            SELECT file_hash, last_modified FROM file_hashes WHERE file_path = ?
        )";

        const char* SetFileHashSql = R"(
            INSERT OR REPLACE INTO file_hashes (file_path, file_hash, last_modified) VALUES (?, ?, ?)
        )";

        sqlite3_prepare_v2(m_Db, SelectSql, -1, &m_StmtSelect, nullptr);
        sqlite3_prepare_v2(m_Db, InsertSql, -1, &m_StmtInsert, nullptr);
        sqlite3_prepare_v2(m_Db, DeleteSql, -1, &m_StmtDelete, nullptr);
        sqlite3_prepare_v2(m_Db, AddDepSql, -1, &m_StmtAddDep, nullptr);
        sqlite3_prepare_v2(m_Db, GetDepsSql, -1, &m_StmtGetDeps, nullptr);
        sqlite3_prepare_v2(m_Db, RemoveDepsSql, -1, &m_StmtRemoveDeps, nullptr);
        sqlite3_prepare_v2(m_Db, AddRevDepSql, -1, &m_StmtAddRevDep, nullptr);
        sqlite3_prepare_v2(m_Db, RemoveRevDepSql, -1, &m_StmtRemoveRevDep, nullptr);
        sqlite3_prepare_v2(m_Db, GetRevDepsSql, -1, &m_StmtGetReverseDeps, nullptr);
        sqlite3_prepare_v2(m_Db, GetFileHashSql, -1, &m_StmtGetFileHash, nullptr);
        sqlite3_prepare_v2(m_Db, SetFileHashSql, -1, &m_StmtSetFileHash, nullptr);
      }

      void FinalizeStatements()
      {
        auto FinalizeStmt = [](sqlite3_stmt*& Stmt) {
          if (Stmt)
          {
            sqlite3_finalize(Stmt);
            Stmt = nullptr;
          }
        };

        FinalizeStmt(m_StmtSelect);
        FinalizeStmt(m_StmtInsert);
        FinalizeStmt(m_StmtDelete);
        FinalizeStmt(m_StmtAddDep);
        FinalizeStmt(m_StmtGetDeps);
        FinalizeStmt(m_StmtRemoveDeps);
        FinalizeStmt(m_StmtAddRevDep);
        FinalizeStmt(m_StmtRemoveRevDep);
        FinalizeStmt(m_StmtGetReverseDeps);
        FinalizeStmt(m_StmtGetFileHash);
        FinalizeStmt(m_StmtSetFileHash);
      }

    private:
      sqlite3* m_Db = nullptr;

      // Cache entry statements
      sqlite3_stmt* m_StmtSelect = nullptr;
      sqlite3_stmt* m_StmtInsert = nullptr;
      sqlite3_stmt* m_StmtDelete = nullptr;

      // Dependency statements
      sqlite3_stmt* m_StmtAddDep = nullptr;
      sqlite3_stmt* m_StmtGetDeps = nullptr;
      sqlite3_stmt* m_StmtRemoveDeps = nullptr;
      sqlite3_stmt* m_StmtAddRevDep = nullptr;
      sqlite3_stmt* m_StmtRemoveRevDep = nullptr;
      sqlite3_stmt* m_StmtGetReverseDeps = nullptr;

      // File hash cache statements
      sqlite3_stmt* m_StmtGetFileHash = nullptr;
      sqlite3_stmt* m_StmtSetFileHash = nullptr;
  };

  // Simple file hashing implementation using XXHash
  // This is a placeholder - the actual implementation is in XXHash.cpp
  uint64_t HashFile(const std::string& Path)
  {
    std::ifstream File(Path, std::ios::binary);
    if (!File)
    {
      return 0;
    }

    // Read file contents
    std::vector<char> Buffer(std::istreambuf_iterator<char>(File), {});

    // Simple hash (in production use XXH3)
    uint64_t Hash = 0x517cc1b727220a95ULL;
    for (size_t I = 0; I < Buffer.size(); ++I)
    {
      Hash ^= static_cast<uint64_t>(static_cast<uint8_t>(Buffer[I])) << ((I % 8) * 8);
      Hash *= 0x9e3779b97f4a7c15ULL;
    }

    return Hash;
  }

} // namespace SnAPI::AssetPipeline
