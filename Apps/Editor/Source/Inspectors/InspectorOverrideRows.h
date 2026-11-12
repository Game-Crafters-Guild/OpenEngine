#pragma once

#include "ECS/Component/ComponentFieldRegistry.h" // ECS::ComponentTypeId
#include "UI/Interaction/ContextMenuManipulator.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

struct InspectorContext;
struct OverrideMenuTarget;
class UIElement;

/// Binds `row` to field `field` of component `typeId`. On a single selection, the override marks follow the field.
/// A right-click opens one menu: the override entries, a separator, `rowItems`. `field` is the reflected name, or the
/// scene key when there is none (InspectorFieldForSceneKey). A null `row` binds nothing.
void BindFieldRow(const InspectorContext& context, ECS::ComponentTypeId typeId, UIElement* row, std::string field,
                  std::vector<ContextMenuManipulator::Item> rowItems = {});

/// Opens, on a right-click on `element`, the entries InspectorRegistry::QueryOverrideMenu gives `target`, and
/// runs `refresh` after the chosen entry. A right-click opens nothing while the target has no entries.
void AttachOverrideMenu(UIElement& element, const OverrideMenuTarget& target, std::function<void()> refresh);

} // namespace GameEngine
