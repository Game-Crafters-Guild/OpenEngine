// The prefab instance in the editor's views: the Scene View's pick climbs to the instance root,
// the instance root's Hierarchy row shows the prefab glyph over the glyph of its mesh, and the
// row's class tints the glyph and badges a member's glyph through the editor's own stylesheets.

#include <gtest/gtest.h>

#include "Editor/EditorTreeTitleIconVars.h"
#include "Editor/Hierarchy/HierarchyEntityIcon.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "RecordingContextMenu.h"
#include "Picking/PickRootResolver.h"
#include "EditorChangeNotifications.h"
#include "Prefabs/PrefabHierarchyRows.h"

#include "AssetCore/GUID.h"
#include "Components/AssetRef.h"
#include "Components/Hierarchy.h"
#include "Components/PrefabInstance.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Types/StringUtils.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UIRgTestHarness.h"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using GameEngine::GUID;
using GameEngine::Components::MeshRenderer;
using GameEngine::Components::ModelRef;
using GameEngine::Components::Parent;
using GameEngine::Components::PrefabInstance;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;

// The theme sheets in theme.css import order.
constexpr const char* kThemeSheets[] = {
    "Assets/UI/theme/tokens.css",
    "Assets/UI/theme/core.css",
    "Assets/UI/theme/views.css",
    "Assets/UI/theme/widgets.css",
};
constexpr uint32_t kPrefabTeal = 0xFF2EC4B6u;   // --ui_color_prefab
constexpr uint32_t kErrorRed = 0xFFFF6B6Bu;     // --ui_color_error_text
constexpr uint32_t kNeutralGlyph = 0xFFC8C8C8u; // the Hierarchy's neutral glyph tint

