#pragma once

#include "Types/StringId.h"

#include <cstdint>
#include <functional>

namespace GameEngine {

class GameUIHost;
class UIManager;
class UIElement;

// Process-wide handle to the play-mode HUD host. The Player and the editor
// Game View register the GameUIHost that composites UIDocument entities onto
// the game view; the Scene View preview host must not. User C++ systems bind
// buttons via GetUIManager() the same way GameUIHostTests do.
//
// Main thread only (same rule as EngineCore::SetRuntimeInput).
namespace GameUI {

// Non-owning. Pass nullptr to clear. Destroying the registered host also clears.
void SetHost(GameUIHost* host);

GameUIHost* GetHost();

// nullptr when no gameplay host is registered (edit mode without a Game View,
// or the first Player frames before the host is created).
UIManager* GetUIManager();

// Per-entity subtree root for a UIDocument, or nullptr if that document is not
// bound on the gameplay host.
UIElement* FindDocumentRoot(uint64_t entityId);

// Element `id` in any bound document, or nullptr: FindElementById("score"_sid),
// or FindElementById(HashStringId(name)) for an id built at runtime. An id two
// documents share resolves to the one owned by the lowest entity id, so the
// winner is stable. On a match, outOwnerEntityId (if non-null) receives that
// owning UIDocument entity, so a game with several documents can pin later
// lookups to one of them through FindDocumentRoot(owner), which returns nullptr
// while that document loads or after play stops: check it before FindById.
// outOwnerEntityId is untouched when nothing matches.
//
// An empty id is never registered; FindElementById(0) returns nullptr.
UIElement* FindElementById(StringId id, uint64_t* outOwnerEntityId = nullptr);

// Bumps when a document layout binds, a subtree is removed, or a .uxml hot
// reload reconciles a bound subtree. A bump does not imply the old elements
// died: a reconcile reuses id-matched elements of unchanged type, handlers
// included, so re-registering on every bump double-subscribes. See
// GameUIHost::GetBindGeneration.
uint64_t GetBindGeneration();

// Increments when play stops and discards bound gameplay-HUD document
// subtrees. The next SyncFromWorld rebuilds them from UIDocument. User
// RegisterEventHandler targets die with the old elements
// while the user module is still mapped. Does not walk the UIManager
// root, so engine handlers on the host itself are untouched.
void NotifyGameplayStopped();

uint64_t GetGameplaySession();

// There is deliberately no session-scoped click registration on this surface.
//
// C++ callers register on document elements directly, through the ordinary UI
// APIs: resolve the element with FindElementById above and
// call UIElement::RegisterEventHandler. The TREE LIFECYCLE owns those handlers —
// play-stop drops every bound UIDocument subtree (GameUIHost::ResetBoundDocuments,
// #1061), which destroys the elements and with them their handlers, and a native
// module unload revokes any handler whose callable belongs to the unmapped image
// (UI/ModuleOwnedHandlers.h). Neither needs a session gate to be correct.
//
// The gate still exists for MANAGED callers, privately inside the scripting ABI
// (GE_UIElement_RegisterEvent): a C# callback can outlive its collectible load
// context and nothing native observes managed unloads, so it needs a
// token-and-session shape that a C++ caller holding the element does not.
// Keeping that shape off this header is the point: a public wrapper here would be
// an engine callable wrapping a caller's callable, which is exactly the nesting
// that hides a user module's ownership from the unload-time revocation.

} // namespace GameUI
} // namespace GameEngine
