// UnLeaf v6.0 - Registry Policy Controller Implementation
// Layer 1: OS-level permanent EcoQoS exclusion via registry policies

#include "registry_policy.h"
#include <sstream>
#include <algorithm>

namespace unleaf {

RegistryPolicyController& RegistryPolicyController::Instance() {
    static RegistryPolicyController instance;
    return instance;
}

RegistryPolicyController::RegistryPolicyController()
    : initialized_(false) {
}

RegistryPolicyController::~RegistryPolicyController() {
    // Note: CleanupAllPolicies() should be called explicitly before destruction
    // to ensure proper cleanup order
}

bool RegistryPolicyController::Initialize(const std::wstring& baseDir) {
    baseDir_ = baseDir;
    initialized_ = true;
    LOG_INFO(L"Policy: Registry controller initialized");
    return true;
}

std::wstring RegistryPolicyController::ExtractFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

bool RegistryPolicyController::EnsureKeyExists(HKEY hRoot, const std::wstring& subKey, HKEY* outKey) {
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

bool RegistryPolicyController::SetDwordValue(HKEY hKey, const std::wstring& valueName, DWORD value) {
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

bool RegistryPolicyController::DeleteValue(HKEY hKey, const std::wstring& valueName) {
    LONG result = RegDeleteValueW(hKey, valueName.c_str());
    return (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND);
}

bool RegistryPolicyController::DisablePowerThrottling(const std::wstring& exePath) {
    // Registry path: HKLM\SYSTEM\CurrentControlSet\Control\Power\PowerThrottling
    // Value: <exePath> = 1 (disabled)

    HKEY hKey = nullptr;
    if (!EnsureKeyExists(HKEY_LOCAL_MACHINE, REG_POWER_THROTTLING_PATH, &hKey)) {
        DWORD error = GetLastError();
        std::wstringstream ss;
        ss << L"[POLICY] Failed to open PowerThrottling key (error=" << error << L")";
        LOG_DEBUG(ss.str());
        return false;
    }

    // Value name is the full path, value is 1 to disable throttling
    bool success = SetDwordValue(hKey, exePath, 1);
    RegCloseKey(hKey);

    if (success) {
        std::wstringstream ss;
        ss << L"[POLICY] Disabled power throttling for: " << ExtractFileName(exePath);
        LOG_DEBUG(ss.str());
    }

    return success;
}

bool RegistryPolicyController::EnablePowerThrottling(const std::wstring& exePath) {
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        REG_POWER_THROTTLING_PATH,
        0,
        KEY_READ | KEY_WRITE,
        &hKey
    );

    if (result != ERROR_SUCCESS) {
        return true;  // Key doesn't exist, nothing to remove
    }

    bool success = DeleteValue(hKey, exePath);
    RegCloseKey(hKey);

    if (success) {
        std::wstringstream ss;
        ss << L"[POLICY] Enabled power throttling for: " << ExtractFileName(exePath);
        LOG_DEBUG(ss.str());
    }

    return success;
}

bool RegistryPolicyController::SetIFEOPerfOptions(const std::wstring& exeName, DWORD cpuPriority) {
    // Registry path: HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<exeName>\PerfOptions
    // Value: CpuPriorityClass = <cpuPriority>

    std::wstring keyPath = std::wstring(REG_IFEO_PATH) + L"\\" + exeName + L"\\PerfOptions";

    HKEY hKey = nullptr;
    if (!EnsureKeyExists(HKEY_LOCAL_MACHINE, keyPath, &hKey)) {
        DWORD error = GetLastError();
        std::wstringstream ss;
        ss << L"[POLICY] Failed to create IFEO PerfOptions for " << exeName << L" (error=" << error << L")";
        LOG_DEBUG(ss.str());
        return false;
    }

    bool success = SetDwordValue(hKey, L"CpuPriorityClass", cpuPriority);
    RegCloseKey(hKey);

    if (success) {
        std::wstringstream ss;
        ss << L"[POLICY] Set IFEO CpuPriorityClass=" << cpuPriority << L" for: " << exeName;
        LOG_DEBUG(ss.str());
    }

    return success;
}

bool RegistryPolicyController::RemoveIFEOPerfOptions(const std::wstring& exeName) {
    std::wstring keyPath = std::wstring(REG_IFEO_PATH) + L"\\" + exeName + L"\\PerfOptions";

    // Delete the PerfOptions subkey
    LONG result = RegDeleteKeyW(HKEY_LOCAL_MACHINE, keyPath.c_str());

    if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND) {
        // Also try to delete parent key if empty
        std::wstring parentPath = std::wstring(REG_IFEO_PATH) + L"\\" + exeName;
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, parentPath.c_str());  // Ignore result

        std::wstringstream ss;
        ss << L"[POLICY] Removed IFEO PerfOptions for: " << exeName;
        LOG_DEBUG(ss.str());
        return true;
    }

