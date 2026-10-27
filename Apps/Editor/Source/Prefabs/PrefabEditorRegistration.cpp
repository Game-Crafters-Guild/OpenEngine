#include "Prefabs/PrefabHierarchyRows.h"

#include "Assets/AssetCreation.h"
#include "Assets/AssetsFolderDropRegistry.h"
#include "Editor/DragDropPayloads.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Editor/Hierarchy/HierarchyEntityDecode.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Prefabs/CreatePrefabCommand.h"
#include "Prefabs/CreatePrefabDialog.h"
#include "Prefabs/PrefabBar.h"
#include "Prefabs/PrefabInstanceDescriptions.h"
#include "Prefabs/PrefabRemovedComponents.h"
#include "Prefabs/PrefabThumbnailHandler.h"
#include "Prefabs/UnpackPrefabCommand.h"
#include "SceneView/SceneViewNoticeOverlay.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/EditorIcons.h"
#include "UI/UIManager.h"
#include "EditorContext.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Components/HierarchyQueries.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Core/Engine.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/PrefabAuthoring.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

constexpr uint32_t kCmdCreatePrefab = 0x5101u;
constexpr uint32_t kCmdSelectPrefabAsset = 0x5102u;
constexpr uint32_t kCmdUnpackPrefab = 0x5103u;
constexpr uint32_t kCmdUnpackPrefabCompletely = 0x5104u;
constexpr int kPrefabMenuPriority = -46;
constexpr const char* kShortcutGroup = "Hierarchy";
constexpr const char* kCreatePrefabShortcut = "Create Prefab";

// What a confirmed Create Prefab dialog acts on: the roots, one prefab each, and the services of the
// Hierarchy that opened the dialog.
struct CreatePrefabTarget
{
    ECS::World* World = nullptr;
    std::vector<ECS::EntityHandle> Roots;
    EditorChangeNotifications* Notifications = nullptr;
    UndoRedoService* Undo = nullptr;
    std::function<void(ECS::EntityHandle)> SelectEntity;
    std::function<void()> NotifyWorldStructureChanged;
};

// Create Prefab applies to an entity that is not an instance and is not inside one.
bool CanCreatePrefab(ECS::World& world, ECS::EntityHandle entity)
{
    const AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    return ClassifyPrefabRow(world, entity, registry).Role == PrefabRowRole::None;
}

// The file name of the prefab of `root`: the entity's name, or "Prefab" when that cannot name a file.
std::string PrefabFileStem(ECS::World& world, ECS::EntityHandle root)
{
    const auto* name = world.GetComponent<Components::Name>(root);
    const std::string stem = name ? TrimWhitespace(std::string(name->View())) : std::string();
    return CreatePrefabDialog::DescribeNameProblem(stem).empty() ? stem : std::string("Prefab");
}

// True for an instance root that is inside no other instance: Unpack applies to it.
bool CanUnpack(ECS::World& world, ECS::EntityHandle entity)
{
    const AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    const auto* parent = world.GetComponent<Components::Parent>(entity);
    return ClassifyPrefabRow(world, entity, registry).Role == PrefabRowRole::InstanceRoot &&
           !FindInstanceRoot(world, parent ? parent->Entity : ECS::EntityHandle{}).IsValid();
}

// The path of the prefab file of the nearest instance at or above `entity`; empty when there is none
// or the file is not in the project.
std::filesystem::path FindPrefabAssetPath(ECS::World& world, ECS::EntityHandle entity)
{
    const ECS::EntityHandle root = FindInstanceRoot(world, entity);
    const auto* instance = root.IsValid() ? world.GetComponent<Components::PrefabInstance>(root) : nullptr;
    AssetMetadata metadata;
    if (!instance ||
        !EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(instance->Source, metadata))
        return {};
    return metadata.Path;
}

