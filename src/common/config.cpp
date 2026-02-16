// UnLeaf - Configuration Manager Implementation
// INI-based configuration with JSON migration support

#include "config.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <regex>

namespace fs = std::filesystem;

namespace {
void StripUtf8Bom(std::string& content) {
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }
}
} // anonymous namespace

namespace unleaf {

UnLeafConfig& UnLeafConfig::Instance() {
    static UnLeafConfig instance;
    return instance;
}

UnLeafConfig::UnLeafConfig()
    : logLevel_(LogLevel::LOG_INFO)
    , logEnabled_(true)
    , lastModTime_(0) {
}

bool UnLeafConfig::Initialize(const std::wstring& baseDir) {
    CSLockGuard lock(cs_);

    configPath_ = baseDir + L"\\" + CONFIG_FILENAME;

    // Check for INI file first
    if (fs::exists(configPath_)) {
        return Reload();
    }

    // Try to migrate from old JSON format
    if (MigrateFromJson(baseDir)) {
        return true;
    }

    // Create default config if neither exists
    targets_.clear();
    return Save();
}

bool UnLeafConfig::MigrateFromJson(const std::wstring& baseDir) {
    std::wstring oldPath = baseDir + L"\\" + CONFIG_FILENAME_OLD;

    if (!fs::exists(oldPath)) {
        return false;
    }

    try {
        // Read old JSON file
        std::ifstream file(oldPath, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        file.close();

        // Parse JSON content
        if (!ParseJson(content)) {
            return false;
        }

        // Save as new INI format
        if (!Save()) {
            return false;
        }

        // Delete old JSON file
        fs::remove(oldPath);

        LOG_INFO(L"Config: Migrated from JSON to INI format");
        return true;
    }
    catch (const std::exception& e) {
        // Log exception details
        LOG_ERROR(std::wstring(L"Config: Migration error - ") +
                  std::wstring(e.what(), e.what() + std::strlen(e.what())));
        return false;
    }
    catch (...) {
        LOG_ERROR(L"Config: Migration error - unknown exception");
        return false;
    }
}

bool UnLeafConfig::Reload() {
    CSLockGuard lock(cs_);

    try {
        if (!fs::exists(configPath_)) {
            return false;
        }

        // File size guard (max 1MB)
        auto fsize = fs::file_size(configPath_);
        if (fsize > 1048576) {
            LOG_ERROR(L"Config: File too large (" + std::to_wstring(fsize) + L" bytes), max 1MB");
            return false;
        }

        // Read file content
        std::ifstream file(configPath_, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        file.close();

        // Update modification time
        lastModTime_ = GetFileModTime();

        // Parse INI
        if (!ParseIni(content)) {
            return false;
        }

        // Notify change callback
        if (changeCallback_) {
            changeCallback_();
        }

        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR(std::string("Config: Load error - ") + e.what());
        return false;
    }
}

bool UnLeafConfig::Save() {
    CSLockGuard lock(cs_);

    try {
        std::string iniContent = SerializeIni();

        std::ofstream file(configPath_, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }

        file << iniContent;
        file.close();

        lastModTime_ = GetFileModTime();
        return true;
    }
    catch (const std::exception& e) {
        // Log exception details
        LOG_ERROR(std::wstring(L"Config: Save error - ") +
                  std::wstring(e.what(), e.what() + std::strlen(e.what())));
        return false;
    }
    catch (...) {
        LOG_ERROR(L"Config: Save error - unknown exception");
        return false;
    }
}

// INI Parser
bool UnLeafConfig::ParseIni(const std::string& contentIn) {
    try {
        std::string content = contentIn;
        StripUtf8Bom(content);

        targets_.clear();
        logLevel_ = LogLevel::LOG_INFO;
        logEnabled_ = true;

        std::istringstream stream(content);
        std::string line;
        std::string currentSection;

        while (std::getline(stream, line)) {
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);

            // Skip empty lines and comments
            if (line.empty() || line[0] == ';' || line[0] == '#') {
                continue;
            }

            // Section header
            if (line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                // Convert to lowercase for comparison
                std::transform(currentSection.begin(), currentSection.end(),
                              currentSection.begin(),
                              [](unsigned char c) { return static_cast<char>(::tolower(c)); });

                // Warn on unknown sections
                if (currentSection != "targets" && currentSection != "logging") {
                    // Convert section name to wide string for logging
                    std::wstring wideSection(currentSection.begin(), currentSection.end());
                    LOG_ALERT(L"Config: Unknown section ignored: [" + wideSection + L"]");
                }
                continue;
            }

            // Key=Value pair
            size_t eqPos = line.find('=');
            if (eqPos == std::string::npos) continue;

            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);

            // Trim key and value
            size_t keyEnd = key.find_last_not_of(" \t");
            if (keyEnd != std::string::npos) key = key.substr(0, keyEnd + 1);
            size_t valStart = value.find_first_not_of(" \t");
            if (valStart != std::string::npos) value = value.substr(valStart);

            if (currentSection == "targets") {
                // key = process name, value = 1 (enabled) or 0 (disabled)
                bool enabled = (value == "1" || value == "true");

                // Convert to wide string
                int size = MultiByteToWideChar(CP_UTF8, 0, key.c_str(),
                                               static_cast<int>(key.size()), nullptr, 0);
                std::wstring wideName(size, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, key.c_str(),
                                   static_cast<int>(key.size()), &wideName[0], size);

                // Validate process name before adding
                if (!IsValidProcessName(wideName)) {
                    LOG_ALERT(L"Config: Invalid target ignored: " + wideName);
                    continue;
                }
                if (IsCriticalProcess(wideName)) {
                    LOG_ALERT(L"Config: Critical process blocked: " + wideName);
                    continue;
                }

                targets_.emplace_back(wideName, enabled);
            }
            else if (currentSection == "logging") {
                std::string lowerKey = key;
                std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                              [](unsigned char c) { return static_cast<char>(::tolower(c)); });

                if (lowerKey == "loglevel") {
                    std::string lowerValue = value;
                    std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(),
                                  [](unsigned char c) { return static_cast<char>(::toupper(c)); });

                    if (lowerValue == "ERROR") {
                        logLevel_ = LogLevel::LOG_ERROR;
                    } else if (lowerValue == "ALERT") {
                        logLevel_ = LogLevel::LOG_ALERT;
                    } else if (lowerValue == "INFO") {
                        logLevel_ = LogLevel::LOG_INFO;
                    } else if (lowerValue == "DEBUG") {
                        logLevel_ = LogLevel::LOG_DEBUG;
                    } else {
                        // Warn on invalid LogLevel value
                        std::wstring wideValue(value.begin(), value.end());
                        LOG_ALERT(L"Config: Invalid LogLevel ignored (using INFO): " + wideValue);
                    }
                }
                else if (lowerKey == "logenabled") {
                    std::string lowerValue = value;
                    std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(),
                                  [](unsigned char c) { return static_cast<char>(::tolower(c)); });
                    logEnabled_ = (lowerValue == "1" || lowerValue == "true" || lowerValue == "yes" || lowerValue == "on");
                }
                else {
                    // Warn on unknown keys in [Logging]
                    std::wstring wideKey(key.begin(), key.end());
                    LOG_DEBUG(L"Config: Unknown key in [Logging]: " + wideKey);
                }
            }
        }

        return true;
    }
    catch (const std::exception& e) {
        // Log exception details
        LOG_ERROR(std::wstring(L"Config: Parse error - ") +
                  std::wstring(e.what(), e.what() + std::strlen(e.what())));
        return false;
    }
    catch (...) {
        LOG_ERROR(L"Config: Parse error - unknown exception");
        return false;
    }
}