std::string ReadEditorFile(const std::string& relativePath)
{
    std::ifstream in(std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The glyph and the title of one Hierarchy row, laid out under a real UIManager.
struct HierarchyRowElements
{
    GameEngine::UIElement* Icon = nullptr;
    GameEngine::UIElement* Title = nullptr;
};

// Adds the editor's theme sheets to `ui` in theme.css import order.
void AddThemeSheets(GameEngine::UIManager& ui)
{
    for (const char* sheetPath : kThemeSheets)
    {
        GameEngine::Stylesheet sheet{};
        ASSERT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(ReadEditorFile(sheetPath), sheet))
            << "stylesheet did not parse: " << sheetPath;
        ui.AddStylesheet(std::make_shared<const GameEngine::Stylesheet>(std::move(sheet)));
    }
}

// Makes a Hierarchy row with the classes `rowClasses` and a glyph of kind `iconClass` the root of
// `ui`, and resolves its styles. The elements live until the next call.
HierarchyRowElements ShowHierarchyRow(GameEngine::UIManager& ui, std::initializer_list<const char*> rowClasses,
                                      const char* iconClass)
{
    auto panel = std::make_unique<GameEngine::UIElement>();
    panel->AddClass("hierarchy-panel");
    auto view = std::make_unique<GameEngine::UIElement>();
    auto tree = std::make_unique<GameEngine::UIElement>();
    tree->AddClass("tree");
    auto row = std::make_unique<GameEngine::UIElement>();
    row->AddClass("tree-item");
    for (const char* rowClass : rowClasses)
        row->AddClass(rowClass);
    auto title = std::make_unique<GameEngine::UIElement>();
    title->AddClass("tree-title");
    title->AddClass("tree-title-no-icon");
    auto icon = std::make_unique<GameEngine::UIElement>();
    icon->AddClass("hierarchy-preview-icon");
    icon->AddClass("entity-icon");
    icon->AddClass(iconClass);
    const HierarchyRowElements elements{icon.get(), title.get()};
    row->AddChild(std::move(title));
    row->AddChild(std::move(icon));
    tree->AddChild(std::move(row));
    view->AddChild(std::move(tree));
    panel->AddChild(std::move(view));
    ui.SetRoot(std::move(panel));
    ui.Update(1.0f / 60.0f, /*interactive=*/true);
    return elements;
}

// The glyph tint a Hierarchy row with `rowClasses` resolves for a glyph of kind `iconClass`.
uint32_t ResolveHierarchyGlyphTint(GameEngine::UIManager& ui, std::initializer_list<const char*> rowClasses,
                                   const char* iconClass)
{
    return ShowHierarchyRow(ui, rowClasses, iconClass).Icon->GetResolvedStyle().Visual.BackgroundImage.Tint;
}

// Sets the Hierarchy icon size on the tree view that ShowHierarchyRow made, as the Hierarchy settings do.
void SetHierarchyIconSize(GameEngine::UIManager& ui, float iconSizePx)
{
    GameEngine::ApplyTreeTitleIconLayoutVars(ui.GetRootElement()->GetChildren().front().get(), iconSizePx);
    ui.Update(1.0f / 60.0f, /*interactive=*/true);
}

// The smallest Hierarchy icon size that carries the member badge.
constexpr float kSmallestBadgedIconPx = 20.0f;

// True when `title` draws the teal member badge.
bool ShowsMemberBadge(const GameEngine::UIElement& title)
{
    const GameEngine::BackgroundImageStyle& image = title.GetResolvedStyle().Visual.BackgroundImage;
    return image.HasImage && image.Source.Value.ends_with("prefab-member-badge.svg") && image.Tint == kPrefabTeal;
}

class PrefabHierarchyTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // The prefab menu reads the asset registry, so the engine must run.
        GameEngine::EngineCore& engine = GameEngine::EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            GameEngine::ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        static GameEngine::Editor::EditorChangeNotifications s_Notifications;
        GameEngine::Editor::RegisterPrefabEditor(s_Notifications, nullptr);
    }

    // An entity that renders `model`, under `parent` when it is valid.
    EntityHandle CreateModelEntity(const GUID& model, EntityHandle parent)
    {
        const EntityHandle e = m_World.CreateEntity();
        MeshRenderer renderer{};
        renderer.modelAssetGuid = ModelRef(model);
        m_World.AddComponentImmediate(e, renderer);
        if (parent.IsValid())
            m_World.AddComponentImmediate(e, Parent{parent});
        return e;
    }

    EntityHandle CreateInstanceRoot(const GUID& model, EntityHandle parent)
    {
        const EntityHandle e = CreateModelEntity(model, parent);
        m_World.AddComponentImmediate(e, PrefabInstance{GUID::Generate()});
        return e;
    }

    World m_World;
};

TEST_F(PrefabHierarchyTests, APickInsideAnInstanceClimbsToItsRoot)
{
    // The root renders another model than its children, so the model rule alone stops below it.
    const GUID crateModel = GUID::Generate();
    const GUID lidModel = GUID::Generate();
    const EntityHandle root = CreateInstanceRoot(crateModel, {});
    const EntityHandle lid = CreateModelEntity(lidModel, root);
    const EntityHandle handle = CreateModelEntity(lidModel, lid);

    EXPECT_EQ(GameEngine::Editor::Picking::ResolvePickRoot(m_World, handle), root);
    EXPECT_EQ(GameEngine::Editor::Picking::ResolvePickRoot(m_World, root), root);
}

TEST_F(PrefabHierarchyTests, APickInsideANestedInstanceClimbsToTheOutermostRoot)
{
    const GUID houseModel = GUID::Generate();
    const GUID porchModel = GUID::Generate();
    const EntityHandle house = CreateInstanceRoot(houseModel, {});
    const EntityHandle porch = CreateInstanceRoot(porchModel, house);
    const EntityHandle step = CreateModelEntity(porchModel, porch);

    EXPECT_EQ(GameEngine::Editor::Picking::ResolvePickRoot(m_World, step), house);
}

