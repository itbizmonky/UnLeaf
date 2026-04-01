// UnLeaf - Registry Policy Manager Implementation (v5)
// IFEO/PowerThrottle split with per-entry state machine.
// Lock order: policyCs_ → manifestCs_ (never reversed, never held simultaneously)
// policyCs_ holds ZERO I/O.
//
// DESIGN CONTRACT:
// EngineCore guarantees canonicalized paths via CanonicalizePath().
// RegistryPolicyManager assumes canonical input.
// NormalizePath is defensive only and must not be relied upon for primary normalization.
//
// DO NOT:
// - Rely on NormalizePath as primary normalization
// - Pass raw/unresolved paths into RegistryPolicyManager

#include "registry_manager.h"
#include <fstream>

namespace unleaf {

RegistryPolicyManager& RegistryPolicyManager::Instance() {
    static RegistryPolicyManager instance;
    return instance;
}

RegistryPolicyManager::RegistryPolicyManager()
    : initialized_(false) {
}

RegistryPolicyManager::~RegistryPolicyManager() {
    // Drain any remaining pending removal nodes to avoid leaks
    PendingRemovalNode* node = pendingHead_.exchange(nullptr, std::memory_order_acquire);
    while (node) {
        PendingRemovalNode* next = node->next;
        delete node;
        node = next;
    }
}

bool RegistryPolicyManager::Initialize(const std::wstring& baseDir) {
    // Startup single-threaded — no lock needed
    baseDir_ = baseDir;
    manifestPath_ = baseDir + L"\\" + POLICY_MANIFEST_FILENAME;
    initialized_ = true;

    // File I/O — outside lock (single-threaded at startup)
    LoadManifest();

    LOG_INFO(L"[REGISTRY] Policy manager initialized");
    return true;
}

// ====================================================================
// Manifest Persistence
// ====================================================================

bool RegistryPolicyManager::LoadManifest() {
    // ★ Call outside policyCs_ (file I/O).
    //    Caller guarantees exclusivity:
    //    - Initialize: startup single-threaded
    //    - RemoveAllPolicies: separate process (Manager), Service not running

    std::wifstream file(manifestPath_);
    if (!file.is_open()) {
        LOG_DEBUG(L"[REGISTRY] Manifest not found (normal)");
        return true;
    }

    std::wstring line;
    enum class Section { NONE, IFEO, PT, LEGACY } section = Section::NONE;

    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n' || line.back() == L' '))
            line.pop_back();
        if (line.empty() || line[0] == L';') continue;

        if (line == L"[IFEOPolicies]")         { section = Section::IFEO;   continue; }
        if (line == L"[PowerThrottlePolicies]") { section = Section::PT;     continue; }
        if (line == L"[AppliedPolicies]")       { section = Section::LEGACY; continue; }
        if (line[0] == L'[') { section = Section::NONE; continue; }

        switch (section) {
        case Section::IFEO:
            ifeoAppliedSet_.insert(ToLower(line));
            break;

        case Section::PT: {
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) {
                std::wstring regPath = line.substr(0, eq);
                std::wstring exeName = line.substr(eq + 1);
                if (!regPath.empty() && !exeName.empty()) {
                    std::wstring canonPath = NormalizePath(regPath);
                    std::wstring lowerName = ToLower(exeName);
                    if (policyMap_.count(canonPath) == 0) {
                        policyMap_[canonPath] = { PolicyState::COMMITTED, lowerName, regPath };
                        ifeoRefCount_[lowerName]++;
                    }
                }
            }
            break;
        }

        case Section::LEGACY: {
            // Migration from v1 format: exeName=fullPath
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) {
                std::wstring exeName = line.substr(0, eq);
                std::wstring fullPath = line.substr(eq + 1);
                if (!exeName.empty() && !fullPath.empty()) {
                    std::wstring lowerName = ToLower(exeName);
                    std::wstring canonPath = NormalizePath(fullPath);
                    ifeoAppliedSet_.insert(lowerName);
                    if (policyMap_.count(canonPath) == 0) {
                        policyMap_[canonPath] = { PolicyState::COMMITTED, lowerName, fullPath };
                        ifeoRefCount_[lowerName]++;
                    }
                }
            }
            break;
        }

