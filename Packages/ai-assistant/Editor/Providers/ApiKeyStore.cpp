#include "ApiKeyStore.h"

#include <cstdlib>
#include <utility>

namespace GameEngine
{
ApiKeyStore::Backend ApiKeyStore::SystemBackend()
{
    return {.Store = &Platform::StoreSecret,
            .Read = &Platform::ReadSecret,
            .Delete = &Platform::DeleteSecret,
            .StoreName = std::string(Platform::SecretStoreName())};
}

ApiKeyStore::ApiKeyStore(Backend backend) : m_Backend(std::move(backend))
{
}

// The variable is read first: when it is set, Read does not read the store. HasSavedKey reads
// the store in all cases, so the setup check reads it once also when the variable is set.
ApiKeyStore::Key ApiKeyStore::Read() const
{
    const char* variable = std::getenv(std::string(kVariable).c_str());
    if (variable && *variable)
        return {.From = Source::Environment, .Value = variable, .StoreStatus = Platform::SecretStoreStatus::Done};

    Platform::SecretReadResult saved = m_Backend.Read(kCredentialName);
    if (saved.Status == Platform::SecretStoreStatus::Done && !saved.Value.empty())
        return {.From = Source::Saved, .Value = std::move(saved.Value), .StoreStatus = saved.Status};
    const Platform::SecretStoreStatus status =
        saved.Status == Platform::SecretStoreStatus::Done ? Platform::SecretStoreStatus::NotFound : saved.Status;
    return {.From = Source::None, .StoreStatus = status};
}

Platform::SecretStoreStatus ApiKeyStore::Save(std::string_view key) const
{
    if (key.empty())
        return Platform::SecretStoreStatus::Failed;
    return m_Backend.Store(kCredentialName, key);
}

Platform::SecretStoreStatus ApiKeyStore::Remove() const
{
    return m_Backend.Delete(kCredentialName);
}

bool ApiKeyStore::HasSavedKey() const
{
    const Platform::SecretReadResult saved = m_Backend.Read(kCredentialName);
    return saved.Status == Platform::SecretStoreStatus::Done && !saved.Value.empty();
}
} // namespace GameEngine
