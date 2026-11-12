#include "SecretStore/SystemSecretStore.h"
#include "Utf8ToWide_Win32.h"

#include <windows.h>
#include <wincred.h>

namespace GameEngine::Platform
{
namespace
{
// ERROR_NO_SUCH_LOGON_SESSION: the session has no credential set (a service account or a
// network logon without a profile), so the store cannot be used.
SecretStoreStatus StatusOfError(DWORD error)
{
    if (error == ERROR_NOT_FOUND)
        return SecretStoreStatus::NotFound;
    if (error == ERROR_NO_SUCH_LOGON_SESSION)
        return SecretStoreStatus::NoStore;
    return SecretStoreStatus::Failed;
}
} // namespace

// A generic credential with CRED_PERSIST_LOCAL_MACHINE: this user on this machine, never roamed
// to other machines of a domain profile. Only processes of this user's logon sessions read it.
SecretStoreStatus SystemStoreSecret(std::string_view name, std::string_view value)
{
    std::wstring target = Utf8ToWide(name);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    // CredWriteW copies the blob and does not write to it.
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (CredWriteW(&credential, 0))
        return SecretStoreStatus::Done;
    const SecretStoreStatus status = StatusOfError(GetLastError());
    return status == SecretStoreStatus::NotFound ? SecretStoreStatus::Failed : status;
}

SecretReadResult SystemReadSecret(std::string_view name)
{
    const std::wstring target = Utf8ToWide(name);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return {.Status = StatusOfError(GetLastError())};
    SecretReadResult result{.Status = SecretStoreStatus::Done};
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0)
    {
        result.Value.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                            credential->CredentialBlobSize);
        // The returned copy is the only one left in this process once CredFree runs.
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    }
    CredFree(credential);
    return result;
}

SecretStoreStatus SystemDeleteSecret(std::string_view name)
{
    const std::wstring target = Utf8ToWide(name);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
        return SecretStoreStatus::Done;
    return StatusOfError(GetLastError());
}

std::string_view SystemSecretStoreName()
{
    return "Windows Credential Manager";
}

} // namespace GameEngine::Platform
