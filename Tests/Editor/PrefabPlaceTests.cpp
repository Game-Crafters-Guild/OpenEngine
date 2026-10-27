// Prefabs in the editor (design 4.2, 4.3 and 2.9): the drop that places an instance and its undo
// step, Create Prefab and its undo step, the Create Prefab dialog, the drop on an Assets folder, and
// the Hierarchy rows of an instance.

#include <gtest/gtest.h>

#include "Assets/AssetsFolderDropRegistry.h"
#include "Editor/DragDropPayloads.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EntityDuplicate.h"
#include "Editor/Hierarchy/HierarchyEntityIcon.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "EditorChangeNotifications.h"
#include "EditorContext.h"
#include "Prefabs/CreatePrefabCommand.h"
#include "Prefabs/CreatePrefabDialog.h"
#include "Prefabs/PrefabBar.h"
#include "Prefabs/PrefabHierarchyRows.h"
#include "Prefabs/PrefabInstanceDescriptions.h"
#include "Prefabs/PrefabRemovedComponents.h"
#include "Prefabs/PrefabService.h"
#include "Prefabs/PrefabThumbnailHandler.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/ControlLayoutLoader.h"
#include "RecordingContextMenu.h"
#include "SceneView/SceneViewDropPlacement.h"
#include "SceneViewNoticeRecorder.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/SceneViewDropUndoCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/PrefabAuthoring.h"
#include "Scene/SceneIO.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/InspectorSection.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Interaction/FocusQueries.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr std::string_view kCratePrefab = "[prefab version=1]\n"
                                          "[entity id=\"root\"]\n"
                                          "[entity id=\"lid\" parent=\"root\"]\n"
                                          "Name.value = \"Lid\"\n";

// The editor's change channel the prefab rows of every test listen to.
Editor::EditorChangeNotifications& Notifications()
{
    static Editor::EditorChangeNotifications s_Notifications;
    return s_Notifications;
}

std::size_t CountInstances(ECS::World& world)
{
    std::size_t count = 0;
    world.Query<ECS::Read<Components::PrefabInstance>>().Each(
        [&count](ECS::EntityHandle, const Components::PrefabInstance&) { ++count; });
    return count;
}

// The suite's engine is started once and left running for every suite (EngineShutdownEnvironment
// stops it); each test writes its prefabs into a folder of its own under the asset root.
class PrefabPlaceTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        m_Folder = engine.GetAssetManager().GetAssetRoot() /
                   ("ge_prefab_place_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Folder, ec);
    }

    // Writes `text` as the asset `file` and registers it, as a scan of the Assets folder would.
    std::filesystem::path WriteAsset(const std::string& file, std::string_view text)
    {
        const auto path = (m_Folder / file).lexically_normal();
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        EXPECT_TRUE(EngineCore::GetInstance().GetAssetManager().GetRegistry().RegisterAssetByPath(path).IsOk());
        return path;
    }

    ECS::EntityHandle PlaceAt(ECS::World& world, const std::filesystem::path& file, float x, float y, float z)
    {
        Components::Transform at{};
        at.SetIdentity();
        at.Translate(x, y, z);
        return Editor::PrefabService(EngineCore::GetInstance().GetAssetManager()).Place(world, file, at, {});
    }

    std::filesystem::path m_Folder;
};

} // namespace

// A drop places the instance live: the root at the drop point, marked with its prefab, with the
// prefab's child under it, and last in the Hierarchy order.
TEST_F(PrefabPlaceTests, ADroppedPrefabIsALiveInstanceWithItsTree)
{
    const std::filesystem::path crate = WriteAsset("Props/Crate.prefab", kCratePrefab);
    ECS::World world;

    const ECS::EntityHandle root = PlaceAt(world, crate, 1.0f, 2.0f, 3.0f);

    ASSERT_TRUE(root.IsValid());
    const auto* marker = world.GetComponent<Components::PrefabInstance>(root);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->Source, EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(crate));
    const auto* transform = world.GetComponent<Components::Transform>(root);
    ASSERT_NE(transform, nullptr);
    EXPECT_FLOAT_EQ(transform->GetPosition().x, 1.0f);
    EXPECT_FLOAT_EQ(transform->GetPosition().z, 3.0f);
    EXPECT_NE(world.GetComponent<Components::HierarchyOrder>(root), nullptr);

    std::vector<std::string> children;
    world.Query<ECS::Read<Components::Parent>, ECS::Read<Components::Name>>().Each(
        [&children, root](ECS::EntityHandle, const Components::Parent& parent, const Components::Name& name)
        {
            if (parent.Entity == root)
                children.emplace_back(name.View());
        });
    EXPECT_EQ(children, std::vector<std::string>{"Lid"});
}

// A file the asset database does not know places nothing.
TEST_F(PrefabPlaceTests, AnUnknownPrefabFilePlacesNothing)
{
    ECS::World world;
    const ECS::EntityHandle root =
        PlaceAt(world, m_Folder / "Missing.prefab", 0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(root.IsValid());
    EXPECT_EQ(world.GetEntityCount(), 0u);
}

// The drop is one undo step named for the prefab: undo removes the root and its child, redo brings
// the instance back with its tree.
TEST_F(PrefabPlaceTests, TheDropIsOneUndoStepNamedForThePrefab)
{
    const std::filesystem::path crate = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, crate, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());

    Editor::UndoRedoService undo;
    const std::vector<ECS::EntityHandle> roots{root};
    undo.CommitAlreadyApplied(std::make_unique<Editor::SceneViewDropUndoCommand>(
        Editor::SceneViewDropUndoCommand::NameFor(world, roots), &world, nullptr, roots,
        std::vector<UI::Interaction::ItemId>{}, 0, std::vector<UI::Interaction::ItemId>{}, 0, nullptr));
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.PeekUndoName(), "Place Crate");

    undo.Undo();
    EXPECT_EQ(CountInstances(world), 0u);
    EXPECT_EQ(world.GetEntityCount(), 0u);

    undo.Redo();
    EXPECT_EQ(CountInstances(world), 1u);
    EXPECT_EQ(world.GetEntityCount(), 2u);
}

// A drop of several roots names their count.
TEST(PrefabDropStepNameTests, SeveralRootsNameTheirCount)
{
    ECS::World world;
    const std::vector<ECS::EntityHandle> roots{world.CreateEntity(), world.CreateEntity(), world.CreateEntity()};
    EXPECT_EQ(Editor::SceneViewDropUndoCommand::NameFor(world, roots), "Place 3 Entities");
}

// A navigation grid dropped on a scene is refused with the way to use it; a file of a kind no drop
// places names its kind; a drop that places one of its files, or assigns a texture or a material
// dropped alone, is not refused.
TEST(PrefabDropRefusalTests, AFileTheSceneCannotPlaceIsRefusedWithANotice)
{
    using Paths = std::vector<std::filesystem::path>;
    EXPECT_EQ(Editor::DescribeRefusedDrop(Paths{"Navigation/Town.navgrid"}),
              "Navigation Grid files cannot be placed in a scene. Add a Navigation Surface component and assign the "
              "file to its Authored Cells.");
    EXPECT_EQ(Editor::DescribeRefusedDrop(Paths{"Levels/Town.scene"}), "Scene files cannot be placed in a scene.");
    EXPECT_EQ(Editor::DescribeRefusedDrop(Paths{"Models/House.blend"}), ".blend files cannot be placed in a scene.");
    EXPECT_EQ(Editor::DescribeRefusedDrop(Paths{"Textures/A.png", "Textures/B.png"}),
              "Texture files cannot be placed in a scene.");

    EXPECT_EQ(Editor::DescribeRefusedDrop(Paths{"Props/Crate.prefab", "Navigation/Town.navgrid"}), "");
    EXPECT_EQ(Editor::DescribeRefusedDrop(Paths{"Models/House.FBX"}), "");
    EXPECT_EQ(Editor::DescribeRefusedDrop(Paths{"Textures/A.png"}), "");
    EXPECT_EQ(Editor::DescribeRefusedDrop(Paths{"Materials/Pine.mat"}), "");
}

// Creates an entity named `name` under `parent` (none when invalid) with the scene identity `tag`.
static ECS::EntityHandle CreateTagged(ECS::World& world, std::string_view name, std::string_view tag,
                                      ECS::EntityHandle parent)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::Name component{};
    name.copy(component.value, sizeof(component.value) - 1);
    world.AddComponentImmediate(entity, component);
    Components::SceneEntityTag identity{};
    tag.copy(identity.value, sizeof(identity.value) - 1);
    world.AddComponentImmediate(entity, identity);
    if (parent.IsValid())
        world.AddComponentImmediate(entity, Components::Parent{parent});
    return entity;
}

static std::string ReadFileText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Create Prefab is one undo step: the undo deletes the file and gives the entities back their
// identities, the redo writes the file again under the same GUID and makes the instance again.
TEST_F(PrefabPlaceTests, CreatePrefabIsOneUndoStepThatDeletesAndRewritesTheFile)
{
    ECS::World world;
    const ECS::EntityHandle crate = CreateTagged(world, "Crate", "crate", {});
    const ECS::EntityHandle lid = CreateTagged(world, "Lid", "lid", crate);
    const std::filesystem::path file = m_Folder / "Crate.prefab";
    std::filesystem::create_directories(m_Folder);

    std::string error;
    auto command = Editor::CreatePrefabCommand::Create(world, nullptr, crate, file, false, error);
    ASSERT_NE(command, nullptr) << error;
    const GUID prefab = command->GetPrefab();
    Editor::UndoRedoService undo;
    undo.CommitAlreadyApplied(std::move(command));
    EXPECT_STREQ(undo.PeekUndoName(), "Create Prefab Crate");
    ASSERT_TRUE(std::filesystem::exists(file));
    EXPECT_EQ(world.GetComponent<Components::SceneEntityTag>(lid)->View(), "crate.lid");

    undo.Undo();
    EXPECT_FALSE(std::filesystem::exists(file));
    EXPECT_EQ(CountInstances(world), 0u);
    EXPECT_EQ(world.GetComponent<Components::SceneEntityTag>(lid)->View(), "lid");

    undo.Redo();
    ASSERT_TRUE(std::filesystem::exists(file));
    const auto* marker = world.GetComponent<Components::PrefabInstance>(crate);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->Source, prefab);
    EXPECT_EQ(world.GetComponent<Components::SceneEntityTag>(lid)->View(), "crate.lid");
}

