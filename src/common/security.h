#pragma once
// UnLeaf v7.5 - IPC Security Helpers
// Named Pipe DACL and Client Authorization

#include <Windows.h>
#include <AclAPI.h>
#include <sddl.h>

namespace unleaf {

// v7.5: Command permission levels
enum class CommandPermission {
    PUBLIC,        // Any connected client (GET_STATUS, GET_LOGS, GET_STATS, GET_CONFIG)
    ADMIN,         // Administrator required (ADD_TARGET, REMOVE_TARGET, SET_INTERVAL)
    SYSTEM_ONLY    // SYSTEM or elevated admin (STOP_SERVICE)
};

// v7.5: Authorization result
enum class AuthResult {
    AUTHORIZED,
    UNAUTHORIZED,
    ERROR_IMPERSONATION,
    ERROR_TOKEN
};

// v7.5: Security descriptor builder for Named Pipe
// Restricts access to SYSTEM + Administrators only
class PipeSecurityDescriptor {
public:
    PipeSecurityDescriptor()
        : pSD_(nullptr), pACL_(nullptr), pSystemSid_(nullptr), pAdminsSid_(nullptr), initialized_(false) {
        ZeroMemory(&sa_, sizeof(sa_));
    }

    ~PipeSecurityDescriptor() {
        Cleanup();
    }

    // Non-copyable
    PipeSecurityDescriptor(const PipeSecurityDescriptor&) = delete;
    PipeSecurityDescriptor& operator=(const PipeSecurityDescriptor&) = delete;

    bool Initialize() {
        if (initialized_) return true;

        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

        // Create SYSTEM SID (S-1-5-18)
        if (!AllocateAndInitializeSid(&ntAuthority, 1,
                SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
                &pSystemSid_)) {
            Cleanup();
            return false;
        }

        // Create Administrators SID (S-1-5-32-544)
        if (!AllocateAndInitializeSid(&ntAuthority, 2,
                SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                0, 0, 0, 0, 0, 0, &pAdminsSid_)) {
            Cleanup();
            return false;
        }

        // Set up EXPLICIT_ACCESS entries
        EXPLICIT_ACCESSW ea[2] = {};

        // SYSTEM: Full access
        ea[0].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
        ea[0].grfAccessMode = SET_ACCESS;
        ea[0].grfInheritance = NO_INHERITANCE;
        ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(pSystemSid_);

        // Administrators: Full access
        ea[1].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
        ea[1].grfAccessMode = SET_ACCESS;
        ea[1].grfInheritance = NO_INHERITANCE;
        ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_ALIAS;
        ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(pAdminsSid_);

        // Create ACL with these entries
        if (SetEntriesInAclW(2, ea, nullptr, &pACL_) != ERROR_SUCCESS) {
            Cleanup();
            return false;
        }

        // Allocate and initialize security descriptor
        pSD_ = static_cast<PSECURITY_DESCRIPTOR>(
            LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
        if (!pSD_) {
            Cleanup();
            return false;
        }

        if (!InitializeSecurityDescriptor(pSD_, SECURITY_DESCRIPTOR_REVISION)) {
            Cleanup();
            return false;
        }

        // Set the DACL
        if (!SetSecurityDescriptorDacl(pSD_, TRUE, pACL_, FALSE)) {
            Cleanup();
            return false;
        }

        // Set up SECURITY_ATTRIBUTES
        sa_.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa_.lpSecurityDescriptor = pSD_;
        sa_.bInheritHandle = FALSE;

        initialized_ = true;
        return true;
    }

    SECURITY_ATTRIBUTES* GetSecurityAttributes() {
        return initialized_ ? &sa_ : nullptr;
    }

    bool IsInitialized() const { return initialized_; }

private:
    void Cleanup() {
        if (pACL_) {
            LocalFree(pACL_);
            pACL_ = nullptr;
        }
        if (pSystemSid_) {
            FreeSid(pSystemSid_);
            pSystemSid_ = nullptr;
        }
        if (pAdminsSid_) {
            FreeSid(pAdminsSid_);
            pAdminsSid_ = nullptr;
        }
        if (pSD_) {
            LocalFree(pSD_);
            pSD_ = nullptr;
        }
        initialized_ = false;
    }

    SECURITY_ATTRIBUTES sa_;
    PSECURITY_DESCRIPTOR pSD_;
    PACL pACL_;
    PSID pSystemSid_;
    PSID pAdminsSid_;
    bool initialized_;
};

// v7.5: Token verification helpers

// Check if token belongs to Administrators group
inline bool IsTokenAdmin(HANDLE hToken) {
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminSid = nullptr;

    if (AllocateAndInitializeSid(&ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminSid)) {
        CheckTokenMembership(hToken, adminSid, &isAdmin);
        FreeSid(adminSid);
    }
    return isAdmin != FALSE;
}

// Check if token is SYSTEM account
inline bool IsTokenSystem(HANDLE hToken) {
    BOOL isSystem = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID systemSid = nullptr;

    if (AllocateAndInitializeSid(&ntAuthority, 1,
            SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
            &systemSid)) {
        CheckTokenMembership(hToken, systemSid, &isSystem);
        FreeSid(systemSid);
    }
    return isSystem != FALSE;
}

} // namespace unleaf
