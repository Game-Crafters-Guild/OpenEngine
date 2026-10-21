#pragma once

#include <memory>
#include <string>

namespace GameEngine
{
class Label;
}

namespace GameEngine::Editor
{

/// The id of the state label in a pending asset's header; the inspector updates it by id.
inline constexpr const char* kPendingAssetSuffixLabelId = "InspectorPendingAssetSuffix";

/// The inspector header's title while a selected asset's load runs: the asset's name, which
/// sizes to its text and ellipsises first, and right after it the state suffix ("preparing…"),
/// hidden until the asset type has one to show. The row's free space goes after the suffix.
struct PendingAssetHeaderLabels
{
    std::unique_ptr<Label> Name;
    std::unique_ptr<Label> Suffix;
};

PendingAssetHeaderLabels MakePendingAssetHeaderLabels(const std::string& name);

} // namespace GameEngine::Editor