TEST_F(PrefabHierarchyTests, AnInstanceRootRowShowsThePrefabGlyphOverItsModel)
{
    const EntityHandle root = CreateInstanceRoot(GUID::Generate(), {});
    const EntityHandle plain = CreateModelEntity(GUID::Generate(), {});
    GameEngine::UIElement rootRow;
    GameEngine::UIElement plainRow;

    GameEngine::Editor::ApplyHierarchyEntityIconClasses(&m_World, &rootRow, root);
    GameEngine::Editor::ApplyHierarchyEntityIconClasses(&m_World, &plainRow, plain);

    EXPECT_TRUE(rootRow.HasClass("hierarchy-entity-prefab"));
    EXPECT_FALSE(rootRow.HasClass("hierarchy-entity-model"));
    EXPECT_TRUE(GameEngine::Editor::EntityIconIsCssOnly(rootRow));
    EXPECT_TRUE(plainRow.HasClass("hierarchy-entity-model"));
}

TEST(PrefabHierarchyGlyphTint, TheRowClassTintsTheInstanceGlyph)
{
    std::unique_ptr<GameEngine::Rendering::IDevice> device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    GameEngine::UIManager ui(device.get());
    AddThemeSheets(ui);
    if (HasFatalFailure())
        return;

    EXPECT_EQ(ResolveHierarchyGlyphTint(ui, {"hierarchy-prefab-root"}, "hierarchy-entity-prefab"), kPrefabTeal);
    EXPECT_EQ(ResolveHierarchyGlyphTint(ui, {"hierarchy-prefab-missing"}, "hierarchy-entity-prefab"), kErrorRed);
    EXPECT_EQ(ResolveHierarchyGlyphTint(ui, {}, "hierarchy-entity-empty"), kNeutralGlyph);
}

// A member rests in the full teal with the badge on its glyph, also when selected; the root has no badge.
TEST(PrefabHierarchyGlyphTint, AMemberRowShowsAFullTealNameAndABadgeOnItsGlyph)
{
    std::unique_ptr<GameEngine::Rendering::IDevice> device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    GameEngine::UIManager ui(device.get());
    AddThemeSheets(ui);
    if (HasFatalFailure())
        return;

    const HierarchyRowElements member = ShowHierarchyRow(ui, {"hierarchy-prefab-member"}, "hierarchy-entity-shape-cube");
    SetHierarchyIconSize(ui, kSmallestBadgedIconPx);
    EXPECT_EQ(member.Title->GetResolvedStyle().Visual.LocalOpacity, 1.0f);
    EXPECT_EQ(member.Title->GetResolvedStyle().Visual.Color, kPrefabTeal);
    EXPECT_TRUE(ShowsMemberBadge(*member.Title));
    // The badge is the prefab cube in miniature: three face paths and a hexagon outline. It has no
    // round shape, so it does not read as the round override dot in the gutter.
    const std::string badge = ReadEditorFile("Assets/Icons/prefab-member-badge.svg");
    EXPECT_EQ(GameEngine::CountOccurrences(badge, "<path"), 4u) << "three cube faces and the outline";
    const std::string_view text = badge;
    for (std::size_t at = text.find("<path"); at != std::string_view::npos; at = text.find("<path", at + 1))
    {
        const std::size_t data = text.find(" d=\"", at) + 4;
        const std::string_view pathData = text.substr(data, text.find('"', data) - data);
        EXPECT_EQ(pathData.find_first_of("AaCcQqSsTt"), std::string_view::npos) << "a curve in the path: " << pathData;
    }
    EXPECT_EQ(badge.find("<rect"), std::string::npos);
    EXPECT_EQ(badge.find("<circle"), std::string::npos);
    EXPECT_EQ(badge.find("<ellipse"), std::string::npos);
    // The badge is at the left edge of the title, so that edge must be inside the glyph.
    EXPECT_GT(member.Title->GetLayoutX(), member.Icon->GetLayoutX());
    EXPECT_LT(member.Title->GetLayoutX(), member.Icon->GetLayoutX() + member.Icon->GetLayoutWidth());

    const HierarchyRowElements selected =
        ShowHierarchyRow(ui, {"hierarchy-prefab-member", "selected"}, "hierarchy-entity-shape-cube");
    SetHierarchyIconSize(ui, kSmallestBadgedIconPx);
    EXPECT_EQ(selected.Title->GetResolvedStyle().Visual.LocalOpacity, 1.0f);
    EXPECT_EQ(selected.Title->GetResolvedStyle().Visual.Color, kPrefabTeal);
    EXPECT_TRUE(ShowsMemberBadge(*selected.Title));

    const HierarchyRowElements root = ShowHierarchyRow(ui, {"hierarchy-prefab-root"}, "hierarchy-entity-prefab");
    EXPECT_FALSE(root.Title->GetResolvedStyle().Visual.BackgroundImage.HasImage);
}