// The roots among `entities` that no other entity of `entities` holds in its subtree, in order.
std::vector<ECS::EntityHandle> FindTopRoots(ECS::World& world, const std::vector<ECS::EntityHandle>& entities)
{
    std::vector<ECS::EntityHandle> roots;
    for (const ECS::EntityHandle entity : entities)
    {
        if (!world.IsValid(entity) || std::find(roots.begin(), roots.end(), entity) != roots.end())
            continue;
        const bool held = std::any_of(entities.begin(), entities.end(), [&world, entity](ECS::EntityHandle other)
                                      { return other != entity && Components::IsDescendantOf(world, entity, other); });
        if (!held)
            roots.push_back(entity);
    }
    return roots;
}

// Why `roots` cannot each become a prefab, with the fix; empty when they can.
std::string DescribeCreateRefusal(ECS::World& world, const std::vector<ECS::EntityHandle>& roots)
{
    if (roots.empty())
        return "Select an entity in the scene to create a prefab.";
    for (const ECS::EntityHandle root : roots)
    {
        if (!CanCreatePrefab(world, root))
            return std::format("{} is part of a prefab instance. Unpack the instance, or select entities outside "
                               "prefab instances.",
                               EntityDisplayName(world, root));
    }
    return {};
}

// Creates a prefab of each root at its file, as one undo step, and selects the one root. When one root
// fails, the prefabs made before it are undone, so the files and the scene are as before.
bool CreatePrefabsFromDialog(const CreatePrefabTarget& target, const std::vector<std::filesystem::path>& prefabFiles,
                             bool replace, std::string& outError)
{
    if (prefabFiles.size() != target.Roots.size())
        return false;
    std::vector<std::unique_ptr<CreatePrefabCommand>> commands;
    for (std::size_t index = 0; index < target.Roots.size(); ++index)
    {
        auto command = CreatePrefabCommand::Create(*target.World, target.Notifications, target.Roots[index],
                                                   prefabFiles[index], replace, outError);
        if (!command)
            break;
        commands.push_back(std::move(command));
    }
    const bool created = commands.size() == target.Roots.size();
    if (!created)
    {
        std::for_each(commands.rbegin(), commands.rend(), [](const auto& command) { command->Undo(); });
        if (!commands.empty())
            outError += " No prefab was created.";
    }
    const bool several = target.Roots.size() > 1;
    if (created && target.Undo)
    {
        if (several)
            target.Undo->BeginCompound(std::format("Create {} Prefabs", target.Roots.size()));
        for (auto& command : commands)
            target.Undo->CommitAlreadyApplied(std::move(command));
        if (several)
            target.Undo->EndCompound();
    }
    if (target.NotifyWorldStructureChanged)
        target.NotifyWorldStructureChanged();
    if (created && !several && target.SelectEntity)
        target.SelectEntity(target.Roots.front());
    return created;
}

// The words the Inspector shows for the field of `reference`.
std::string ReferenceFieldWords(const Scene::OutgoingReference& reference)
{
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(reference.Component))
    {
        if (field.Name == reference.Field)
            return FieldDisplayName(field);
    }
    return IdentifierToWords(reference.Field);
}

// The dialog's line for `reference` with the Inspector's names: "Sky's Sun Light points at Sun".
std::string DescribeOutgoingReference(ECS::World& world, const Scene::OutgoingReference& reference)
{
    return std::format("{}'s {} points at {}", EntityDisplayName(world, reference.Entity),
                       ReferenceFieldWords(reference), EntityDisplayName(world, reference.Target));
}

// What the dialog opens with, and the target it acts on.
struct CreatePrefabDialogRequest
{
    CreatePrefabDialog::Request Dialog;
    CreatePrefabTarget Target;
};

// The root of the window that holds `host`: a modal overlay covers its parent only, and a panel's top
// element is not the window root. A host in no window gives the top element above it.
UIElement& FindDialogRoot(UIElement& host)
{
    UIManager* window = host.GetOwnerManager();
    if (UIElement* root = window ? window->GetRootElement() : nullptr)
        return *root;
    UIElement* top = &host;
    while (top->GetParent())
        top = top->GetParent();
    return *top;
}

