// The Base section's rows for a Function terrain: the Function dropdown of the registered names, the
// Seed field, the notice for a name that no module registers, and the Generating badge in the Base
// header while the function generates the world for its seed.
//
// BOUNDARY: the inspector is built in process over a live World. The terrain is not provisioned, and
// the inspector rebuild after a commit is not run.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainInspector.h"

#include "Components/Terrain/Terrain.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Input/KeyCodes.h"
#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"
#include "TerrainECS/TerrainService.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr const char* kAlpha = "EditorTests.FunctionRows.Alpha";
constexpr const char* kBeta = "EditorTests.FunctionRows.Beta";
constexpr const char* kSlow = "EditorTests.FunctionRows.Slow";
constexpr const char* kMissing = "EditorTests.FunctionRows.Missing";
constexpr const char* kLate = "EditorTests.FunctionRows.Late";

class Level final : public TerrainECS::ITerrainHeightFunction
{
public:
    explicit Level(const TerrainECS::TerrainHeightFunctionContext&) {}
    void HeightRow(float32, std::span<const float32> x, float32, std::span<float32> outMeters) const override
    {
        std::fill_n(outMeters.begin(), x.size(), 0.0f);
    }
    TerrainECS::TerrainHeightRange HeightRange(float32, float32, float32, float32, float32) const override
    {
        return {};
    }
};

std::atomic<bool> g_ReleaseSlow{false};

// A function whose build waits until the test releases it, or 10 s at most.
class Slow final : public TerrainECS::ITerrainHeightFunction
{
public:
    explicit Slow(const TerrainECS::TerrainHeightFunctionContext&)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!g_ReleaseSlow.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void HeightRow(float32, std::span<const float32> x, float32, std::span<float32> outMeters) const override
    {
        std::fill_n(outMeters.begin(), x.size(), 0.0f);
    }
    TerrainECS::TerrainHeightRange HeightRange(float32, float32, float32, float32, float32) const override
    {
        return {};
    }
};

template <typename T>
T* FindFirst(UIElement& element)
{
    if (auto* found = dynamic_cast<T*>(&element))
        return found;
    for (const auto& child : element.GetChildren())
        if (T* found = FindFirst<T>(*child))
            return found;
    return nullptr;
}

UIElement* FindById(UIElement& element, const std::string& id)
{
    if (element.GetId() == id)
        return &element;
    for (const auto& child : element.GetChildren())
        if (UIElement* found = FindById(*child, id))
            return found;
    return nullptr;
}

// A part of the Seed row's SeedField: its IntField or its dice Button.
template <typename T>
T* FindSeedPart(UIElement& parent)
{
    UIElement* field = FindById(parent, "terrain-base-seed");
    return field ? FindFirst<T>(*field) : nullptr;
}

void CollectLabelTexts(UIElement& element, std::vector<std::string>& texts)
{
    if (auto* label = dynamic_cast<Label*>(&element))
        texts.push_back(label->GetText());
    for (const auto& child : element.GetChildren())
        CollectLabelTexts(*child, texts);
}

bool ShowsText(UIElement& element, const std::string& text)
{
    std::vector<std::string> texts;
    CollectLabelTexts(element, texts);
    return std::ranges::find(texts, text) != texts.end();
}

std::string UnregisteredNotice(const char* name)
{
    return std::string("No loaded module registers the height function '") + name +
           "'. The terrain is flat until one does. Save the game module's source in the project's Assets "
           "folder so the editor builds it, or pick another function.";
}

void RunFrameCallbacks(const std::vector<std::function<void()>>& callbacks)
{
    for (const auto& callback : callbacks)
        callback();
}

// The section whose header reads the title.
Foldout* FindSection(UIElement& element, const std::string& title)
{
    if (auto* foldout = dynamic_cast<Foldout*>(&element); foldout && foldout->GetTitle() == title)
        return foldout;
    for (const auto& child : element.GetChildren())
        if (Foldout* found = FindSection(*child, title))
            return found;
    return nullptr;
}

// The status badge in the section's header, or nullptr before the section shows one.
Label* FindHeaderBadge(Foldout& section)
{
    for (const auto& child : section.GetHeader()->GetChildren())
        if (child->HasClass("foldout-badge"))
            return dynamic_cast<Label*>(child.get());
    return nullptr;
}

// Each row of the section and whether it shows: a row that appears or hides moves the rows below it.
std::vector<std::pair<const UIElement*, bool>> RowsShown(const Foldout& section)
{
    std::vector<std::pair<const UIElement*, bool>> rows;
    for (const auto& child : section.GetContentContainer()->GetChildren())
        rows.emplace_back(child.get(), !child->HasClass("hidden"));
    return rows;
}

// Runs the frame callbacks until the element hides, or 10 s at most.
bool WaitUntilHidden(UIElement& element, const std::vector<std::function<void()>>& callbacks)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!element.HasClass("hidden") && std::chrono::steady_clock::now() < deadline)
    {
        RunFrameCallbacks(callbacks);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return element.HasClass("hidden");
}