// A Create on a name in use is refused without the consent to replace. A Replace keeps the replaced
// file's bytes, and its undo writes them back.
TEST_F(PrefabPlaceTests, CreatePrefabReplaceRefusesWithoutConsentAndItsUndoRestoresTheOldFile)
{
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle barrel = CreateTagged(world, "Barrel", "barrel", {});

    std::string error;
    EXPECT_EQ(Editor::CreatePrefabCommand::Create(world, nullptr, barrel, file, false, error), nullptr);
    EXPECT_NE(error.find("already exists"), std::string::npos) << error;
    EXPECT_EQ(ReadFileText(file), kCratePrefab);

    auto command = Editor::CreatePrefabCommand::Create(world, nullptr, barrel, file, true, error);
    ASSERT_NE(command, nullptr) << error;
    EXPECT_NE(ReadFileText(file), kCratePrefab);
    Editor::UndoRedoService undo;
    undo.CommitAlreadyApplied(std::move(command));
    undo.Undo();
    EXPECT_EQ(ReadFileText(file), kCratePrefab);
    undo.Redo();
    EXPECT_NE(ReadFileText(file).find("[prefab version=1]"), std::string::npos);
    EXPECT_NE(world.GetComponent<Components::PrefabInstance>(barrel), nullptr);
}

// The undo of a Replace rebuilds the prefab from the file it writes back: a drop after the undo
// places the old content.
TEST_F(PrefabPlaceTests, AnUndoOfAReplaceRestoresThePrefabThatADropPlaces)
{
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle barrel = CreateTagged(world, "Barrel", "barrel", {});
    CreateTagged(world, "Hoop", "hoop", barrel);
    CreateTagged(world, "Ring", "ring", barrel);

    std::string error;
    auto command = Editor::CreatePrefabCommand::Create(world, nullptr, barrel, file, true, error);
    ASSERT_NE(command, nullptr) << error;
    Editor::UndoRedoService undo;
    undo.CommitAlreadyApplied(std::move(command));
    undo.Undo();
    ASSERT_EQ(ReadFileText(file), kCratePrefab);

    const ECS::EntityHandle placed = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(placed.IsValid());
    EXPECT_EQ(Scene::CountPrefabEntities(world, placed), 2u) << "the drop places the root and Lid";
}

// A Replace of a prefab that instances in the scene use is refused with their count, and the file
// stays as it is.
TEST_F(PrefabPlaceTests, AReplaceOfAPrefabThatInstancesUseIsRefusedWithTheCount)
{
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    ASSERT_TRUE(PlaceAt(world, file, 0.0f, 0.0f, 0.0f).IsValid());
    ASSERT_TRUE(PlaceAt(world, file, 2.0f, 0.0f, 0.0f).IsValid());
    const ECS::EntityHandle barrel = CreateTagged(world, "Barrel", "barrel", {});

    std::string error;
    EXPECT_EQ(Editor::CreatePrefabCommand::Create(world, nullptr, barrel, file, true, error), nullptr);
    EXPECT_EQ(error, "Crate.prefab cannot be replaced while 2 instances in this scene use it. Choose another name.");
    EXPECT_EQ(ReadFileText(file), kCratePrefab);
    EXPECT_EQ(world.GetComponent<Components::PrefabInstance>(barrel), nullptr);
}

// The undo of a Create after a save of a scene that uses the prefab reverts the instance and keeps
// the file, so the saved scene still opens; the redo makes the instance again.
TEST_F(PrefabPlaceTests, AnUndoOfCreateAfterASavedSceneUsesThePrefabKeepsTheFile)
{
    ECS::World world;
    const ECS::EntityHandle crate = CreateTagged(world, "Crate", "crate", {});
    CreateTagged(world, "Lid", "lid", crate);
    const std::filesystem::path file = m_Folder / "Crate.prefab";
    std::filesystem::create_directories(m_Folder);
    std::string error;
    auto command = Editor::CreatePrefabCommand::Create(world, nullptr, crate, file, false, error);
    ASSERT_NE(command, nullptr) << error;
    Editor::UndoRedoService undo;
    undo.CommitAlreadyApplied(std::move(command));

    EngineCore& engine = EngineCore::GetInstance();
    Scene::AssetManagerSceneResolver resolver(engine.GetAssetManager(), engine.GetAssetManager().GetAssetRoot(),
                                              Scene::AssetManagerSceneResolver::UnknownFile::Register);
    Scene::SaveOptions options;
    options.assetResolver = &resolver;
    const std::filesystem::path scene = m_Folder / "Yard.scene";
    ASSERT_TRUE(Scene::SaveSceneToFile(world, scene, options)) << Scene::GetLastSceneIOError().message;
    ASSERT_TRUE(engine.GetAssetManager().GetRegistry().RegisterAssetByPath(scene).IsOk());

    undo.Undo();
    EXPECT_EQ(world.GetComponent<Components::PrefabInstance>(crate), nullptr);
    EXPECT_TRUE(std::filesystem::exists(file)) << "the saved scene uses the file";
    undo.Redo();
    ASSERT_NE(world.GetComponent<Components::PrefabInstance>(crate), nullptr);
    EXPECT_TRUE(std::filesystem::exists(file));
}

namespace
{

// One call of the dialog's create handler.
struct CreateRequest
{
    std::filesystem::path File;
    bool Replace = false;
};

// The dialog request for `names` in `folder`, under the asset root.
Editor::CreatePrefabDialog::Request DialogRequest(std::vector<std::string> names, std::size_t entityCount,
                                                  std::vector<std::string> outgoing, const std::filesystem::path& folder)
{
    return {std::move(names), entityCount, std::move(outgoing), folder,
            EngineCore::GetInstance().GetAssetManager().GetAssetRoot()};
}

template <typename Control>
Control& DialogPart(Editor::CreatePrefabDialog& dialog, StringId id)
{
    auto* part = dynamic_cast<Control*>(dialog.FindById(id));
    EXPECT_NE(part, nullptr);
    return *part;
}

} // namespace

// The dialog shows the name, the folder under Assets and the entity count; Create hands the file
// to the handler and closes the dialog.
TEST_F(PrefabPlaceTests, TheCreatePrefabDialogShowsTheFieldsAndCreatesInTheFolder)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::CreatePrefabDialog dialog;
    std::vector<CreateRequest> requests;
    dialog.Open(DialogRequest({"Crate"}, 3, {}, m_Folder),
        [&requests](const std::vector<std::filesystem::path>& files, bool replace, std::string&)
        {
            requests.push_back({files.front(), replace});
            return true;
        },
        {});
    ASSERT_TRUE(dialog.IsOpen());
    EXPECT_EQ(DialogPart<TextField>(dialog, "create-prefab-name"_sid).GetValue(), "Crate");
    EXPECT_EQ(DialogPart<Label>(dialog, "create-prefab-folder"_sid).GetText(),
              "Assets/" + m_Folder.filename().string());
    EXPECT_EQ(DialogPart<Label>(dialog, "create-prefab-count"_sid).GetText(), "The prefab holds 3 entities.");
    EXPECT_FALSE(DialogPart<Label>(dialog, "create-prefab-references"_sid).HasClass("visible"));

    DialogPart<Button>(dialog, "create-prefab-create"_sid).TriggerClick();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].File, (m_Folder / "Crate.prefab").lexically_normal());
    EXPECT_FALSE(requests[0].Replace);
    EXPECT_FALSE(dialog.IsOpen());

    dialog.Open(DialogRequest({"Trigger"}, 1,
                              {"Trigger's Target points at Gate", "Trigger's Source points at Door"},
                              m_Folder),
                [](const std::vector<std::filesystem::path>&, bool, std::string&) { return true; }, {});
    Label& references = DialogPart<Label>(dialog, "create-prefab-references"_sid);
    EXPECT_TRUE(references.HasClass("visible"));
    EXPECT_EQ(references.GetText(), "These fields point at entities outside the prefab. This instance keeps them; the "
                                    "prefab file leaves them empty.\nTrigger's Target points at Gate\n"
                                    "Trigger's Source points at Door");
    const std::vector<std::string> oneLine{"Trigger's Target points at Gate"};
    EXPECT_EQ(Editor::CreatePrefabDialog::ListOutgoingReferences(oneLine, 1),
              "This field points at an entity outside the prefab. This instance keeps it; the prefab file leaves it "
              "empty.\nTrigger's Target points at Gate");
}

// A file with the name in the folder: the first Create says so and the button reads Replace; the
// second Create replaces. A refused create keeps the dialog open with the cause; Cancel closes it
// and calls nothing.
TEST_F(PrefabPlaceTests, TheCreatePrefabDialogAsksBeforeAReplaceAndCancelChangesNothing)
{
    UIRegistration::RegisterBuiltInControls();
    const std::filesystem::path existing = WriteAsset("Crate.prefab", kCratePrefab);
    Editor::CreatePrefabDialog dialog;
    std::vector<CreateRequest> requests;
    bool refuse = true;
    const auto handler = [&requests, &refuse](const std::vector<std::filesystem::path>& files, bool replace, std::string& outError)
    {
        requests.push_back({files.front(), replace});
        if (refuse)
            outError = "Cannot replace the file.";
        return !refuse;
    };
    dialog.Open(DialogRequest({"Crate"}, 1, {}, m_Folder), handler, {});
    auto& create = DialogPart<Button>(dialog, "create-prefab-create"_sid);
    auto& message = DialogPart<Label>(dialog, "create-prefab-message"_sid);

    create.TriggerClick();
    EXPECT_TRUE(requests.empty()) << "the first Create only asks";
    EXPECT_EQ(create.GetText(), "Replace");
    EXPECT_TRUE(message.HasClass("visible"));
    EXPECT_NE(message.GetText().find("Crate.prefab"), std::string::npos) << message.GetText();

    create.TriggerClick();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_TRUE(requests[0].Replace);
    EXPECT_TRUE(dialog.IsOpen()) << "a refused create keeps the dialog open";
    EXPECT_EQ(message.GetText(), "Cannot replace the file.");

    requests.clear();
    DialogPart<Button>(dialog, "create-prefab-cancel"_sid).TriggerClick();
    EXPECT_FALSE(dialog.IsOpen());
    EXPECT_TRUE(requests.empty());
    EXPECT_EQ(ReadFileText(existing), kCratePrefab) << "Cancel leaves the file as it was";
}