// Opens the dialog under `root` for `request`, and attaches the dialog on the first open.
void ShowCreatePrefabDialog(UIElement& root, CreatePrefabDialogRequest request)
{
    auto* dialog = dynamic_cast<CreatePrefabDialog*>(root.FindById(HashStringId(CreatePrefabDialog::kElementId)));
    if (!dialog)
    {
        auto owned = std::make_unique<CreatePrefabDialog>();
        dialog = owned.get();
        root.AddChild(std::move(owned));
    }
    ECS::World* world = request.Target.World;
    dialog->Open(std::move(request.Dialog),
                 [target = std::move(request.Target)](const std::vector<std::filesystem::path>& files, bool replace,
                                                      std::string& outError)
                 { return CreatePrefabsFromDialog(target, files, replace, outError); },
                 [world](const std::filesystem::path& file)
                 { return CreatePrefabCommand::DescribeReplaceRefusal(*world, file); });
}

// Opens the dialog for `target` with the folder set to `folder`, a name per root from the root's name,
// and the references that leave the roots by name.
void OpenCreatePrefabDialog(UIElement& host, CreatePrefabTarget target, const std::filesystem::path& folder)
{
    CreatePrefabDialogRequest request{{}, std::move(target)};
    ECS::World& world = *request.Target.World;
    for (const ECS::EntityHandle root : request.Target.Roots)
    {
        request.Dialog.Names.push_back(PrefabFileStem(world, root));
        request.Dialog.EntityCount += Scene::CountPrefabEntities(world, root);
        for (const Scene::OutgoingReference& reference : Scene::CollectOutgoingReferences(world, root))
            request.Dialog.OutgoingReferences.push_back(DescribeOutgoingReference(world, reference));
    }
    request.Dialog.Folder = folder;
    request.Dialog.AssetsRoot = EngineCore::GetInstance().GetResolvedAssetRoot();
    UIElement& root = FindDialogRoot(host);
    if (!UIElement::IsInEventDispatch())
    {
        ShowCreatePrefabDialog(root, std::move(request));
        return;
    }
    // A menu command and a drop run inside an event dispatch, and an attach there invalidates the
    // dispatch's iterators. The dialog opens at the next safe point.
    const bool posted = root.PostSafeAction([root = &root, request = std::move(request)]() mutable
                                            { ShowCreatePrefabDialog(*root, std::move(request)); });
    if (!posted)
        Logger::Log::Error("Create Prefab: the dialog cannot open because its host is in no window.");
}

// The folder the dialog opens with: the one the Assets panel lists, or the asset root when no panel
// lists a folder.
std::filesystem::path DefaultPrefabFolder(const EditorContext* editor)
{
    std::filesystem::path folder;
    if (editor && editor->AssetsPanelFolder)
        folder = editor->AssetsPanelFolder();
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec))
        return EngineCore::GetInstance().GetResolvedAssetRoot();
    return folder;
}

// Opens the dialog for the roots of the context's entities; no entity, or a root that cannot become a
// prefab, shows a notice with the fix instead.
void OpenCreatePrefabDialog(const HierarchyCommandContext& context)
{
    if (!context.World || !context.DialogHost)
        return;
    std::vector<ECS::EntityHandle> roots;
    if (context.HasTargetEntity)
        roots = FindTopRoots(*context.World, context.TargetEntities.empty() ? std::vector{context.TargetEntity}
                                                                            : context.TargetEntities);
    if (std::string refusal = DescribeCreateRefusal(*context.World, roots); !refusal.empty())
    {
        ShowSceneViewNotice(std::move(refusal));
        return;
    }
    OpenCreatePrefabDialog(*context.DialogHost,
                           {context.World, roots, context.ChangeNotifications, context.Undo, context.SelectEntity,
                            context.NotifyWorldStructureChanged},
                           DefaultPrefabFolder(context.EditorCtx));
}

// The roots of a Hierarchy drag, each of which becomes a prefab when it is dropped on an Assets folder.
std::vector<ECS::EntityHandle> DraggedRoots(const AssetsFolderDrop& drop)
{
    ECS::World* world = drop.EditorCtx ? drop.EditorCtx->World : nullptr;
    return world ? FindTopRoots(*world, DecodeHierarchyEntities(drop.Request)) : std::vector<ECS::EntityHandle>{};
}