        default: break;
        }
    }

    if (!ifeoAppliedSet_.empty() || !policyMap_.empty()) {
        wchar_t buf[128];
        swprintf_s(buf, L"[REGISTRY] Manifest loaded: %zu IFEO + %zu PT entries",
                   ifeoAppliedSet_.size(), policyMap_.size());
        LOG_INFO(buf);
    }
    return true;
}

void RegistryPolicyManager::SaveManifestAtomic() {
    // Phase 1: snapshot (policyCs_ — ZERO I/O)
    std::set<std::wstring> ifeoCopy;
    std::vector<std::pair<std::wstring, std::wstring>> ptCopy;  // registryPath, lowerExeName
    uint64_t snapshotVersion;

    auto takeSnapshot = [&]() {
        CSLockGuard lock(policyCs_);
        ifeoCopy = ifeoAppliedSet_;
        ptCopy.clear();
        ptCopy.reserve(policyMap_.size());
        for (const auto& [key, entry] : policyMap_) {
            if (entry.state == PolicyState::COMMITTED) {
                ptCopy.emplace_back(entry.registryPath, entry.lowerExeName);
            }
        }
        snapshotVersion = stateVersion_.load(std::memory_order_acquire);
    };

    takeSnapshot();

    // Retry snapshot if stateVersion_ changed (max 2 retries)
    for (int retry = 0; retry < 2; ++retry) {
        if (stateVersion_.load(std::memory_order_acquire) == snapshotVersion) break;
        takeSnapshot();
    }

    // Phase 2: file I/O (manifestCs_)
    CSLockGuard mlock(manifestCs_);

    uint64_t preWriteVer = stateVersion_.load(std::memory_order_acquire);
    if (preWriteVer != snapshotVersion) {
        uint32_t count = staleLogCount_.fetch_add(1, std::memory_order_relaxed);
        if (count % 100 == 0) {
            wchar_t vbuf[128];
            swprintf_s(vbuf, L"[REGISTRY] SaveManifestAtomic: benign stale (snap=%llu, current=%llu)",
                       static_cast<unsigned long long>(snapshotVersion),
                       static_cast<unsigned long long>(preWriteVer));
            LOG_DEBUG(vbuf);
        }
    }

    std::wstring tempPath = manifestPath_ + L".tmp";
    bool writeOk = false;

    {
        std::wofstream f(tempPath, std::ios::out | std::ios::trunc);
        if (f.is_open()) {
            f << L"; UnLeaf Registry Policy Manifest - Auto-managed\n"
              << L"[IFEOPolicies]\n";
            for (const auto& name : ifeoCopy) f << name << L"\n";
            f << L"\n[PowerThrottlePolicies]\n";
            for (const auto& [path, name] : ptCopy) f << path << L"=" << name << L"\n";
            f.flush();
            writeOk = !f.fail();
        }
    }

    if (!writeOk) {
        DeleteFileW(tempPath.c_str());
        LOG_ERROR(L"[REGISTRY] SaveManifestAtomic: write failed: " + tempPath);
        return;
    }

    // FlushFileBuffers for data integrity
    HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
    }

    if (!MoveFileExW(tempPath.c_str(), manifestPath_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"[REGISTRY] SaveManifestAtomic: MoveFileExW failed (error=%lu)", err);
        LOG_ERROR(buf);

        Sleep(10);
        if (!MoveFileExW(tempPath.c_str(), manifestPath_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tempPath.c_str());
        }
        return;
    }

#ifndef NDEBUG
    // Debug only: flush directory entry
    HANDLE hDir = CreateFileW(baseDir_.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hDir != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(hDir);
        CloseHandle(hDir);
    }
#endif
}

bool RegistryPolicyManager::DeleteManifest() {
    BOOL result = DeleteFileW(manifestPath_.c_str());
    if (result || GetLastError() == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    LOG_DEBUG(L"[REGISTRY] Failed to delete manifest");
    return false;
}

// ====================================================================
// Registry Operations (ALL called outside locks)
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
        hRoot, subKey.c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
        nullptr, outKey, &disposition
    );
    return (result == ERROR_SUCCESS);
}

