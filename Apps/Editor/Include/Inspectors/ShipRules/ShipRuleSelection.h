#pragma once

#include "Engine/Build/ShipRules.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Editor
{

/// The ship rules of the files and folders an Inspector selection holds, as the Ship in builds row reads them.
struct ShipRuleSelection
{
    struct Item
    {
        std::filesystem::path Path;
        std::string RelativePath; ///< Canonical, relative to the asset root.
        bool IsFolder = false;
        bool NeverShips = false;       ///< Not game content (AssetCollector::IsShippableContent).
        std::optional<ShipRule> Own;   ///< The asset's kv rule, or the folder's own file.
        bool OwnIsValid = true;        ///< False when the stored kv text is not a rule.
        ShipRules::Resolved Inherited; ///< The rule of the folder files above the item.
    };

    /// What the row shows.
    struct State
    {
        int SelectedIndex = -1;      ///< In kChoices, when every item has the same own rule; else -1.
        std::string NoSelectionText; ///< The dropdown text when SelectedIndex is -1.
        std::string Caption;         ///< The line under the row; empty for none.
        bool AnyOwn = false;         ///< An item has a rule of its own, so the row offers to clear it.
        bool NeverShips = false;     ///< An item is not game content, so the dropdown is off.
    };

    /// The dropdown's choices, in order.
    static constexpr ShipRule kChoices[] = {ShipRule::Inferred, ShipRule::Always, ShipRule::Never};

    /// Reads `paths`, and registers a file the registry does not hold yet (a file made this session).
    /// False when a path is outside `assetRoot`, is the root itself, or is a file that is not an asset:
    /// the selection then has no row.
    static bool TryRead(AssetManager& assets, const std::filesystem::path& assetRoot,
                        std::span<const std::filesystem::path> paths, ShipRuleSelection& outSelection);

    /// The name the row shows for `rule`: "Inferred", "Always" or "Never".
    static std::string Label(ShipRule rule);

    /// The row's state, for example "Always (from Data/)" for an asset that inherits Always from Data/.
    State Describe() const;

    std::vector<Item> Items;
    std::vector<std::string> Errors; ///< Folder files above the selection that are not valid.
};

} // namespace GameEngine::Editor