// A name in use by a file that instances in the scene use: the first Create states why the file
// cannot be replaced and disables Create, so neither a click nor Enter offers a Replace; a new name
// enables Create again.
TEST_F(PrefabPlaceTests, TheCreatePrefabDialogRefusesAReplaceThatInstancesBlockBeforeItIsOffered)
{
    UIRegistration::RegisterBuiltInControls();
    // First local: destroyed last, after the UIManager released its buffers.
    SharedDeviceReleaseRetirement releaseRetirement;
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "Device init failed";
    UIManager ui(device);
    auto root = std::make_unique<UIElement>();
    auto ownedDialog = std::make_unique<Editor::CreatePrefabDialog>();
    Editor::CreatePrefabDialog& dialog = *ownedDialog;
    root->AddChild(std::move(ownedDialog));
    ui.SetRoot(std::move(root));
    const std::filesystem::path existing = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    ASSERT_TRUE(PlaceAt(world, existing, 0.0f, 0.0f, 0.0f).IsValid());
    std::vector<CreateRequest> requests;
    dialog.Open(DialogRequest({"Crate"}, 1, {}, m_Folder),
        [&requests](const std::vector<std::filesystem::path>& files, bool replace, std::string&)
        {
            requests.push_back({files.front(), replace});
            return true;
        },
        [&world](const std::filesystem::path& file)
        { return Editor::CreatePrefabCommand::DescribeReplaceRefusal(world, file); });
    ui.DrainDeferredActionsOnce();
    auto& create = DialogPart<Button>(dialog, "create-prefab-create"_sid);
    auto& message = DialogPart<Label>(dialog, "create-prefab-message"_sid);

    create.TriggerClick();
    EXPECT_EQ(message.GetText(), "Crate.prefab cannot be replaced while 1 instance in this scene uses it. Choose "
                                 "another name.");
    EXPECT_TRUE(message.HasClass("visible"));
    EXPECT_EQ(create.GetText(), "Create") << "no Replace is offered";
    EXPECT_FALSE(create.IsEnabled());
    create.TriggerClick();
    ui.OnKey(Input::kKeyCode_Enter, Input::kKeyActionPress, 0);
    EXPECT_TRUE(requests.empty()) << "neither a click nor Enter creates while Create is disabled";

    ui.OnKey(Input::kKeyCode_End, Input::kKeyActionPress, 0);
    ui.OnChar('2');
    EXPECT_TRUE(create.IsEnabled()) << "a new name enables Create";
    EXPECT_FALSE(message.HasClass("visible"));
    create.TriggerClick();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].File, (m_Folder / "Crate2.prefab").lexically_normal());
    EXPECT_EQ(ReadFileText(existing), kCratePrefab);
}

// Keys through a real UIManager: Enter in the name field presses Create with the default name. Escape
// after an edit reverts the edit and creates nothing; the next Escape closes the dialog.
TEST_F(PrefabPlaceTests, EnterInTheCreatePrefabNameCreatesAndEscapeCancels)
{
    UIRegistration::RegisterBuiltInControls();
    // First local: destroyed last, after the UIManager released its buffers.
    SharedDeviceReleaseRetirement releaseRetirement;
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "Device init failed";
    UIManager ui(device);
    auto root = std::make_unique<UIElement>();
    auto ownedDialog = std::make_unique<Editor::CreatePrefabDialog>();
    Editor::CreatePrefabDialog* dialog = ownedDialog.get();
    root->AddChild(std::move(ownedDialog));
    ui.SetRoot(std::move(root));
    std::vector<CreateRequest> requests;
    const auto handler = [&requests](const std::vector<std::filesystem::path>& files, bool replace, std::string&)
    {
        requests.push_back({files.front(), replace});
        return true;
    };
    auto& name = DialogPart<TextField>(*dialog, "create-prefab-name"_sid);

    dialog->Open(DialogRequest({"Crate"}, 1, {}, m_Folder), handler, {});
    ui.DrainDeferredActionsOnce();
    ASSERT_EQ(ui.GetFocusedElementId(), name.GetId()) << "the dialog focuses the name field";
    ui.OnKey(Input::kKeyCode_Enter, Input::kKeyActionPress, 0);
    ASSERT_EQ(requests.size(), 1u) << "Enter presses Create";
    EXPECT_EQ(requests[0].File, (m_Folder / "Crate.prefab").lexically_normal());
    EXPECT_FALSE(dialog->IsOpen());

    requests.clear();
    dialog->Open(DialogRequest({"Crate"}, 1, {}, m_Folder), handler, {});
    ui.DrainDeferredActionsOnce();
    ui.OnChar('X');
    ui.OnKey(Input::kKeyCode_Escape, Input::kKeyActionPress, 0);
    EXPECT_TRUE(dialog->IsOpen()) << "the first Escape reverts the edit";
    EXPECT_EQ(name.GetValue(), "Crate");
    ui.OnKey(Input::kKeyCode_Escape, Input::kKeyActionPress, 0);
    EXPECT_FALSE(dialog->IsOpen()) << "the next Escape cancels";
    EXPECT_TRUE(requests.empty()) << "Escape creates nothing";
}

namespace
{

// The tree id of a Hierarchy row: the entity index in the low bits, its version above them.
std::uint64_t HierarchyTreeId(ECS::EntityHandle entity)
{
    return (static_cast<std::uint32_t>(entity.index) & ECS::kEntityIndexMask) |
           ((static_cast<std::uint32_t>(entity.version) & ECS::kEntityVersionMask) << ECS::kEntityIndexBits);
}

UI::Interaction::DropRequest HierarchyDrag(std::vector<std::uint64_t> treeIds)
{
    UI::Interaction::DropRequest request;
    request.payload = UI::Interaction::DragPayload::Create(Editor::HierarchyEntityDragPayload{std::move(treeIds), {}});
    return request;
}

} // namespace

// A Hierarchy entity dropped on an Assets folder becomes a prefab there at once, named for the entity,
// in one undo step; a name in use gets " (1)", " (2)" and on, as a paste does. A drag of an instance is
// refused.
TEST_F(PrefabPlaceTests, AHierarchyEntityDroppedOnAnAssetsFolderCreatesThePrefabThere)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    ECS::World world;
    const ECS::EntityHandle sphere = CreateTagged(world, "Sphere", "sphere", {});
    CreateTagged(world, "Lid", "lid", sphere);
    const ECS::EntityHandle second = CreateTagged(world, "Sphere", "sphere_2", {});
    std::filesystem::create_directories(m_Folder);
    Editor::UndoRedoService undo;
    EditorContext editor;
    editor.World = &world;
    editor.UndoRedo = &undo;
    const auto& registry = Editor::AssetsFolderDropRegistry::Get();

    const UI::Interaction::DropRequest request = HierarchyDrag({HierarchyTreeId(sphere)});
    const Editor::AssetsFolderDrop drop{request, m_Folder, &editor};
    ASSERT_TRUE(registry.Accepts(request.payload.TypeId));
    const UI::Interaction::DropFeedback feedback = registry.CanDrop(drop);
    ASSERT_TRUE(feedback.Allowed) << feedback.Reason;
    registry.PerformDrop(drop);

    const std::filesystem::path file = m_Folder / "Sphere.prefab";
    ASSERT_TRUE(std::filesystem::exists(file)) << "the drop writes the prefab in its folder, with no dialog";
    ASSERT_NE(world.GetComponent<Components::PrefabInstance>(sphere), nullptr);
    EXPECT_STREQ(undo.PeekUndoName(), "Create Prefab Sphere");
    EXPECT_FALSE(registry.CanDrop(drop).Allowed) << "an instance is part of a prefab already";

    const UI::Interaction::DropRequest secondRequest = HierarchyDrag({HierarchyTreeId(second)});
    registry.PerformDrop({secondRequest, m_Folder, &editor});
    EXPECT_TRUE(std::filesystem::exists(m_Folder / "Sphere (1).prefab")) << "a name in use gets a number";
    ASSERT_NE(world.GetComponent<Components::PrefabInstance>(second), nullptr);

    undo.Undo();
    undo.Undo();
    EXPECT_FALSE(std::filesystem::exists(file));
    EXPECT_FALSE(std::filesystem::exists(m_Folder / "Sphere (1).prefab"));
    EXPECT_EQ(CountInstances(world), 0u);
    EXPECT_FALSE(undo.CanUndo());
}

// A drop of an entity with a field that names an entity outside it: the notice states what the
// instance and the file keep, and the drop returns the new file for the Assets panel to select.
TEST_F(PrefabPlaceTests, ADropCreateWithAnOutsideReferenceSaysWhatItKeepsAndReturnsTheNewFile)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    ECS::World world;
    const ECS::EntityHandle sun = CreateTagged(world, "Sun", "sun", {});
    const ECS::EntityHandle rig = CreateTagged(world, "Sky Rig", "sky_rig", {});
    Components::SkyEnvironment sky{};
    sky.SunLight = sun;
    world.AddComponentImmediate(rig, sky);
    ASSERT_EQ(Scene::CollectOutgoingReferences(world, rig).size(), 1u);
    std::filesystem::create_directories(m_Folder);
    Editor::UndoRedoService undo;
    EditorContext editor;
    editor.World = &world;
    editor.UndoRedo = &undo;
    Editor::TestStubs::LastSceneViewNotice().clear();

    const UI::Interaction::DropRequest request = HierarchyDrag({HierarchyTreeId(rig)});
    const std::vector<std::filesystem::path> written =
        Editor::AssetsFolderDropRegistry::Get().PerformDrop({request, m_Folder, &editor});

    const std::filesystem::path file = m_Folder / "Sky Rig.prefab";
    ASSERT_TRUE(std::filesystem::exists(file));
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNotice(),
              "Created Sky Rig.prefab. 1 field points at an entity outside the prefab. This instance keeps the "
              "reference; the prefab file leaves the field empty.");
    EXPECT_EQ(written, std::vector<std::filesystem::path>{file});

    Editor::TestStubs::LastSceneViewNotice().clear();
    const ECS::EntityHandle crate = CreateTagged(world, "Crate", "crate", {});
    EXPECT_EQ(Editor::AssetsFolderDropRegistry::Get().PerformDrop(
                  {HierarchyDrag({HierarchyTreeId(crate)}), m_Folder, &editor}),
              std::vector<std::filesystem::path>{m_Folder / "Crate.prefab"});
    EXPECT_TRUE(Editor::TestStubs::LastSceneViewNotice().empty()) << "no outside reference, no notice";
    EXPECT_TRUE(Editor::AssetsFolderDropRegistry::Get()
                    .PerformDrop({HierarchyDrag({HierarchyTreeId(crate)}), m_Folder, &editor})
                    .empty())
        << "a refused drop writes no file";
}

// The menu command runs inside an event dispatch, where an attach to the window root invalidates the
// dispatch's iterators. The dialog attaches and opens at the next safe point.
namespace
{

// The plugin that RegisterPrefabEditor registers, or null.
Editor::IEditorPlugin* FindPrefabPlugin()
{
    for (Editor::IEditorPlugin* plugin : Editor::EditorPluginRegistry::Get().GetPlugins())
    {
        if (std::string_view(plugin->GetDescriptor().Id) == "prefabs")
            return plugin;
    }
    return nullptr;
}

// The command of the Create Prefab entry that the plugin's menu shows for `entity`.
uint32_t CreatePrefabCommandOf(Editor::IEditorPlugin& prefabs, ECS::World& world, ECS::EntityHandle entity)
{
    ContextMenuBuilder builder;
    prefabs.BuildHierarchyContextMenu(builder, {&world, entity, true, nullptr});
    EditorTesting::RecordingContextMenu menu;
    builder.Build(&menu);
    return menu.CommandOf("Create Prefab\xe2\x80\xa6");
}

} // namespace