bool RegistryPolicyManager::SetDwordValue(HKEY hKey, const std::wstring& valueName, DWORD value) {
    LONG result = RegSetValueExW(
        hKey, valueName.c_str(), 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(DWORD)
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
        wchar_t buf[128];
        swprintf_s(buf, L"[REGISTRY] Failed to open PowerThrottling key (error=%lu)", error);
        LOG_DEBUG(buf);
        return false;
    }

    bool success = SetDwordValue(hKey, exePath, 1);
    RegCloseKey(hKey);

    if (success) {
        LOG_DEBUG(L"[REGISTRY] Disabled power throttling for: " + ExtractFileName(exePath));
    }

    return success;
}

bool RegistryPolicyManager::EnablePowerThrottling(const std::wstring& exePath) {
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, REG_POWER_THROTTLING_PATH,
        0, KEY_READ | KEY_WRITE, &hKey
    );

    if (result != ERROR_SUCCESS) {
        LOG_DEBUG(L"[REGISTRY] PowerThrottling key not found, skip removal");
        return true;  // Idempotent
    }

    bool success = DeleteValue(hKey, exePath);
    RegCloseKey(hKey);

    if (success) {
        LOG_DEBUG(L"[REGISTRY] Enabled power throttling for: " + ExtractFileName(exePath));
    }

    return success;
}

bool RegistryPolicyManager::SetIFEOPerfOptions(const std::wstring& exeName, DWORD cpuPriority) {
    std::wstring keyPath = std::wstring(REG_IFEO_PATH) + L"\\" + exeName + L"\\PerfOptions";

    HKEY hKey = nullptr;
    if (!EnsureKeyExists(HKEY_LOCAL_MACHINE, keyPath, &hKey)) {
        DWORD error = GetLastError();
        wchar_t buf[192];
        swprintf_s(buf, L"[REGISTRY] Failed to create IFEO PerfOptions for %s (error=%lu)",
                   exeName.c_str(), error);
        LOG_DEBUG(buf);
        return false;
    }

    bool success = SetDwordValue(hKey, L"CpuPriorityClass", cpuPriority);
    RegCloseKey(hKey);

    if (success) {
        wchar_t buf[128];
        swprintf_s(buf, L"[REGISTRY] Set IFEO CpuPriorityClass=%lu for: %s",
                   cpuPriority, exeName.c_str());
        LOG_DEBUG(buf);
    }

    return success;
}

