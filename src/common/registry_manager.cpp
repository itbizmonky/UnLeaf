// UnLeaf - Registry Policy Manager Implementation
// Centralized management of all registry modifications made by UnLeaf.
// Ensures complete cleanup on service stop and service unregistration.

#include "registry_manager.h"
#include <sstream>
#include <fstream>
#include <algorithm>

namespace unleaf {

RegistryPolicyManager& RegistryPolicyManager::Instance() {
    static RegistryPolicyManager instance;
    return instance;
}

RegistryPolicyManager::RegistryPolicyManager()
    : initialized_(false) {
}

RegistryPolicyManager::~RegistryPolicyManager() {
}

bool RegistryPolicyManager::Initialize(const std::wstring& baseDir) {
    CSLockGuard lock(policyCs_);
    baseDir_ = baseDir;
    manifestPath_ = baseDir + L"\\" + POLICY_MANIFEST_FILENAME;
    initialized_ = true;

    // Load existing manifest to restore in-memory state (crash recovery)
    LoadManifest();

    LOG_INFO(L"[REGISTRY] Policy manager initialized");
    return true;
}

// ====================================================================
// Manifest Persistence
// ====================================================================

bool RegistryPolicyManager::LoadManifest() {
    // Load manifest file into appliedPolicies_ map.
    // Missing file is normal (first run or after clean shutdown).

    std::wifstream file(manifestPath_);
    if (!file.is_open()) {
        LOG_DEBUG(L"[REGISTRY] Manifest not found (normal)");
        return true;  // Not an error
    }

    std::wstring line;
    bool inSection = false;

    while (std::getline(file, line)) {
        // Trim trailing whitespace
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n' || line.back() == L' ')) {
            line.pop_back();
        }

        // Skip empty lines and comments
        if (line.empty() || line[0] == L';') continue;

        // Section header
        if (line == L"[AppliedPolicies]") {
            inSection = true;
            continue;
        }
        if (line[0] == L'[') {
            inSection = false;
            continue;
        }

        // Parse key=value in [AppliedPolicies] section
        if (inSection) {
            size_t eqPos = line.find(L'=');
            if (eqPos != std::wstring::npos) {
                std::wstring exeName = line.substr(0, eqPos);
                std::wstring fullPath = line.substr(eqPos + 1);
                if (!exeName.empty() && !fullPath.empty()) {
                    std::wstring lowerName = ToLower(exeName);
                    appliedPolicies_[lowerName] = fullPath;
                }
            }
        }
    }

    if (!appliedPolicies_.empty()) {
        std::wstringstream ss;
        ss << L"[REGISTRY] Manifest loaded: " << appliedPolicies_.size() << L" entries restored";
        LOG_INFO(ss.str());
    }

    return true;
}

bool RegistryPolicyManager::SaveManifest() {
    std::wofstream file(manifestPath_, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        LOG_DEBUG(L"[REGISTRY] Failed to save manifest");
        return false;
    }

    file << L"; UnLeaf Registry Policy Manifest - Auto-managed\n";
    file << L"; Tracks registry modifications for cleanup on service unregistration\n";
    file << L"[AppliedPolicies]\n";

    for (const auto& [name, path] : appliedPolicies_) {
        file << name << L"=" << path << L"\n";
    }

    file.flush();
    file.close();
    return true;
}

bool RegistryPolicyManager::DeleteManifest() {
    BOOL result = DeleteFileW(manifestPath_.c_str());
    if (result || GetLastError() == ERROR_FILE_NOT_FOUND) {
        return true;  // Deleted or already absent — both normal
    }
    LOG_DEBUG(L"[REGISTRY] Failed to delete manifest");
    return false;
}

// ====================================================================
// Registry Operations (migrated from registry_policy.cpp)
// ====================================================================

std::wstring RegistryPolicyManager::ExtractFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

bool RegistryPolicyManager::EnsureKeyExists(HKEY hRoot, const std::wstring& subKey, HKEY* outKey) {
    DWORD disposition = 0;
    LONG result = RegCreateKeyExW(
        hRoot,
        subKey.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE,
        nullptr,
        outKey,
        &disposition
    );
    return (result == ERROR_SUCCESS);
}