// Types the text into the field the way a keyboard does, then presses Enter.
void TypeAndCommit(IntField& field, const std::string& text)
{
    TextInput* editor = FindFirst<TextInput>(field);
    ASSERT_NE(editor, nullptr);
    editor->SetValue("");
    for (const char c : text)
        editor->OnChar(static_cast<unsigned char>(c));
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
}

class TerrainFunctionBaseRowsTest : public ::testing::Test
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
        // The Generating notice asks the service. Other suites in this binary share it, so it stays up.
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();
        TerrainECS::RegisterTerrainHeightFunction(kBeta, &TerrainECS::MakeTerrainHeightFunction<Level>);
        TerrainECS::RegisterTerrainHeightFunction(kAlpha, &TerrainECS::MakeTerrainHeightFunction<Level>);
        TerrainECS::RegisterTerrainHeightFunction(kSlow, &TerrainECS::MakeTerrainHeightFunction<Slow>);
    }

    void Inspect(const char* function, uint64 seed)
    {
        RegisterTerrainInspector();
        Components::Terrain terrain{};
        terrain.Domain = Components::TerrainDomain::Planar;
        terrain.BaseSource = Components::TerrainBaseSource::Function;
        std::string(function).copy(terrain.HeightFunction, sizeof(terrain.HeightFunction) - 1);
        terrain.BaseSeed = seed;
        entity = world.Create(terrain).GetHandle();

        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::Terrain>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = entity;
        ctx.Undo = &undo;
        ctx.FrameRefreshCallbacks = &frameCallbacks;
        ctx.RequestInspectorRefresh = [this] { ++refreshRequests; };
        (*inspector)(ctx);
    }

    const Components::Terrain& Stored() const { return *world.GetComponent<Components::Terrain>(entity); }

    ECS::World world{nullptr};
    ECS::EntityHandle entity{};
    Editor::UndoRedoService undo;
    UIElement parent{"div"};
    std::vector<std::function<void()>> frameCallbacks;
    int refreshRequests = 0;
};

TEST_F(TerrainFunctionBaseRowsTest, AFunctionTerrainShowsItsSourceItsFunctionAmongTheRegisteredNamesAndItsSeed)
{
    ASSERT_NO_FATAL_FAILURE(Inspect(kBeta, 7u));
    auto* source = dynamic_cast<Dropdown*>(FindById(parent, "terrain-base-source"));
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->GetSelectedLabel(), "Function");

    auto* function = dynamic_cast<Dropdown*>(FindById(parent, "terrain-height-function"));
    ASSERT_NE(function, nullptr);
    EXPECT_EQ(function->GetSelectedValue(), kBeta);
    std::vector<std::string> options;
    CollectLabelTexts(*function->GetItemsContainer(), options);
    const auto alpha = std::ranges::find(options, kAlpha);
    const auto beta = std::ranges::find(options, kBeta);
    ASSERT_NE(alpha, options.end());
    ASSERT_NE(beta, options.end());
    EXPECT_LT(alpha, beta) << "the names are sorted";

    auto* seed = FindSeedPart<IntField>(parent);
    ASSERT_NE(seed, nullptr);
    EXPECT_EQ(seed->GetValue(), 7);
    EXPECT_TRUE(ShowsText(parent, "Height Function")) << "the row's label names what the docs call it";
    EXPECT_FALSE(ShowsText(parent, UnregisteredNotice(kBeta)));
    EXPECT_TRUE(ShowsText(parent, "Height Range"));
}

TEST_F(TerrainFunctionBaseRowsTest, ANameNoModuleRegistersStaysMarkedWithANotice)
{
    ASSERT_NO_FATAL_FAILURE(Inspect(kMissing, 1u));
    auto* function = dynamic_cast<Dropdown*>(FindById(parent, "terrain-height-function"));
    ASSERT_NE(function, nullptr);
    EXPECT_EQ(function->GetSelectedValue(), kMissing);
    EXPECT_EQ(function->GetSelectedLabel(), std::string(kMissing) + " (not registered)");
    EXPECT_TRUE(ShowsText(parent, UnregisteredNotice(kMissing)));
    EXPECT_FALSE(ShowsText(parent, "Height Range")) << "a flat base has no heights to span";
    EXPECT_EQ(FixedStringView(Stored().HeightFunction), kMissing) << "the stored name is never replaced";
}

TEST_F(TerrainFunctionBaseRowsTest, TheSectionIsBuiltAgainWhenAModuleRegistersTheStoredName)
{
    // A name registered outside a module stays registered, so each run of this test uses a new name.
    static int runs = 0;
    const std::string late = std::string(kLate) + "." + std::to_string(++runs);
    ASSERT_NO_FATAL_FAILURE(Inspect(late.c_str(), 1u));
    RunFrameCallbacks(frameCallbacks);
    EXPECT_EQ(refreshRequests, 0);

    TerrainECS::RegisterTerrainHeightFunction(late, &TerrainECS::MakeTerrainHeightFunction<Level>);
    RunFrameCallbacks(frameCallbacks);
    RunFrameCallbacks(frameCallbacks);
    EXPECT_EQ(refreshRequests, 1);
}

