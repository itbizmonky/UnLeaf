#pragma once
// UnLeaf - Configuration Manager
// INI-based configuration with file watching

#include "types.h"
#include "scoped_handle.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>

class ConfigParserTest;

namespace unleaf {

class UnLeafConfig {
public:
    // Singleton access
    static UnLeafConfig& Instance();

    // Initialize with base directory
    bool Initialize(const std::wstring& baseDir);

    // Reload configuration from file
    bool Reload();

    // Save current configuration to file
    bool Save();

    // Getters
    const std::vector<TargetProcess>& GetTargets() const { return targets_; }
    const std::wstring& GetConfigPath() const { return configPath_; }
    LogLevel GetLogLevel() const { return logLevel_; }
    bool IsLogEnabled() const { return logEnabled_; }

    // Target management
    bool AddTarget(const std::wstring& name);
    bool RemoveTarget(const std::wstring& name);
    bool IsTargetEnabled(const std::wstring& name) const;
    bool SetTargetEnabled(const std::wstring& name, bool enabled);

    void SetLogEnabled(bool enabled);

    // Check if file has been modified since last load
    bool HasFileChanged() const;

    // Callback for configuration changes
    using ConfigChangeCallback = std::function<void()>;
    void SetChangeCallback(ConfigChangeCallback callback);

private:
    friend class ::ConfigParserTest;

    UnLeafConfig();
    ~UnLeafConfig() = default;
    UnLeafConfig(const UnLeafConfig&) = delete;
    UnLeafConfig& operator=(const UnLeafConfig&) = delete;

    // Parse INI content
    bool ParseIni(const std::string& content);

    // Serialize to INI
    std::string SerializeIni() const;

    // Migrate from old JSON format
    bool MigrateFromJson(const std::wstring& baseDir);

    // Parse legacy JSON content (for migration)
    bool ParseJson(const std::string& content);

    // Get file modification time
    uint64_t GetFileModTime() const;

    std::wstring configPath_;
    std::vector<TargetProcess> targets_;
    LogLevel logLevel_;
    bool logEnabled_;

    mutable CriticalSection cs_;
    uint64_t lastModTime_;
    ConfigChangeCallback changeCallback_;
};

} // namespace unleaf