// At every icon size from 20 px up the badge sits on the glyph's bottom-right corner, grows with the
// glyph, and leaves the name where a row without the badge puts it.
TEST(PrefabHierarchyGlyphTint, TheMemberBadgeFollowsTheGlyphAtEveryIconSize)
{
    std::unique_ptr<GameEngine::Rendering::IDevice> device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    GameEngine::UIManager ui(device.get());
    AddThemeSheets(ui);
    if (HasFatalFailure())
        return;

    float smallerBadgePx = 0.0f;
    for (const float iconSizePx : {kSmallestBadgedIconPx, 32.0f, 64.0f})
    {
        SCOPED_TRACE(iconSizePx);
        const HierarchyRowElements plain = ShowHierarchyRow(ui, {}, "hierarchy-entity-shape-cube");
        SetHierarchyIconSize(ui, iconSizePx);
        const float plainNameX = plain.Title->GetLayoutX() + plain.Title->GetLayoutPadding().Left;

        const HierarchyRowElements member = ShowHierarchyRow(ui, {"hierarchy-prefab-member"}, "hierarchy-entity-shape-cube");
        SetHierarchyIconSize(ui, iconSizePx);
        const GameEngine::BackgroundImageStyle& badge = member.Title->GetResolvedStyle().Visual.BackgroundImage;
        ASSERT_TRUE(ShowsMemberBadge(*member.Title));
        EXPECT_EQ(badge.SizeX, badge.SizeY);
        EXPECT_GT(badge.SizeX, smallerBadgePx) << "the badge grows with the glyph";
        smallerBadgePx = badge.SizeX;

        const float glyphRight = member.Icon->GetLayoutX() + member.Icon->GetLayoutWidth();
        const float glyphBottom = member.Icon->GetLayoutY() + member.Icon->GetLayoutHeight();
        const float badgeLeft = member.Title->GetLayoutX();
        const float badgeBottom = member.Title->GetLayoutY() + member.Title->GetLayoutHeight();
        const float nameX = badgeLeft + member.Title->GetLayoutPadding().Left;
        EXPECT_EQ(member.Icon->GetLayoutWidth(), iconSizePx);
        EXPECT_LT(badgeLeft, glyphRight) << "the badge starts on the glyph";
        EXPECT_GT(badgeLeft + badge.SizeX, glyphRight) << "the badge ends past the glyph's edge";
        EXPECT_LT(badgeLeft + badge.SizeX, nameX) << "the badge ends before the name";
        EXPECT_NEAR(badgeBottom, glyphBottom, 0.5f) << "the badge sits on the glyph's bottom edge";
        EXPECT_NEAR(nameX, plainNameX, 0.5f) << "the name does not move";
    }
}

