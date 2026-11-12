#include "Platform/SecretStore.h"

#include "SecretStore/SystemSecretStore.h"

#include <cstddef>

namespace GameEngine::Platform
{
namespace
{
// The longest secret name in bytes that the store functions accept.
constexpr std::size_t kMaxSecretNameBytes = 256;

// The name is not empty and at most kMaxSecretNameBytes, the value at most kMaxSecretBytes, and
// neither holds a NUL.
bool IsValidSecret(std::string_view name, std::string_view value)
{
    return !name.empty() && name.size() <= kMaxSecretNameBytes && value.size() <= kMaxSecretBytes &&
           name.find('\0') == std::string_view::npos && value.find('\0') == std::string_view::npos;
}
} // namespace

SecretStoreStatus StoreSecret(std::string_view name, std::string_view value)
{
    if (!IsValidSecret(name, value))
        return SecretStoreStatus::Failed;
    return SystemStoreSecret(name, value);
}

SecretReadResult ReadSecret(std::string_view name)
{
    if (!IsValidSecret(name, {}))
        return {.Status = SecretStoreStatus::Failed};
    return SystemReadSecret(name);
}

SecretStoreStatus DeleteSecret(std::string_view name)
{
    if (!IsValidSecret(name, {}))
        return SecretStoreStatus::Failed;
    return SystemDeleteSecret(name);
}

std::string_view SecretStoreName()
{
    return SystemSecretStoreName();
}

} // namespace GameEngine::Platform
