#pragma once

#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <typeindex>
#include <mutex>
#include <vector>

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "ECS/ModuleRegistration.h"
#include "AssetCore/AssetTypes.h"
#include "Audio/AudioHandles.h"

namespace GameEngine {

class IThumbnailProvider;
class UIElement;
class InspectorSection;
struct EditorContext;

namespace Platform { class Window; }
namespace ECS { class World; struct EntityHandle; }
namespace Editor { class UndoRedoService; class EditorChangeNotifications; class ColorPickerScope; }

// Callbacks for an inspector-initiated color picker.
// argb is the clamped 0-255 color; intensity is the HDR multiplier from the picker (>= 1).
struct ColorPickerCallbacks
{
    std::function<void(uint32_t argb, float intensity)> onApply;
    std::function<void()> onCancel;
    std::function<void(uint32_t argb, float intensity)> onValueChanging;
    // Optional: the picker closes, calling none of the above, when this scope
    // closes. Read when the picker opens.
    Editor::ColorPickerScope* scope = nullptr;
};

// Opens a standalone native color picker window. initialArgb is 0xAARRGGBB; initialIntensity is the HDR multiplier.
using OpenColorPickerWindowFn = std::function<void(uint32_t initialArgb, float initialIntensity, ColorPickerCallbacks callbacks)>;

// Context passed to inspector functions when building UI for a target object.
struct InspectorContext {
    UIElement* Parent = nullptr;      // Parent UI element to which inspector UI should be added
    ECS::World* World = nullptr;      // ECS world (may be null for non-ECS targets)
    ECS::EntityHandle Entity{};       // Primary entity being inspected (invalid for non-entity targets)
    std::vector<ECS::EntityHandle> Entities; // All selected entities (includes Entity; empty = single-edit)
    void* Object = nullptr;           // Pointer to the component/asset being inspected
    Editor::UndoRedoService* Undo = nullptr; // optional editor undo/redo service
    Editor::EditorChangeNotifications* ChangeNotifications = nullptr; // optional editor change notifications
    IThumbnailProvider* Thumbnails = nullptr; // optional thumbnail provider for asset previews
    // Optional: allow inspectors to query the Assets panel selection without directly
    // depending on the AssetsPanel/Controller types.
    std::function<std::vector<std::filesystem::path>()> GetSelectedAssetPaths;
    // Optional: when an asset inspector starts/stops audio preview, it sets the current handle here
    // so the panel can stop playback when focus is lost (e.g. selection changes).
    std::function<void(Audio::AudioEmitterHandle)> SetAudioPreviewHandle;

    // Optional: open a native color picker window (same as the one in Settings).
    OpenColorPickerWindowFn OpenColorPickerWindow;

    // Optional: inspectors that need throttled live refresh push a callback here.
    // Owned by InspectorPanel; cleared on each ShowEntity rebuild.
    std::vector<std::function<void()>>* SimulationRefreshCallbacks = nullptr;
    // Optional: inspectors that need per-frame visual refresh push a callback here. Keep these cheap.
    // Owned by InspectorPanel; cleared on each ShowEntity rebuild.
    std::vector<std::function<void()>>* FrameRefreshCallbacks = nullptr;
    // Optional: inspectors that mark overridden fields push a callback here that sets the marks again.
    // Owned by InspectorPanel; cleared with the section's other refresh callbacks.
    std::vector<std::function<void()>>* OverrideMarkRefreshCallbacks = nullptr;
    // Optional: sets every override mark of the shown entity again, in place, with no rebuild.
    std::function<void()> RefreshOverrideMarks;

    // Optional: navigate to and highlight an asset in the asset browser by path.
    std::function<void(const std::filesystem::path&)> PingAsset;
    // Optional: navigate to and highlight an asset while preserving current inspector target.
    std::function<void(const std::filesystem::path&)> PingAssetPreserveInspector;

    // Optional: select an entity in the editor, as clicking it in the Hierarchy would.
    std::function<void(ECS::EntityHandle)> SelectEntity;

    // Optional: open a script file in the internal Script Editor panel.
    std::function<void(const std::filesystem::path&)> OpenScript;

    // Optional: open a material shader graph .glsl in the Node Graph panel — an
    // explicit destination, for inspectors that offer it as a choice.
    std::function<void(const std::filesystem::path&)> OpenMaterialGraph;

    // Optional: open an asset with the editor's open-asset policy — the same
    // routing an Assets browser double-click takes (Script Editor, Node Graph,
    // Inspector, OS, ...), so an inspector's "Open" button never invents its own.
    std::function<void(const std::filesystem::path&)> OpenAsset;

