#include "SecretStore/SystemSecretStore.h"

#include <libsecret/secret.h>

#include <string>

namespace GameEngine::Platform
{
namespace
{
// One attribute, the secret's name. With SECRET_SCHEMA_NONE, the match also includes the schema
// name, so only a secret that this schema stored is found.
const SecretSchema* Schema()
{
    static const SecretSchema schema = {
        "org.gameengine.Secret",
        SECRET_SCHEMA_NONE,
        {{"name", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    };
    return &schema;
}

// A D-Bus error means no secret service answers on the session bus: no store for this session.
SecretStoreStatus StatusOfError(GError* error)
{
    const SecretStoreStatus status =
        error->domain == G_DBUS_ERROR || error->domain == G_IO_ERROR ? SecretStoreStatus::NoStore : SecretStoreStatus::Failed;
    g_error_free(error);
    return status;
}
} // namespace

// The default collection of the secret service (GNOME Keyring, or KWallet through its bridge),
// which the service unlocks with the user's login.
SecretStoreStatus SystemStoreSecret(std::string_view name, std::string_view value)
{
    const std::string nameText(name);
    std::string valueText(value);
    GError* error = nullptr;
    const gboolean stored = secret_password_store_sync(Schema(), SECRET_COLLECTION_DEFAULT, nameText.c_str(),
                                                       valueText.c_str(), nullptr, &error, "name", nameText.c_str(),
                                                       nullptr);
    valueText.assign(valueText.size(), '\0');
    if (error)
        return StatusOfError(error);
    return stored ? SecretStoreStatus::Done : SecretStoreStatus::Failed;
}

SecretReadResult SystemReadSecret(std::string_view name)
{
    const std::string nameText(name);
    GError* error = nullptr;
    gchar* password =
        secret_password_lookup_sync(Schema(), nullptr, &error, "name", nameText.c_str(), nullptr);
    if (error)
        return {.Status = StatusOfError(error)};
    if (!password)
        return {.Status = SecretStoreStatus::NotFound};
    SecretReadResult result{.Status = SecretStoreStatus::Done, .Value = password};
    // secret_password_free clears the memory before it frees it.
    secret_password_free(password);
    return result;
}

SecretStoreStatus SystemDeleteSecret(std::string_view name)
{
    const std::string nameText(name);
    GError* error = nullptr;
    const gboolean removed = secret_password_clear_sync(Schema(), nullptr, &error, "name", nameText.c_str(), nullptr);
    if (error)
        return StatusOfError(error);
    return removed ? SecretStoreStatus::Done : SecretStoreStatus::NotFound;
}

std::string_view SystemSecretStoreName()
{
    return "the secret service";
}

} // namespace GameEngine::Platform
