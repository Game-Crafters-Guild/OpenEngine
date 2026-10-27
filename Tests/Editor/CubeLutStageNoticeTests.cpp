// The notice under a Cube LUT effect's Stage row: a display LUT at Grade clips above 1, and a log or
// scene LUT at Finish receives display color. It is an inspector notice whose one action chooses the
// stage the LUT is made for. The inspector builds it from the LUT asset; an asset that is not loaded
// yet is loaded, and the inspector is asked to rebuild once the load completes.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/CubeLut/CubeLutEffectInspector.h"
#include "Inspectors/CubeLut/CubeLutStageNotice.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Rendering/Utils/CubeLutColorDomain.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIElement.h"

#include "../TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace GameEngine;
using Components::CubeLutStage;
using InspectorLayoutTesting::FindByClass;
using InspectorLayoutTesting::InspectorLayoutFixture;
using Rendering::CubeLutInputEncoding;

namespace
{

// A notice as the user reads it: its text, and its action button when it shows one.
struct NoticeView
{
    std::string Text;
    Button* Action = nullptr;
};

void CollectNotices(UIElement* root, std::vector<NoticeView>& out)
{
    if (!root)
        return;
    if (auto* notice = dynamic_cast<EditorUI::InspectorNotice*>(root))
    {
        NoticeView view{};
        if (const auto* text = dynamic_cast<const Label*>(FindByClass(notice, "inspector-notice-text")))
            view.Text = text->GetText();
        auto* action = dynamic_cast<Button*>(FindByClass(notice, "inspector-notice-action"));
        view.Action = action && !action->HasClass("hidden") ? action : nullptr;
        out.push_back(view);
        return;
    }
    for (const auto& child : root->GetChildren())
        CollectNotices(child.get(), out);
}

std::vector<NoticeView> Notices(UIElement* root)
{
    std::vector<NoticeView> out;
    CollectNotices(root, out);
    return out;
}

void Click(Button& button)
{
    UIEvent e{};
    e.Id = kEventButtonClick;
    e.Target = &button;
    e.CurrentTarget = &button;
    button.DispatchEvent(e);
}

// An empty row under the window, standing in for the inspector's section.
UIElement* AddRow(UIElement& window)
{
    auto row = std::make_unique<UIElement>();
    UIElement* raw = row.get();
    window.AddChild(std::move(row));
    return raw;
}

// What the inspector shows: on its first build, and on the build after the refresh it asked for; the
// final notice's action label and the stage a click on it chose.
struct NoticeRun
{
    std::vector<NoticeView> First;
    size_t CallbacksAfterFirst = 0;
    int Refreshes = 0;
    std::vector<NoticeView> Final;
    std::string ActionLabel;
    std::optional<CubeLutStage> Chosen;
};

} // namespace

class CubeLutStageNoticeTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
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
        s_Root = TestUtils::MakeUniqueTempDirectory("cube_lut_stage_notice");
        std::filesystem::create_directories(s_Root);
        AssetSourceDesc source{};
        source.Alias = "cubelutstagenotice";
        source.Root = s_Root;
        ASSERT_TRUE(engine.GetAssetManager().RegisterSource(source));
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        std::filesystem::remove_all(s_Root, ec);
    }

    // A two-knot identity table over 0 to 1 with the given title, registered as an asset.
    static GUID WriteLut(const std::string& fileName, const std::string& title)
    {
        const std::filesystem::path path = s_Root / fileName;
        std::ofstream out(path, std::ios::binary);
        out << "TITLE \"" << title << "\"\nLUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    out << r << ' ' << g << ' ' << b << '\n';
        out.close();
        const GUID guid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(path);
        EXPECT_FALSE(guid.IsNull()) << "the registry did not take " << path.string();
        return guid;
    }

    // Builds the notice as the effect inspector does, in a window whose manager runs the posted
    // refresh; while a load is pending, runs frames until the refresh is requested, then builds again.
    // Clicks the final notice's action, if it shows one.
    static NoticeRun Run(const GUID& lut, CubeLutStage stage, InspectorLayoutFixture& fixture)
    {
        NoticeRun run{};
        auto root = std::make_unique<UIElement>();
        UIElement* window = root.get();
        if (!fixture.Build(std::move(root)))
            return run;
        std::vector<std::function<void()>> callbacks;
        InspectorContext ctx{};
        ctx.Parent = AddRow(*window);
        ctx.FrameRefreshCallbacks = &callbacks;
        ctx.RequestInspectorRefresh = [&run] { ++run.Refreshes; };
        const auto choose = [&run](CubeLutStage chosen) { run.Chosen = chosen; };

        AddCubeLutStageNotice(ctx, lut, stage, choose);
        run.First = Notices(ctx.Parent);
        run.CallbacksAfterFirst = callbacks.size();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!callbacks.empty() && run.Refreshes == 0 && std::chrono::steady_clock::now() < deadline)
        {
            EngineCore::GetInstance().GetAssetManager().Update();
            for (const auto& callback : callbacks)
                callback();
            fixture.Ui->Update(1.0f / 60.0f, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (run.Refreshes == 0)
        {
            run.Final = run.First;
        }
        else
        {
            callbacks.clear();
            ctx.Parent = AddRow(*window);
            AddCubeLutStageNotice(ctx, lut, stage, choose);
            run.Final = Notices(ctx.Parent);
        }
        if (!run.Final.empty() && run.Final[0].Action)
        {
            run.ActionLabel = run.Final[0].Action->GetText();
            Click(*run.Final[0].Action);
        }
        return run;
    }

    static std::filesystem::path s_Root;
};