    // Optional: rebuild the current inspector target after an inspector-owned
    // setting changes which affects visible rows. The rebuild is deferred to the
    // panel's next safe point, so a refresh callback may call it: the rebuild
    // destroys the callbacks it runs from.
    std::function<void()> RequestInspectorRefresh;

    // Optional: an asset inspector that can re-present a reloaded payload
    // without being rebuilt pushes a callback here. When the shown asset
    // reloads the panel runs them first and rebuilds only if one returns false
    // (or none is registered), so a payload rebuild landing seconds after the
    // edit does not take the focus, an open popup and the scroll with it.
    // Owned by InspectorPanel; cleared on each rebuild.
    std::vector<std::function<bool()>>* AssetReloadedCallbacks = nullptr;

    // Optional: resolve the current ECS world at callback time.
    // Inspector callbacks should prefer this over the captured World pointer
    // when the world may have changed between inspector build and callback invocation.
    std::function<ECS::World*()> GetWorld;

    // Optional: host window for native context menus in inspectors.
    Platform::Window* Window = nullptr;
    const EditorContext* EditorCtx = nullptr;

    // Mesh Renderer material inspector: persisted display order for multi-slot models
    // (permutation of 0..matCount-1). Null when not used.
    std::function<std::vector<uint32_t>*(uint32_t matCount)> GetMaterialSlotDisplayOrder;

    // Register each per-slot InspectorSection for header drag-reorder (matCount > 1 only).
    // Wired by InspectorPanel to reuse the same section-drag pipeline as component headers.
    std::function<void(InspectorSection* slotSection, UIElement* slotsContainer, uint32_t modelSlotIndex)>
        RegisterMaterialSlotSectionForDrag;

    // The InspectorSection that owns this inspector's content root.
    // Inspectors may use this to update the section header (e.g. icon tint).
    InspectorSection* Section = nullptr;

    // User preference from Settings > UI > Inspector. When false, inspectors should
    // omit nonessential explanatory/info cards and keep only editable controls.
    bool ShowInfoCards = true;
};

using InspectorFn = std::function<void(const InspectorContext&)>;

// The state a selected asset's inspector header shows after the asset's name while its load has
// not landed ("preparing…"), or empty for none, which leaves the default "Loading <file>..."
// line. Main thread, polled a few times a second while the load runs, so it reads state rather
// than doing work.
using AssetLoadingSuffixFn = std::function<std::string(const GUID& guid)>;

// Where the Inspector adds an entity banner: under the entity's header, or after its components.
enum class EntityBannerPlacement
{
    BeforeComponents,
    AfterComponents,
};

// How one component of an entity differs from the source the entity derives from, as a registered
// answerer reports it. Empty for an entity that derives from nothing.
struct ComponentOverrides
{
    // True when the component or any of its fields differs from the source.
    bool Overridden = false;
    // The header badge for the component ("Added"), and its tooltip; empty for none.
    std::string Badge;
    std::string BadgeTooltip;
    // The reflected names of the fields that differ from the source.
    std::vector<std::string> Fields;

    bool IsFieldOverridden(std::string_view fieldName) const
    {
        for (const std::string& field : Fields)
            if (field == fieldName)
                return true;
        return false;
    }
};

using ComponentOverridesFn =
    std::function<ComponentOverrides(ECS::World& world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId)>;

// Simple registry for custom inspectors.
class InspectorRegistry {
public:
    static InspectorRegistry& Get();

    // Register a custom inspector for a component type T.
    template<typename T>
    void RegisterComponentInspector(InspectorFn fn) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_ComponentInspectors[std::type_index(typeid(T))] = fn;
        const ECS::ComponentTypeId id = ECS::GetComponentTypeId<T>();
        m_ComponentInspectorsById[id] = std::move(fn);
        m_ModuleOwners[id] = ECS::GetActiveRegistrationModule();
    }