// INI Serializer
std::string UnLeafConfig::SerializeIni() const {
    std::ostringstream oss;

    // Header comment
    oss << "; UnLeaf Configuration\n";
    oss << "; Auto-generated - Do not edit while service is running\n\n";

    oss << "[Logging]\n";
    oss << "; Log level: ERROR, ALERT, INFO, DEBUG\n";
    switch (logLevel_) {
        case LogLevel::LOG_ERROR: oss << "LogLevel=ERROR\n"; break;
        case LogLevel::LOG_ALERT: oss << "LogLevel=ALERT\n"; break;
        case LogLevel::LOG_INFO:  oss << "LogLevel=INFO\n";  break;
        case LogLevel::LOG_DEBUG: oss << "LogLevel=DEBUG\n"; break;
    }
    oss << "; Log output: 1=enabled, 0=disabled\n";
    oss << "LogEnabled=" << (logEnabled_ ? "1" : "0") << "\n";
    oss << "\n";

    // Targets section
    oss << "[Targets]\n";
    for (const auto& target : targets_) {
        // Convert wide string to UTF-8
        std::string name;
        int size = WideCharToMultiByte(CP_UTF8, 0, target.name.c_str(),
                                       static_cast<int>(target.name.size()),
                                       nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            name.resize(size);
            WideCharToMultiByte(CP_UTF8, 0, target.name.c_str(),
                               static_cast<int>(target.name.size()),
                               &name[0], size, nullptr, nullptr);
        }

        oss << name << "=" << (target.enabled ? "1" : "0") << "\n";
    }

    return oss.str();
}