std::filesystem::path CubeLutStageNoticeTests::s_Root;

// The notice names the file as it is spelled, the input and what goes wrong, for each direction (its
// action names the fix); a LUT at the stage it is made for gets none.
TEST(CubeLutStageNotice, TheTextNamesTheMismatch)
{
    EXPECT_EQ(CubeLutStageNoticeText("Warm.cube", CubeLutInputEncoding::Rec709SRGB, 1.0f, CubeLutStage::Grade),
              "Warm.cube is a display LUT (Rec.709 / sRGB input): at Grade it clips scene color above 1.");
    EXPECT_EQ(CubeLutStageNoticeText("Show.cube", CubeLutInputEncoding::ACEScct, 1.0f, CubeLutStage::Finish),
              "Show.cube is a log LUT (ACEScct input): at Finish it receives display color and crushes the "
              "image.");
    EXPECT_EQ(CubeLutStageNoticeText("Hdr.cube", CubeLutInputEncoding::Linear, 16.0f, CubeLutStage::Finish),
              "Hdr.cube is a scene LUT (Linear, 0 to 16 input): at Finish it receives display color and crushes "
              "the image.");
    EXPECT_EQ(CubeLutStageNoticeText("Warm.cube", CubeLutInputEncoding::Rec709SRGB, 1.0f, CubeLutStage::Finish), "");
    EXPECT_EQ(CubeLutStageNoticeText("Show.cube", CubeLutInputEncoding::ACEScct, 1.0f, CubeLutStage::Grade), "");
}

TEST_F(CubeLutStageNoticeTests, ADisplayLutAtGradeShowsTheNotice)
{
    const GUID lut = WriteLut("DisplayAtGrade.cube", "display linear bake");
    InspectorLayoutFixture fixture;
    const NoticeRun run = Run(lut, CubeLutStage::Grade, fixture);
    if (!fixture.Ui)
        GTEST_SKIP() << fixture.Diagnostic;
    ASSERT_EQ(run.Final.size(), 1u);
    EXPECT_EQ(run.Final[0].Text, "DisplayAtGrade.cube is a display LUT (Linear, 0 to 1 input): at Grade it clips scene "
                                 "color above 1.");
    EXPECT_EQ(run.ActionLabel, "Choose Finish");
    EXPECT_EQ(run.Chosen, CubeLutStage::Finish) << "the action chooses the stage a display LUT is made for";
}

TEST_F(CubeLutStageNoticeTests, ALogLutAtGradeShowsNone)
{
    const GUID lut = WriteLut("LogAtGrade.cube", "ACEScct look");
    InspectorLayoutFixture fixture;
    const NoticeRun run = Run(lut, CubeLutStage::Grade, fixture);
    if (!fixture.Ui)
        GTEST_SKIP() << fixture.Diagnostic;
    EXPECT_EQ(run.Refreshes, 1) << "the load completed and the inspector was asked to rebuild";
    EXPECT_TRUE(run.Final.empty()) << run.Final.front().Text;
}

TEST_F(CubeLutStageNoticeTests, ALogLutAtFinishShowsTheMirrorNotice)
{
    const GUID lut = WriteLut("LogAtFinish.cube", "ACEScct look");
    InspectorLayoutFixture fixture;
    const NoticeRun run = Run(lut, CubeLutStage::Finish, fixture);
    if (!fixture.Ui)
        GTEST_SKIP() << fixture.Diagnostic;
    ASSERT_EQ(run.Final.size(), 1u);
    EXPECT_EQ(run.Final[0].Text, "LogAtFinish.cube is a log LUT (ACEScct input): at Finish it receives display "
                                 "color and crushes the image.");
    EXPECT_EQ(run.ActionLabel, "Choose Grade");
    EXPECT_EQ(run.Chosen, CubeLutStage::Grade) << "the action chooses the stage a log LUT is made for";
}

