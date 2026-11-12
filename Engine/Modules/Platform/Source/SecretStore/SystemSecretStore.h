#pragma once

#include "Platform/SecretStore.h"

#include <string_view>

// The calls to this system's secret store, one definition per platform. SecretStore.cpp checks
// the name and the value before it calls them, so they receive only a valid pair.
namespace GameEngine::Platform
{

SecretStoreStatus SystemStoreSecret(std::string_view name, std::string_view value);
SecretReadResult SystemReadSecret(std::string_view name);
SecretStoreStatus SystemDeleteSecret(std::string_view name);
std::string_view SystemSecretStoreName();

} // namespace GameEngine::Platform