// Legacy JSON parser (for migration only)
bool UnLeafConfig::ParseJson(const std::string& contentIn) {
    try {
        std::string content = contentIn;
        StripUtf8Bom(content);

        targets_.clear();

        // Parse targets array
        std::regex targetsRegex("\"targets\"\\s*:\\s*\\[([^\\]]*)\\]");
        std::smatch targetsMatch;
        if (std::regex_search(content, targetsMatch, targetsRegex)) {
            std::string targetsContent = targetsMatch[1].str();

            // Parse each target object
            std::regex targetRegex("\\{\\s*\"name\"\\s*:\\s*\"([^\"]+)\"(?:\\s*,\\s*\"enabled\"\\s*:\\s*(true|false))?\\s*\\}");
            auto targetBegin = std::sregex_iterator(targetsContent.begin(), targetsContent.end(), targetRegex);
            auto targetEnd = std::sregex_iterator();

            for (auto it = targetBegin; it != targetEnd; ++it) {
                std::smatch match = *it;
                std::string name = match[1].str();
                bool enabled = true;
                if (match[2].matched) {
                    enabled = (match[2].str() == "true");
                }

                // Convert to wide string
                int size = MultiByteToWideChar(CP_UTF8, 0, name.c_str(),
                                               static_cast<int>(name.size()), nullptr, 0);
                std::wstring wideName(size, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, name.c_str(),
                                   static_cast<int>(name.size()), &wideName[0], size);

                targets_.emplace_back(wideName, enabled);
            }
        }

        return true;
    }
    catch (const std::exception& e) {
        // Log exception details
        LOG_ERROR(std::wstring(L"Config: JSON parse error - ") +
                  std::wstring(e.what(), e.what() + std::strlen(e.what())));
        return false;
    }
    catch (...) {
        LOG_ERROR(L"Config: JSON parse error - unknown exception");
        return false;
    }
}

bool UnLeafConfig::AddTarget(const std::wstring& name) {
    CSLockGuard lock(cs_);

    // Check if already exists
    std::wstring lowerName = ToLower(name);
    for (const auto& target : targets_) {
        if (ToLower(target.name) == lowerName) {
            return false;
        }
    }

    // Check if critical process
    if (IsCriticalProcess(name)) {
        return false;
    }

    targets_.emplace_back(name, true);
    return true;
}

bool UnLeafConfig::RemoveTarget(const std::wstring& name) {
    CSLockGuard lock(cs_);

    std::wstring lowerName = ToLower(name);
    auto it = std::remove_if(targets_.begin(), targets_.end(),
        [&lowerName](const TargetProcess& t) {
            return ToLower(t.name) == lowerName;
        });

    if (it != targets_.end()) {
        targets_.erase(it, targets_.end());
        return true;
    }
    return false;
}

bool UnLeafConfig::IsTargetEnabled(const std::wstring& name) const {
    CSLockGuard lock(cs_);

    std::wstring lowerName = ToLower(name);
    for (const auto& target : targets_) {
        if (ToLower(target.name) == lowerName) {
            return target.enabled;
        }
    }
    return false;
}

bool UnLeafConfig::SetTargetEnabled(const std::wstring& name, bool enabled) {
    CSLockGuard lock(cs_);

    std::wstring lowerName = ToLower(name);
    for (auto& target : targets_) {
        if (ToLower(target.name) == lowerName) {
            target.enabled = enabled;
            return true;
        }
    }
    return false;
}

bool UnLeafConfig::HasFileChanged() const {
    return GetFileModTime() != lastModTime_;
}

uint64_t UnLeafConfig::GetFileModTime() const {
    try {
        if (fs::exists(configPath_)) {
            auto ftime = fs::last_write_time(configPath_);
            return static_cast<uint64_t>(ftime.time_since_epoch().count());
        }
    }
    catch (const std::exception& e) {
        // Log at debug level (expected during rotation)
        LOG_DEBUG(std::wstring(L"Config: GetFileModTime - ") +
                  std::wstring(e.what(), e.what() + std::strlen(e.what())));
    }
    catch (...) {
        LOG_DEBUG(L"Config: GetFileModTime - unknown exception");
    }
    return 0;
}

void UnLeafConfig::SetChangeCallback(ConfigChangeCallback callback) {
    CSLockGuard lock(cs_);
    changeCallback_ = std::move(callback);
}

void UnLeafConfig::SetLogEnabled(bool enabled) {
    CSLockGuard lock(cs_);
    logEnabled_ = enabled;
}

} // namespace unleaf
