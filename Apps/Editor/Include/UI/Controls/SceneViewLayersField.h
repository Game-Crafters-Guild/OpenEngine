#pragma once

#include "Events/Event.h"
#include "UI/Controls/MaskField.h"

namespace GameEngine
{

// The Scene View toolbar's layer filter: a MaskField on the project's layer names that reads and writes
// Editor::SceneViewLayerFilter. Instantiated from UXML via the UI element factory registry.
class SceneViewLayersField final : public MaskField
{
  public:
    SceneViewLayersField();

  private:
    // Shows the filter of the project now open: at construction and when a project opens (the render
    // settings apply refreshes the layer fields).
    void ShowOpenProjectsFilter();
    void OnLayersChanged(uint32 bits, bool on);

    EventSubscription m_Refreshed;
};

} // namespace GameEngine