    return false;
}

bool RegistryPolicyController::AddToExclusionList(const std::wstring& exePath) {
    // This is implemented as DisablePowerThrottling - same mechanism
    return DisablePowerThrottling(exePath);
}

bool RegistryPolicyController::RemoveFromExclusionList(const std::wstring& exePath) {
    return EnablePowerThrottling(exePath);
}

bool RegistryPolicyController::ApplyFullExclusion(const std::wstring& exePath, const std::wstring& exeName) {
    bool success = true;

    // Layer 1a: Power Throttling registry exclusion
    if (!DisablePowerThrottling(exePath)) {
        success = false;
    }

    // Layer 1b: IFEO PerfOptions for CPU priority
    if (!SetIFEOPerfOptions(exeName, IFEO_CPU_PRIORITY_HIGH)) {
        success = false;
    }

    // Track applied policy for cleanup
    if (success) {
        CSLockGuard lock(policyCs_);

        // Check if already tracked
        std::wstring lowerName = ToLower(exeName);
        bool found = false;
        for (const auto& name : appliedPolicies_) {
            if (ToLower(name) == lowerName) {
                found = true;
                break;
            }
        }

        if (!found) {
            appliedPolicies_.push_back(exeName);
        }
    }

    return success;
}

bool RegistryPolicyController::RemoveFullExclusion(const std::wstring& exePath, const std::wstring& exeName) {
    bool success = true;

    // Remove Power Throttling exclusion
    if (!EnablePowerThrottling(exePath)) {
        success = false;
    }

    // Remove IFEO PerfOptions
    if (!RemoveIFEOPerfOptions(exeName)) {
        success = false;
    }

    // Remove from tracked list
    {
        CSLockGuard lock(policyCs_);
        std::wstring lowerName = ToLower(exeName);
        appliedPolicies_.erase(
            std::remove_if(appliedPolicies_.begin(), appliedPolicies_.end(),
                [&lowerName](const std::wstring& name) {
                    return ToLower(name) == lowerName;
                }),
            appliedPolicies_.end()
        );
    }

    return success;
}

void RegistryPolicyController::CleanupAllPolicies() {
    CSLockGuard lock(policyCs_);

    if (appliedPolicies_.empty()) {
        LOG_DEBUG(L"Policy: No policies to cleanup");
        return;
    }

    std::wstringstream ss;
    ss << L"[POLICY] Cleaning up " << appliedPolicies_.size() << L" policies";
    LOG_DEBUG(ss.str());

    // Note: We don't have the full path, so we can only remove IFEO entries
    // Power throttling entries require the full path which we don't store
    for (const auto& exeName : appliedPolicies_) {
        RemoveIFEOPerfOptions(exeName);
    }

    appliedPolicies_.clear();
    LOG_INFO(L"Policy: Cleanup complete");
}

bool RegistryPolicyController::IsPolicyApplied(const std::wstring& exeName) const {
    CSLockGuard lock(policyCs_);
    std::wstring lowerName = ToLower(exeName);

    for (const auto& name : appliedPolicies_) {
        if (ToLower(name) == lowerName) {
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> RegistryPolicyController::GetPolicyList() const {
    CSLockGuard lock(policyCs_);
    return appliedPolicies_;
}

} // namespace unleaf