    // Register an inspector for a ComponentTypeId without a concrete ECS component type
    // (e.g. inspector-only pseudo sections).
    void RegisterComponentInspectorByTypeId(ECS::ComponentTypeId id, InspectorFn fn)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_ComponentInspectorsById[id] = std::move(fn);
        m_ModuleOwners[id] = ECS::GetActiveRegistrationModule();
    }

    // C12 editor-kind unload refusal diagnostics: append a description of
    // every inspector attributed to `moduleId` (inspector callables are module
    // code and pin its images mapped). Defined in InspectorRegistry.cpp (needs
    // the component registry for display names).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

    // Used by late package-module loading to refresh only an inspector whose
    // implementation was registered or replaced by that module.
    bool IsComponentInspectorOwnedBy(ECS::ComponentTypeId typeId, std::string_view moduleId) const;
    bool IsAssetInspectorOwnedBy(AssetType type, std::string_view moduleId) const;

    // Try to get a registered inspector for component type T. Returns nullptr if none.
    template<typename T>
    InspectorFn* TryGetComponentInspector() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_ComponentInspectors.find(std::type_index(typeid(T)));
        if (it == m_ComponentInspectors.end()) {
            return nullptr;
        }
        return &it->second;
    }

    // Try to get a registered inspector for a component type id. Returns nullptr if none.
    InspectorFn* TryGetComponentInspector(ECS::ComponentTypeId typeId)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_ComponentInspectorsById.find(typeId);
        if (it == m_ComponentInspectorsById.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    // Register the banner `key` that the Inspector adds at `placement` on an entity; a second
    // registration of `key` replaces the first in its place. The function adds nothing for an entity
    // it has no banner for. Banners show in registration order.
    void RegisterEntityBanner(std::string_view key, EntityBannerPlacement placement, InspectorFn fn)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (EntityBanner& banner : m_EntityBanners)
        {
            if (banner.Key == key)
            {
                banner.Placement = placement;
                banner.Fn = std::move(fn);
                return;
            }
        }
        m_EntityBanners.push_back({std::string(key), placement, std::move(fn)});
    }
    // The registered entity banners at `placement`, in registration order.
    std::vector<InspectorFn> GetEntityBanners(EntityBannerPlacement placement) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::vector<InspectorFn> banners;
        for (const EntityBanner& banner : m_EntityBanners)
            if (banner.Placement == placement)
                banners.push_back(banner.Fn);
        return banners;
    }
    // Set the one answerer the component sections and the default field rows ask how a component
    // differs from its entity's source; a second call replaces the first. The entity header asks for
    // the Name component.
    void SetComponentOverridesAnswerer(ComponentOverridesFn fn)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_ComponentOverrides = std::move(fn);
    }
    // How `typeId` on `entity` differs from the entity's source; empty when no answerer is set.
    ComponentOverrides QueryComponentOverrides(ECS::World& world, ECS::EntityHandle entity,
                                               ECS::ComponentTypeId typeId) const
    {
        ComponentOverridesFn answerer;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            answerer = m_ComponentOverrides;
        }
        return answerer ? answerer(world, entity, typeId) : ComponentOverrides{};
    }
    // Register an inspector for an asset type (Material, Shader, etc.)
    void RegisterAssetInspector(AssetType type, InspectorFn fn)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const int key = static_cast<int>(type);
        m_AssetInspectors[key] = std::move(fn);
        m_AssetInspectorOwners[key] = ECS::GetActiveRegistrationModule();
    }

    // Register the pending-load header suffix for an asset type, beside its asset inspector and
    // from the same module.
    void RegisterAssetLoadingSuffix(AssetType type, AssetLoadingSuffixFn fn)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_AssetLoadingSuffixes[static_cast<int>(type)] = std::move(fn);
    }

    // The registered pending-load header suffix for an asset of `type`, or empty when none is
    // registered or it has nothing to say now.
    std::string AssetLoadingSuffix(AssetType type, const GUID& guid)
    {
        AssetLoadingSuffixFn fn;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            auto it = m_AssetLoadingSuffixes.find(static_cast<int>(type));
            if (it == m_AssetLoadingSuffixes.end())
                return {};
            fn = it->second;
        }
        return fn(guid);
    }

    // Try to get a registered inspector for an asset type. Returns nullptr if none.
    InspectorFn* TryGetAssetInspector(AssetType type)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_AssetInspectors.find(static_cast<int>(type));
        if (it == m_AssetInspectors.end())
        {
            return nullptr;
        }
        return &it->second;
    }

private:
    InspectorRegistry() = default;

    std::unordered_map<std::type_index, InspectorFn> m_ComponentInspectors;
    std::unordered_map<ECS::ComponentTypeId, InspectorFn> m_ComponentInspectorsById;
    std::unordered_map<int, InspectorFn> m_AssetInspectors;
    struct EntityBanner
    {
        std::string Key;
        EntityBannerPlacement Placement = EntityBannerPlacement::BeforeComponents;
        InspectorFn Fn;
    };
    std::vector<EntityBanner> m_EntityBanners;
    ComponentOverridesFn m_ComponentOverrides;
    std::unordered_map<int, AssetLoadingSuffixFn> m_AssetLoadingSuffixes;
    // Module stamp per component-inspector registration (C12 attribution;
    // registry bookkeeping — see ECS/ModuleRegistration.h).
    std::unordered_map<ECS::ComponentTypeId, ECS::ModuleRegistrationStamp> m_ModuleOwners;
    std::unordered_map<int, ECS::ModuleRegistrationStamp> m_AssetInspectorOwners;
    mutable std::mutex m_Mutex;
};

} // namespace GameEngine