bool RegistryPolicyManager::SetDwordValue(HKEY hKey, const std::wstring& valueName, DWORD value) {
    LONG result = RegSetValueExW(
        hKey,
        valueName.c_str(),
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(DWORD)
    );
    return (result == ERROR_SUCCESS);
}

bool RegistryPolicyManager::DeleteValue(HKEY hKey, const std::wstring& valueName) {
    LONG result = RegDeleteValueW(hKey, valueName.c_str());
    return (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND);
}

bool RegistryPolicyManager::DisablePowerThrottling(const std::wstring& exePath) {
    HKEY hKey = nullptr;
    if (!EnsureKeyExists(HKEY_LOCAL_MACHINE, REG_POWER_THROTTLING_PATH, &hKey)) {
        DWORD error = GetLastError();
        std::wstringstream ss;
        ss << L"[REGISTRY] Failed to open PowerThrottling key (error=" << error << L")";
        LOG_DEBUG(ss.str());
        return false;
    }

    bool success = SetDwordValue(hKey, exePath, 1);
    RegCloseKey(hKey);

    if (success) {
        std::wstringstream ss;
        ss << L"[REGISTRY] Disabled power throttling for: " << ExtractFileName(exePath);
        LOG_DEBUG(ss.str());
    }

    return success;
}

bool RegistryPolicyManager::EnablePowerThrottling(const std::wstring& exePath) {
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        REG_POWER_THROTTLING_PATH,
        0,
        KEY_READ | KEY_WRITE,
        &hKey
    );

    if (result != ERROR_SUCCESS) {
        // Key doesn't exist — nothing to remove (idempotent)
        LOG_DEBUG(L"[REGISTRY] PowerThrottling key not found, skip removal");
        return true;
    }

    bool success = DeleteValue(hKey, exePath);
    RegCloseKey(hKey);

    if (success) {
        std::wstringstream ss;
        ss << L"[REGISTRY] Enabled power throttling for: " << ExtractFileName(exePath);
        LOG_DEBUG(ss.str());
    }

    return success;
}

bool RegistryPolicyManager::SetIFEOPerfOptions(const std::wstring& exeName, DWORD cpuPriority) {
    std::wstring keyPath = std::wstring(REG_IFEO_PATH) + L"\\" + exeName + L"\\PerfOptions";

    HKEY hKey = nullptr;
    if (!EnsureKeyExists(HKEY_LOCAL_MACHINE, keyPath, &hKey)) {
        DWORD error = GetLastError();
        std::wstringstream ss;
        ss << L"[REGISTRY] Failed to create IFEO PerfOptions for " << exeName << L" (error=" << error << L")";
        LOG_DEBUG(ss.str());
        return false;
    }

    bool success = SetDwordValue(hKey, L"CpuPriorityClass", cpuPriority);
    RegCloseKey(hKey);

    if (success) {
        std::wstringstream ss;
        ss << L"[REGISTRY] Set IFEO CpuPriorityClass=" << cpuPriority << L" for: " << exeName;
        LOG_DEBUG(ss.str());
    }

    return success;
}

bool RegistryPolicyManager::RemoveIFEOPerfOptions(const std::wstring& exeName) {
    std::wstring keyPath = std::wstring(REG_IFEO_PATH) + L"\\" + exeName + L"\\PerfOptions";

    LONG result = RegDeleteKeyW(HKEY_LOCAL_MACHINE, keyPath.c_str());

    if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND) {
        // Also try to delete parent key if empty
        std::wstring parentPath = std::wstring(REG_IFEO_PATH) + L"\\" + exeName;
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, parentPath.c_str());  // Ignore result

        if (result == ERROR_FILE_NOT_FOUND) {
            LOG_DEBUG(L"[REGISTRY] IFEO key not found for " + exeName + L", skip removal");
        } else {
            LOG_DEBUG(L"[REGISTRY] Removed IFEO PerfOptions for: " + exeName);
        }
        return true;
    }

    return false;
}

// ====================================================================
// Policy Application and Cleanup
// ====================================================================

