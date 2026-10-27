#pragma once

#include "UI/UIElement.h"

#include <string>
#include <vector>

namespace GameEngine::Editor
{

/// The Inspector's headers, after an instance entity's components, for the components the prefab
/// gives the entity and the instance removed: one muted header each, with the component's icon, the
/// override edge and the Removed badge, and no options.
class PrefabRemovedComponents final : public UIElement
{
  public:
    /// One header for each scene token in `components`, in that order.
    explicit PrefabRemovedComponents(const std::vector<std::string>& components);
};

} // namespace GameEngine::Editor