// Under 20 px the badge is too small to read as a cube and reads as a second dot beside the override
// dot, so it is not drawn: the teal name alone marks the member, and the name stays where it is.
TEST(PrefabHierarchyGlyphTint, UnderTwentyPixelsAMemberRowShowsNoBadge)
{
    std::unique_ptr<GameEngine::Rendering::IDevice> device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    GameEngine::UIManager ui(device.get());
    AddThemeSheets(ui);
    if (HasFatalFailure())
        return;

    for (const float iconSizePx : {16.0f, kSmallestBadgedIconPx - 1.0f})
    {
        SCOPED_TRACE(iconSizePx);
        const HierarchyRowElements plain = ShowHierarchyRow(ui, {}, "hierarchy-entity-shape-cube");
        SetHierarchyIconSize(ui, iconSizePx);
        const float plainNameX = plain.Title->GetLayoutX() + plain.Title->GetLayoutPadding().Left;

        const HierarchyRowElements member = ShowHierarchyRow(ui, {"hierarchy-prefab-member"}, "hierarchy-entity-shape-cube");
        SetHierarchyIconSize(ui, iconSizePx);
        EXPECT_FALSE(member.Title->GetResolvedStyle().Visual.BackgroundImage.HasImage);
        EXPECT_EQ(member.Title->GetResolvedStyle().Visual.Color, kPrefabTeal) << "the name still marks the member";
        EXPECT_NEAR(member.Title->GetLayoutX() + member.Title->GetLayoutPadding().Left, plainNameX, 0.5f);
    }

    const HierarchyRowElements member = ShowHierarchyRow(ui, {"hierarchy-prefab-member"}, "hierarchy-entity-shape-cube");
    SetHierarchyIconSize(ui, kSmallestBadgedIconPx);
    EXPECT_TRUE(ShowsMemberBadge(*member.Title)) << "the badge shows from 20 px up";
}

} // namespace

// The menu on an instance offers Create Prefab, disabled there with the reason, and no entry without a command.
TEST_F(PrefabHierarchyTests, TheMenuOnAnInstanceListsCreatePrefabAndNoUnimplementedEntry)
{
    const EntityHandle root = CreateInstanceRoot(GUID::Generate(), {});
    GameEngine::Editor::IEditorPlugin* prefabs = nullptr;
    for (GameEngine::Editor::IEditorPlugin* plugin : GameEngine::Editor::EditorPluginRegistry::Get().GetPlugins())
    {
        if (std::string_view(plugin->GetDescriptor().Id) == "prefabs")
            prefabs = plugin;
    }
    ASSERT_NE(prefabs, nullptr);
    GameEngine::ContextMenuBuilder builder;
    prefabs->BuildHierarchyContextMenu(builder, {&m_World, root, true, nullptr});
    EditorTesting::RecordingContextMenu menu;
    builder.Build(&menu);

    std::vector<std::string> titles;
    for (const auto& row : menu.Rows)
        titles.push_back(row.Title);
    const std::vector<std::string> expected{"Create Prefab\xe2\x80\xa6", "Select Prefab", "Unpack", "Unpack Completely"};
    EXPECT_EQ(titles, expected);
    for (const auto& row : menu.Rows)
        EXPECT_NE(row.CommandId, 0u) << row.Title;
    EXPECT_FALSE(menu.IsEnabled("Create Prefab\xe2\x80\xa6")) << "an instance is a prefab already";
    EXPECT_NE(menu.TooltipOf("Create Prefab\xe2\x80\xa6").find("is part of a prefab instance. Unpack the instance"),
              std::string::npos)
        << "the disabled entry says why and what to do";
    EXPECT_FALSE(menu.IsEnabled("Select Prefab")) << "the prefab file of this instance is not in the project";
    EXPECT_TRUE(menu.IsEnabled("Unpack"));
    EXPECT_TRUE(menu.IsEnabled("Unpack Completely"));
    EXPECT_EQ(menu.ShortcutOf("Create Prefab\xe2\x80\xa6"),
              GameEngine::Editor::CatalogShortcutLabel("Hierarchy", "Create Prefab"));

    const EntityHandle member = CreateModelEntity(GUID::Generate(), root);
    GameEngine::ContextMenuBuilder memberBuilder;
    prefabs->BuildHierarchyContextMenu(memberBuilder, {&m_World, member, true, nullptr});
    EditorTesting::RecordingContextMenu memberMenu;
    memberBuilder.Build(&memberMenu);
    EXPECT_FALSE(memberMenu.IsEnabled("Unpack")) << "a member is no instance root";
}
