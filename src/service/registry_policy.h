#pragma once
// UnLeaf v6.0 - Registry Policy Controller
// Layer 1: OS-level permanent EcoQoS exclusion via registry policies

#include "../common/types.h"
#include "../common/logger.h"
#include <string>
#include <vector>

namespace unleaf {

// Registry paths for Power Throttling control
constexpr const wchar_t* REG_POWER_THROTTLING_PATH =
    L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling";
constexpr const wchar_t* REG_IFEO_PATH =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

// IFEO PerfOptions CPU priority values
constexpr DWORD IFEO_CPU_PRIORITY_HIGH = 3;      // High priority
constexpr DWORD IFEO_CPU_PRIORITY_ABOVE_NORMAL = 2;
constexpr DWORD IFEO_CPU_PRIORITY_NORMAL = 1;

class RegistryPolicyController {
public:
    // Singleton access
    static RegistryPolicyController& Instance();

    // Initialize controller with base directory for cleanup tracking
    bool Initialize(const std::wstring& baseDir);

    // Disable power throttling for a specific executable path
    // Equivalent to: powercfg /powerthrottling disable /path:<exePath>
    bool DisablePowerThrottling(const std::wstring& exePath);

    // Enable power throttling for a specific executable (restore default)
    bool EnablePowerThrottling(const std::wstring& exePath);

    // Set IFEO PerfOptions for CPU priority
    // Creates/modifies: HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<exeName>\PerfOptions
    bool SetIFEOPerfOptions(const std::wstring& exeName, DWORD cpuPriority);

    // Remove IFEO PerfOptions
    bool RemoveIFEOPerfOptions(const std::wstring& exeName);

    // Add executable to power throttling exclusion list
    bool AddToExclusionList(const std::wstring& exePath);

    // Remove executable from exclusion list
    bool RemoveFromExclusionList(const std::wstring& exePath);

    // Apply all policies for a target executable
    // Combines: DisablePowerThrottling + SetIFEOPerfOptions + AddToExclusionList
    bool ApplyFullExclusion(const std::wstring& exePath, const std::wstring& exeName);

    // Remove all policies for a target executable
    bool RemoveFullExclusion(const std::wstring& exePath, const std::wstring& exeName);

    // Cleanup all policies created by UnLeaf (called on uninstall/service stop)
    void CleanupAllPolicies();

    // Check if policies are already applied for an executable
    bool IsPolicyApplied(const std::wstring& exeName) const;

    // Get list of executables with active policies
    std::vector<std::wstring> GetPolicyList() const;

private:
    RegistryPolicyController();
    ~RegistryPolicyController();
    RegistryPolicyController(const RegistryPolicyController&) = delete;
    RegistryPolicyController& operator=(const RegistryPolicyController&) = delete;

    // Helper: Extract filename from full path
    static std::wstring ExtractFileName(const std::wstring& path);

    // Helper: Create registry key if not exists
    static bool EnsureKeyExists(HKEY hRoot, const std::wstring& subKey, HKEY* outKey);

    // Helper: Set DWORD value
    static bool SetDwordValue(HKEY hKey, const std::wstring& valueName, DWORD value);

    // Helper: Delete value
    static bool DeleteValue(HKEY hKey, const std::wstring& valueName);

    // Track applied policies for cleanup
    std::vector<std::wstring> appliedPolicies_;
    mutable CriticalSection policyCs_;
    std::wstring baseDir_;
    bool initialized_;
};

} // namespace unleaf
