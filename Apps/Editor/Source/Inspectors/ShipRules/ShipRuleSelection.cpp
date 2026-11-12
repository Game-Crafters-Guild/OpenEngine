#include "Inspectors/ShipRules/ShipRuleSelection.h"

#include "AssetCore/PathNormalization.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <array>

namespace GameEngine::Editor
{

std::string ShipRuleSelection::Label(ShipRule rule)
{
    switch (rule)
    {
    case ShipRule::Always:
        return "Always";
    case ShipRule::Never:
        return "Never";
    case ShipRule::Inferred:
        break;
    }
    return "Inferred";
}

namespace
{

// "Always (from Data/)", "Never (from the asset root)" or "Inferred (default)".
std::string InheritedText(const ShipRules::Resolved& inherited)
{
    if (inherited.From == ShipRules::Source::Default)
        return "Inferred (default)";
    return ShipRuleSelection::Label(inherited.Rule) + " (from " + (inherited.Folder.empty() ? "the asset root" : inherited.Folder) + ")";
}

// What clearing the own rule gives: "Folder Data/ says Always." or "The default is Inferred."
std::string WithoutOwnRuleText(const ShipRules::Resolved& inherited)
{
    if (inherited.From == ShipRules::Source::Default)
        return "The default is Inferred.";
    const std::string folder = inherited.Folder.empty() ? "The asset root" : "Folder " + inherited.Folder;
    return folder + " says " + ShipRuleSelection::Label(inherited.Rule) + ".";
}

bool SameInherited(const ShipRules::Resolved& a, const ShipRules::Resolved& b)
{
    return a.Rule == b.Rule && a.From == b.From && a.Folder == b.Folder;
}

} // namespace

bool ShipRuleSelection::TryRead(AssetManager& assets, const std::filesystem::path& assetRoot,
                                std::span<const std::filesystem::path> paths, ShipRuleSelection& outSelection)
{
    outSelection = {};
    std::vector<std::string> relativePaths;
    for (const std::filesystem::path& path : paths)
    {
        Item item;
        item.Path = path;
        std::error_code ec;
        item.IsFolder = std::filesystem::is_directory(path, ec);
        if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, path, item.RelativePath) || item.RelativePath.empty())
            return false;
        // ResolveAssetGuid registers a file made this session, which the file watcher may not have registered yet.
        if (!item.IsFolder && assets.ResolveAssetGuid(path).IsNull())
            return false;
        item.NeverShips = !AssetCollector::IsShippableContent(PathFromUtf8(item.RelativePath));
        relativePaths.push_back(item.RelativePath);
        outSelection.Items.push_back(std::move(item));
    }

    ShipRules rules = ShipRules::LoadAbove(assetRoot, relativePaths, outSelection.Errors);
    for (Item& item : outSelection.Items)
    {
        item.Inherited = rules.ResolveInherited(item.RelativePath);
        if (item.IsFolder)
        {
            item.Own = rules.OwnRule(item.RelativePath);
            continue;
        }
        std::string stored;
        if (!assets.GetRegistry().TryGetMetaValue(item.Path, std::string(ShipRules::kAssetMetaKey), stored))
            continue;
        item.Own = ShipRules::Parse(stored);
        item.OwnIsValid = item.Own.has_value();
    }
    return !outSelection.Items.empty();
}

ShipRuleSelection::State ShipRuleSelection::Describe() const
{
    State state;
    if (Items.empty())
        return state;
    const Item& first = Items.front();
    std::array<size_t, std::size(kChoices)> effectiveCounts{};
    bool sameOwn = true;
    bool sameInherited = true;
    for (const Item& item : Items)
    {
        state.AnyOwn |= item.Own.has_value() || !item.OwnIsValid;
        state.NeverShips |= item.NeverShips;
        sameOwn &= item.OwnIsValid && item.Own == first.Own;
        sameInherited &= !item.Own && item.OwnIsValid && SameInherited(item.Inherited, first.Inherited);
        const ShipRule effective = item.Own.value_or(item.Inherited.Rule);
        ++effectiveCounts[static_cast<size_t>(std::find(std::begin(kChoices), std::end(kChoices), effective) -
                                              std::begin(kChoices))];
    }

    if (sameOwn && first.Own)
    {
        state.SelectedIndex = static_cast<int>(std::find(std::begin(kChoices), std::end(kChoices), *first.Own) -
                                               std::begin(kChoices));
        state.Caption = Items.size() == 1 ? "Set here. " + WithoutOwnRuleText(first.Inherited)
                                          : "Set here on all " + std::to_string(Items.size()) + " items.";
    }
    else if (sameInherited)
    {
        state.NoSelectionText = InheritedText(first.Inherited);
    }
    else if (Items.size() == 1)
    {
        state.NoSelectionText = "Not valid";
        state.Caption = "The stored rule is not Inferred, Always or Never. Pick one, or clear it.";
    }
    else
    {
        state.NoSelectionText = "Mixed";
        std::vector<std::string> counts;
        for (size_t choice = 0; choice < std::size(kChoices); ++choice)
        {
            if (effectiveCounts[choice] != 0)
                counts.push_back(std::to_string(effectiveCounts[choice]) + " " + Label(kChoices[choice]));
        }
        state.Caption = Join(counts, ", ") + ".";
    }

    if (state.NeverShips)
        state.Caption = "Source files, Editor/ folders, build output and scene backups never ship.";
    if (!Errors.empty())
        state.Caption = Errors.front();
    return state;
}

} // namespace GameEngine::Editor
