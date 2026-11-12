#include "SecretStore/SystemSecretStore.h"

// The web build, and a Linux build without libsecret: no secret store. There is no file
// fallback: a key in a file is a plain key.
namespace GameEngine::Platform
{

SecretStoreStatus SystemStoreSecret(std::string_view, std::string_view)
{
    return SecretStoreStatus::NoStore;
}

SecretReadResult SystemReadSecret(std::string_view)
{
    return {.Status = SecretStoreStatus::NoStore};
}

SecretStoreStatus SystemDeleteSecret(std::string_view)
{
    return SecretStoreStatus::NoStore;
}

std::string_view SystemSecretStoreName()
{
    return {};
}

} // namespace GameEngine::Platform
