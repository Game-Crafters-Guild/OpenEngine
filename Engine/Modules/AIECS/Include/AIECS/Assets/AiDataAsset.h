#pragma once

#include "AssetCore/Asset.h"

#include <concepts>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace GameEngine::AIECS
{

/// The format of the AI data file that parses into `TData`: `kType` (the asset type), `kNoun` (the log's
/// name for the file) and `Parse` (the parser). The header of each asset type specializes it.
template <typename TData>
struct AiDataFormat;

/// A data type whose AiDataFormat gives the asset type, the noun and a parser of the file text.
template <typename TData>
concept AiDataFile = requires(std::string_view text, TData& data, std::string& error) {
    { AiDataFormat<TData>::kType } -> std::convertible_to<AssetType>;
    { AiDataFormat<TData>::kNoun } -> std::convertible_to<const char*>;
    { AiDataFormat<TData>::Parse(text, data, error) } -> std::same_as<bool>;
};

/// An AI data file as an asset, created by the asset type registry's factory (CoreAssetRegistrations).
/// `TData` is the parsed data of the file. The loaded data is immutable and held through a shared pointer:
/// a reload parses new data and publishes it, and a file that does not parse keeps the last good data
/// published. GetData is safe from any thread. The other members are in AiDataAsset.inl, as Engine.dll
/// exports no template instance.
template <AiDataFile TData>
class AiDataAsset final : public Asset
{
  public:
    AiDataAsset(const GUID& guid, const std::filesystem::path& path) : Asset(guid, AiDataFormat<TData>::kType, path)
    {
    }

    bool Load() override;
    bool LoadFromData(std::span<const uint8> data) override;
    void Unload() override;

    /// The published data; null until loaded.
    std::shared_ptr<const TData> GetData() const;

  protected:
    bool ReloadFromData(std::span<const uint8> data) override;

  private:
    // The parsed text, or null with the reason logged.
    std::shared_ptr<const TData> ParseBytes(std::span<const uint8> data) const;
    void Publish(std::shared_ptr<const TData> data);

    // Guards m_Data: each critical section is one shared_ptr copy or swap.
    mutable std::mutex m_DataMutex;
    std::shared_ptr<const TData> m_Data;
};

} // namespace GameEngine::AIECS

#include "AIECS/Assets/AiDataAsset.inl"