// The menu's Create Prefab opens the dialog at the next safe point, with the name of the entity and
// each field that points outside it in the Inspector's words.
TEST_F(PrefabPlaceTests, CreatePrefabFromTheMenuOpensTheDialogAtTheNextSafePoint)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    // First local: destroyed last, after the UIManager released its buffers.
    SharedDeviceReleaseRetirement releaseRetirement;
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "Device init failed";
    ECS::World world;
    const ECS::EntityHandle crate = CreateTagged(world, "Crate", "crate", {});
    Components::SkyEnvironment sky{};
    sky.SunLight = CreateTagged(world, "Sun", "sun", {});
    world.AddComponentImmediate(crate, sky);
    EditorContext editor;
    editor.World = &world;
    UIManager ui(device);
    auto ownedRoot = std::make_unique<UIElement>();
    ownedRoot->AddChild(std::make_unique<UIElement>());
    UIElement& view = *ownedRoot->GetChildren().front();
    ui.SetRoot(std::move(ownedRoot));
    UIElement& root = *ui.GetRootElement();

    Editor::IEditorPlugin* prefabs = FindPrefabPlugin();
    ASSERT_NE(prefabs, nullptr);
    const uint32_t command = CreatePrefabCommandOf(*prefabs, world, crate);
    ASSERT_NE(command, 0u);
    Editor::HierarchyCommandContext context;
    context.World = &world;
    context.TargetEntity = crate;
    context.HasTargetEntity = true;
    context.EditorCtx = &editor;
    context.DialogHost = &view;

    UIElement::SetInEventDispatch(true);
    EXPECT_TRUE(prefabs->HandleHierarchyCommand(command, context));
    const bool attachedInDispatch = root.FindById(HashStringId(Editor::CreatePrefabDialog::kElementId)) != nullptr;
    UIElement::SetInEventDispatch(false);
    EXPECT_FALSE(attachedInDispatch) << "the command does not attach the dialog inside the dispatch";

    ui.DrainDeferredActionsOnce();
    auto* dialog =
        dynamic_cast<Editor::CreatePrefabDialog*>(root.FindById(HashStringId(Editor::CreatePrefabDialog::kElementId)));
    ASSERT_NE(dialog, nullptr) << "the dialog attaches at the next safe point";
    EXPECT_TRUE(dialog->IsOpen());
    EXPECT_EQ(DialogPart<TextField>(*dialog, "create-prefab-name"_sid).GetValue(), "Crate");
    const std::string references = DialogPart<Label>(*dialog, "create-prefab-references"_sid).GetText();
    EXPECT_NE(references.find("Crate's Sun Light points at Sun"), std::string::npos) << references;
}

// Create Prefab with nothing selected, as Ctrl+Shift+P runs it on an empty selection, shows the fix and
// opens no dialog.
TEST_F(PrefabPlaceTests, CreatePrefabWithNothingSelectedShowsTheFix)
{
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    ECS::World world;
    Editor::IEditorPlugin* prefabs = FindPrefabPlugin();
    ASSERT_NE(prefabs, nullptr);
    const uint32_t command = CreatePrefabCommandOf(*prefabs, world, CreateTagged(world, "Crate", "crate", {}));
    ASSERT_NE(command, 0u);
    UIElement host;
    Editor::HierarchyCommandContext context;
    context.World = &world;
    context.DialogHost = &host;

    Editor::TestStubs::LastSceneViewNotice().clear();
    EXPECT_TRUE(prefabs->HandleHierarchyCommand(command, context));
    EXPECT_EQ(Editor::TestStubs::LastSceneViewNotice(), "Select an entity in the scene to create a prefab.");
    EXPECT_EQ(host.FindById(HashStringId(Editor::CreatePrefabDialog::kElementId)), nullptr);
}

// A Create of several roots that fails at one root undoes the prefabs made before it: no file, no
// instance and no undo step remain, and the dialog stays open and says so.
TEST_F(PrefabPlaceTests, ACreateOfSeveralRootsThatFailsAtOneRootLeavesNoPrefab)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    // First local: destroyed last, after the UIManager released its buffers.
    SharedDeviceReleaseRetirement releaseRetirement;
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "Device init failed";
    ECS::World world;
    const ECS::EntityHandle crate = CreateTagged(world, "Crate", "crate", {});
    const ECS::EntityHandle barrel = CreateTagged(world, "Barrel", "barrel", {});
    std::filesystem::create_directories(m_Folder);
    Editor::UndoRedoService undo;
    EditorContext editor;
    editor.World = &world;
    editor.AssetsPanelFolder = [this]() { return m_Folder; };
    UIManager ui(device);
    ui.SetRoot(std::make_unique<UIElement>());
    UIElement& root = *ui.GetRootElement();
    Editor::IEditorPlugin* prefabs = FindPrefabPlugin();
    ASSERT_NE(prefabs, nullptr);
    Editor::HierarchyCommandContext context;
    context.World = &world;
    context.TargetEntity = crate;
    context.TargetEntities = {crate, barrel};
    context.HasTargetEntity = true;
    context.EditorCtx = &editor;
    context.Undo = &undo;
    context.DialogHost = &root;
    ASSERT_TRUE(prefabs->HandleHierarchyCommand(CreatePrefabCommandOf(*prefabs, world, crate), context));
    auto* dialog =
        dynamic_cast<Editor::CreatePrefabDialog*>(root.FindById(HashStringId(Editor::CreatePrefabDialog::kElementId)));
    ASSERT_NE(dialog, nullptr);

    WriteAsset("Barrel.prefab", kCratePrefab);
    DialogPart<Button>(*dialog, "create-prefab-create"_sid).TriggerClick();
    EXPECT_FALSE(std::filesystem::exists(m_Folder / "Crate.prefab")) << "the first prefab is undone";
    EXPECT_EQ(CountInstances(world), 0u);
    EXPECT_EQ(undo.GetUndoCount(), 0u);
    EXPECT_TRUE(dialog->IsOpen());
    EXPECT_NE(DialogPart<Label>(*dialog, "create-prefab-message"_sid).GetText().find("No prefab was created."),
              std::string::npos);
}

namespace
{

// The prefab marker a decorated Hierarchy row shows, or empty when it shows none.
std::string PrefabMarkerState(const UIElement& row)
{
    if (row.HasClass("hierarchy-prefab-overridden"))
        return "overridden";
    if (row.HasClass("hierarchy-prefab-added"))
        return "added";
    return {};
}

void DecorateRow(ECS::World& world, ECS::EntityHandle entity, UIElement& row)
{
    Editor::HierarchyRowParts parts{row, nullptr};
    Editor::ApplyHierarchyRowDecorations(world, entity, parts);
}

// The first entity whose parent is `parent`, or an invalid handle when it has no child.
ECS::EntityHandle FindChild(ECS::World& world, ECS::EntityHandle parent)
{
    ECS::EntityHandle child;
    world.Query<ECS::Read<Components::Parent>>().Each(
        [&child, parent](ECS::EntityHandle entity, const Components::Parent& link)
        {
            if (link.Entity == parent && !child.IsValid())
                child = entity;
        });
    return child;
}

} // namespace

