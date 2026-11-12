#pragma once

#include "Providers/ApiKeyStore.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace GameEngine
{
// A credential store in memory for the tests: never the system's store, so a test never reads,
// writes or deletes the user's real key.
struct FakeApiKeyStore
{
    struct State
    {
        std::optional<std::string> Saved;
        // Read, Store and Delete give this status when it is not Done (NoStore, Failed).
        Platform::SecretStoreStatus Refuse = Platform::SecretStoreStatus::Done;
        int Reads = 0;
        std::string LastName;
        // The thread of the last Store or Delete call.
        std::thread::id WriteThread;
    };

    std::shared_ptr<State> Data = std::make_shared<State>();

    ApiKeyStore Store(std::string storeName = "the test store") const
    {
        ApiKeyStore::Backend backend;
        backend.StoreName = std::move(storeName);
        backend.Store = [data = Data](std::string_view name, std::string_view value) {
            data->LastName = std::string(name);
            data->WriteThread = std::this_thread::get_id();
            if (data->Refuse != Platform::SecretStoreStatus::Done)
                return data->Refuse;
            data->Saved = std::string(value);
            return Platform::SecretStoreStatus::Done;
        };
        backend.Read = [data = Data](std::string_view name) {
            ++data->Reads;
            data->LastName = std::string(name);
            if (data->Refuse != Platform::SecretStoreStatus::Done)
                return Platform::SecretReadResult{.Status = data->Refuse};
            if (!data->Saved)
                return Platform::SecretReadResult{.Status = Platform::SecretStoreStatus::NotFound};
            return Platform::SecretReadResult{.Status = Platform::SecretStoreStatus::Done, .Value = *data->Saved};
        };
        backend.Delete = [data = Data](std::string_view name) {
            data->LastName = std::string(name);
            data->WriteThread = std::this_thread::get_id();
            if (data->Refuse != Platform::SecretStoreStatus::Done)
                return data->Refuse;
            if (!data->Saved)
                return Platform::SecretStoreStatus::NotFound;
            data->Saved.reset();
            return Platform::SecretStoreStatus::Done;
        };
        return ApiKeyStore(std::move(backend));
    }
};
} // namespace GameEngine
