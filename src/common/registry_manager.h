#pragma once
// UnLeaf - Registry Policy Manager
// Centralized management of all registry modifications made by UnLeaf.
// Ensures complete cleanup on service stop and service unregistration.

#include "../common/types.h"
#include "../common/logger.h"
#include <string>
#include <map>
#include <vector>

namespace unleaf {

// ====================================================================
// UnLeaf が管理するレジストリパス (一元定義)
// ====================================================================
// A. EcoQoS 永続除外ポリシー
//    HKLM\SYSTEM\CurrentControlSet\Control\Power\PowerThrottling\<FullExePath> = 1
constexpr const wchar_t* REG_POWER_THROTTLING_PATH =
    L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling";

// B. IFEO 優先度設定
//    HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\
//    Image File Execution Options\<ExeName>\PerfOptions\CpuPriorityClass = 3
constexpr const wchar_t* REG_IFEO_PATH =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

constexpr DWORD IFEO_CPU_PRIORITY_HIGH = 3;

// マニフェストファイル名
constexpr const wchar_t* POLICY_MANIFEST_FILENAME = L"UnLeaf_policies.ini";
// ====================================================================

class RegistryPolicyManager {
public:
    static RegistryPolicyManager& Instance();
    bool Initialize(const std::wstring& baseDir);

    // --- ポリシー適用 (Service Engine: プロセス検出時に呼ぶ) ---
    bool ApplyPolicy(const std::wstring& exeName, const std::wstring& exeFullPath);

    // --- ポリシー全削除 ---
    // CleanupAllPolicies: サービス正常停止時 (インメモリ map から削除)
    void CleanupAllPolicies();
    // RemoveAllPolicies: サービス登録解除時 (マニフェスト経由、セーフティネット)
    // RemoveAllPolicies is fully idempotent by design.
    // Missing manifest, missing registry keys, and empty in-memory map
    // are all treated as normal conditions (not errors).
    // This guarantees safe repeated calls in any lifecycle scenario.
    void RemoveAllPolicies();

    // --- 照会 ---
    bool IsPolicyApplied(const std::wstring& exeName) const;
    std::vector<std::wstring> GetAppliedPolicies() const;

private:
    RegistryPolicyManager();
    ~RegistryPolicyManager();
    RegistryPolicyManager(const RegistryPolicyManager&) = delete;
    RegistryPolicyManager& operator=(const RegistryPolicyManager&) = delete;

    // マニフェスト永続化
    bool LoadManifest();
    bool SaveManifest();
    bool DeleteManifest();

    // レジストリ操作
    bool DisablePowerThrottling(const std::wstring& exePath);
    bool EnablePowerThrottling(const std::wstring& exePath);
    bool SetIFEOPerfOptions(const std::wstring& exeName, DWORD cpuPriority);
    bool RemoveIFEOPerfOptions(const std::wstring& exeName);

    // ヘルパー
    static std::wstring ExtractFileName(const std::wstring& path);
    static bool EnsureKeyExists(HKEY hRoot, const std::wstring& subKey, HKEY* outKey);
    static bool SetDwordValue(HKEY hKey, const std::wstring& valueName, DWORD value);
    static bool DeleteValue(HKEY hKey, const std::wstring& valueName);

    // 内部: 単一エントリ削除
    void RemoveSinglePolicy(const std::wstring& exeName, const std::wstring& fullPath);

    // 追跡: exeName(小文字) → exeFullPath
    std::map<std::wstring, std::wstring> appliedPolicies_;
    mutable CriticalSection policyCs_;
    std::wstring baseDir_;
    std::wstring manifestPath_;
    bool initialized_;
};

} // namespace unleaf
