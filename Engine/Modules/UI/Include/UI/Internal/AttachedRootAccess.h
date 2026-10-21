#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{

// The authored-id index's private window onto UIElement's attached root
// (UI::ElementIdIndex, UIManager::AdoptRoot).
//
// AttachedRoot is the manager root an element's DFS-parent chain ends at, or nullptr when
// the chain ends anywhere else. It is maintained at every DFS edge write, so reading it
// costs no walk; the only other writer is the root swap, which marks the element a
// UIManager holds as its root.
struct UIAttachedRootAccess
{
    static const UIElement* AttachedRoot(const UIElement& el) { return el.m_AttachedRoot; }

    // Marks `el` as a UIManager's root and rewrites the attached root of its DFS subtree to
    // match. A root is never unmarked: AdoptRoot destroys the root it replaces.
    static void MarkManagerRoot(UIElement& el)
    {
        el.m_IsManagerRoot = true;
        el.NoteDfsParentChanged();
    }
};

} // namespace GameEngine
