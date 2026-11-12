#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine::Platform
{

/// The result of a call to the operating system's secret store.
enum class SecretStoreStatus : uint8_t
{
    /// The call did what it was asked to do.
    Done,
    /// No secret has the name.
    NotFound,
    /// This system has no secret store that the process can use (the web build, or a Linux
    /// session without a secret service).
    NoStore,
    /// The store refused the call, or the name or the value is not valid.
    Failed,
};

/// The value of a secret and the status of the read. Value is empty unless Status is Done.
struct SecretReadResult
{
    SecretStoreStatus Status = SecretStoreStatus::Failed;
    std::string Value;
};

/// The largest value in bytes that StoreSecret accepts on every platform: the Windows
/// Credential Manager limit for a credential blob.
inline constexpr std::size_t kMaxSecretBytes = 2560;

/// Stores UTF-8 `value` under UTF-8 `name` for the current user on this machine, and replaces a
/// value of the same name: the Credential Manager (not roamed), the login Keychain or libsecret,
/// never a file. An empty name, a name over 256 bytes, a value over kMaxSecretBytes or a NUL in
/// either gives Failed, and the store is not called; the same name rule holds for every call.
/// @return Done, NoStore or Failed.
SecretStoreStatus StoreSecret(std::string_view name, std::string_view value);

/// Reads the secret `name` that StoreSecret stored. Can block while the store asks the user
/// for access (the macOS Keychain), so call it from a worker thread, never the UI thread.
/// @return Done with the value, NotFound, NoStore or Failed.
SecretReadResult ReadSecret(std::string_view name);

/// Deletes the secret `name`.
/// @return Done, NotFound when no secret has the name, NoStore or Failed.
SecretStoreStatus DeleteSecret(std::string_view name);

/// The name of this system's secret store, for a user-facing line ("Windows Credential
/// Manager", "Keychain", "the secret service"). Empty when the system has no store.
std::string_view SecretStoreName();

} // namespace GameEngine::Platform