// A LUT not loaded yet: the first build shows nothing and waits for the load; the load's completion
// asks for one rebuild, and the rebuild shows the notice.
TEST_F(CubeLutStageNoticeTests, AnUnloadedLutShowsTheNoticeOnceItsLoadCompletes)
{
    const GUID lut = WriteLut("UnloadedAtGrade.cube", "display linear bake");
    ASSERT_EQ(EngineCore::GetInstance().GetAssetManager().GetAsset(lut), nullptr) << "the LUT must start unloaded";
    InspectorLayoutFixture fixture;
    const NoticeRun run = Run(lut, CubeLutStage::Grade, fixture);
    if (!fixture.Ui)
        GTEST_SKIP() << fixture.Diagnostic;
    EXPECT_TRUE(run.First.empty());
    EXPECT_EQ(run.CallbacksAfterFirst, 1u) << "the first build waits for the load";
    EXPECT_EQ(run.Refreshes, 1);
    ASSERT_EQ(run.Final.size(), 1u);
    EXPECT_NE(run.Final[0].Text.find("UnloadedAtGrade.cube is a display LUT"), std::string::npos) << run.Final[0].Text;
    EXPECT_EQ(run.ActionLabel, "Choose Finish");
}

// The effect inspector's wiring: its notice's action changes the effect's stage and asks for the
// inspector rebuild that drops the notice and shows the new stage in the Stage row (a plain commit
// leaves the inspector as it was, which is right for the row that made the edit and wrong for a
// notice's button).
TEST_F(CubeLutStageNoticeTests, TheEffectInspectorsNoticeChoosesFinishAndRebuildsTheInspector)
{
    const GUID lut = WriteLut("WiredAtGrade.cube", "display linear bake");
    RegisterCubeLutEffectInspector();
    InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::CubeLutEffect>();
    ASSERT_NE(inspector, nullptr);

    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::CubeLutEffect effect{};
    effect.LutAssetGuid.Set(lut);
    effect.Stage = CubeLutStage::Grade;
    world.AddComponentImmediate(entity, effect);

    Editor::EditorChangeNotifications notifications;
    std::vector<Editor::EditorChangeNotifications::ChangeKind> kinds;
    const auto token = notifications.SubscribeComponentChanged(
        [&kinds](const Editor::EditorChangeNotifications::ComponentChangedEvent& e) { kinds.push_back(e.kind); });

    InspectorLayoutFixture fixture;
    auto root = std::make_unique<UIElement>();
    UIElement* window = root.get();
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;

    // The first build waits for the LUT's load; the build after it shows the notice.
    std::vector<std::function<void()>> callbacks;
    int refreshes = 0;
    InspectorContext ctx{};
    ctx.World = &world;
    ctx.Entity = entity;
    ctx.ChangeNotifications = &notifications;
    ctx.FrameRefreshCallbacks = &callbacks;
    ctx.RequestInspectorRefresh = [&refreshes] { ++refreshes; };
    ctx.Parent = AddRow(*window);
    (*inspector)(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!callbacks.empty() && refreshes == 0 && std::chrono::steady_clock::now() < deadline)
    {
        EngineCore::GetInstance().GetAssetManager().Update();
        for (const auto& callback : callbacks)
            callback();
        fixture.Ui->Update(1.0f / 60.0f, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    callbacks.clear();
    ctx.Parent = AddRow(*window);
    (*inspector)(ctx);

    const std::vector<NoticeView> notices = Notices(ctx.Parent);
    ASSERT_EQ(notices.size(), 1u);
    ASSERT_NE(notices[0].Action, nullptr);
    Click(*notices[0].Action);

    const auto* stored = world.GetComponent<Components::CubeLutEffect>(entity);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->Stage, CubeLutStage::Finish) << "the notice's action commits the stage";
    EXPECT_NE(std::find(kinds.begin(), kinds.end(), Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild),
              kinds.end())
        << "the inspector is asked to rebuild, so the Stage row reads Finish and the notice goes";
}

// Without a stage callback the notice shows no button: an action that does nothing is not offered.
TEST_F(CubeLutStageNoticeTests, ANoticeWithoutACallbackShowsNoAction)
{
    const GUID lut = WriteLut("NoCallbackAtGrade.cube", "display linear bake");
    InspectorLayoutFixture fixture;
    auto root = std::make_unique<UIElement>();
    UIElement* window = root.get();
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    std::vector<std::function<void()>> callbacks;
    int refreshes = 0;
    InspectorContext ctx{};
    ctx.FrameRefreshCallbacks = &callbacks;
    ctx.RequestInspectorRefresh = [&refreshes] { ++refreshes; };
    ctx.Parent = AddRow(*window);
    AddCubeLutStageNotice(ctx, lut, CubeLutStage::Grade, {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!callbacks.empty() && refreshes == 0 && std::chrono::steady_clock::now() < deadline)
    {
        EngineCore::GetInstance().GetAssetManager().Update();
        for (const auto& callback : callbacks)
            callback();
        fixture.Ui->Update(1.0f / 60.0f, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ctx.Parent = AddRow(*window);
    AddCubeLutStageNotice(ctx, lut, CubeLutStage::Grade, {});
    const std::vector<NoticeView> notices = Notices(ctx.Parent);
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].Action, nullptr);
}