bool RegistryPolicyManager::ApplyPolicy(const std::wstring& exeName, const std::wstring& exeFullPath) {
    CSLockGuard lock(policyCs_);

    std::wstring lowerName = ToLower(exeName);

    // Check if already applied (idempotent)
    if (appliedPolicies_.find(lowerName) != appliedPolicies_.end()) {
        return true;
    }

    // MUST: SaveManifest BEFORE registry writes (Scenario 14 crash-safety).
    // If crash occurs after manifest save but before registry write:
    //   -> manifest exists -> RemoveAllPolicies can clean up non-existent keys (idempotent)
    // If crash occurs before manifest save:
    //   -> registry not yet written -> no leak
    appliedPolicies_[lowerName] = exeFullPath;
    SaveManifest();

    // Registry writes
    bool success = true;
    if (!DisablePowerThrottling(exeFullPath)) {
        success = false;
    }
    if (!SetIFEOPerfOptions(exeName, IFEO_CPU_PRIORITY_HIGH)) {
        success = false;
    }

    if (success) {
        std::wstringstream ss;
        ss << L"[REGISTRY] Applied EcoQoS exclusion for " << exeName;
        LOG_INFO(ss.str());
    }

    return success;
}

void RegistryPolicyManager::RemoveSinglePolicy(const std::wstring& exeName, const std::wstring& fullPath) {
    // Remove PowerThrottling entry (idempotent — key not found is normal)
    EnablePowerThrottling(fullPath);

    // Remove IFEO PerfOptions entry (idempotent — key not found is normal)
    RemoveIFEOPerfOptions(exeName);

    std::wstringstream ss;
    ss << L"[REGISTRY] Removed EcoQoS exclusion for " << exeName;
    LOG_INFO(ss.str());
}

void RegistryPolicyManager::CleanupAllPolicies() {
    // Called on normal service stop.
    // Uses in-memory map (primary source of truth during runtime).
    CSLockGuard lock(policyCs_);

    if (appliedPolicies_.empty()) {
        LOG_DEBUG(L"[REGISTRY] No policies to cleanup");
        DeleteManifest();
        return;
    }

    size_t count = appliedPolicies_.size();

    for (const auto& [name, path] : appliedPolicies_) {
        RemoveSinglePolicy(name, path);
    }

    appliedPolicies_.clear();

    // Normal shutdown complete — manifest no longer needed
    DeleteManifest();

    std::wstringstream ss;
    ss << L"[REGISTRY] Cleanup complete (" << count << L" entries removed)";
    LOG_INFO(ss.str());
}

void RegistryPolicyManager::RemoveAllPolicies() {
    // Called on service unregistration (Manager safety net).
    // RemoveAllPolicies is fully idempotent by design.
    // Missing manifest, missing registry keys, and empty in-memory map
    // are all treated as normal conditions (not errors).
    // This guarantees safe repeated calls in any lifecycle scenario.
    CSLockGuard lock(policyCs_);

    // Load latest manifest state (may have been written by service before crash)
    LoadManifest();

    if (appliedPolicies_.empty()) {
        LOG_DEBUG(L"[REGISTRY] RemoveAllPolicies: no entries to clean");
        DeleteManifest();
        return;
    }

    size_t count = appliedPolicies_.size();

    for (const auto& [name, path] : appliedPolicies_) {
        RemoveSinglePolicy(name, path);
    }

    appliedPolicies_.clear();
    DeleteManifest();

    std::wstringstream ss;
    ss << L"[REGISTRY] All policies removed (" << count << L" entries cleaned)";
    LOG_INFO(ss.str());
}

bool RegistryPolicyManager::IsPolicyApplied(const std::wstring& exeName) const {
    CSLockGuard lock(policyCs_);
    std::wstring lowerName = ToLower(exeName);
    return appliedPolicies_.find(lowerName) != appliedPolicies_.end();
}

std::vector<std::wstring> RegistryPolicyManager::GetAppliedPolicies() const {
    CSLockGuard lock(policyCs_);
    std::vector<std::wstring> result;
    result.reserve(appliedPolicies_.size());
    for (const auto& [name, path] : appliedPolicies_) {
        result.push_back(name);
    }
    return result;
}

} // namespace unleaf