// The Hierarchy rows of an instance: an entity that differs from the prefab shows the dot, an entity
// added on the instance shows the plus and no member tone, and an unchanged entity shows neither.
TEST_F(PrefabPlaceTests, TheRowsOfAnInstanceMarkAnOverrideAndAnAddedEntity)
{
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path crate = WriteAsset("Props/Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, crate, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    ECS::EntityHandle lid;
    world.Query<ECS::Read<Components::Parent>>().Each(
        [&lid, root](ECS::EntityHandle entity, const Components::Parent& parent)
        {
            if (parent.Entity == root)
                lid = entity;
        });
    ASSERT_TRUE(lid.IsValid());
    UIElement rootRow;
    UIElement lidRow;
    DecorateRow(world, root, rootRow);
    DecorateRow(world, lid, lidRow);
    EXPECT_EQ(PrefabMarkerState(rootRow), "");
    EXPECT_EQ(PrefabMarkerState(lidRow), "") << "a placed instance has no overrides";
    EXPECT_TRUE(lidRow.HasClass("hierarchy-prefab-member"));

    Components::Name* lidName = world.GetComponentForWrite<Components::Name>(lid);
    ASSERT_NE(lidName, nullptr);
    *lidName = Components::Name{};
    std::string_view("Grip").copy(lidName->value, sizeof(lidName->value) - 1);
    const ECS::EntityHandle sign = world.CreateEntity();
    world.AddComponentImmediate(sign, Components::Parent{lid});
    const ECS::EntityHandle signPost = world.CreateEntity();
    world.AddComponentImmediate(signPost, Components::Parent{sign});
    Notifications().NotifyComponentCommit<Components::Name>(&world, lid);
    Editor::NotifyWorldStructure(&Notifications(), &world);
    UIElement signRow;
    UIElement signPostRow;
    DecorateRow(world, root, rootRow);
    DecorateRow(world, lid, lidRow);
    DecorateRow(world, sign, signRow);
    DecorateRow(world, signPost, signPostRow);

    EXPECT_EQ(PrefabMarkerState(rootRow), "");
    EXPECT_EQ(PrefabMarkerState(lidRow), "overridden");
    EXPECT_EQ(lidRow.GetTooltip(), "This entity differs from its prefab.");
    EXPECT_TRUE(lidRow.HasClass("hierarchy-prefab-member"));
    EXPECT_EQ(PrefabMarkerState(signRow), "added");
    EXPECT_FALSE(signRow.HasClass("hierarchy-prefab-member")) << "an added entity is not part of the prefab";
    EXPECT_EQ(PrefabMarkerState(signPostRow), "") << "the plus marks the added entity only";
    EXPECT_FALSE(signPostRow.HasClass("hierarchy-prefab-member"));
}

// The root of a nested instance is also a member of the outer instance: its row has the root class
// for its glyph and the member class for the badge.
TEST_F(PrefabPlaceTests, ANestedInstanceRootRowIsAlsoAMemberOfTheOuterInstance)
{
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    WriteAsset("Props/Wheel.prefab", "[prefab version=1]\n"
                                     "[entity id=\"root\"]\n"
                                     "[entity id=\"hub\" parent=\"root\"]\n");
    const std::string wheelPath = m_Folder.filename().generic_string() + "/Props/Wheel.prefab";
    const std::string cartText = "[prefab version=1]\n"
                                 "[resource id=\"wheel\" path=\"" +
                                 wheelPath +
                                 "\"]\n"
                                 "[entity id=\"root\"]\n"
                                 "[prefab id=\"wheel\" source=\"wheel\" parent=\"root\"]\n";
    const std::filesystem::path cart = WriteAsset("Props/Cart.prefab", cartText);
    ECS::World world;
    const ECS::EntityHandle cartRoot = PlaceAt(world, cart, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(cartRoot.IsValid());
    const ECS::EntityHandle wheel = FindChild(world, cartRoot);
    ASSERT_TRUE(wheel.IsValid());
    ASSERT_TRUE(world.HasComponent<Components::PrefabInstance>(wheel));
    const ECS::EntityHandle hub = FindChild(world, wheel);
    ASSERT_TRUE(hub.IsValid());
    UIElement cartRow;
    UIElement wheelRow;
    UIElement hubRow;
    DecorateRow(world, cartRoot, cartRow);
    DecorateRow(world, wheel, wheelRow);
    DecorateRow(world, hub, hubRow);

    EXPECT_TRUE(cartRow.HasClass("hierarchy-prefab-root"));
    EXPECT_FALSE(cartRow.HasClass("hierarchy-prefab-member")) << "the outermost root is in no other instance";
    EXPECT_TRUE(wheelRow.HasClass("hierarchy-prefab-root"));
    EXPECT_TRUE(wheelRow.HasClass("hierarchy-prefab-member"));
    EXPECT_TRUE(hubRow.HasClass("hierarchy-prefab-member"));
    EXPECT_FALSE(hubRow.HasClass("hierarchy-prefab-root"));
}

// A root whose prefab the project does not have shows red, and its tooltip names the prefab and the fix.
TEST_F(PrefabPlaceTests, AMissingPrefabRowNamesThePrefabAndTheFixInItsTooltip)
{
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    ECS::World world;
    const GUID source = GUID::Generate();
    const ECS::EntityHandle root = world.CreateEntity();
    world.AddComponentImmediate(root, Components::PrefabInstance{source});
    UIElement row;
    row.AddChild(std::make_unique<UIElement>());
    UIElement& title = *row.GetChildren().front();
    Editor::HierarchyRowParts parts{row, &title};

    Editor::ApplyHierarchyRowDecorations(world, root, parts);

    EXPECT_TRUE(row.HasClass("hierarchy-prefab-missing"));
    EXPECT_EQ(title.GetTooltip(),
              "Prefab file missing: " + source.ToString() +
                  ". Restore it or delete this entity. While it is missing, the scene cannot be opened again.");
}

// An instance keeps its description while the editor reports no change to it: a structural change
// elsewhere in the world describes nothing again, and a committed change to a member does.
TEST_F(PrefabPlaceTests, TheRowsDescribeAnInstanceAgainOnlyWhenTheEditorReportsAChangeToIt)
{
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path crate = WriteAsset("Props/Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, crate, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    ECS::EntityHandle lid;
    world.Query<ECS::Read<Components::Parent>>().Each(
        [&lid, root](ECS::EntityHandle entity, const Components::Parent& parent)
        {
            if (parent.Entity == root)
                lid = entity;
        });
    ASSERT_TRUE(lid.IsValid());
    UIElement lidRow;
    DecorateRow(world, lid, lidRow);
    ASSERT_EQ(PrefabMarkerState(lidRow), "");

    // The rename is the same write a script makes: the editor does not report it.
    Components::Name* lidName = world.GetComponentForWrite<Components::Name>(lid);
    ASSERT_NE(lidName, nullptr);
    std::string_view("Grip").copy(lidName->value, sizeof(lidName->value) - 1);
    CreateTagged(world, "Gate", "gate", {});
    DecorateRow(world, lid, lidRow);
    EXPECT_EQ(PrefabMarkerState(lidRow), "") << "a refresh with no reported change describes nothing again";

    Notifications().NotifyComponentCommit<Components::Name>(&world, lid);
    DecorateRow(world, lid, lidRow);
    EXPECT_EQ(PrefabMarkerState(lidRow), "overridden");
}

namespace
{

// The command of the Hierarchy menu's entry titled `title` on `entity`; 0 when there is none.
uint32_t HierarchyMenuCommand(ECS::World& world, ECS::EntityHandle entity, const std::string& title)
{
    ContextMenuBuilder builder;
    Editor::EditorPluginRegistry::Get().BuildHierarchyContextMenu(builder, {&world, entity, true, nullptr});
    EditorTesting::RecordingContextMenu menu;
    builder.Build(&menu);
    return menu.CommandOf(title);
}

// The first child of `parent`, or the invalid handle.
ECS::EntityHandle FirstChildOf(ECS::World& world, ECS::EntityHandle parent)
{
    const std::vector<ECS::EntityHandle> children = Components::ChildrenOf(world, parent);
    return children.empty() ? ECS::EntityHandle{} : children.front();
}

// The context of a Hierarchy command on `entity` in `world` with `undo`.
Editor::HierarchyCommandContext CommandContext(ECS::World& world, ECS::EntityHandle entity,
                                               Editor::UndoRedoService& undo)
{
    Editor::HierarchyCommandContext context;
    context.World = &world;
    context.TargetEntity = entity;
    context.HasTargetEntity = true;
    context.TargetEntities = {entity};
    context.Undo = &undo;
    return context;
}

std::string TagOf(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity);
    return tag ? std::string(tag->View()) : std::string();
}

} // namespace

// Design 4.8: Unpack from the Hierarchy menu makes the instance plain entities in one undo step named
// for it; the undo gives the instance back and the redo unpacks again.
TEST_F(PrefabPlaceTests, UnpackFromTheMenuIsOneUndoStepAndTheRedoUnpacksAgain)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle crate = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(crate.IsValid());
    Components::Name name{};
    std::string_view("Crate").copy(name.value, sizeof(name.value) - 1);
    world.AddComponentImmediate(crate, name);
    const std::string lidTag = TagOf(world, FirstChildOf(world, crate));
    Editor::UndoRedoService undo;

    const uint32_t unpack = HierarchyMenuCommand(world, crate, "Unpack");
    ASSERT_NE(unpack, 0u);
    EXPECT_TRUE(Editor::EditorPluginRegistry::Get().HandleHierarchyCommand(unpack, CommandContext(world, crate, undo)));
    EXPECT_EQ(CountInstances(world), 0u);
    EXPECT_STREQ(undo.PeekUndoName(), "Unpack Crate");
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_NE(TagOf(world, FirstChildOf(world, crate)), lidTag);

    undo.Undo();
    EXPECT_EQ(CountInstances(world), 1u);
    EXPECT_EQ(TagOf(world, FirstChildOf(world, crate)), lidTag);
    undo.Redo();
    EXPECT_EQ(CountInstances(world), 0u);

    const uint32_t completely = HierarchyMenuCommand(world, crate, "Unpack Completely");
    ASSERT_NE(completely, 0u);
    undo.Undo();
    EXPECT_TRUE(Editor::EditorPluginRegistry::Get().HandleHierarchyCommand(completely, CommandContext(world, crate, undo)));
    EXPECT_STREQ(undo.PeekUndoName(), "Unpack Completely Crate");
    EXPECT_EQ(CountInstances(world), 0u);
}

// Design 4.3: Select <Prefab> on an entity of an instance selects the prefab file in the Assets panel.
TEST_F(PrefabPlaceTests, SelectAssetShowsThePrefabFileOfTheInstanceInTheAssetsPanel)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle crate = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    const ECS::EntityHandle lid = FirstChildOf(world, crate);
    ASSERT_TRUE(lid.IsValid());
    Editor::UndoRedoService undo;

    const uint32_t select = HierarchyMenuCommand(world, lid, "Select Crate");
    ASSERT_NE(select, 0u);
    std::vector<std::filesystem::path> pinged;
    Editor::HierarchyCommandContext context = CommandContext(world, lid, undo);
    context.PingAsset = [&pinged](const std::filesystem::path& path) { pinged.push_back(path); };
    EXPECT_TRUE(Editor::EditorPluginRegistry::Get().HandleHierarchyCommand(select, context));
    ASSERT_EQ(pinged.size(), 1u);
    EXPECT_TRUE(std::filesystem::equivalent(pinged.front(), file)) << pinged.front().string();
    EXPECT_FALSE(undo.CanUndo()) << "a selection in the Assets panel is no scene edit";
}

// Design 4.3: Duplicate copies an instance with its overrides; the copy is an instance of the same
// prefab with identities of its own, and the duplicate is one undo step.
TEST_F(PrefabPlaceTests, ADuplicateOfAnInstanceIsAnInstanceWithItsOverridesInOneUndoStep)
{
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle crate = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    const ECS::EntityHandle lid = FirstChildOf(world, crate);
    Components::Name renamed{};
    std::string_view("Top").copy(renamed.value, sizeof(renamed.value) - 1);
    world.AddComponentImmediate(lid, renamed);

    const Editor::EntityDuplicateResult duplicate = Editor::DuplicateEntitySubtreeRoots(world, {crate});
    ASSERT_EQ(duplicate.NewRoots.size(), 1u);
    const ECS::EntityHandle copy = duplicate.NewRoots.front();
    const ECS::EntityHandle copyLid = FirstChildOf(world, copy);
    ASSERT_TRUE(copyLid.IsValid());
    EXPECT_EQ(CountInstances(world), 2u);
    EXPECT_NE(TagOf(world, copy), TagOf(world, crate));
    EXPECT_EQ(TagOf(world, copyLid), TagOf(world, copy) + ".lid");
    std::vector<Scene::PrefabInstanceEntity> described;
    ASSERT_TRUE(Scene::DescribePrefabInstance(world, copy, described));
    ASSERT_EQ(described.size(), 2u);
    EXPECT_TRUE(described[1].HasOverrides()) << "the copy keeps the renamed lid";
    EXPECT_FALSE(described[1].AddedOnInstance);

    Editor::UndoRedoService undo;
    std::vector<ECS::EntityHandle> clones{copy, copyLid};
    undo.CommitAlreadyApplied(std::make_unique<Editor::DuplicateEntitiesCommand>("Duplicate", &world, nullptr,
                                                                                 clones, nullptr, nullptr));
    undo.Undo();
    EXPECT_EQ(CountInstances(world), 1u);
    undo.Redo();
    EXPECT_EQ(CountInstances(world), 2u);
    EXPECT_EQ(TagOf(world, copyLid), TagOf(world, copy) + ".lid") << "the redo gives the copy its identities";
}

