#pragma once
// UnLeaf - Registry Policy Manager (v5: IFEO/PowerThrottle split)
// Design Contract: DC-1~DC-6, REQ-1~REQ-8 compliant.
// Lock order: policyCs_ → manifestCs_ (never reversed, never held simultaneously)
// policyCs_ holds ZERO I/O. All registry/file I/O is performed outside locks.
//
// DESIGN CONTRACT:
// EngineCore guarantees canonicalized paths via CanonicalizePath().
// RegistryPolicyManager assumes canonical input.
// NormalizePath is defensive only and must not be relied upon for primary normalization.
//
// DO NOT:
// - Rely on NormalizePath as primary normalization
// - Pass raw/unresolved paths into RegistryPolicyManager

#include "../common/types.h"
#include "../common/logger.h"
#include <string>
#include <set>
#include <map>
#include <vector>
#include <atomic>
#include <fstream>

namespace unleaf {

// ====================================================================
// Registry path definitions
// ====================================================================
constexpr const wchar_t* REG_POWER_THROTTLING_PATH =
    L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling";

constexpr const wchar_t* REG_IFEO_PATH =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

constexpr DWORD IFEO_CPU_PRIORITY_HIGH = 3;

constexpr const wchar_t* POLICY_MANIFEST_FILENAME = L"UnLeaf_policies.ini";
// ====================================================================

// Per-entry state (protected by policyCs_)
enum class PolicyState : uint8_t {
    APPLYING,    // Registry write in progress
    COMMITTED    // Registry verified, memory consistent
};

struct PolicyEntry {
    PolicyState state;
    std::wstring lowerExeName;
    std::wstring registryPath;   // original path for registry value name
};

// Lock-free pending removal node (Treiber stack)
struct PendingRemovalNode {
    std::wstring exeName;
    std::wstring fullPath;
    uint8_t reenqueueCount;
    PendingRemovalNode* next;
};

class RegistryPolicyManager {
public:
    static RegistryPolicyManager& Instance();
    bool Initialize(const std::wstring& baseDir);

    // Policy application
    bool ApplyPolicy(const std::wstring& exeName, const std::wstring& exeFullPath);
    bool ApplyIFEOOnly(const std::wstring& exeName);  // Proactive IFEO (name-only targets)

    // Proactive sync: remove entries not in config
    void ReconcileWithConfig(const std::set<std::wstring>& desiredNames,
                             const std::set<std::wstring>& desiredPaths);

    // Policy cleanup
    void CleanupAllPolicies();   // Normal service stop
    void RemoveAllPolicies();    // Service unregistration (Manager safety net)

    // Registry integrity verification + repair (called from SafetyNet, 30min interval)
    void VerifyAndRepair();

    // Query
    bool IsPolicyApplied(const std::wstring& exeName) const;
    bool IsPolicyValid(const std::wstring& canonPath) const;
    bool HasPolicy(const std::wstring& canonPath) const;
    std::vector<std::wstring> GetAppliedPolicies() const;

private:
    RegistryPolicyManager();
    ~RegistryPolicyManager();
    RegistryPolicyManager(const RegistryPolicyManager&) = delete;
    RegistryPolicyManager& operator=(const RegistryPolicyManager&) = delete;

    // Manifest persistence
    bool LoadManifest();         // Call outside policyCs_ (single-threaded or exclusive)
    void SaveManifestAtomic();
    bool DeleteManifest();

    // Registry I/O (ALL called outside locks)
    bool DisablePowerThrottling(const std::wstring& exePath);
    bool EnablePowerThrottling(const std::wstring& exePath);
    bool SetIFEOPerfOptions(const std::wstring& exeName, DWORD cpuPriority);
    bool RemoveIFEOPerfOptions(const std::wstring& exeName);
    bool IFEOKeyExists(const std::wstring& lowerName) const;
    bool PowerThrottleValueExists(const std::wstring& registryPath) const;

    // Helpers (unchanged from v1)
    static std::wstring ExtractFileName(const std::wstring& path);
    static bool EnsureKeyExists(HKEY hRoot, const std::wstring& subKey, HKEY* outKey);
    static bool SetDwordValue(HKEY hKey, const std::wstring& valueName, DWORD value);
    static bool DeleteValue(HKEY hKey, const std::wstring& valueName);

    // Remove operations
    void RemoveSinglePolicy(const std::wstring& exeName, const std::wstring& fullPath);
    void RemoveSinglePolicyInternal(const std::wstring& exeName,
                                    const std::wstring& fullPath,
                                    uint8_t reenqueueCount);
    void DrainPendingRemovals();

    // Lock-free pending queue operations (Treiber stack)
    void EnqueuePendingRemoval(PendingRemovalNode* node);
    PendingRemovalNode* StealAllPendingRemovals();

    // ── Policy state (policyCs_ protected, ZERO I/O under lock) ──
    std::map<std::wstring, PolicyEntry>  policyMap_;       // canonPath → entry
    std::set<std::wstring>               ifeoAppliedSet_;  // lowerExeName (COMMITTED only)
    std::map<std::wstring, size_t>       ifeoRefCount_;    // lowerExeName → path count
    mutable CriticalSection              policyCs_;

    // ── Lock-free pending removal queue (REQ-2) ──
    std::atomic<PendingRemovalNode*>     pendingHead_{nullptr};

    // ── Monotonic version counter (REQ-4: fetch_add only, never reset) ──
    std::atomic<uint64_t>                stateVersion_{0};

    // ── Manifest I/O lock ──
    mutable CriticalSection              manifestCs_;

    // ── VerifyAndRepair single-execution guard (REQ-6) ──
    std::atomic<bool>                    verifyRunning_{false};

    // ── Stale manifest log throttle ──
    std::atomic<uint32_t>                staleLogCount_{0};

    // ── Common ──
    std::wstring baseDir_;
    std::wstring manifestPath_;
    bool initialized_;
};

} // namespace unleaf
