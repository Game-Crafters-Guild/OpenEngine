#pragma once

// EditorComponentTraits: the editor behavior of a component type as data, registered by the code
// that owns the component. The editor reads each field without naming the component.
//
// EditorSDK.dll holds the one registry that Editor.exe and package modules share. A registration
// replaces the one of its type id; a replaced module stays mapped, so its callables stay valid.

#include "ECS/ECS.h"
#include "ECS/ModuleRegistration.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class UndoRedoService;

// Runs once a generic edit is applied, inside the edit's undo step (see
// EditorComponentTraits::BeforeGenericEdit). `label` names that step.
using GenericEditUndoRecorder = std::function<void(UndoRedoService& undo, const std::string& label)>;

// The parts of a Hierarchy row a decoration may style (see
// EditorComponentTraits::DecorateHierarchyRow). Title is null while the row has none.
struct HierarchyRowParts
{
    UIElement& Row;
    UIElement* Title = nullptr;
};

struct EditorComponentTraits
{
    // Inspector section title (empty = fall back to the built-in name table /
    // reflected type name).
    std::string DisplayName;
    // CSS class for the inspector section icon (empty = built-in chain).
    std::string InspectorIconClass;
    // Add Component menu category the component is listed under (empty = the
    // built-in category table).
    std::string InspectorCategory;
    // Inspector section header tooltip (empty = the built-in tooltip table).
    std::string InspectorTooltip;
    // The component gets no inspector section of its own: another component's
    // inspector shows what an author needs of it.
    bool HideInInspector = false;
    // CSS class added to the entity's hierarchy row when the component is
    // present (empty = no contribution).
    std::string HierarchyRowClass;
    // Runs on every Hierarchy row as it binds, whatever its entity holds, for a
    // component whose editor marks the entities under it too. It sets and clears
    // only the classes and the tooltip it owns. Unset = no decoration.
    std::function<void(ECS::World&, ECS::EntityHandle, HierarchyRowParts&)> DecorateHierarchyRow;
    // The entity is its own pick target: the instance-root upward walk stops
    // here instead of grouping it under a model root.
    bool IsPickInstanceRoot = false;
    // A Scene View click on any entity under the entity climbs to it: the
    // outermost such ancestor is the pick root, and a second click goes inside.
    bool IsPickClimbRoot = false;
    // The transform gizmo pivots at the entity origin (the local pivot
    // transformed to world), not at rendered-content anchors.
    bool GizmoPivotAtEntityOrigin = false;
    // The inspector section leads the entity's sections, above Transform: for
    // a component that is the one thing an author edits on an entity whose
    // other sections are generated (a fence's span piece).
    bool LeadsInspector = false;
    // For a component that owns a stack of other components, the way a volume
    // owns its effects: true for each component type that is an entry of that
    // stack. On an entity with this component, an entry's inspector section
    // renders inside this component's section and drag-reorders among the other
    // entries there. Register refuses traits that claim a component type,
    // registered before them, that another host already claims; a type
    // registered after two hosts that claim it is not refused (#2216). Unset =
    // the section hosts no other sections.
    std::function<bool(ECS::ComponentTypeId)> HostsInspectorSection;
    // CSS class added to the body of each section this one hosts (empty = none).
    std::string HostedSectionBodyClass;
    // The enable dot's tooltip for every entry this component hosts that registers none of
    // its own, in the form of EnableToggleTooltip below, naming what it switches as an effect
    // ("Switch this effect on or off. Off ..."). Empty = the generic tooltip.
    std::string HostedEnableToggleTooltip;
    // An entry on an entity without this component is inactive and stays a
    // top-level section; its header carries this badge, with this warning as the
    // badge's and the header's tooltip.
    std::string MissingHostBadge;
    std::string MissingHostWarning;
    // The section header's enable dot tooltip for this type, replacing the generic
    // one: what the dot switches, then what off does, as in "Switch this component on
    // or off. Off stops its sound." (ComponentEnabledToggleTooltip). Empty = the
    // generic tooltip.
    std::string EnableToggleTooltip;
    // What editor text calls an entity with this component while it has no name
    // (EntityDisplayName): "Untitled mark-up". Empty = its id (EntityIdLabel).
    std::string UntitledName;
    // For a component whose system writes to other entities (a sky drives its
    // linked light's color): editor code that edits without knowing the
    // component (a component's enable toggle, an entity's creation) calls
    // this before the edit and runs the recorder it returns once the edit is
    // applied, inside the same undo step, so undoing the edit also puts back
    // what the system overwrites because of it. Called before
    // every such edit, whatever entity it touches: what the system writes
    // follows from the whole world, not from the edited entity alone. Unset,
    // or an empty recorder, records nothing.
    std::function<GenericEditUndoRecorder(ECS::World&)> BeforeGenericEdit;
};

class EditorComponentTraitsRegistry
{
public:
    static EditorComponentTraitsRegistry& Get();

    // Replace-semantics: a re-registration for the same type id overwrites the
    // previous traits (module hot-reload replays its registrations). Traits whose
    // HostsInspectorSection claims a registered component type that another
    // component's traits already host are refused with an error naming both
    // hosts and the type.
    void Register(ECS::ComponentTypeId typeId, EditorComponentTraits traits);

    // Copy-out lookup: callers get a stable snapshot, never a pointer into the
    // map (registrations can land at any time from module loads).
    bool TryGet(ECS::ComponentTypeId typeId, EditorComponentTraits& outTraits) const;

    // The first UntitledName the traits of `types` set, in their order; empty for none. One
    // lookup per type under one lock, copying the name alone (EntityDisplayName's fallback).
    std::string UntitledNameOf(const std::vector<ECS::ComponentTypeId>& types) const;

    // The component whose traits host `hostedTypeId`'s inspector section (see
    // EditorComponentTraits::HostsInspectorSection), with its traits; false when
    // no registered component hosts it. Register keeps the host unique for a
    // component type registered before its hosts; for a type registered after
    // two hosts that claim it, the first match in map order answers (#2216).
    bool TryGetSectionHost(ECS::ComponentTypeId hostedTypeId, ECS::ComponentTypeId& outHostTypeId,
                           EditorComponentTraits& outHostTraits) const;

    using Entries = std::vector<std::pair<ECS::ComponentTypeId, EditorComponentTraits>>;

    // The (typeId, traits) pairs for presence scans (hierarchy row classes, pick roots): one
    // immutable list that Register replaces, so a call copies a pointer and never the traits.
    std::shared_ptr<const Entries> Snapshot() const;

    // C12 editor-kind unload refusal diagnostics: append a description of
    // every entry attributed to `moduleId` (the traits' accessor callables are
    // module code and pin its images mapped).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    EditorComponentTraitsRegistry() = default;

    mutable std::mutex m_Mutex;
    // Held for a whole Register, so a host's claim check and its insert are one
    // step; the claim predicates run outside m_Mutex.
    std::mutex m_RegistrationMutex;
    std::unordered_map<ECS::ComponentTypeId, EditorComponentTraits> m_Traits;
    std::shared_ptr<const Entries> m_Snapshot = std::make_shared<const Entries>();
    // Module stamp per registration (attribution is registry bookkeeping, not
    // part of the module-facing traits struct).
    std::unordered_map<ECS::ComponentTypeId, ECS::ModuleRegistrationStamp> m_ModuleOwners;
};

} // namespace GameEngine::Editor
