#pragma once

#include "Platform/SecretStore.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace GameEngine
{
/// The Claude API key: the ANTHROPIC_API_KEY environment variable when it is set and not
/// empty, else the key the user saved in the system's credential store. The key is never
/// written to a file, a preference or the project. Copyable; thread-safe when its backend is.
class ApiKeyStore
{
public:
    /// The credential store calls; SystemBackend() is the real store, a test passes a fake.
    struct Backend
    {
        std::function<Platform::SecretStoreStatus(std::string_view name, std::string_view value)> Store;
        std::function<Platform::SecretReadResult(std::string_view name)> Read;
        std::function<Platform::SecretStoreStatus(std::string_view name)> Delete;
        /// The store's name for a user-facing line; empty when the system has no store.
        std::string StoreName;
    };

    /// Where Read() found the key.
    enum class Source : uint8_t
    {
        None,
        Saved,
        Environment,
    };

    struct Key
    {
        Source From = Source::None;
        /// Empty unless From is not None.
        std::string Value;
        /// The status of the credential store read; Done when the environment variable won.
        Platform::SecretStoreStatus StoreStatus = Platform::SecretStoreStatus::NotFound;
    };

    /// The name of the saved key in the credential store.
    static constexpr std::string_view kCredentialName = "GameEngine/Editor/ai-assistant/anthropic-api-key";
    /// The environment variable that is used instead of a saved key when it is set.
    static constexpr std::string_view kVariable = "ANTHROPIC_API_KEY";

    /// Platform::StoreSecret, ReadSecret, DeleteSecret and SecretStoreName.
    static Backend SystemBackend();

    explicit ApiKeyStore(Backend backend);

    /// The key a turn uses. Reads the credential store, which can block (the macOS Keychain asks
    /// the user), so call it from a worker thread.
    Key Read() const;
    /// Saves `key` under kCredentialName, replacing a saved key. An empty key gives Failed.
    Platform::SecretStoreStatus Save(std::string_view key) const;
    /// Deletes the saved key; NotFound when no key is saved.
    Platform::SecretStoreStatus Remove() const;
    /// True when a key is saved, whether or not the environment variable is set. Reads the
    /// store, so call it from a worker thread, as Read().
    bool HasSavedKey() const;
    /// Backend::StoreName.
    const std::string& StoreName() const { return m_Backend.StoreName; }

private:
    Backend m_Backend;
};
} // namespace GameEngine