// A Hierarchy drag dropped on an Assets folder makes a prefab of each dragged root there.
UI::Interaction::DropFeedback CanDropEntityOnFolder(const AssetsFolderDrop& drop)
{
    if (!drop.Request.payload.TryGet<HierarchyEntityDragPayload>())
        return {false, "Drag entities from the Hierarchy to create prefabs."};
    const std::vector<ECS::EntityHandle> roots = DraggedRoots(drop);
    if (roots.empty())
        return {false, "The entities are not in the open scene."};
    std::string refusal = DescribeCreateRefusal(*drop.EditorCtx->World, roots);
    return {refusal.empty(), std::move(refusal)};
}

// The drop makes a prefab of each root at once in the drop folder, named for the root, with a number
// when the name is taken, and returns the new files; the menu's dialog is the path that asks for a
// name. A notice states what happens to the fields that name an entity outside the prefabs.
std::vector<std::filesystem::path> CreatePrefabFromFolderDrop(const AssetsFolderDrop& drop)
{
    if (!CanDropEntityOnFolder(drop).Allowed)
        return {};
    const EditorContext& editor = *drop.EditorCtx;
    const CreatePrefabTarget target{editor.World, DraggedRoots(drop), editor.ChangeNotifications, editor.UndoRedo,
                                    {}, editor.OnSceneDirty};
    std::vector<std::filesystem::path> files;
    std::size_t outgoing = 0;
    for (const ECS::EntityHandle root : target.Roots)
    {
        files.push_back(MakeNumberedFilePath(drop.Folder, PrefabFileStem(*target.World, root) + ".prefab", files));
        outgoing += Scene::CollectOutgoingReferences(*target.World, root).size();
    }
    std::string error = std::format("Every name for a prefab in {} is taken. Drop on another folder.",
                                    drop.Folder.filename().string());
    const bool named = std::find(files.begin(), files.end(), std::filesystem::path()) == files.end();
    if (!named || !CreatePrefabsFromDialog(target, files, false, error))
    {
        ShowSceneViewNotice(std::move(error));
        return {};
    }
    if (outgoing > 0)
    {
        const std::string created = files.size() == 1 ? files.front().filename().string()
                                                      : std::format("{} prefabs", files.size());
        ShowSceneViewNotice(std::format("Created {}. {}", created, CreatePrefabDialog::DescribeOutgoingReferences(outgoing)));
    }
    return files;
}

// Selects the prefab file of the target's instance in the Assets panel.
void SelectPrefabAsset(const HierarchyCommandContext& context)
{
    if (!context.HasTargetEntity || !context.World || !context.PingAsset)
        return;
    if (const std::filesystem::path path = FindPrefabAssetPath(*context.World, context.TargetEntity); !path.empty())
        context.PingAsset(path);
}

// Unpacks the target instance to `depth` as one undo step; a refusal shows its notice.
void UnpackPrefab(const HierarchyCommandContext& context, Scene::PrefabUnpackDepth depth)
{
    if (!context.HasTargetEntity || !context.World)
        return;
    std::string error;
    auto command =
        UnpackPrefabCommand::Create(*context.World, context.ChangeNotifications, context.TargetEntity, depth, error);
    if (!command)
    {
        ShowSceneViewNotice(std::move(error));
        return;
    }
    if (context.Undo)
        context.Undo->CommitAlreadyApplied(std::move(command));
    if (context.NotifyWorldStructureChanged)
        context.NotifyWorldStructureChanged();
}

// The Inspector's prefab bar on every entity of an instance. Each Inspector build describes the
// instance again, and the bar, the component headers and the field rows read that description.
void AddPrefabBar(const InspectorContext& context)
{
    if (!context.World || !context.Parent || !FindInstanceRoot(*context.World, context.Entity).IsValid())
        return;
    PrefabInstanceDescriptions::Get().Forget(*context.World, context.Entity);
    context.Parent->AddChild(std::make_unique<PrefabBar>(*context.World, context.Entity, context.ChangeNotifications,
                                                         context.PingAsset, context.RefreshOverrideMarks,
                                                         context.RequestInspectorRefresh));
}