// Design 4.2.4: a drag of several roots onto an Assets folder makes one prefab per root, named for each,
// with a number on a name in use, in one undo step; a child dragged with its parent is part of the parent's.
TEST_F(PrefabPlaceTests, SeveralRootsDroppedOnAFolderBecomeOnePrefabEachInOneUndoStep)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    ECS::World world;
    const ECS::EntityHandle crate = CreateTagged(world, "Crate", "crate", {});
    const ECS::EntityHandle lid = CreateTagged(world, "Lid", "lid", crate);
    const ECS::EntityHandle barrel = CreateTagged(world, "Barrel", "barrel", {});
    const ECS::EntityHandle second = CreateTagged(world, "Crate", "crate_2", {});
    std::filesystem::create_directories(m_Folder);
    Editor::UndoRedoService undo;
    EditorContext editor;
    editor.World = &world;
    editor.UndoRedo = &undo;
    const auto& registry = Editor::AssetsFolderDropRegistry::Get();

    const UI::Interaction::DropRequest request = HierarchyDrag(
        {HierarchyTreeId(crate), HierarchyTreeId(lid), HierarchyTreeId(barrel), HierarchyTreeId(second)});
    const Editor::AssetsFolderDrop drop{request, m_Folder, &editor};
    const UI::Interaction::DropFeedback feedback = registry.CanDrop(drop);
    ASSERT_TRUE(feedback.Allowed) << feedback.Reason;
    registry.PerformDrop(drop);

    EXPECT_TRUE(std::filesystem::exists(m_Folder / "Crate.prefab"));
    EXPECT_TRUE(std::filesystem::exists(m_Folder / "Barrel.prefab"));
    EXPECT_TRUE(std::filesystem::exists(m_Folder / "Crate (1).prefab")) << "a name in use gets a number";
    EXPECT_EQ(CountInstances(world), 3u) << "the lid is part of the crate's prefab";
    EXPECT_EQ(world.GetComponent<Components::PrefabInstance>(lid), nullptr);
    EXPECT_STREQ(undo.PeekUndoName(), "Create 3 Prefabs");
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_FALSE(registry.CanDrop(drop).Allowed) << "an instance is part of a prefab already";

    undo.Undo();
    EXPECT_EQ(CountInstances(world), 0u);
    EXPECT_FALSE(std::filesystem::exists(m_Folder / "Crate.prefab"));
    EXPECT_FALSE(std::filesystem::exists(m_Folder / "Crate (1).prefab"));
}

// Design 4.2.4: the dialog for several roots lists the file each root makes, numbered when a name is in
// use, and Create hands every file to the handler.
TEST_F(PrefabPlaceTests, TheCreatePrefabDialogForSeveralRootsListsTheirFilesAndCreatesEach)
{
    UIRegistration::RegisterBuiltInControls();
    WriteAsset("Barrel.prefab", kCratePrefab);
    Editor::CreatePrefabDialog dialog;
    std::vector<std::filesystem::path> created;
    dialog.Open(DialogRequest({"Crate", "Crate", "Barrel"}, 5, {}, m_Folder),
                [&created](const std::vector<std::filesystem::path>& files, bool replace, std::string&)
                {
                    EXPECT_FALSE(replace);
                    created = files;
                    return true;
                },
                {});
    EXPECT_TRUE(DialogPart<UIElement>(dialog, "create-prefab-name-row"_sid).HasClass("hidden"));
    EXPECT_FALSE(DialogPart<UIElement>(dialog, "create-prefab-files-row"_sid).HasClass("hidden"));
    EXPECT_EQ(DialogPart<Label>(dialog, "create-prefab-files"_sid).GetText(),
              "Crate.prefab, Crate (1).prefab, Barrel (1).prefab");
    EXPECT_EQ(DialogPart<Label>(dialog, "create-prefab-count"_sid).GetText(),
              "Each root becomes one prefab: 3 prefabs, 5 entities.");

    DialogPart<Button>(dialog, "create-prefab-create"_sid).TriggerClick();
    ASSERT_EQ(created.size(), 3u);
    EXPECT_EQ(created[1], (m_Folder / "Crate (1).prefab").lexically_normal());
    EXPECT_EQ(created[2], (m_Folder / "Barrel (1).prefab").lexically_normal());
    EXPECT_FALSE(dialog.IsOpen());
}

namespace
{

// Keeps the prefab handler a feature registers; serves no thumbnails.
class RecordingThumbnailProvider final : public IThumbnailProvider
{
  public:
    void SetAssetsRoot(const std::filesystem::path& /*root*/) override {}

    std::string GetOrRequest(const std::filesystem::path& /*assetPath*/, int /*desiredSize*/,
                             std::function<void(const std::string& relPath)> /*onReady*/,
                             bool /*staticModelListThumbnail*/) override
    {
        return {};
    }

    void RegisterHandler(AssetType type, std::unique_ptr<AssetThumbnailHandler> handler) override
    {
        if (type == AssetType::Prefab)
            PrefabHandler = std::move(handler);
    }

    std::unique_ptr<AssetThumbnailHandler> PrefabHandler;
};

} // namespace

// The Assets panel gets the tile of a .prefab file from the handler the prefab editor registers.
TEST(PrefabAssetTileTests, ThePrefabEditorRegistersThePrefabGlyphAsTheTile)
{
    RecordingThumbnailProvider thumbnails;
    Editor::RegisterPrefabEditor(Notifications(), &thumbnails);
    ASSERT_NE(thumbnails.PrefabHandler, nullptr);
    EXPECT_EQ(thumbnails.PrefabHandler->GetOrRequest("Props/Crate.prefab", 128, nullptr),
              Editor::PrefabThumbnailHandler::kGlyphPath);
}

namespace
{

// Gives `entity` the name `name`, with no change notice.
void SetName(ECS::World& world, ECS::EntityHandle entity, std::string_view name)
{
    Components::Name* component = world.GetComponentForWrite<Components::Name>(entity);
    if (!component)
    {
        world.AddComponentImmediate(entity, Components::Name{});
        component = world.GetComponentForWrite<Components::Name>(entity);
    }
    *component = Components::Name{};
    name.copy(component->value, sizeof(component->value) - 1);
}

// The text of the label `id` in the prefab bar that the Inspector's banners add for `entity`; a marker
// when they add no single bar.
std::string PrefabBarText(ECS::World& world, ECS::EntityHandle entity, std::string_view id)
{
    UIElement content;
    InspectorContext context;
    context.Parent = &content;
    context.World = &world;
    context.Entity = entity;
    context.ChangeNotifications = &Notifications();
    for (const InspectorFn& banner : InspectorRegistry::Get().GetEntityBanners(EntityBannerPlacement::BeforeComponents))
        banner(context);
    const auto& children = content.GetChildren();
    if (children.size() != 1 || !dynamic_cast<const Editor::PrefabBar*>(children.front().get()))
        return "(no single bar)";
    const auto* label = dynamic_cast<const Label*>(children.front()->FindById(HashStringId(id)));
    return label ? label->GetText() : std::string("(no label)");
}

} // namespace

// Design 4.4: the Inspector shows the prefab bar on every entity of an instance and on no other. The
// first line names the prefab and the instance; the second counts the instance's overrides.
TEST_F(PrefabPlaceTests, TheInspectorShowsThePrefabBarOnEveryEntityOfAnInstance)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    Components::Name* rootName = world.GetComponentForWrite<Components::Name>(root);
    ASSERT_NE(rootName, nullptr);
    *rootName = Components::Name{};
    std::string_view("Crate_01").copy(rootName->value, sizeof(rootName->value) - 1);
    const ECS::EntityHandle lid = FindChild(world, root);
    ASSERT_TRUE(lid.IsValid());
    const ECS::EntityHandle plain = world.CreateEntity();
    const ECS::EntityHandle missing = world.CreateEntity();
    world.AddComponentImmediate(missing, Components::PrefabInstance{GUID::Generate()});

    EXPECT_EQ(PrefabBarText(world, root, "prefab-bar-title"), "Instance of Crate");
    EXPECT_EQ(PrefabBarText(world, lid, "prefab-bar-title"), "Part of Crate in Crate_01");
    EXPECT_EQ(PrefabBarText(world, missing, "prefab-bar-title"), "Instance of a missing prefab");
    EXPECT_EQ(PrefabBarText(world, plain, "prefab-bar-title"), "(no single bar)");
    EXPECT_EQ(PrefabBarText(world, lid, "prefab-bar-count"), "No overrides") << "the root's name is placement";

    Components::Name* lidName = world.GetComponentForWrite<Components::Name>(lid);
    ASSERT_NE(lidName, nullptr);
    *lidName = Components::Name{};
    std::string_view("Top").copy(lidName->value, sizeof(lidName->value) - 1);
    EXPECT_EQ(PrefabBarText(world, root, "prefab-bar-count"), "1 override");
}

// The bar reads the world again when a change to it is committed, and not at each step of a drag.
TEST_F(PrefabPlaceTests, ThePrefabBarCountsAnOverrideWhenItsChangeIsCommitted)
{
    UIRegistration::RegisterBuiltInControls();
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    ASSERT_TRUE(lid.IsValid());
    Editor::PrefabBar bar(world, lid, &Notifications(), {}, {}, {});
    const auto* count = dynamic_cast<const Label*>(bar.FindById("prefab-bar-count"_sid));
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->GetText(), "No overrides");

    Components::Name* lidName = world.GetComponentForWrite<Components::Name>(lid);
    ASSERT_NE(lidName, nullptr);
    *lidName = Components::Name{};
    std::string_view("Top").copy(lidName->value, sizeof(lidName->value) - 1);
    Notifications().NotifyComponentChange<Components::Name>(&world, lid,
                                                            Editor::EditorChangeNotifications::ChangeKind::Preview);
    EXPECT_EQ(count->GetText(), "No overrides") << "a drag step reads nothing";
    Notifications().NotifyComponentCommit<Components::Name>(&world, lid);
    EXPECT_EQ(count->GetText(), "1 override on Top") << "a member counts its own overrides";
}

namespace
{

// A prefab whose child carries a reflection probe, so the default rows show its fields.
constexpr std::string_view kProbePrefab = "[prefab version=1]\n"
                                          "[entity id=\"root\"]\n"
                                          "[entity id=\"lid\" parent=\"root\"]\n"
                                          "Name.value = \"Lid\"\n"
                                          "ReflectionProbe.Intensity = 1\n";

// True when `element` holds the override edge as a child.
bool HasOverrideMark(const UIElement& element)
{
    for (const auto& child : element.GetChildren())
        if (child->HasClass("inspector-override-mark"))
            return true;
    return false;
}

// The default inspector's row whose label reads `labelText` under `parent`, or null.
const UIElement* FindFieldRow(const UIElement& parent, std::string_view labelText)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child->HasClass("inspector-row"))
            for (const auto& cell : child->GetChildren())
                for (const auto& part : cell->GetChildren())
                    if (const auto* label = dynamic_cast<const Label*>(part.get()); label && label->GetText() == labelText)
                        return child.get();
        if (const UIElement* found = FindFieldRow(*child, labelText))
            return found;
    }
    return nullptr;
}

} // namespace