bool RegistryPolicyManager::RemoveIFEOPerfOptions(const std::wstring& exeName) {
    std::wstring keyPath = std::wstring(REG_IFEO_PATH) + L"\\" + exeName + L"\\PerfOptions";

    LONG result = RegDeleteKeyW(HKEY_LOCAL_MACHINE, keyPath.c_str());

    if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND) {
        // Try to delete parent key if empty
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

bool RegistryPolicyManager::IFEOKeyExists(const std::wstring& lowerName) const {
    std::wstring keyPath = std::wstring(REG_IFEO_PATH) + L"\\" + lowerName + L"\\PerfOptions";
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ, &hKey);
    if (result == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool RegistryPolicyManager::PowerThrottleValueExists(const std::wstring& registryPath) const {
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_POWER_THROTTLING_PATH, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return false;

    DWORD value = 0;
    DWORD size = sizeof(DWORD);
    DWORD type = 0;
    result = RegQueryValueExW(hKey, registryPath.c_str(), nullptr, &type,
                              reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(hKey);
    return (result == ERROR_SUCCESS && type == REG_DWORD);
}

// ====================================================================
// Lock-free Pending Removal Queue (Treiber Stack)
// ====================================================================

void RegistryPolicyManager::EnqueuePendingRemoval(PendingRemovalNode* node) {
    node->next = pendingHead_.load(std::memory_order_relaxed);
    while (!pendingHead_.compare_exchange_weak(
        node->next, node,
        std::memory_order_release, std::memory_order_relaxed)) {
        // CAS retry (lock-free)
    }
}

PendingRemovalNode* RegistryPolicyManager::StealAllPendingRemovals() {
    return pendingHead_.exchange(nullptr, std::memory_order_acquire);
}

// ====================================================================
// Policy Application and Cleanup
// ====================================================================

bool RegistryPolicyManager::ApplyPolicy(
    const std::wstring& exeName, const std::wstring& exeFullPath)
{
    std::wstring lowerName = ToLower(exeName);
    std::wstring canonPath = CanonicalizePath(exeFullPath);

    UNLEAF_ASSERT_CANONICAL(canonPath);

    bool ifeoNeeded = false;
    bool needInsert = false;

    // ── Phase 1: Fast path + contention handling (policyCs_ — ZERO I/O) ──
    {
        CSLockGuard lock(policyCs_);
        auto it = policyMap_.find(canonPath);
        if (it != policyMap_.end()) {
            if (it->second.state == PolicyState::COMMITTED) {
                return true;  // Already fully applied
            }
            // State is APPLYING — fall through to backoff
        } else {
            needInsert = true;
        }
    }

    // If APPLYING detected — exponential backoff (REQ-8)
    if (!needInsert) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            Sleep(static_cast<DWORD>(1) << attempt);  // 1ms, 2ms, 4ms

            CSLockGuard lock(policyCs_);
            auto it = policyMap_.find(canonPath);
            if (it == policyMap_.end()) {
                needInsert = true;
                break;  // Entry was removed; we can insert
            }
            if (it->second.state == PolicyState::COMMITTED) {
                return true;  // Completed by other thread
            }
            // Still APPLYING — continue backoff
        }

        if (!needInsert) {
            // 3 retries exhausted — return false (DC-6: uncertain → false)
            LOG_DEBUG(L"[REGISTRY] ApplyPolicy: contention timeout: " + canonPath);
            return false;
        }
    }

    // ── Phase 2: Insert APPLYING entry (policyCs_ — ZERO I/O) ──
    {
        CSLockGuard lock(policyCs_);

        // Double-check: another thread may have inserted while we were outside the lock
        auto it = policyMap_.find(canonPath);
        if (it != policyMap_.end()) {
            if (it->second.state == PolicyState::COMMITTED) {
                return true;
            }
            // Still APPLYING from another thread — fail
            LOG_DEBUG(L"[REGISTRY] ApplyPolicy: concurrent insert detected: " + canonPath);
            return false;
        }

        // Determine what registry writes are needed
        auto refIt = ifeoRefCount_.find(lowerName);
        ifeoNeeded = (refIt == ifeoRefCount_.end() || refIt->second == 0);

        // Insert as APPLYING
        policyMap_[canonPath] = { PolicyState::APPLYING, lowerName, exeFullPath };
        ifeoRefCount_[lowerName]++;
    }

    // ── Phase 3: Registry write + verify (NO lock held — REQ-5) ──
    // PowerThrottle: path-specific (per installation). IFEO: exe-name-based, global
    // across ALL instances of the same executable name (OS-level behavior).
    bool ptSuccess = DisablePowerThrottling(exeFullPath);
    bool ptVerified = ptSuccess && PowerThrottleValueExists(exeFullPath);

    bool ifeoVerified = true;
    if (ifeoNeeded) {
        bool ifeoSuccess = SetIFEOPerfOptions(exeName, IFEO_CPU_PRIORITY_HIGH);
        ifeoVerified = ifeoSuccess && IFEOKeyExists(lowerName);
    }

    if (!ptVerified || !ifeoVerified) {
        // Registry write/verify failed — rollback in-memory state (DC-6)
        {
            CSLockGuard lock(policyCs_);
            policyMap_.erase(canonPath);
            auto refIt = ifeoRefCount_.find(lowerName);
            if (refIt != ifeoRefCount_.end()) {
                if (--refIt->second == 0) {
                    ifeoRefCount_.erase(refIt);
                }
            }
        }

        if (!ptVerified) {
            LOG_DEBUG(L"[REGISTRY] ApplyPolicy: PT write/verify failed: " + canonPath);
        }
        if (!ifeoVerified) {
            LOG_DEBUG(L"[REGISTRY] ApplyPolicy: IFEO write/verify failed: " + lowerName);
        }
        return false;
    }

    // ── Phase 4: Commit (policyCs_ — ZERO I/O) ──
    {
        CSLockGuard lock(policyCs_);
        auto it = policyMap_.find(canonPath);
        if (it != policyMap_.end() && it->second.state == PolicyState::APPLYING) {
            it->second.state = PolicyState::COMMITTED;
            ifeoAppliedSet_.insert(lowerName);
            stateVersion_.fetch_add(1, std::memory_order_release);
        }
    }

    // ── Phase 5: Post-commit (NO lock held) ──
    DrainPendingRemovals();
    SaveManifestAtomic();

    LOG_INFO(L"[REGISTRY] Applied EcoQoS exclusion for " + lowerName);
    return true;
}

// ====================================================================
// Proactive IFEO-only Policy (for name-based targets without known path)
// ====================================================================

bool RegistryPolicyManager::ApplyIFEOOnly(const std::wstring& exeName) {
    std::wstring lowerName = ToLower(exeName);

    // Fast path: already applied (policyCs_ — ZERO I/O)
    {
        CSLockGuard lock(policyCs_);
        if (ifeoAppliedSet_.count(lowerName) > 0) {
            return true;
        }
    }

    // Registry write (NO lock held)
    bool success = SetIFEOPerfOptions(lowerName, IFEO_CPU_PRIORITY_HIGH);
    if (!success || !IFEOKeyExists(lowerName)) {
        LOG_DEBUG(L"[REGISTRY] ApplyIFEOOnly: write/verify failed: " + lowerName);
        return false;
    }

    // Commit (policyCs_ — ZERO I/O)
    {
        CSLockGuard lock(policyCs_);
        ifeoAppliedSet_.insert(lowerName);
        stateVersion_.fetch_add(1, std::memory_order_release);
    }

    SaveManifestAtomic();
    LOG_INFO(L"[REGISTRY] IFEO policy applied (proactive): " + lowerName);
    return true;
}

// ====================================================================
// Proactive Reconciliation: remove stale entries not in config
// ====================================================================

void RegistryPolicyManager::ReconcileWithConfig(
    const std::set<std::wstring>& desiredNames,
    const std::set<std::wstring>& desiredPaths)
{
    constexpr int MAX_RECONCILE_RETRIES = 2;

    for (int attempt = 0; attempt <= MAX_RECONCILE_RETRIES; ++attempt) {
        // Phase 1: Identify stale entries (policyCs_ — ZERO I/O)
        uint64_t snapshotVersion;
        std::set<std::wstring> staleIFEO;
        std::vector<std::pair<std::wstring, std::wstring>> stalePT;  // registryPath, lowerExeName

        {
            CSLockGuard lock(policyCs_);
            snapshotVersion = stateVersion_.load(std::memory_order_acquire);

            // Stale IFEO: in ifeoAppliedSet_ but not in desiredNames,
            // and not referenced by any desired path entry
            for (const auto& name : ifeoAppliedSet_) {
                if (desiredNames.count(name) > 0) continue;
                bool referencedByPath = false;
                for (const auto& [path, entry] : policyMap_) {
                    if (entry.lowerExeName == name && desiredPaths.count(path) > 0) {
                        referencedByPath = true;
                        break;
                    }
                }
                if (!referencedByPath) {
                    staleIFEO.insert(name);
                }
            }

            // Stale PT: in policyMap_ but path not in desiredPaths
            // and exe name not in desiredNames (name target still protects via IFEO)
            for (const auto& [path, entry] : policyMap_) {
                if (desiredPaths.count(path) > 0) continue;
                if (desiredNames.count(entry.lowerExeName) > 0) continue;
                if (entry.state == PolicyState::COMMITTED) {
                    stalePT.emplace_back(entry.registryPath, entry.lowerExeName);
                }
            }
        }

        if (staleIFEO.empty() && stalePT.empty()) return;

        // Race check: if stateVersion_ changed since Phase 1, ApplyPolicy may have
        // added entries that our stale list would incorrectly remove
        if (stateVersion_.load(std::memory_order_acquire) != snapshotVersion) {
            if (attempt < MAX_RECONCILE_RETRIES) continue;  // retry from Phase 1
            LOG_DEBUG(L"[REGISTRY] ReconcileWithConfig: stateVersion changed, aborting reconciliation");
            return;
        }

        // Phase 2: Registry removal (NO lock held)
        for (const auto& [path, name] : stalePT) {
            EnablePowerThrottling(path);
        }
        for (const auto& name : staleIFEO) {
            RemoveIFEOPerfOptions(name);
        }

        // Phase 3: In-memory cleanup (policyCs_ — ZERO I/O)
        {
            CSLockGuard lock(policyCs_);
            for (const auto& name : staleIFEO) {
                ifeoAppliedSet_.erase(name);
                ifeoRefCount_.erase(name);
            }
            for (const auto& [path, name] : stalePT) {
                policyMap_.erase(path);
                auto rc = ifeoRefCount_.find(name);
                if (rc != ifeoRefCount_.end()) {
                    if (--rc->second == 0) {
                        ifeoRefCount_.erase(rc);
                    }
                }
            }
            stateVersion_.fetch_add(1, std::memory_order_release);
        }

        SaveManifestAtomic();

        wchar_t buf[128];
        swprintf_s(buf, L"[REGISTRY] Reconciled: removed %zu IFEO + %zu PT stale entries",
                   staleIFEO.size(), stalePT.size());
        LOG_INFO(buf);
        return;
    }
}

// ====================================================================
// Policy Removal
// ====================================================================

void RegistryPolicyManager::RemoveSinglePolicy(
    const std::wstring& exeName, const std::wstring& fullPath)
{
    RemoveSinglePolicyInternal(exeName, fullPath, 0);
}

void RegistryPolicyManager::RemoveSinglePolicyInternal(
    const std::wstring& exeName, const std::wstring& fullPath,
    uint8_t reenqueueCount)
{
    std::wstring lowerName = ToLower(exeName);
    std::wstring normalizedPath = CanonicalizePath(fullPath);
    std::wstring registryPath;
    bool shouldRemoveIFEO = false;
    bool deferred = false;

    // Phase 1: Map update (policyCs_ — ZERO I/O)
    {
        CSLockGuard lock(policyCs_);

        auto it = policyMap_.find(normalizedPath);
        if (it == policyMap_.end()) return;  // Already removed

        if (it->second.state == PolicyState::APPLYING) {
            // Cannot remove while Apply is in progress — defer
            if (reenqueueCount >= 3) {
                LOG_ALERT(L"[REGISTRY] RemoveSinglePolicyInternal: re-enqueue limit, dropped: " + normalizedPath);
                return;  // VerifyAndRepair will catch this
            }
            deferred = true;
        } else {
            // COMMITTED — proceed with removal
            registryPath = it->second.registryPath;
            policyMap_.erase(it);

            auto rc = ifeoRefCount_.find(lowerName);
            if (rc != ifeoRefCount_.end()) {
                if (--rc->second == 0) {
                    ifeoAppliedSet_.erase(lowerName);
                    ifeoRefCount_.erase(rc);
                    shouldRemoveIFEO = true;
                }
            }

            stateVersion_.fetch_add(1, std::memory_order_release);
        }
    }
    // policyCs_ released here

    if (deferred) {
        auto* node = new PendingRemovalNode{exeName, fullPath,
                                             static_cast<uint8_t>(reenqueueCount + 1), nullptr};
        EnqueuePendingRemoval(node);
        LOG_DEBUG(L"[REGISTRY] RemoveSinglePolicyInternal: deferred: " + fullPath);
        return;
    }

    // Phase 2: Registry delete (NO lock held)
    if (!EnablePowerThrottling(registryPath)) {
        LOG_ERROR(L"[REGISTRY] RemoveSinglePolicyInternal: EnablePowerThrottling failed: " + registryPath);
    }
    if (shouldRemoveIFEO) {
        if (!RemoveIFEOPerfOptions(exeName)) {
            LOG_ERROR(L"[REGISTRY] RemoveSinglePolicyInternal: RemoveIFEOPerfOptions failed: " + exeName);
        }
    }
}

void RegistryPolicyManager::DrainPendingRemovals() {
    // REQ-3: Time-slice based drain (MAX_ROUNDS banned)
    ULONGLONG deadline = GetTickCount64() + 5;  // 5ms budget

    PendingRemovalNode* batch = StealAllPendingRemovals();
    if (!batch) return;

    // Reverse linked list for FIFO order
    PendingRemovalNode* reversed = nullptr;
    while (batch) {
        PendingRemovalNode* next = batch->next;
        batch->next = reversed;
        reversed = batch;
        batch = next;
    }
    batch = reversed;

    // Process within time budget
    while (batch && GetTickCount64() < deadline) {
        PendingRemovalNode* node = batch;
        batch = batch->next;

        RemoveSinglePolicyInternal(node->exeName, node->fullPath, node->reenqueueCount);
        delete node;
    }

    // Re-enqueue remaining (time budget exhausted)
    while (batch) {
        PendingRemovalNode* node = batch;
        batch = batch->next;
        EnqueuePendingRemoval(node);
    }
}

void RegistryPolicyManager::CleanupAllPolicies() {
    // ★ Called after Stop() joins engineControlThread_.
    //    No concurrent ApplyPolicy possible.

    // Phase 1: Copy + clear (policyCs_ — ZERO I/O)
    std::set<std::wstring> ifeoCopy;
    std::vector<std::pair<std::wstring, std::wstring>> ptCopy;  // lowerExeName, registryPath
    bool wasEmpty = false;
    {
        CSLockGuard lock(policyCs_);
        if (ifeoAppliedSet_.empty() && policyMap_.empty()) {
            wasEmpty = true;
        } else {
            ifeoCopy = ifeoAppliedSet_;
            for (const auto& [key, entry] : policyMap_) {
                ptCopy.emplace_back(entry.lowerExeName, entry.registryPath);
            }

            ifeoAppliedSet_.clear();
            policyMap_.clear();
            ifeoRefCount_.clear();
            stateVersion_.fetch_add(1, std::memory_order_release);
        }
    }

    if (wasEmpty) {
        DeleteManifest();
        return;
    }

    // Phase 2: Registry delete (NO lock held)
    for (const auto& [name, path] : ptCopy) {
        EnablePowerThrottling(path);
    }
    for (const auto& name : ifeoCopy) {
        RemoveIFEOPerfOptions(name);
    }

    DeleteManifest();
    DrainPendingRemovals();

    wchar_t buf[128];
    swprintf_s(buf, L"[REGISTRY] Cleanup complete (%zu IFEO + %zu PT removed)",
               ifeoCopy.size(), ptCopy.size());
    LOG_INFO(buf);
}

void RegistryPolicyManager::RemoveAllPolicies() {
    // ★ Called from Manager process. Service not running.
    // Fully idempotent by design.

    // File I/O — outside lock (separate process, no contention)
    LoadManifest();

    // policyCs_ lock: copy + clear
    std::set<std::wstring> ifeoCopy;
    std::vector<std::pair<std::wstring, std::wstring>> ptCopy;
    {
        CSLockGuard lock(policyCs_);
        if (ifeoAppliedSet_.empty() && policyMap_.empty()) {
            // Nothing to do
        } else {
            ifeoCopy = ifeoAppliedSet_;
            for (const auto& [key, entry] : policyMap_) {
                ptCopy.emplace_back(entry.lowerExeName, entry.registryPath);
            }
            ifeoAppliedSet_.clear();
            policyMap_.clear();
            ifeoRefCount_.clear();
        }
    }

    // Registry delete (NO lock held)
    for (const auto& [name, path] : ptCopy) {
        EnablePowerThrottling(path);
    }
    for (const auto& name : ifeoCopy) {
        RemoveIFEOPerfOptions(name);
    }

    DeleteManifest();

    size_t total = ifeoCopy.size() + ptCopy.size();
    if (total > 0) {
        wchar_t buf[128];
        swprintf_s(buf, L"[REGISTRY] All policies removed (%zu IFEO + %zu PT)",
                   ifeoCopy.size(), ptCopy.size());
        LOG_INFO(buf);
    }
}

void RegistryPolicyManager::VerifyAndRepair() {
    // REQ-6: CAS single-execution guard
    bool expected = false;
    if (!verifyRunning_.compare_exchange_strong(expected, true)) return;

    // RAII guard: always reset verifyRunning_
    struct VerifyGuard {
        std::atomic<bool>& flag;
        VerifyGuard(std::atomic<bool>& f) : flag(f) {}
        ~VerifyGuard() { flag.store(false, std::memory_order_release); }
    } guard(verifyRunning_);

    // Phase 1: Snapshot COMMITTED entries only (policyCs_ — ZERO I/O)
    std::set<std::wstring> ifeoCopy;
    std::vector<std::pair<std::wstring, std::wstring>> ptCopy;  // canonPath, registryPath
    {
        CSLockGuard lock(policyCs_);
        ifeoCopy = ifeoAppliedSet_;
        for (const auto& [key, entry] : policyMap_) {
            if (entry.state == PolicyState::COMMITTED) {
                ptCopy.emplace_back(key, entry.registryPath);
            }
        }
    }

    // Phase 2: Registry verify + repair (NO lock held)
    size_t repaired = 0;
    for (const auto& name : ifeoCopy) {
        if (!IFEOKeyExists(name)) {
            LOG_ALERT(L"[REGISTRY] VerifyAndRepair: IFEO missing, re-applying: " + name);
            SetIFEOPerfOptions(name, IFEO_CPU_PRIORITY_HIGH);
            repaired++;
        }
    }
    for (const auto& [key, regPath] : ptCopy) {
        if (!PowerThrottleValueExists(regPath)) {
            LOG_ALERT(L"[REGISTRY] VerifyAndRepair: PT missing, re-applying: " + regPath);
            DisablePowerThrottling(regPath);
            repaired++;
        }
    }

    wchar_t buf[128];
    swprintf_s(buf, L"[REGISTRY] VerifyAndRepair: checked %zu IFEO + %zu PT, repaired %zu",
               ifeoCopy.size(), ptCopy.size(), repaired);
    LOG_INFO(buf);

    DrainPendingRemovals();
}

bool RegistryPolicyManager::IsPolicyValid(const std::wstring& canonPath) const {
    std::wstring normalized = CanonicalizePath(canonPath);
    std::wstring lowerName;
    std::wstring regPath;
    {
        CSLockGuard lock(policyCs_);
        auto it = policyMap_.find(normalized);
        if (it == policyMap_.end() || it->second.state != PolicyState::COMMITTED)
            return false;
        lowerName = it->second.lowerExeName;
        regPath = it->second.registryPath;
    }
    return IFEOKeyExists(lowerName) && PowerThrottleValueExists(regPath);
}

bool RegistryPolicyManager::HasPolicy(const std::wstring& canonPath) const {
    std::wstring normalized = CanonicalizePath(canonPath);
    CSLockGuard lock(policyCs_);
    auto it = policyMap_.find(normalized);
    return (it != policyMap_.end() && it->second.state == PolicyState::COMMITTED);
}

bool RegistryPolicyManager::IsPolicyApplied(const std::wstring& exeName) const {
    CSLockGuard lock(policyCs_);
    return ifeoAppliedSet_.count(ToLower(exeName)) > 0;
}

std::vector<std::wstring> RegistryPolicyManager::GetAppliedPolicies() const {
    CSLockGuard lock(policyCs_);
    std::vector<std::wstring> result;
    result.reserve(ifeoAppliedSet_.size());
    for (const auto& name : ifeoAppliedSet_) {
        result.push_back(name);
    }
    return result;
}

} // namespace unleaf
