#pragma once

#include "Types/StringId.h"
#include "UI/Controls/Mount.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <cstdint>

namespace GameEngine
{
class GameViewPanel;
class SceneViewPanel;

namespace Editor
{

/// The Scene View and Game View panels mounted in one window's dock, for the callers that
/// ask every frame (frame routing, controller sync, overlay declaration, binding). Each
/// EditorWindowContext owns one.
///
/// Each call reads the panel live through the window's dock Mount: a tab activation swaps
/// the Mount's target. Returns nullptr when the window has no UI, the Mount is absent, or
/// its active target is another panel. UI thread only.
class MountedViewPanels final
{
  public:
    SceneViewPanel* SceneView(UIManager* ui);
    GameViewPanel* GameView(UIManager* ui);

  private:
    friend struct MountedViewPanelsTestAccess;

    // The Mount that FindById(id) returns from the window's root, kept between calls. It
    // looks up again only when the manager or a tree generation changed or the held Mount
    // was destroyed, so a hit and a miss both skip the lookup while the dock is unchanged.
    class HeldMount final
    {
      public:
        Mount* Find(UIManager* ui, StringId id)
        {
            if (!ui)
                return nullptr;
            const uint64_t structureGeneration = ui->GetTreeStructureGeneration();
            const uint64_t idGeneration = ui->GetElementIdGeneration();
            Mount* held = m_Mount.Get();
            if (m_Manager == ui && m_StructureGeneration == structureGeneration &&
                m_IdGeneration == idGeneration && (held != nullptr) == m_Found)
                return held;

            UIElement* root = ui->GetRootElement();
            Mount* found = root ? dynamic_cast<Mount*>(root->FindById(id)) : nullptr;
            m_Mount = UIElement::MakeWeakRef(found);
            m_Manager = ui;
            m_StructureGeneration = structureGeneration;
            m_IdGeneration = idGeneration;
            m_Found = found != nullptr;
            return found;
        }

      private:
        UIElement::WeakRef<Mount> m_Mount;
        const UIManager* m_Manager = nullptr;
        uint64_t m_StructureGeneration = 0;
        uint64_t m_IdGeneration = 0;
        bool m_Found = false;
    };

    HeldMount m_SceneViewMount;
    HeldMount m_GameViewMount;
};

} // namespace Editor
} // namespace GameEngine