// Design 4.4: an overridden field's row carries the mark, and the other rows of the component do not.
TEST_F(PrefabPlaceTests, TheDefaultRowsMarkTheFieldsAnInstanceOverrides)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    auto* probe = world.GetComponentForWrite<Components::ReflectionProbe>(lid);
    ASSERT_NE(probe, nullptr);
    probe->Intensity = 2.0f;
    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, lid);

    const ECS::ComponentTypeId probeType = ECS::GetComponentTypeId<Components::ReflectionProbe>();
    const ComponentOverrides overrides = InspectorRegistry::Get().QueryComponentOverrides(world, lid, probeType);
    EXPECT_TRUE(overrides.Overridden);
    EXPECT_TRUE(overrides.Badge.empty()) << "the prefab has the probe";
    EXPECT_EQ(overrides.Fields, std::vector<std::string>{"Intensity"});

    UIElement parent{"div"};
    InspectorContext context;
    context.Parent = &parent;
    context.World = &world;
    context.Entity = lid;
    ASSERT_TRUE(RenderDefaultComponentInspector(context, probeType));
    const UIElement* intensity = FindFieldRow(parent, "Intensity");
    const UIElement* boxProjection = FindFieldRow(parent, "Box Projection");
    ASSERT_NE(intensity, nullptr);
    ASSERT_NE(boxProjection, nullptr);
    EXPECT_TRUE(HasOverrideMark(*intensity));
    EXPECT_FALSE(HasOverrideMark(*boxProjection));
}

// The entity's name is a field row of the Name component: an overridden name gets the mark too.
TEST_F(PrefabPlaceTests, TheEntityHeaderMarksAnOverriddenName)
{
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    UIElement header;
    UIElement nameField;
    UpdateEntityNameOverrideMark(header, nameField, world, lid);
    EXPECT_FALSE(header.HasClass("inspector-header-container-overridden")) << "a placed instance has no overrides";

    SetName(world, lid, "Top");
    Notifications().NotifyComponentCommit<Components::Name>(&world, lid);
    UpdateEntityNameOverrideMark(header, nameField, world, lid);
    EXPECT_TRUE(header.HasClass("inspector-header-container-overridden"));
    EXPECT_TRUE(nameField.HasClass("inspector-header-overridden"));

    SetName(world, lid, "Lid");
    Notifications().NotifyComponentCommit<Components::Name>(&world, lid);
    UpdateEntityNameOverrideMark(header, nameField, world, lid);
    EXPECT_FALSE(header.HasClass("inspector-header-container-overridden")) << "the name is the prefab's again";
    EXPECT_FALSE(nameField.HasClass("inspector-header-overridden"));
}

// An entity that the instance added has the override edge and the "Added" badge on its header, as an
// added component has on its section header; a member of the prefab has neither.
TEST_F(PrefabPlaceTests, TheEntityHeaderOfAnAddedEntityReadsAdded)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    const ECS::EntityHandle sign = world.CreateEntity();
    world.AddComponentImmediate(sign, Components::Parent{root});
    Editor::NotifyWorldStructure(&Notifications(), &world);

    UIElement header;
    auto ownedField = std::make_unique<UIElement>();
    UIElement* nameField = ownedField.get();
    header.AddChild(std::move(ownedField));
    std::unique_ptr<UIElement> trailing =
        Editor::ControlLayoutLoader::Load("UI/inspectors/EntityHeader/EntityHeader.uxml", {});
    ASSERT_NE(trailing, nullptr);
    header.AddChild(std::move(trailing));
    const auto* badge = dynamic_cast<const Label*>(header.FindById("inspector-header-badge"_sid));
    ASSERT_NE(badge, nullptr) << "the header's layout declares the badge";
    EXPECT_TRUE(badge->HasClass("hidden")) << "the badge is hidden until an override shows it";

    UpdateEntityNameOverrideMark(header, *nameField, world, sign);
    UpdateEntityNameOverrideMark(header, *nameField, world, sign);
    EXPECT_TRUE(header.HasClass("inspector-header-container-overridden"));
    EXPECT_EQ(header.GetChildren().size(), 2u) << "the marks add no element to the header";
    EXPECT_FALSE(badge->HasClass("hidden"));
    EXPECT_EQ(badge->GetText(), "Added");
    EXPECT_FALSE(badge->GetTooltip().empty());
    EXPECT_TRUE(badge->HasClass("inspector-section-header-badge"));
    EXPECT_TRUE(badge->HasClass("inspector-section-header-badge-neutral"));

    UpdateEntityNameOverrideMark(header, *nameField, world, lid);
    EXPECT_FALSE(header.HasClass("inspector-header-container-overridden"));
    EXPECT_TRUE(badge->HasClass("hidden")) << "a member of the prefab has no badge";
}

// Design 4.4: a component the instance added reads "Added"; one it removed gets a header after the
// components; the root's placement marks nothing.
TEST_F(PrefabPlaceTests, AnAddedComponentHasItsBadgeAndARemovedOneItsHeaderAfterTheComponents)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    const ECS::ComponentTypeId probeType = ECS::GetComponentTypeId<Components::ReflectionProbe>();
    world.RemoveComponentImmediate<Components::ReflectionProbe>(lid);
    world.AddComponentImmediate(root, Components::ReflectionProbe{});
    Editor::NotifyWorldStructure(&Notifications(), &world);

    const ComponentOverrides added = InspectorRegistry::Get().QueryComponentOverrides(world, root, probeType);
    EXPECT_TRUE(added.Overridden);
    EXPECT_EQ(added.Badge, "Added");
    EXPECT_TRUE(added.Fields.empty()) << "the added component's fields are part of the addition";
    const ECS::ComponentTypeId transformType = ECS::GetComponentTypeId<Components::Transform>();
    EXPECT_FALSE(InspectorRegistry::Get().QueryComponentOverrides(world, root, transformType).Overridden)
        << "the root's placement is no override";

    UIElement content;
    InspectorContext context;
    context.Parent = &content;
    context.World = &world;
    context.Entity = lid;
    for (const InspectorFn& banner : InspectorRegistry::Get().GetEntityBanners(EntityBannerPlacement::AfterComponents))
        banner(context);
    ASSERT_EQ(content.GetChildren().size(), 1u);
    const auto* removed = dynamic_cast<const Editor::PrefabRemovedComponents*>(content.GetChildren().front().get());
    ASSERT_NE(removed, nullptr);
    ASSERT_EQ(removed->GetChildren().size(), 1u);
    const auto* header = dynamic_cast<const InspectorSection*>(removed->GetChildren().front().get());
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->GetTitle(), "Reflection Probe");
}

// The bar sets the Inspector's marks again in place when a commit changes the entity's own overrides,
// and not when a commit leaves them as they were. Neither asks for a rebuild.
TEST_F(PrefabPlaceTests, ThePrefabBarRefreshesTheMarksInPlaceWhenTheEntitysOverridesChange)
{
    UIRegistration::RegisterBuiltInControls();
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    int refreshes = 0;
    int rebuilds = 0;
    Editor::PrefabBar bar(world, lid, &Notifications(), {}, [&refreshes]() { ++refreshes; },
                          [&rebuilds]() { ++rebuilds; });

    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, lid);
    EXPECT_EQ(refreshes, 0) << "a commit that changes no override";
    world.GetComponentForWrite<Components::ReflectionProbe>(lid)->Intensity = 3.0f;
    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, lid);
    EXPECT_EQ(refreshes, 1);
    EXPECT_EQ(rebuilds, 0) << "a rebuild ends an edit in the Inspector";
}

// A commit that changes the overrides of another entity of the instance updates the count and keeps
// the Inspector as it is.
TEST_F(PrefabPlaceTests, ThePrefabBarKeepsTheInspectorWhenAnotherEntitysOverridesChange)
{
    UIRegistration::RegisterBuiltInControls();
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    int rebuilds = 0;
    Editor::PrefabBar bar(world, lid, &Notifications(), {}, {}, [&rebuilds]() { ++rebuilds; });
    const auto* count = dynamic_cast<const Label*>(bar.FindById("prefab-bar-count"_sid));
    ASSERT_NE(count, nullptr);

    SetName(world, root, "Probe_01");
    world.AddComponentImmediate(root, Components::ReflectionProbe{});
    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, root);
    EXPECT_EQ(count->GetText(), "No overrides on Lid");
    EXPECT_EQ(count->GetTooltip(), "1 override on Probe_01") << "the member names the instance's count";
    EXPECT_EQ(rebuilds, 0) << "the shown entity's overrides are as they were";
}

// A commit on an entity of another instance leaves the bar's description as it was: the bar does not
// describe its instance again, so a change the editor did not report stays unread.
TEST_F(PrefabPlaceTests, ThePrefabBarReadsNothingAtACommitInAnotherInstance)
{
    UIRegistration::RegisterBuiltInControls();
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle shown = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    const ECS::EntityHandle other = PlaceAt(world, file, 4.0f, 0.0f, 0.0f);
    ASSERT_TRUE(shown.IsValid());
    ASSERT_TRUE(other.IsValid());
    const ECS::EntityHandle shownLid = FindChild(world, shown);
    const ECS::EntityHandle otherLid = FindChild(world, other);
    int refreshes = 0;
    int rebuilds = 0;
    Editor::PrefabBar bar(world, shownLid, &Notifications(), {}, [&refreshes]() { ++refreshes; },
                          [&rebuilds]() { ++rebuilds; });
    const auto* count = dynamic_cast<const Label*>(bar.FindById("prefab-bar-count"_sid));
    ASSERT_NE(count, nullptr);

    world.GetComponentForWrite<Components::ReflectionProbe>(shownLid)->Intensity = 3.0f;
    constexpr int kUnrelatedCommits = 100;
    for (int commit = 0; commit < kUnrelatedCommits; ++commit)
        Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, otherLid);
    EXPECT_EQ(count->GetText(), "No overrides") << "the bar described its instance again";
    EXPECT_EQ(refreshes, 0);

    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, shownLid);
    EXPECT_EQ(count->GetText(), "1 override on Lid");
    EXPECT_EQ(refreshes, 1);
    EXPECT_EQ(rebuilds, 0);
}

// On a member of a nested instance the bar names the nearest prefab, selects that prefab's file and
// counts the member's own overrides; on a member with none it names the outer instance's count.
TEST_F(PrefabPlaceTests, ThePrefabBarOfANestedMemberNamesItsScope)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    WriteAsset("Props/Wheel.prefab", "[prefab version=1]\n"
                                     "[entity id=\"root\"]\n"
                                     "[entity id=\"hub\" parent=\"root\"]\n"
                                     "Name.value = \"Hub\"\n");
    const std::string wheelPath = m_Folder.filename().generic_string() + "/Props/Wheel.prefab";
    const std::string cartText = "[prefab version=1]\n"
                                 "[resource id=\"wheel\" path=\"" +
                                 wheelPath +
                                 "\"]\n"
                                 "[entity id=\"root\"]\n"
                                 "[prefab id=\"wheel\" source=\"wheel\" parent=\"root\"]\n";
    const std::filesystem::path cart = WriteAsset("Props/Cart.prefab", cartText);
    ECS::World world;
    const ECS::EntityHandle cartRoot = PlaceAt(world, cart, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(cartRoot.IsValid());
    SetName(world, cartRoot, "Cart_01");
    const ECS::EntityHandle wheel = FindChild(world, cartRoot);
    ASSERT_TRUE(wheel.IsValid());
    const ECS::EntityHandle hub = FindChild(world, wheel);
    ASSERT_TRUE(hub.IsValid());
    SetName(world, hub, "Spoke");

    Editor::PrefabBar bar(world, hub, &Notifications(), {}, {}, {});
    const auto* title = dynamic_cast<const Label*>(bar.FindById("prefab-bar-title"_sid));
    const auto* count = dynamic_cast<const Label*>(bar.FindById("prefab-bar-count"_sid));
    const auto* select = dynamic_cast<const Button*>(bar.FindById("prefab-bar-select-asset"_sid));
    ASSERT_NE(title, nullptr);
    ASSERT_NE(count, nullptr);
    ASSERT_NE(select, nullptr);
    EXPECT_EQ(title->GetText(), "Part of Wheel in Cart_01");
    EXPECT_EQ(count->GetText(), "1 override on Spoke");
    EXPECT_EQ(select->GetText(), "Select Wheel");
    EXPECT_TRUE(count->GetTooltip().empty()) << "an entity with overrides of its own has no tooltip";
    Editor::PrefabBar wheelBar(world, wheel, &Notifications(), {}, {}, {});
    const auto* wheelCount = dynamic_cast<const Label*>(wheelBar.FindById("prefab-bar-count"_sid));
    ASSERT_NE(wheelCount, nullptr);
    EXPECT_TRUE(wheelCount->GetText().starts_with("No overrides on ")) << wheelCount->GetText();
    EXPECT_EQ(wheelCount->GetText().find("Cart_01"), std::string::npos) << "one sentence on the bar";
    EXPECT_EQ(wheelCount->GetTooltip(), "1 override on Cart_01");
}