// The reflected field of `typeId` that the scene key `key` writes: the field of that name, or the
// array field whose name the element's index follows. Empty when no reflected field matches.
std::string_view ReflectedFieldForKey(ECS::ComponentTypeId typeId, std::string_view key)
{
    std::string_view arrayField;
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(typeId))
    {
        if (EqualsIgnoreCase(key, field.Name))
            return field.Name;
        const bool indexFollows = key.size() > field.Name.size() && StartsWithIgnoreCase(key, field.Name) &&
                                  key[field.Name.size()] >= '0' && key[field.Name.size()] <= '9';
        if (indexFollows && field.Name.size() > arrayField.size())
            arrayField = field.Name;
    }
    return arrayField;
}

// The Inspector's answer for one component of an instance entity: the fields it overrides and whether
// the instance added it.
ComponentOverrides QueryPrefabComponentOverrides(ECS::World& world, ECS::EntityHandle entity,
                                                 ECS::ComponentTypeId typeId)
{
    ComponentOverrides answer;
    const Scene::PrefabInstanceEntity* described = PrefabInstanceDescriptions::Get().FindEntity(world, entity);
    if (!described)
        return answer;
    // The entity header asks for the Name: an entity the instance added reads "Added" there.
    if (described->AddedOnInstance && typeId == ECS::GetComponentTypeId<Components::Name>())
    {
        answer.Overridden = true;
        answer.Badge = "Added";
        answer.BadgeTooltip = "Added on this instance. The prefab does not have this entity.";
        return answer;
    }
    const std::string_view token = Scene::SceneSchemaRegistry::SchemaTokenForComponentType(typeId);
    if (token.empty())
        return answer;
    for (const Scene::PrefabOverride& entry : described->Overrides)
    {
        if (entry.Component != token)
            continue;
        if (entry.Kind == Scene::PrefabOverrideKind::AddedComponent)
        {
            answer.Overridden = true;
            answer.Badge = "Added";
            answer.BadgeTooltip = "Added on this instance. The prefab does not have this component.";
        }
        else if (entry.Kind == Scene::PrefabOverrideKind::Field)
        {
            answer.Overridden = true;
            const std::string_view field = ReflectedFieldForKey(typeId, entry.Field);
            if (!field.empty() && !answer.IsFieldOverridden(field))
                answer.Fields.emplace_back(field);
        }
    }
    return answer;
}

// The muted headers, after an instance entity's components, of the components it removed.
void AddRemovedComponentHeaders(const InspectorContext& context)
{
    if (!context.World || !context.Parent)
        return;
    const Scene::PrefabInstanceEntity* described = PrefabInstanceDescriptions::Get().FindEntity(*context.World, context.Entity);
    if (!described)
        return;
    std::vector<std::string> removed;
    for (const Scene::PrefabOverride& entry : described->Overrides)
        if (entry.Kind == Scene::PrefabOverrideKind::RemovedComponent)
            removed.push_back(entry.Component);
    if (!removed.empty())
        context.Parent->AddChild(std::make_unique<PrefabRemovedComponents>(removed));
}

// The menu entry that selects the prefab file, named for the prefab as the Inspector's bar names it.
std::string SelectPrefabLabel(ECS::World& world, ECS::EntityHandle entity)
{
    const std::string name = PrefabNameOf(world, FindInstanceRoot(world, entity));
    return "Prefab/Select " + (name.empty() ? std::string("Prefab") : name);
}

// The Hierarchy's prefab entries, shown on every entity row and enabled where their command applies.
class PrefabHierarchyMenu final : public IEditorPlugin
{
  public:
    const Plugins::PluginDescriptor& GetDescriptor() const override
    {
        static const Plugins::PluginDescriptor descriptor{"prefabs", "Prefabs", "1", true};
        return descriptor;
    }

