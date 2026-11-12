#pragma once

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <string>
#include <string_view>
#include <utility>

namespace GameEngine::AIECS
{

template <AiDataFile TData>
bool AiDataAsset<TData>::Load()
{
    ByteBuffer bytes;
    if (!ReadFileBytesShared(GetPath(), bytes))
    {
        Logger::Log::Error("{} '{}' could not be read", AiDataFormat<TData>::kNoun, GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return LoadFromData(bytes);
}

template <AiDataFile TData>
bool AiDataAsset<TData>::LoadFromData(std::span<const uint8> data)
{
    std::shared_ptr<const TData> parsed = ParseBytes(data);
    if (!parsed)
    {
        SetState(AssetState::Failed);
        return false;
    }
    Publish(std::move(parsed));
    SetState(AssetState::Loaded);
    return true;
}

template <AiDataFile TData>
void AiDataAsset<TData>::Unload()
{
    Publish(nullptr);
    SetState(AssetState::Unloaded);
}

template <AiDataFile TData>
std::shared_ptr<const TData> AiDataAsset<TData>::GetData() const
{
    std::lock_guard lock(m_DataMutex);
    return m_Data;
}

template <AiDataFile TData>
bool AiDataAsset<TData>::ReloadFromData(std::span<const uint8> data)
{
    std::shared_ptr<const TData> parsed = ParseBytes(data);
    if (!parsed)
        return false;
    Publish(std::move(parsed));
    return true;
}

template <AiDataFile TData>
std::shared_ptr<const TData> AiDataAsset<TData>::ParseBytes(std::span<const uint8> data) const
{
    auto parsed = std::make_shared<TData>();
    std::string error;
    if (!AiDataFormat<TData>::Parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()),
                                    *parsed, error))
    {
        Logger::Log::Error("{} '{}' was not loaded: {}", AiDataFormat<TData>::kNoun, GetPath().string(), error);
        return nullptr;
    }
    // A data type with a StableId takes the GUID of the asset that loaded it.
    if constexpr (requires { parsed->StableId = GUID(); })
        parsed->StableId = GetGUID();
    return parsed;
}

template <AiDataFile TData>
void AiDataAsset<TData>::Publish(std::shared_ptr<const TData> data)
{
    {
        std::lock_guard lock(m_DataMutex);
        m_Data.swap(data);
    }
    // `data` holds the previous data now and is released here, outside the lock.
}

} // namespace GameEngine::AIECS