// An entity that the instance added says so in the bar's title and has no count line: it is one of the
// root's overrides and has none of its own.
TEST_F(PrefabPlaceTests, ThePrefabBarOfAnAddedEntitySaysItWasAdded)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    SetName(world, root, "Crate_01");
    const ECS::EntityHandle sign = world.CreateEntity();
    world.AddComponentImmediate(sign, Components::Parent{root});
    Editor::NotifyWorldStructure(&Notifications(), &world);

    EXPECT_EQ(PrefabBarText(world, sign, "prefab-bar-title"), "Added to Crate_01");
    Editor::PrefabBar bar(world, sign, &Notifications(), {}, {}, {});
    const auto* count = dynamic_cast<const Label*>(bar.FindById("prefab-bar-count"_sid));
    ASSERT_NE(count, nullptr);
    EXPECT_TRUE(count->HasClass("hidden")) << count->GetText();
    EXPECT_EQ(PrefabBarText(world, root, "prefab-bar-count"), "1 override") << "the root counts the added entity";
}

// A committed field change marks the member's Hierarchy row with no change to the world's structure.
TEST_F(PrefabPlaceTests, TheRowOfAMemberMarksAnOverrideAtTheCommit)
{
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    UIElement lidRow;
    DecorateRow(world, lid, lidRow);
    EXPECT_EQ(PrefabMarkerState(lidRow), "");

    world.GetComponentForWrite<Components::ReflectionProbe>(lid)->Intensity = 3.0f;
    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, lid);
    DecorateRow(world, lid, lidRow);
    EXPECT_EQ(PrefabMarkerState(lidRow), "overridden");
}

// A channel made where a destroyed one was is tracked: its commits drop the stale description.
TEST_F(PrefabPlaceTests, TheDescriptionsTrackAChannelMadeWhereADestroyedOneWas)
{
    std::optional<Editor::EditorChangeNotifications> channel;
    channel.emplace();
    Editor::PrefabInstanceDescriptions::Get().Track(*channel);
    channel.reset();
    channel.emplace();
    Editor::PrefabInstanceDescriptions::Get().Track(*channel);
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    const Editor::DescribedPrefabInstance* before = Editor::PrefabInstanceDescriptions::Get().FindInstance(world, lid);
    ASSERT_NE(before, nullptr);
    ASSERT_FALSE(before->Find(lid)->HasOverrides());

    world.GetComponentForWrite<Components::ReflectionProbe>(lid)->Intensity = 3.0f;
    channel->NotifyComponentCommit<Components::ReflectionProbe>(&world, lid);
    const Editor::DescribedPrefabInstance* after = Editor::PrefabInstanceDescriptions::Get().FindInstance(world, lid);
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->Find(lid)->HasOverrides()) << "the commit on the new channel left the description stale";
    Editor::PrefabInstanceDescriptions::Get().Track(Notifications());
}

// Unpack takes the shown entity out of its instance: the bar asks for the rebuild that drops it.
TEST_F(PrefabPlaceTests, ThePrefabBarRebuildsTheInspectorWhenTheEntityLeavesItsInstance)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle crate = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(crate.IsValid());
    const ECS::EntityHandle lid = FirstChildOf(world, crate);
    int rebuilds = 0;
    Editor::PrefabBar bar(world, lid, &Notifications(), {}, {}, [&rebuilds]() { ++rebuilds; });
    Editor::UndoRedoService undo;
    Editor::HierarchyCommandContext context = CommandContext(world, crate, undo);
    context.NotifyWorldStructureChanged = [&world]() { Editor::NotifyWorldStructure(&Notifications(), &world); };

    const uint32_t unpack = HierarchyMenuCommand(world, crate, "Unpack");
    ASSERT_NE(unpack, 0u);
    ASSERT_TRUE(Editor::EditorPluginRegistry::Get().HandleHierarchyCommand(unpack, context));
    EXPECT_EQ(rebuilds, 1);
}

// The Inspector header commits the name at each keystroke. On a member of an instance the first one adds
// an override: the bar marks the name in place, and the field keeps the focus and the next keystrokes.
TEST_F(PrefabPlaceTests, ANameTypedOnAMemberKeepsTheFieldFocusedAndMarksItInPlace)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    // First local: destroyed last, after the UIManager released its buffers.
    SharedDeviceReleaseRetirement releaseRetirement;
    Rendering::IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "Device init failed";
    const std::filesystem::path file = WriteAsset("Crate.prefab", kCratePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    ASSERT_TRUE(lid.IsValid());

    UIManager ui(device);
    auto ownedRoot = std::make_unique<UIElement>();
    UIElement* inspector = ownedRoot.get();
    auto ownedHeader = std::make_unique<UIElement>();
    UIElement* header = ownedHeader.get();
    auto ownedField = std::make_unique<TextField>();
    TextField* field = ownedField.get();
    field->SetId("entity-name");
    field->SetFocusable(true);
    field->SetValue("Lid");
    field->SetOnValueChanging(
        [&world, lid](const std::string& name)
        {
            SetName(world, lid, name);
            Notifications().NotifyComponentCommit<Components::Name>(&world, lid);
        });
    header->AddChild(std::move(ownedField));
    inspector->AddChild(std::move(ownedHeader));
    int rebuilds = 0;
    const auto refreshMarks = [&]() { UpdateEntityNameOverrideMark(*header, *field, world, lid); };
    // A rebuild makes a new header, as the Inspector does.
    const auto rebuild = [&]()
    {
        ++rebuilds;
        header->RemoveAllChildren();
    };
    inspector->AddChild(std::make_unique<Editor::PrefabBar>(world, lid, &Notifications(), Editor::PrefabBar::SelectAssetHandler{},
                                                            refreshMarks, rebuild));
    ui.SetRoot(std::move(ownedRoot));
    ui.SetFocusById("entity-name");
    ui.DrainDeferredActionsOnce();
    ASSERT_TRUE(UI::FocusIsInTextInput(ui, inspector));

    ui.OnChar('X');
    ui.OnChar('Y');
    EXPECT_EQ(rebuilds, 0) << "the commit rebuilt the header under the edit";
    ASSERT_EQ(header->GetChildren().size(), 1u);
    EXPECT_EQ(ui.GetFocusedElementId(), "entity-name");
    EXPECT_EQ(field->GetValue().size(), 5u) << field->GetValue();
    EXPECT_EQ(world.GetComponent<Components::Name>(lid)->View(), field->GetValue());
    EXPECT_TRUE(header->HasClass("inspector-header-container-overridden"));
    EXPECT_TRUE(field->HasClass("inspector-header-overridden"));

    ui.ClearFocus();
    EXPECT_FALSE(UI::FocusIsInTextInput(ui, inspector)) << "the Inspector holds its rebuild until the focus leaves";
}

// A component removed on the instance changes the removed headers, which only a rebuild draws: the bar
// asks for it, and the Inspector runs it once no text field has the focus.
TEST_F(PrefabPlaceTests, ThePrefabBarAsksForARebuildWhenTheInstanceRemovesAComponent)
{
    UIRegistration::RegisterBuiltInControls();
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    int refreshes = 0;
    int rebuilds = 0;
    Editor::PrefabBar bar(world, lid, &Notifications(), {}, [&refreshes]() { ++refreshes; },
                          [&rebuilds]() { ++rebuilds; });

    world.RemoveComponentImmediate<Components::ReflectionProbe>(lid);
    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, lid);
    EXPECT_EQ(rebuilds, 1);
    EXPECT_EQ(refreshes, 1);
}

// The rows of a member set their marks again in place: the same row gains the mark at a commit that
// overrides its field and loses it at a commit that sets the prefab's value again.
TEST_F(PrefabPlaceTests, TheDefaultRowsSetTheirMarksAgainInPlace)
{
    UIRegistration::RegisterBuiltInControls();
    Editor::RegisterPrefabEditor(Notifications(), nullptr);
    const std::filesystem::path file = WriteAsset("Probe.prefab", kProbePrefab);
    ECS::World world;
    const ECS::EntityHandle root = PlaceAt(world, file, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(root.IsValid());
    const ECS::EntityHandle lid = FindChild(world, root);
    const ECS::ComponentTypeId probeType = ECS::GetComponentTypeId<Components::ReflectionProbe>();
    UIElement parent{"div"};
    std::vector<std::function<void()>> refreshMarks;
    InspectorContext context;
    context.Parent = &parent;
    context.World = &world;
    context.Entity = lid;
    context.OverrideMarkRefreshCallbacks = &refreshMarks;
    ASSERT_TRUE(RenderDefaultComponentInspector(context, probeType));
    ASSERT_EQ(refreshMarks.size(), 1u);
    const UIElement* intensity = FindFieldRow(parent, "Intensity");
    ASSERT_NE(intensity, nullptr);
    EXPECT_FALSE(HasOverrideMark(*intensity));

    world.GetComponentForWrite<Components::ReflectionProbe>(lid)->Intensity = 2.0f;
    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, lid);
    refreshMarks.front()();
    EXPECT_EQ(FindFieldRow(parent, "Intensity"), intensity) << "the row was built again";
    EXPECT_TRUE(HasOverrideMark(*intensity));

    world.GetComponentForWrite<Components::ReflectionProbe>(lid)->Intensity = 1.0f;
    Notifications().NotifyComponentCommit<Components::ReflectionProbe>(&world, lid);
    refreshMarks.front()();
    EXPECT_FALSE(HasOverrideMark(*intensity));
}