    void BuildHierarchyContextMenu(ContextMenuBuilder& builder, const HierarchyContextMenuContext& context) override
    {
        if (!context.HasTargetEntity || !context.World)
            return;
        ECS::World& world = *context.World;
        const ECS::EntityHandle entity = context.TargetEntity;
        const std::string createRefusal = DescribeCreateRefusal(world, {entity});
        const uint32_t selectFlags = FlagsFor(!FindPrefabAssetPath(world, entity).empty());
        const uint32_t unpackFlags = FlagsFor(CanUnpack(world, entity));
        builder.AddItem("Prefab", 0, MenuItemFlag_None, kPrefabMenuPriority, EditorIcons::kCube);
        builder.AddItem("Prefab/Create Prefab\xe2\x80\xa6", kCmdCreatePrefab, FlagsFor(createRefusal.empty()),
                        kPrefabMenuPriority, {}, CatalogShortcutLabel(kShortcutGroup, kCreatePrefabShortcut),
                        createRefusal);
        builder.AddItem(SelectPrefabLabel(world, entity), kCmdSelectPrefabAsset, selectFlags, kPrefabMenuPriority);
        builder.AddItem("Prefab/Unpack", kCmdUnpackPrefab, unpackFlags, kPrefabMenuPriority);
        builder.AddItem("Prefab/Unpack Completely", kCmdUnpackPrefabCompletely, unpackFlags, kPrefabMenuPriority);
    }

    bool HandleHierarchyCommand(uint32_t commandId, const HierarchyCommandContext& context) override
    {
        switch (commandId)
        {
        case kCmdCreatePrefab:
            OpenCreatePrefabDialog(context);
            return true;
        case kCmdSelectPrefabAsset:
            SelectPrefabAsset(context);
            return true;
        case kCmdUnpackPrefab:
            UnpackPrefab(context, Scene::PrefabUnpackDepth::Outer);
            return true;
        case kCmdUnpackPrefabCompletely:
            UnpackPrefab(context, Scene::PrefabUnpackDepth::Completely);
            return true;
        default:
            return false;
        }
    }

    uint32_t HierarchyCommandForKey(int key, int mods) const override
    {
        return MatchesCatalogShortcut(kShortcutGroup, kCreatePrefabShortcut, key, mods) ? kCmdCreatePrefab : 0u;
    }

  private:
    static uint32_t FlagsFor(bool enabled) { return enabled ? MenuItemFlag_None : MenuItemFlag_Disabled; }
};

PrefabHierarchyMenu& Menu()
{
    static PrefabHierarchyMenu s_Menu;
    return s_Menu;
}

} // namespace

void RegisterPrefabEditor(EditorChangeNotifications& notifications, IThumbnailProvider* thumbnails)
{
    PrefabInstanceDescriptions::Get().Track(notifications);
    EditorComponentTraits traits;
    traits.HierarchyRowClass = "hierarchy-entity-prefab";
    traits.IsPickClimbRoot = true;
    traits.DecorateHierarchyRow = &DecoratePrefabRow;
    traits.HideInInspector = true;
    EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::PrefabInstance>(),
                                                  std::move(traits));
    InspectorRegistry::Get().RegisterEntityBanner("prefab-bar", EntityBannerPlacement::BeforeComponents, &AddPrefabBar);
    InspectorRegistry::Get().RegisterEntityBanner("prefab-removed-components", EntityBannerPlacement::AfterComponents,
                                                  &AddRemovedComponentHeaders);
    InspectorRegistry::Get().SetComponentOverridesAnswerer(&QueryPrefabComponentOverrides);
    EditorPluginRegistry::Get().RegisterPlugin(Menu());
    AssetsFolderDropRegistry::Get().Register(UI::Interaction::GetPayloadTypeId<HierarchyEntityDragPayload>(),
                                             {&CanDropEntityOnFolder, &CreatePrefabFromFolderDrop});
    if (thumbnails)
        thumbnails->RegisterHandler(AssetType::Prefab, std::make_unique<PrefabThumbnailHandler>());
}

} // namespace GameEngine::Editor