TEST_F(TerrainFunctionBaseRowsTest, ASeedEditAndAFunctionPickEachUndo)
{
    ASSERT_NO_FATAL_FAILURE(Inspect(kBeta, 7u));
    auto* seed = FindSeedPart<IntField>(parent);
    auto* function = dynamic_cast<Dropdown*>(FindById(parent, "terrain-height-function"));
    ASSERT_NE(seed, nullptr);
    ASSERT_NE(function, nullptr);

    ASSERT_NO_FATAL_FAILURE(TypeAndCommit(*seed, "123456789"));
    EXPECT_EQ(Stored().BaseSeed, 123456789u);
    function->SetSelectedValue(kAlpha);
    EXPECT_EQ(FixedStringView(Stored().HeightFunction), kAlpha);

    undo.Undo();
    EXPECT_EQ(FixedStringView(Stored().HeightFunction), kBeta);
    EXPECT_EQ(Stored().BaseSeed, 123456789u);
    undo.Undo();
    EXPECT_EQ(Stored().BaseSeed, 7u);
}

TEST_F(TerrainFunctionBaseRowsTest, TheDiceButtonSetsANewSeedAsOneUndoStep)
{
    ASSERT_NO_FATAL_FAILURE(Inspect(kBeta, 7u));
    auto* dice = FindSeedPart<Button>(parent);
    ASSERT_NE(dice, nullptr) << "the Seed row has no dice button";
    EXPECT_EQ(dice->GetTooltip(), "New seed");
    const size_t undoSteps = undo.GetUndoCount();

    dice->TriggerClick();
    EXPECT_NE(Stored().BaseSeed, 7u);
    EXPECT_LE(Stored().BaseSeed, static_cast<uint64>(INT_MAX)) << "the Seed field shows every seed it sets";
    EXPECT_EQ(undo.GetUndoCount(), undoSteps + 1);
    auto* seed = FindSeedPart<IntField>(parent);
    ASSERT_NE(seed, nullptr);
    EXPECT_EQ(static_cast<uint64>(seed->GetValue()), Stored().BaseSeed) << "the field shows the new seed";

    undo.Undo();
    EXPECT_EQ(Stored().BaseSeed, 7u);
}

TEST_F(TerrainFunctionBaseRowsTest, TheBaseHeaderShowsTheGeneratingBadgeWhileTheFunctionBuildsAndTheRowsKeepTheirPlace)
{
    g_ReleaseSlow = false;
    ASSERT_NO_FATAL_FAILURE(Inspect(kSlow, 3u));
    Foldout* base = FindSection(parent, "Base");
    ASSERT_NE(base, nullptr);
    Label* badge = FindHeaderBadge(*base);
    ASSERT_NE(badge, nullptr) << "the Base header has no badge";
    EXPECT_FALSE(badge->HasClass("hidden"));
    EXPECT_EQ(badge->GetText(), "Generating");
    EXPECT_EQ(badge->GetTooltip(), std::string("'") + kSlow + "' for seed 3...");
    EXPECT_TRUE(std::ranges::any_of(base->GetHeader()->GetChildren(), [](const auto& child) {
        return child->HasClass("inspector-component-section-title") && child->HasClass("foldout-title-badged");
    })) << "the badged title does not wear the class of the inspector's 37 % rule";
    const auto rowsWhileGenerating = RowsShown(*base);

    g_ReleaseSlow = true;
    EXPECT_TRUE(WaitUntilHidden(*badge, frameCallbacks)) << "the badge stays after the world is generated";
    EXPECT_EQ(RowsShown(*base), rowsWhileGenerating) << "a row appeared or hid, so the rows below moved";
}

TEST_F(TerrainFunctionBaseRowsTest, TheGeneratingBadgeShowsAfterASeedTypedInTheSeedField)
{
    g_ReleaseSlow = true;
    ASSERT_NO_FATAL_FAILURE(Inspect(kSlow, 5u));
    Foldout* base = FindSection(parent, "Base");
    auto* seed = FindSeedPart<IntField>(parent);
    ASSERT_NE(base, nullptr);
    ASSERT_NE(seed, nullptr);
    Label* badge = FindHeaderBadge(*base);
    ASSERT_NE(badge, nullptr) << "the Base header has no badge";
    ASSERT_TRUE(WaitUntilHidden(*badge, frameCallbacks)) << "the world for seed 5 is never generated";

    g_ReleaseSlow = false;
    ASSERT_NO_FATAL_FAILURE(TypeAndCommit(*seed, "6"));
    RunFrameCallbacks(frameCallbacks);
    EXPECT_FALSE(badge->HasClass("hidden"));
    EXPECT_EQ(badge->GetTooltip(), std::string("'") + kSlow + "' for seed 6...");

    g_ReleaseSlow = true;
    EXPECT_TRUE(WaitUntilHidden(*badge, frameCallbacks)) << "the badge stays after the world is generated";
}

} // namespace
