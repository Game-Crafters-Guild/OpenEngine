#include "Inspectors/ShipRules/SetShipRuleCommand.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Types/PathUtils.h"

#include <fstream>
#include <iterator>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

AssetRegistry& Registry()
{
    return EngineCore::GetInstance().GetAssetManager().GetRegistry();
}

// The asset's path in the registry; empty when the GUID does not resolve.
std::filesystem::path CurrentPath(const GUID& asset)
{
    AssetMetadata metadata{};
    if (asset.IsNull() || !Registry().TryGetAssetMetadata(asset, metadata))
        return {};
    return metadata.Path;
}

std::filesystem::path FolderFile(const std::filesystem::path& folder)
{
    return folder / ShipRules::kFolderFileName;
}

} // namespace

SetShipRuleCommand::SetShipRuleCommand(const std::vector<std::filesystem::path>& paths, std::optional<ShipRule> rule,
                                       std::function<void()> onWritten)
    : m_Rule(rule), m_OnWritten(std::move(onWritten))
{
    for (const std::filesystem::path& path : paths)
    {
        Target target;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
            target.Folder = path;
        else
            target.Asset = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(path);
        m_Targets.push_back(std::move(target));
    }
}

void SetShipRuleCommand::Do()
{
    if (!m_PreviousCaptured)
    {
        for (Target& target : m_Targets)
            target.Previous = Read(target);
        m_PreviousCaptured = true;
    }
    for (const Target& target : m_Targets)
    {
        std::optional<std::string> text;
        if (m_Rule)
        {
            const std::string rule(ShipRules::ToText(*m_Rule));
            text = target.Folder.empty() ? rule : "{\"ship\": \"" + rule + "\"}\n";
        }
        Write(target, text);
    }
    if (m_OnWritten)
        m_OnWritten();
}

void SetShipRuleCommand::Undo()
{
    for (const Target& target : m_Targets)
        Write(target, target.Previous);
    if (m_OnWritten)
        m_OnWritten();
}

std::optional<std::string> SetShipRuleCommand::Read(const Target& target)
{
    if (!target.Folder.empty())
    {
        std::ifstream in(FolderFile(target.Folder), std::ios::binary);
        if (!in)
            return std::nullopt;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const std::filesystem::path path = CurrentPath(target.Asset);
    std::string stored;
    if (path.empty() || !Registry().TryGetMetaValue(path, std::string(ShipRules::kAssetMetaKey), stored))
        return std::nullopt;
    return stored;
}

void SetShipRuleCommand::Write(const Target& target, const std::optional<std::string>& text)
{
    if (!target.Folder.empty())
    {
        const std::filesystem::path file = FolderFile(target.Folder);
        std::error_code ec;
        if (!text)
        {
            std::filesystem::remove(file, ec);
            if (ec)
                Logger::Log::Error("Ship rules: cannot delete {} ({}). The folder keeps its rule.", PathToUtf8(file),
                                   ec.message());
            return;
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << *text;
        if (!out)
            Logger::Log::Error("Ship rules: cannot write {}. Make the file and its folder writable, then set the rule "
                               "again.",
                               PathToUtf8(file));
        return;
    }
    const std::filesystem::path path = CurrentPath(target.Asset);
    const std::string key(ShipRules::kAssetMetaKey);
    const bool written =
        !path.empty() && (text ? Registry().SetMetaValue(path, key, *text) : Registry().RemoveMetaValue(path, key));
    if (!written)
        Logger::Log::Error("Ship rules: cannot write the ship rule of {} to the asset database. The asset keeps its "
                           "rule.",
                           path.empty() ? target.Asset.ToString() : PathToUtf8(path));
}

} // namespace GameEngine::Editor
