#pragma once

#include "Types/StringId.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine
{
class UIElement;

namespace UI
{

/// UI-thread-only authored-id index owned by a UIManager, keyed by StringId. Two different
/// ids with the same StringId are refused at registration (Debug asserts, Release logs an
/// error naming both), so a lookup compares no strings. A refused id stays unregistered,
/// even after the element it collided with is destroyed, until its element registers again
/// (an id or owner change).
///
/// Invariants, outside a tree edit in progress:
/// - Registered: exactly the elements this manager owns that have a non-empty id and were
///   not refused. Owner gain registers (UIManager::NotifyElementOwnerChanged), owner loss
///   and destruction unregister (UIManager::UnregisterElementLookups), SetId does both.
///   Ownership outlives attachment: an inactive dock tab's panel stays registered.
/// - Attached root: every element's attached root (UIAttachedRootAccess::AttachedRoot) is
///   the element a UIManager holds as its root at the end of the element's DFS-parent
///   chain, or nullptr. It is rewritten below the edge at every DFS edge write (AddChild,
///   InsertChild, RemoveChild, TakeChild, Mount retargeting and activation, a Mount's
///   destruction) and at the root swap (UIManager::AdoptRoot); destroyed elements leave
///   with their subtree. It holds no epoch and is never recomputed lazily: the writes sit
///   in the only functions that change those edges.
/// - Lookup: Find(subtree, id) returns the first registered element with `id` in DFS order
///   from `subtree` (itself, its children in order, then its Mount target). From a manager
///   root, membership is one comparison of the candidate's attached root; from any other
///   element, it is a walk up the candidate's DFS-parent chain.
///
/// Inside an attach (AddChild, InsertChild) the attached root is written twice: before the
/// owner changes, so a lookup from the root in an owner callback finds the attaching
/// element, and again after the owner callbacks have run, so a callback that moves an
/// ancestor during the attach leaves no stale value behind.
class ElementIdIndex
{
  public:
    void Register(UIElement& element);
    void Unregister(UIElement& element);

    /// Find the first matching id in depth-first order, including subtree itself.
    /// Ordinary children precede a Mount target, independent of registration order.
    /// Returns nullptr for id 0: an empty id is never registered.
    UIElement* Find(const UIElement& subtree, StringId id) const;

  private:
    FastHashMap<StringId, std::vector<UIElement*>> m_Elements;
};

} // namespace UI
} // namespace GameEngine
