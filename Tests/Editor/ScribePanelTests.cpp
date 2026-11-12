// The Scribe panel: a state-tree program file opens in it through the asset opener, its states show as an
// outline in file order, an edit writes the file back through the writer, a refusal shows on the row it
// names, and undo restores the edited field.
#include <gtest/gtest.h>

#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "ECS/ModuleRegistration.h"
#include "Editor/Registries/AssetOpenerRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "InspectorLayoutFixture.h"
#include "Panels/ConfirmActionModal.h"
#include "Panels/SaveSceneChangesModal.h"
#include "Panels/Scribe/ScribeDocument.h"
#include "Panels/Scribe/ScribePanel.h"
#include "Panels/Scribe/ScribeWorkspace.h"
#include "Types/FormatNumber.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIEvents.h"
#include "UI/UiDispatcher.h"
#include "UI/UIManager.h"
#include "UndoRedo/UndoRedoService.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
using Row = ScribeOutlineRow;

constexpr const char* kGuardProgram = R"({ "root": { "id": 1, "name": "Guard", "children": [
  { "id": 2, "name": "Patrol", "tasks": ["Walk"], "transitions": [ { "to": 3, "on": "event", "event": "Alarm" } ] },
  { "id": 3, "name": "Chase", "tasks": ["Walk"], "enterIf": "SeesEnemy",
    "transitions": [ { "to": 2, "on": "timeout", "ticks": 100 } ] } ] } })";

constexpr const char* kCommanderProgram = R"({ "root": { "id": 1, "name": "Commander", "policy": "utility",
  "utility": { "commitmentMargin": 0.1, "minimumTicks": 20 }, "children": [
  { "id": 2, "name": "Attack", "tasks": ["Walk"], "option": { "weight": 2, "considerations": [
    { "input": "ReadySoldiers", "min": 0, "max": 6, "curve": { "preset": "linear" } },
    { "input": "Morale", "curve": { "preset": "logistic", "shape": 8 } } ] } },
  { "id": 3, "name": "Wait", "tasks": ["Walk"], "option": { "considerations": [
    { "input": "Threat", "curve": { "preset": "linear" } } ] } } ] } })";

AIECS::AiTaskStatus TickWalk(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

void TestNothing(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<uint8> out)
{
    for (uint8& value : out)
        value = 0;
}

void ReadNothing(const ECS::World&, std::span<const ECS::EntityHandle>, std::span<float32> out)
{
    for (float32& value : out)
        value = 0.0f;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A fixture program in the writer's own form, so that a save of an unedited program writes the same text.
std::string CanonicalProgram(const char* program)
{
    AIECS::AiStateTreeSource source;
    std::string error;
    std::string text;
    EXPECT_TRUE(AIECS::ParseAiStateTreeSource(program, source, error)) << error;
    EXPECT_TRUE(AIECS::WriteAiStateTree(source, text, error)) << error;
    return text;
}

std::vector<const Row*> RowsOfKind(const std::vector<Row>& rows, Row::Kind kind)
{
    std::vector<const Row*> out;
    for (const Row& row : rows)
        if (row.RowKind == kind)
            out.push_back(&row);
    return out;
}

class ScribePanelTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Directory = TestUtils::MakeUniqueTempDirectory("ScribePanelTests");
        std::filesystem::create_directories(Directory);
        File = Directory / "Guard.scribe";
        std::ofstream(File, std::ios::binary) << CanonicalProgram(kGuardProgram);
        Names.RegisterTask({nullptr, &TickWalk, nullptr, "Walk"});
        Names.RegisterEventType("Alarm");
        Names.RegisterEventType("Retreat");
    }

    // Writes the Commander program, a Utility state with two options, over the fixture file.
    void WriteCommander()
    {
        std::ofstream(File, std::ios::binary | std::ios::trunc) << CanonicalProgram(kCommanderProgram);
        for (const char* input : {"ReadySoldiers", "Morale", "Threat"})
            Names.RegisterInput(&ReadNothing, AIECS::AiInputThreading::CallingThread, input);
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(Directory, ignored);
    }

    std::shared_ptr<ScribeDocument> Open()
    {
        std::string error;
        std::shared_ptr<ScribeDocument> document = ScribeDocument::Open(File, &Undo, &Names, error);
        EXPECT_NE(document, nullptr) << error;
        return document;
    }

    std::filesystem::path Directory;
    std::filesystem::path File;
    AIECS::AiRegistry Names;
    UndoRedoService Undo;
};

const std::filesystem::path kLayoutRelative = std::filesystem::path("UI") / "panels" / "Scribe" / "ScribePanel.uxml";

bool WaitUntil(const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

const UIElement* FindByClass(const UIElement& element, const char* className)
{
    if (element.HasClass(className))
        return &element;
    for (const std::unique_ptr<UIElement>& child : element.GetChildren())
        if (const UIElement* found = FindByClass(*child, className))
            return found;
    return nullptr;
}

// The number of row badges under `element` whose tooltip is `refusal`.
size_t CountBadgesFor(const UIElement& element, const std::string& refusal)
{
    size_t count = element.HasClass("scribe-row-badge") && element.GetTooltip() == refusal ? 1 : 0;
    for (const std::unique_ptr<UIElement>& child : element.GetChildren())
        count += CountBadgesFor(*child, refusal);
    return count;
}

// The first child of `root` that is a T; null when there is none.
template <typename T>
T* FindChildOfType(UIElement& root)
{
    for (const std::unique_ptr<UIElement>& child : root.GetChildren())
        if (T* found = dynamic_cast<T*>(child.get()))
            return found;
    return nullptr;
}

// The first element under `root` that is a T, depth first; null when there is none.
template <typename T>
T* FindFirstOfType(UIElement& root)
{
    if (T* found = dynamic_cast<T*>(&root))
        return found;
    for (const std::unique_ptr<UIElement>& child : root.GetChildren())
        if (T* found = FindFirstOfType<T>(*child))
            return found;
    return nullptr;
}

// The outline row whose text label reads `text`; null when there is none.
UIElement* FindRowWithText(UIElement& element, const std::string& text)
{
    if (element.HasClass("scribe-row"))
    {
        const Label* label = dynamic_cast<const Label*>(FindByClass(element, "scribe-row-text"));
        return label && label->GetText() == text ? &element : nullptr;
    }
    for (const std::unique_ptr<UIElement>& child : element.GetChildren())
        if (UIElement* found = FindRowWithText(*child, text))
            return found;
    return nullptr;
}

void SendPointer(UIElement& element, EventId id, float x)
{
    UIEvent event{};
    event.Id = id;
    event.X = x;
    event.Target = &element;
    event.CurrentTarget = &element;
    element.DispatchEvent(event);
}

// Adds each outline row under `element` to `rows`, in the outline's order.
void CollectRows(const UIElement& element, std::vector<const UIElement*>& rows)
{
    if (element.HasClass("scribe-row"))
        rows.push_back(&element);
    for (const std::unique_ptr<UIElement>& child : element.GetChildren())
        CollectRows(*child, rows);
}

// Lays the panel out at `width` and returns the notice text when it starts with `expected`, or the last text
// after the wait.
// The Scribe panel of `workspace` as the root of `manager`, 600 by 400 pixels.
ScribePanel* ShowPanel(UIManager& manager, const std::shared_ptr<ScribeWorkspace>& workspace)
{
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    manager.SetRoot(std::move(owned));
    UILayoutAccess::SetLastLayoutRect(*panel, 0.0f, 0.0f, 600.0f, 400.0f);
    panel->OnPostLayout();
    return panel;
}

// Selects the option Attack and returns the text editor of its Weight field; null when either is missing.
TextInput* SelectAttackWeight(UIManager& manager, ScribePanel& panel)
{
    UIElement* attack = nullptr;
    if (!WaitUntil(
            [&manager, &attack, &panel]
            {
                manager.GetDispatcher()->Drain();
                attack = FindRowWithText(panel, "Attack");
                return attack != nullptr;
            }))
        return nullptr;
    SendPointer(*attack, kEventMouseDown, 0.0f);
    FloatField* weight = nullptr;
    if (!WaitUntil(
            [&manager, &weight, &panel]
            {
                manager.GetDispatcher()->Drain();
                weight = FindFirstOfType<FloatField>(panel);
                return weight != nullptr;
            }))
        return nullptr;
    return FindChildOfType<TextInput>(*weight);
}

std::string NoticeTextAtWidth(UIManager& manager, ScribePanel& panel, float width, const std::string& expected)
{
    UILayoutAccess::SetLastLayoutRect(panel, 0.0f, 0.0f, width, 291.0f);
    panel.OnPostLayout();
    std::string text;
    WaitUntil(
        [&manager, &panel, &text, &expected]
        {
            manager.GetDispatcher()->Drain();
            const Label* notice = dynamic_cast<const Label*>(FindByClass(panel, "scribe-notice"));
            text = notice ? notice->GetText() : std::string();
            return text.rfind(expected, 0) == 0;
        });
    return text;
}

// The panel binds its shipped layout through the editor source. The headless engine mounts none, so the
// suite mounts a copy of the staged layout and removes it at the end.
class ScribePanelLayoutTests : public ScribePanelTests
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
        s_Root = TestUtils::MakeUniqueTempDirectory("ScribePanelLayoutTests");
        std::filesystem::create_directories((s_Root / kLayoutRelative).parent_path());
        std::filesystem::copy_file(TestPaths::ExecutableDirectory() / "Assets" / kLayoutRelative,
                                   s_Root / kLayoutRelative);

        AssetSourceDesc source{};
        source.Alias = std::string(kAssetSourceAliasEditor);
        source.Root = s_Root;
        source.RegisterFileWatcher = false;
        ASSERT_TRUE(Assets().RegisterSource(source)) << "the editor source alias was already mounted";
        Assets().WaitForStartupScan(kAssetSourceAliasEditor);
    }

    static void TearDownTestSuite()
    {
        bool removed = false;
        if (Assets().BeginUnregisterSource(kAssetSourceAliasEditor, [&removed](bool) { removed = true; }))
            EXPECT_TRUE(WaitUntil(
                [&removed]
                {
                    Assets().Update();
                    return removed;
                }));
        std::error_code ignored;
        std::filesystem::remove_all(s_Root, ignored);
    }

    static AssetManager& Assets() { return EngineCore::GetInstance().GetAssetManager(); }

    static inline std::filesystem::path s_Root;
};
} // namespace

// An open of a .scribe file goes to the Scribe opener, which opens the panel and shows the file in it.
TEST_F(ScribePanelTests, AnOpenOfAProgramFileShowsItInTheScribePanel)
{
    // The registries are the process's; the undo service they keep outlives every test.
    static UndoRedoService registeredUndo;
    ScribePanel::Register(&registeredUndo);
    std::vector<std::string> openedPanels;
    EditorPanelRegistry::Get().SetPanelOpener([&openedPanels](const std::string& id) { openedPanels.push_back(id); });

    EXPECT_TRUE(AssetOpenerRegistry::Get().Open(File));
    EditorPanelRegistry::Get().SetPanelOpener({});

    EXPECT_EQ(openedPanels, std::vector<std::string>{kScribePanelId});
    EditorPanelDescriptor descriptor;
    ASSERT_TRUE(EditorPanelRegistry::Get().TryGetPanel(kScribePanelId, descriptor));
    std::unique_ptr<UIElement> panel = descriptor.Factory();
    auto* scribe = dynamic_cast<ScribePanel*>(panel.get());
    ASSERT_NE(scribe, nullptr);
    ASSERT_NE(scribe->GetDocument(), nullptr) << "the panel shows no program after the open";
    EXPECT_EQ(scribe->GetDocument()->GetPath(), File);
}

TEST_F(ScribePanelTests, TheOutlineListsTheStatesInFileOrderWithTheirItems)
{
    Names.RegisterCondition(&TestNothing, "SeesEnemy");
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);
    const std::vector<Row> rows = document->BuildOutline();

    const std::vector<const Row*> states = RowsOfKind(rows, Row::Kind::State);
    ASSERT_EQ(states.size(), 3u);
    EXPECT_EQ(states[0]->Text, "Guard");
    EXPECT_EQ(states[1]->Text, "Patrol");
    EXPECT_EQ(states[2]->Text, "Chase");
    EXPECT_EQ(states[0]->Depth, 0);
    EXPECT_EQ(states[2]->Depth, 1);
    EXPECT_EQ(states[0]->Detail, "picks the first child") << "a state with children shows its policy in words";
    EXPECT_EQ(states[1]->Detail, "") << "a state without children has no policy to show";

    const std::vector<const Row*> conditions = RowsOfKind(rows, Row::Kind::EnterCondition);
    ASSERT_EQ(conditions.size(), 1u);
    EXPECT_EQ(conditions[0]->Text, "SeesEnemy");
    EXPECT_EQ(conditions[0]->State, states[2]->State);
    const std::vector<const Row*> transitions = RowsOfKind(rows, Row::Kind::Transition);
    ASSERT_EQ(transitions.size(), 2u);
    EXPECT_EQ(transitions[0]->Text, "Chase");
    EXPECT_EQ(transitions[0]->Detail, "on event Alarm");
    EXPECT_EQ(transitions[1]->Detail, "after 100 ticks");

    const std::vector<const Row*> groups = RowsOfKind(rows, Row::Kind::Group);
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0]->Text, "Events");
    EXPECT_EQ(groups[1]->Text, "Inputs");
    EXPECT_EQ(RowsOfKind(rows, Row::Kind::Name).front()->Text, "Alarm");
    EXPECT_TRUE(document->GetRefusals().empty()) << document->GetRefusals().front().Message;
}

TEST_F(ScribePanelTests, ASavedRenameChangesOnlyTheNameInTheFile)
{
    Names.RegisterCondition(&TestNothing, "SeesEnemy");
    const std::string before = ReadText(File);
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    document->RenameState(1, "Wander");
    EXPECT_TRUE(document->IsDirty());
    std::string error;
    ASSERT_TRUE(document->Save(error)) << error;

    std::string expected = before;
    const size_t at = expected.find("\"Patrol\"");
    ASSERT_NE(at, std::string::npos) << before;
    expected.replace(at, 8, "\"Wander\"");
    EXPECT_EQ(ReadText(File), expected);
    EXPECT_FALSE(document->IsDirty());
}

TEST_F(ScribePanelTests, ARefusalShowsOnTheRowItNamesAndClearsWhenTheEditFixesIt)
{
    Names.RegisterCondition(&TestNothing, "Alerted");
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->GetRefusals().size(), 1u);
    EXPECT_EQ(document->GetRefusals()[0].StateId, 3u);
    EXPECT_EQ(document->GetRefusals()[0].Item, AI::StateTreeCompileItem::EnterCondition);

    const std::vector<Row> rows = document->BuildOutline();
    std::vector<const Row*> refused;
    for (const Row& row : rows)
        if (!row.Refusal.empty())
            refused.push_back(&row);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_EQ(refused[0]->RowKind, Row::Kind::EnterCondition);
    EXPECT_EQ(refused[0]->Text, "SeesEnemy");
    EXPECT_EQ(refused[0]->Refusal, "Chase > Enter if SeesEnemy: no loaded code registers the condition 'SeesEnemy'. "
                                   "Choose a registered condition in Enter if, or register it in the game's code with "
                                   "GE_REGISTER_AI_CONDITION. A registration clears this row when the code reloads.")
        << "the refusal names its row, then gives the fix in its field first";

    document->SetEnterCondition(2, "Alerted");
    EXPECT_TRUE(document->GetRefusals().empty()) << document->GetRefusals().front().Message;
    for (const Row& row : document->BuildOutline())
        EXPECT_TRUE(row.Refusal.empty()) << row.Text << ": " << row.Refusal;
}

// A program that does not compile still gets its name check, so each row that needs a fix shows at once.
TEST_F(ScribePanelTests, ACompileRefusalAndAnUnknownNameShowOnTheirRowsAtOnce)
{
    std::ofstream(File, std::ios::binary | std::ios::trunc) << R"({ "root": { "id": 1, "name": "Guard", "children": [
  { "id": 2, "name": "Patrol", "tasks": ["Walk"], "transitions": [ { "to": 9, "on": "event", "event": "Alarm" } ] },
  { "id": 3, "name": "Chase", "tasks": ["Walk"], "enterIf": "SeesEnemy" } ] } })";
    Names.RegisterCondition(&TestNothing, "Alerted");
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->GetRefusals().size(), 2u);
    EXPECT_EQ(document->GetRefusals()[0].Item, AI::StateTreeCompileItem::Transition) << "the compile refusal is first";
    EXPECT_EQ(document->GetRefusals()[1].Item, AI::StateTreeCompileItem::EnterCondition);

    std::vector<const Row*> refused;
    const std::vector<Row> rows = document->BuildOutline();
    for (const Row& row : rows)
        if (!row.Refusal.empty())
            refused.push_back(&row);
    ASSERT_EQ(refused.size(), 2u);
    EXPECT_EQ(refused[0]->RowKind, Row::Kind::Transition);
    EXPECT_NE(refused[0]->Refusal.find("which is not in the program"), std::string::npos) << refused[0]->Refusal;
    EXPECT_EQ(refused[1]->RowKind, Row::Kind::EnterCondition);
    EXPECT_EQ(refused[1]->Refusal.rfind("Chase > Enter if SeesEnemy: no loaded code registers the "
                                        "condition 'SeesEnemy'.",
                                        0),
              0u)
        << refused[1]->Refusal;

    document->SetEnterCondition(2, "Alerted");
    ASSERT_EQ(document->GetRefusals().size(), 1u);
    EXPECT_EQ(document->GetRefusals()[0].Item, AI::StateTreeCompileItem::Transition);
    for (const Row& row : document->BuildOutline())
        if (row.RowKind == Row::Kind::EnterCondition)
            EXPECT_TRUE(row.Refusal.empty()) << row.Text << ": " << row.Refusal;
}

TEST_F(ScribePanelTests, UndoRestoresTheEditedFieldAndRedoAppliesItAgain)
{
    Names.RegisterCondition(&TestNothing, "SeesEnemy");
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    document->SetTransitionEvent(1, 0, "Retreat");
    document->RenameState(2, "Hunt");
    ASSERT_EQ(Undo.GetUndoCount(), 2u);
    EXPECT_EQ(document->GetSource().States[2].Name, "Hunt");
    EXPECT_EQ(document->GetSource().EventNames, std::vector<std::string>{"Retreat"});

    Undo.Undo();
    EXPECT_EQ(document->GetSource().States[2].Name, "Chase");
    Undo.Undo();
    EXPECT_EQ(document->GetSource().EventNames, std::vector<std::string>{"Alarm"});
    EXPECT_FALSE(document->IsDirty()) << "the program is back to the file's text";

    Undo.Redo();
    EXPECT_EQ(document->GetSource().EventNames, std::vector<std::string>{"Retreat"});
    EXPECT_TRUE(document->IsDirty());
}

// The refused row carries the inspector's badge classes, so that the badge draws as the red error pill.
TEST_F(ScribePanelLayoutTests, ARefusedRowShowsTheInspectorErrorBadge)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    manager.SetRoot(std::move(owned));
    UILayoutAccess::SetLastLayoutRect(*panel, 0.0f, 0.0f, 600.0f, 400.0f);
    panel->OnPostLayout();

    const UIElement* badge = nullptr;
    ASSERT_TRUE(WaitUntil(
        [&manager, &badge, panel]
        {
            manager.GetDispatcher()->Drain();
            badge = FindByClass(*panel, "scribe-row-badge");
            return badge != nullptr;
        }))
        << "no badge on the row of the unknown condition SeesEnemy";
    EXPECT_TRUE(badge->HasClass("inspector-section-header-badge"));
    EXPECT_TRUE(badge->HasClass("inspector-section-header-badge-error"));
}

// A panel narrower than 480 px gives the count of refused rows alone in the notice, with the causes in its tooltip.
// A wider panel gives the causes in the notice.
TEST_F(ScribePanelLayoutTests, ANarrowPanelGivesTheCountOfRefusedRowsAloneInTheNotice)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    manager.SetRoot(std::move(owned));

    const std::string wideText =
        "1 condition is not registered: SeesEnemy (1 row). Select a red row to see its fix.";
    const std::string narrowText = "Refused: 1 row needs a fix. Select a red row to see its fix.";
    EXPECT_EQ(NoticeTextAtWidth(manager, *panel, 360.0f, narrowText), narrowText);
    EXPECT_EQ(FindByClass(*panel, "scribe-notice")->GetTooltip(), wideText);
    EXPECT_EQ(NoticeTextAtWidth(manager, *panel, 600.0f, wideText), wideText);
}

// A selected row that is refused shows its refusal over its fields in an error notice, at every width.
TEST_F(ScribePanelLayoutTests, ASelectedRefusedRowShowsItsRefusalOverItsFields)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    manager.SetRoot(std::move(owned));
    UILayoutAccess::SetLastLayoutRect(*panel, 0.0f, 0.0f, 600.0f, 400.0f);
    panel->OnPostLayout();

    UIElement* row = nullptr;
    ASSERT_TRUE(WaitUntil(
        [&manager, &row, panel]
        {
            manager.GetDispatcher()->Drain();
            row = FindRowWithText(*panel, "SeesEnemy");
            return row != nullptr;
        }));
    SendPointer(*row, kEventMouseDown, 0.0f);
    const EditorUI::InspectorNotice* notice = nullptr;
    ASSERT_TRUE(WaitUntil(
        [&manager, &notice, panel]
        {
            manager.GetDispatcher()->Drain();
            notice = FindFirstOfType<EditorUI::InspectorNotice>(*panel);
            return notice != nullptr;
        }))
        << "no refusal over the fields of the selected row";
    EXPECT_TRUE(notice->HasClass("inspector-notice-error"));
    const Label* text = dynamic_cast<const Label*>(FindByClass(*notice, "inspector-notice-text"));
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetText().rfind("Chase > Enter if SeesEnemy: no loaded code registers the "
                                    "condition 'SeesEnemy'.",
                                    0),
              0u)
        << text->GetText();
}

// The registry refuses a repeated state name once; the badge shows on every state row with that name.
TEST_F(ScribePanelLayoutTests, EachStateWithARepeatedNameShowsTheBadge)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    for (uint16 state = 0; state < 3; ++state)
        workspace->GetDocument()->RenameState(state, "Lookout");
    UIManager manager{nullptr, &Assets()};
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    manager.SetRoot(std::move(owned));
    UILayoutAccess::SetLastLayoutRect(*panel, 0.0f, 0.0f, 600.0f, 400.0f);
    panel->OnPostLayout();

    const std::string refusal = "3 states are named Lookout. Give each state its own name.";
    size_t badges = 0;
    WaitUntil(
        [&manager, &badges, &refusal, panel]
        {
            manager.GetDispatcher()->Drain();
            badges = CountBadgesFor(*panel, refusal);
            return badges == 3;
        });
    EXPECT_EQ(badges, 3u) << "one badge for each state named Lookout";
    // The three Lookout rows and the Enter if SeesEnemy row.
    const std::string narrowText = "Refused: 4 rows need a fix. Select a red row to see its fix.";
    EXPECT_EQ(NoticeTextAtWidth(manager, *panel, 360.0f, narrowText), narrowText);
}

// The open prompt goes on the window's root, so its backdrop covers the whole window and not the panel only.
TEST_F(ScribePanelLayoutTests, TheOpenPromptOverUnsavedEditsCoversTheWindow)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    workspace->GetDocument()->RenameState(2, "Hunt");
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    manager.SetRoot(std::make_unique<UIElement>());
    UIElement* root = manager.GetRootElement();
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    root->AddChild(std::move(owned));

    panel->Refresh();
    EXPECT_EQ(FindByClass(*panel, "modal-overlay"), nullptr) << "the prompt covers the panel only";
    EXPECT_NE(FindByClass(*root, "modal-overlay"), nullptr) << "no prompt for the open over unsaved edits";
}

// The Save of the open prompt asks to confirm a refused program, as the Save button does. A cancel of the
// confirm cancels the open and keeps the shown program with its edits.
TEST_F(ScribePanelLayoutTests, TheSaveOfTheOpenPromptAsksToConfirmARefusedProgram)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    const std::shared_ptr<ScribeDocument> shown = workspace->GetDocument();
    ASSERT_NE(shown, nullptr);
    ASSERT_FALSE(shown->GetRefusals().empty()) << "the program must be refused for this test";
    shown->RenameState(2, "Hunt");
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    manager.SetRoot(std::make_unique<UIElement>());
    UIElement* root = manager.GetRootElement();
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    root->AddChild(std::move(owned));

    panel->Refresh();
    SaveSceneChangesModal* openPrompt = FindChildOfType<SaveSceneChangesModal>(*root);
    ASSERT_NE(openPrompt, nullptr) << "no prompt for the open over unsaved edits";
    openPrompt->ChooseSave();
    ConfirmActionModal* saveConfirm = FindChildOfType<ConfirmActionModal>(*root);
    ASSERT_NE(saveConfirm, nullptr) << "the Save wrote a refused program without asking";
    EXPECT_TRUE(saveConfirm->IsVisible());
    EXPECT_TRUE(shown->IsDirty());
    panel->Refresh();
    EXPECT_FALSE(openPrompt->IsVisible()) << "the open prompt showed again over the confirm";

    saveConfirm->ChooseCancel();
    EXPECT_TRUE(workspace->GetPendingOpen().empty()) << "the cancel kept the open waiting";
    EXPECT_EQ(workspace->GetDocument(), shown);
    EXPECT_TRUE(shown->IsDirty());
}

// The open prompt's buttons have ids of their own: a lookup by id finds the button of the shown prompt, not the
// button of the scene editor's hidden prompt earlier in the window.
TEST_F(ScribePanelLayoutTests, TheOpenPromptButtonsHaveIdsOfTheirOwn)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    ASSERT_NE(workspace->GetDocument(), nullptr);
    workspace->GetDocument()->RenameState(2, "Hunt");
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    manager.SetRoot(std::make_unique<UIElement>());
    UIElement* root = manager.GetRootElement();
    root->AddChild(std::make_unique<SaveSceneChangesModal>("scene"));
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    root->AddChild(std::move(owned));

    panel->Refresh();
    SaveSceneChangesModal* openPrompt = nullptr;
    for (const auto& child : root->GetChildren())
        if (auto* prompt = dynamic_cast<SaveSceneChangesModal*>(child.get()); prompt && prompt->IsVisible())
            openPrompt = prompt;
    ASSERT_NE(openPrompt, nullptr) << "no prompt for the open over unsaved edits";
    for (const char* id : {"scribe-save-changes-dont-save", "scribe-save-changes-cancel", "scribe-save-changes-save"})
    {
        const UIElement* button = root->FindById(HashStringId(id));
        while (button && button != openPrompt)
            button = button->GetParent();
        EXPECT_EQ(button, openPrompt) << id << " is not a button of the shown prompt";
    }
}

// The open prompt takes the focus at the next drain, so Enter and Escape reach it.
TEST_F(ScribePanelLayoutTests, TheOpenPromptTakesTheFocusAtTheNextDrain)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    ASSERT_NE(workspace->GetDocument(), nullptr);
    workspace->GetDocument()->RenameState(2, "Hunt");
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    manager.SetRoot(std::make_unique<UIElement>());
    UIElement* root = manager.GetRootElement();
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    root->AddChild(std::move(owned));

    panel->Refresh();
    SaveSceneChangesModal* openPrompt = FindChildOfType<SaveSceneChangesModal>(*root);
    ASSERT_NE(openPrompt, nullptr) << "no prompt for the open over unsaved edits";
    manager.GetDispatcher()->Drain();
    const UIElement* focused = root->FindById(HashStringId(manager.GetFocusedElementId()));
    while (focused && focused != openPrompt)
        focused = focused->GetParent();
    EXPECT_EQ(focused, openPrompt) << "the focus is not on the open prompt";
}

// An open over unsaved edits waits for the user's choice, and a file that does not read keeps the shown program.
TEST_F(ScribePanelTests, AnOpenOverUnsavedEditsKeepsTheShownProgram)
{
    ScribeWorkspace workspace(&Undo, &Names);
    workspace.Open(File);
    const std::shared_ptr<ScribeDocument> shown = workspace.GetDocument();
    ASSERT_NE(shown, nullptr);
    shown->RenameState(2, "Hunt");
    ASSERT_TRUE(shown->IsDirty());

    workspace.Open(File);
    EXPECT_EQ(workspace.GetDocument(), shown) << "the open dropped the unsaved edits without asking";
    EXPECT_EQ(workspace.GetPendingOpen(), File);
    workspace.CancelPendingOpen();
    EXPECT_TRUE(workspace.GetPendingOpen().empty());
    EXPECT_EQ(workspace.GetDocument(), shown);

    workspace.Open(File);
    workspace.ConfirmPendingOpen();
    ASSERT_NE(workspace.GetDocument(), shown) << "the confirmed open kept the edited program";
    EXPECT_FALSE(workspace.GetDocument()->IsDirty());

    const std::shared_ptr<ScribeDocument> clean = workspace.GetDocument();
    workspace.Open(Directory / "Missing.scribe");
    EXPECT_EQ(workspace.GetDocument(), clean) << "a file that does not read replaced the shown program";
    EXPECT_FALSE(workspace.GetOpenError().empty());
}

// Each unregistered name holds a badge on its own row, and a repeated state name on the row of each state with it.
TEST_F(ScribePanelTests, EachUnregisteredNameAndEachRepeatedStateNameIsRefused)
{
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);
    document->RenameState(2, "Patrol");

    ASSERT_EQ(document->GetRefusals().size(), 2u) << "the registry gives the repeated name once";
    std::vector<const Row*> refused;
    const std::vector<Row> rows = document->BuildOutline();
    for (const Row& row : rows)
        if (!row.Refusal.empty())
            refused.push_back(&row);
    ASSERT_EQ(refused.size(), 3u);
    const std::string repeated = "2 states are named Patrol. Give each state its own name.";
    EXPECT_EQ(refused[0]->RowKind, Row::Kind::State) << "the first state named Patrol";
    EXPECT_EQ(refused[0]->State, 1);
    EXPECT_EQ(refused[0]->Refusal, repeated);
    EXPECT_EQ(refused[1]->RowKind, Row::Kind::State) << "the second state named Patrol";
    EXPECT_EQ(refused[1]->State, 2);
    EXPECT_EQ(refused[1]->Refusal, repeated);
    EXPECT_EQ(refused[2]->RowKind, Row::Kind::EnterCondition);
    EXPECT_EQ(refused[2]->Text, "SeesEnemy");
}

// The panel checks the names again when the registry changes, so a name that the game registers later clears.
TEST_F(ScribePanelTests, ANameThatTheGameRegistersLaterClearsTheRefusal)
{
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    ScribePanel panel(workspace);
    panel.Refresh();
    ASSERT_NE(panel.GetDocument(), nullptr);
    ASSERT_EQ(panel.GetDocument()->GetRefusals().size(), 1u);

    Names.RegisterCondition(&TestNothing, "SeesEnemy");
    panel.Refresh();
    EXPECT_TRUE(panel.GetDocument()->GetRefusals().empty()) << panel.GetDocument()->GetRefusals().front().Message;
}

// An undo step whose program is gone changes nothing and does not touch the freed program.
TEST_F(ScribePanelTests, AnUndoAfterTheProgramClosesChangesNothing)
{
    std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);
    document->RenameState(2, "Hunt");
    ASSERT_EQ(Undo.GetUndoCount(), 1u);
    document.reset();
    Undo.Undo();
    EXPECT_EQ(Undo.GetUndoCount(), 0u);
}

// Each consideration of an option is a row under the option's state, after its enter condition.
TEST_F(ScribePanelTests, TheOutlineListsEachConsiderationUnderItsOption)
{
    WriteCommander();
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);
    const std::vector<Row> rows = document->BuildOutline();

    std::vector<std::string> lines;
    for (const Row& row : rows)
        if (row.RowKind == Row::Kind::State || row.RowKind == Row::Kind::Consideration)
            lines.push_back(std::to_string(row.Depth) + " " + row.Text + ": " + row.Detail);
    EXPECT_EQ(lines, (std::vector<std::string>{"0 Commander: picks the child by utility", "1 Attack: rank 0, weight 2",
                                               "2 ReadySoldiers: 0 to 6, linear", "2 Morale: 0 to 1, logistic",
                                               "1 Wait: rank 0, weight 1", "2 Threat: 0 to 1, linear"}));
    const std::vector<const Row*> considerations = RowsOfKind(rows, Row::Kind::Consideration);
    ASSERT_EQ(considerations.size(), 3u);
    EXPECT_EQ(considerations[1]->State, 1);
    EXPECT_EQ(considerations[1]->Item, 1);
    EXPECT_EQ(considerations[2]->State, 2);
    EXPECT_EQ(considerations[2]->Item, 0);
    EXPECT_TRUE(document->GetRefusals().empty()) << document->GetRefusals().front().Message;
}

TEST_F(ScribePanelTests, ASavedRangeEditChangesOnlyThatFieldInTheFile)
{
    WriteCommander();
    const std::string before = ReadText(File);
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    document->SetConsiderationRange(1, 0, 0.0f, 8.0f, ScribeDocument::Phase::Commit);
    std::string error;
    ASSERT_TRUE(document->Save(error)) << error;

    std::string expected = before;
    const std::string key = "\"max\": ";
    const size_t at = expected.find(key);
    ASSERT_NE(at, std::string::npos) << before;
    ASSERT_EQ(expected[at + key.size()], '6') << before;
    expected[at + key.size()] = '8';
    EXPECT_EQ(ReadText(File), expected);
}

// The previews of a drag take no undo step; the commit at its end takes one, and its undo restores the start.
TEST_F(ScribePanelTests, ADragIsOneUndoStepAndUndoRestoresTheStartValue)
{
    WriteCommander();
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);
    const auto weight = [&document]
    { return document->GetSource().Definition.States[0].Utility.Options[0].Weight; };

    document->SetOptionWeight(1, 2.5f, ScribeDocument::Phase::Preview);
    document->SetOptionWeight(1, 3.0f, ScribeDocument::Phase::Preview);
    EXPECT_EQ(weight(), 3.0f) << "a preview shows the value";
    EXPECT_EQ(Undo.GetUndoCount(), 0u);
    document->SetOptionWeight(1, 3.5f, ScribeDocument::Phase::Commit);
    EXPECT_EQ(weight(), 3.5f);
    ASSERT_EQ(Undo.GetUndoCount(), 1u);

    Undo.Undo();
    EXPECT_EQ(weight(), 2.0f);
    EXPECT_FALSE(document->IsDirty()) << "the program is back to the file's text";
}

TEST_F(ScribePanelTests, AnUnregisteredInputShowsItsRefusalOnItsConsiderationRow)
{
    std::ofstream(File, std::ios::binary | std::ios::trunc) << CanonicalProgram(kCommanderProgram);
    Names.RegisterInput(&ReadNothing, AIECS::AiInputThreading::CallingThread, "ReadySoldiers");
    Names.RegisterInput(&ReadNothing, AIECS::AiInputThreading::CallingThread, "Threat");
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    std::vector<const Row*> refused;
    const std::vector<Row> rows = document->BuildOutline();
    for (const Row& row : rows)
        if (!row.Refusal.empty() && row.RowKind != Row::Kind::Name)
            refused.push_back(&row);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_EQ(refused[0]->RowKind, Row::Kind::Consideration);
    EXPECT_EQ(refused[0]->Text, "Morale");
    EXPECT_NE(refused[0]->Refusal.find("'Morale'"), std::string::npos) << refused[0]->Refusal;
}

// A drag step shows its preview in the outline and keeps the field that the user drags; the drag end is one undo step.
TEST_F(ScribePanelLayoutTests, ADragStepUpdatesTheOutlineAndKeepsTheDraggedField)
{
    WriteCommander();
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    manager.SetRoot(std::move(owned));
    UILayoutAccess::SetLastLayoutRect(*panel, 0.0f, 0.0f, 600.0f, 400.0f);
    panel->OnPostLayout();

    UIElement* attack = nullptr;
    ASSERT_TRUE(WaitUntil(
        [&manager, &attack, panel]
        {
            manager.GetDispatcher()->Drain();
            attack = FindRowWithText(*panel, "Attack");
            return attack != nullptr;
        }));
    SendPointer(*attack, kEventMouseDown, 0.0f);
    FloatField* weight = nullptr;
    ASSERT_TRUE(WaitUntil(
        [&manager, &weight, panel]
        {
            manager.GetDispatcher()->Drain();
            weight = FindFirstOfType<FloatField>(*panel);
            return weight != nullptr;
        }))
        << "no Weight field for the selected option";
    const UIElement::WeakRef<FloatField> dragged = UIElement::MakeWeakRef(weight);
    TextInput* editor = FindChildOfType<TextInput>(*weight);
    ASSERT_NE(editor, nullptr);

    SendPointer(*editor, kEventMouseDown, 100.0f);
    SendPointer(*editor, kEventMouseMove, 140.0f);
    for (int drain = 0; drain < 4; ++drain)
        manager.GetDispatcher()->Drain();
    ASSERT_NE(dragged.Get(), nullptr) << "the preview built the dragged field again";
    const float32 previewed = workspace->GetDocument()->GetSource().Definition.States[0].Utility.Options[0].Weight;
    EXPECT_NE(previewed, 2.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 0u);
    const UIElement* row = FindRowWithText(*panel, "Attack");
    ASSERT_NE(row, nullptr);
    const Label* detail = dynamic_cast<const Label*>(FindByClass(*row, "scribe-row-detail"));
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->GetText(), "rank 0, weight " + FormatFloat(previewed));

    SendPointer(*editor, kEventMouseUp, 140.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
}

// An input that its module removed is refused like an input that no module registered, each on its own row.
TEST_F(ScribePanelTests, EachRefusedInputShowsItsRefusalOnTheRowThatNamesIt)
{
    std::ofstream(File, std::ios::binary | std::ios::trunc) << CanonicalProgram(kCommanderProgram);
    ECS::SetActiveRegistrationModule("RemovedModule", 1);
    Names.RegisterInput(&ReadNothing, AIECS::AiInputThreading::CallingThread, "ReadySoldiers");
    ECS::ClearActiveRegistrationModule();
    Names.PurgeModuleRegistrations("RemovedModule", 1);
    Names.RegisterInput(&ReadNothing, AIECS::AiInputThreading::CallingThread, "Threat");
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    std::vector<std::string> refused;
    for (const Row& row : document->BuildOutline())
        if (row.RowKind == Row::Kind::Consideration && !row.Refusal.empty())
        {
            refused.push_back(row.Text);
            EXPECT_NE(row.Refusal.find("'" + row.Text + "'"), std::string::npos) << row.Refusal;
        }
    EXPECT_EQ(refused, (std::vector<std::string>{"ReadySoldiers", "Morale"}));
}

// A refused consideration names its option and its input, and gives the fix in the Input field first. The Inputs
// group marks the inputs that no module registers, and the notice names each cause once.
TEST_F(ScribePanelTests, ARefusedConsiderationNamesItsOptionAndTheInputsGroupMarksTheInput)
{
    std::ofstream(File, std::ios::binary | std::ios::trunc) << CanonicalProgram(kCommanderProgram);
    Names.RegisterInput(&ReadNothing, AIECS::AiInputThreading::CallingThread, "Threat");
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    std::string morale;
    std::vector<std::string> markedInputs;
    bool inInputs = false;
    for (const Row& row : document->BuildOutline())
    {
        if (row.RowKind == Row::Kind::Consideration && row.Text == "Morale")
            morale = row.Refusal;
        if (row.RowKind == Row::Kind::Group)
            inInputs = row.Text == "Inputs";
        if (row.RowKind == Row::Kind::State && row.Text == "Attack")
            EXPECT_STREQ(ScribeDocument::KindWord(row), "Option");
        if (inInputs && row.RowKind == Row::Kind::Name && !row.Refusal.empty())
            markedInputs.push_back(row.Text);
    }
    EXPECT_EQ(morale, "Attack > Consider Morale: no loaded code registers the input 'Morale'. Choose a registered "
                      "input in Input, or register it in the game's code with GE_REGISTER_AI_INPUT. A registration "
                      "clears this row when the code reloads.");
    EXPECT_EQ(markedInputs, (std::vector<std::string>{"ReadySoldiers", "Morale"}));
}

// A remove and an add keep the considerations of the later option, and two undos restore the file text.
TEST_F(ScribePanelTests, RemoveAndAddConsiderationKeepTheLaterOptionAndUndoRestoresTheText)
{
    WriteCommander();
    const std::string before = ReadText(File);
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    document->RemoveConsideration(1, 0);
    const AI::OptionSet& removed = document->GetSource().Definition.States[0].Utility;
    ASSERT_EQ(removed.Options[1].FirstConsideration, 1u);
    ASSERT_EQ(removed.Considerations.size(), 2u);
    document->AddConsideration(1, "Threat");
    const AI::OptionSet& added = document->GetSource().Definition.States[0].Utility;
    ASSERT_EQ(added.Options[1].FirstConsideration, 2u);
    std::vector<std::string> lines;
    for (const Row& row : document->BuildOutline())
        if (row.RowKind == Row::Kind::State || row.RowKind == Row::Kind::Consideration)
            lines.push_back(std::to_string(row.Depth) + " " + row.Text);
    EXPECT_EQ(lines, (std::vector<std::string>{"0 Commander", "1 Attack", "2 Morale", "2 Threat", "1 Wait", "2 Threat"}));
    EXPECT_EQ(Undo.GetUndoCount(), 2u);

    Undo.Undo();
    Undo.Undo();
    std::string error;
    ASSERT_TRUE(document->Save(error)) << error;
    EXPECT_EQ(ReadText(File), before);
}

// A drag far to the left stops the weight at 0, where the program takes it.
TEST_F(ScribePanelLayoutTests, ADragOfTheWeightStopsAtZero)
{
    WriteCommander();
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    ScribePanel* panel = ShowPanel(manager, workspace);
    TextInput* editor = SelectAttackWeight(manager, *panel);
    ASSERT_NE(editor, nullptr);

    SendPointer(*editor, kEventMouseDown, 300.0f);
    SendPointer(*editor, kEventMouseMove, 100.0f);
    SendPointer(*editor, kEventMouseMove, 0.0f);
    EXPECT_EQ(workspace->GetDocument()->GetSource().Definition.States[0].Utility.Options[0].Weight, 0.0f);
    SendPointer(*editor, kEventMouseUp, 0.0f);
    EXPECT_EQ(workspace->GetDocument()->GetSource().Definition.States[0].Utility.Options[0].Weight, 0.0f);
}

// A change of the registered names during a drag keeps the dragged field, and the drag end is one undo step.
TEST_F(ScribePanelLayoutTests, ANameChangeDuringADragKeepsTheDraggedField)
{
    WriteCommander();
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    ScribePanel* panel = ShowPanel(manager, workspace);
    TextInput* editor = SelectAttackWeight(manager, *panel);
    ASSERT_NE(editor, nullptr);
    const UIElement::WeakRef<TextInput> dragged = UIElement::MakeWeakRef(editor);

    SendPointer(*editor, kEventMouseDown, 100.0f);
    SendPointer(*editor, kEventMouseMove, 140.0f);
    Names.RegisterInput(&ReadNothing, AIECS::AiInputThreading::CallingThread, "Threat");
    panel->Refresh();
    ASSERT_NE(dragged.Get(), nullptr) << "the change of the names built the dragged field again";
    SendPointer(*editor, kEventMouseUp, 140.0f);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    EXPECT_FALSE(workspace->GetDocument()->IsPreviewing());
}

// A press on the Weight field, a move past the drag threshold and a release, sent to the UI manager as a mouse
// sends them (hit test, capture, one frame between events), drag the weight and leave the field out of text edit.
TEST_F(ScribePanelLayoutTests, APressAndAMoveOnTheWeightFieldDragTheWeight)
{
    constexpr float kFrameSeconds = 1.0f / 60.0f;
    constexpr float kMovePx = 40.0f;
    WriteCommander();
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    InspectorLayoutTesting::InspectorLayoutFixture fixture;
    fixture.Assets = &Assets();
    fixture.ExtraSheets = {"Assets/UI/panels/Scribe/ScribePanel.css"};
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << fixture.Diagnostic;
    UIManager& manager = *fixture.Ui;
    TextInput* editor = SelectAttackWeight(manager, *panel);
    ASSERT_NE(editor, nullptr);
    ASSERT_TRUE(WaitUntil(
        [&manager, editor]
        {
            manager.Update(kFrameSeconds, /*interactive=*/true);
            return editor->GetLayoutWidth() > 0.0f;
        }));
    const float x = editor->GetLayoutX() + editor->GetLayoutWidth() * 0.5f;
    const float y = editor->GetLayoutY() + editor->GetLayoutHeight() * 0.5f;

    manager.OnMouseMove(x, y);
    manager.Update(kFrameSeconds, /*interactive=*/true);
    manager.OnMouseButton(0, true);
    manager.Update(kFrameSeconds, /*interactive=*/true);
    ASSERT_TRUE(manager.IsMouseCaptured()) << "the press did not reach the field";
    manager.OnMouseMove(x + kMovePx, y);
    manager.Update(kFrameSeconds, /*interactive=*/true);
    manager.OnMouseButton(0, false);
    manager.Update(kFrameSeconds, /*interactive=*/true);

    const float startWeight = 2.0f;
    const float sensitivity = 0.03f;
    EXPECT_NEAR(workspace->GetDocument()->GetSource().Definition.States[0].Utility.Options[0].Weight,
                startWeight + kMovePx * sensitivity, 1e-4f);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    EXPECT_NE(manager.GetFocusedElementId(), editor->GetId()) << "the release put the field in text edit";
}

// A commit of the value that a field holds, as after a typed text that is not a number or a value that the field
// clamps back, adds no undo step and leaves the program as saved.
TEST_F(ScribePanelTests, ACommitThatChangesNothingAddsNoUndoStep)
{
    WriteCommander();
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    document->SetOptionWeight(1, 2.0f, ScribeDocument::Phase::Commit);
    document->SetOptionRank(1, 0);
    document->SetCommitmentMargin(0, 0.1f, ScribeDocument::Phase::Commit);
    document->SetMinimumTicks(0, 20);
    document->SetNoiseMargin(0, 0.0f, ScribeDocument::Phase::Commit);
    document->SetConsiderationRange(1, 0, 0.0f, 6.0f, ScribeDocument::Phase::Commit);
    EXPECT_EQ(Undo.GetUndoCount(), 0u);
    EXPECT_FALSE(document->IsDirty());

    document->SetOptionWeight(1, 3.0f, ScribeDocument::Phase::Commit);
    document->SetOptionWeight(1, 3.0f, ScribeDocument::Phase::Commit);
    EXPECT_EQ(Undo.GetUndoCount(), 1u);
    EXPECT_TRUE(document->IsDirty());
}

// A drag that ends at its start value shows the start value again and adds no undo step.
TEST_F(ScribePanelTests, ADragThatEndsAtItsStartAddsNoUndoStep)
{
    WriteCommander();
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    document->SetOptionWeight(1, 2.5f, ScribeDocument::Phase::Preview);
    document->SetOptionWeight(1, 2.0f, ScribeDocument::Phase::Commit);
    EXPECT_EQ(Undo.GetUndoCount(), 0u);
    EXPECT_FALSE(document->IsPreviewing());
    EXPECT_FALSE(document->IsDirty());
}

// A Minimum above the Maximum is a valid range that the runtime maps downward; the row says so.
TEST_F(ScribePanelTests, ARangeWithItsMinimumAboveItsMaximumReadsDownward)
{
    WriteCommander();
    const std::shared_ptr<ScribeDocument> document = Open();
    ASSERT_NE(document, nullptr);

    document->SetConsiderationRange(1, 0, 9.0f, 6.0f, ScribeDocument::Phase::Commit);
    const std::vector<Row> rows = document->BuildOutline();
    std::vector<std::string> details;
    for (const Row* row : RowsOfKind(rows, Row::Kind::Consideration))
        details.push_back(row->Text + " " + row->Detail);
    EXPECT_EQ(details, (std::vector<std::string>{"ReadySoldiers 9 down to 6, linear", "Morale 0 to 1, logistic",
                                                 "Threat 0 to 1, linear"}));
}

// The Events group marks an event that no module registers, as the Inputs group marks an input.
TEST_F(ScribePanelTests, TheEventsGroupMarksAnEventThatNoModuleRegisters)
{
    AIECS::AiRegistry names;
    names.RegisterTask({nullptr, &TickWalk, nullptr, "Walk"});
    names.RegisterEventType("Retreat");
    std::string error;
    const std::shared_ptr<ScribeDocument> document = ScribeDocument::Open(File, &Undo, &names, error);
    ASSERT_NE(document, nullptr) << error;

    std::vector<std::string> marked;
    bool inEvents = false;
    for (const Row& row : document->BuildOutline())
    {
        if (row.RowKind == Row::Kind::Group)
            inEvents = row.Text == "Events";
        if (inEvents && row.RowKind == Row::Kind::Name && !row.Refusal.empty())
            marked.push_back(row.Refusal);
    }
    EXPECT_EQ(marked, (std::vector<std::string>{"No loaded code registers the event 'Alarm'. Choose a registered event "
                                                "on each transition that fires on it, or register it in the game's "
                                                "code with GE_REGISTER_AI_EVENT. A registration clears this row when "
                                                "the code reloads."}));
}

// A reload that removed the module's only event leaves a transition that holds the name. Its refusal and the mark
// of the Events group name the macro and offer no other event to choose.
TEST_F(ScribePanelTests, ANameThatAReloadRemovedNamesTheMacroThatRegistersIt)
{
    AIECS::AiRegistry names;
    names.RegisterTask({nullptr, &TickWalk, nullptr, "Walk"});
    ECS::SetActiveRegistrationModule("GuardModule", 1);
    names.RegisterEventType("Alarm");
    ECS::ClearActiveRegistrationModule();
    names.RetireSupersededModuleRegistrations("GuardModule", 2);
    std::string error;
    const std::shared_ptr<ScribeDocument> document = ScribeDocument::Open(File, &Undo, &names, error);
    ASSERT_NE(document, nullptr) << error;

    std::vector<std::string> refusals;
    bool inEvents = false;
    for (const Row& row : document->BuildOutline())
    {
        if (row.RowKind == Row::Kind::Group)
            inEvents = row.Text == "Events";
        if ((row.RowKind == Row::Kind::Transition || (inEvents && row.RowKind == Row::Kind::Name)) &&
            !row.Refusal.empty())
            refusals.push_back(row.Refusal);
    }
    EXPECT_EQ(refusals,
              (std::vector<std::string>{
                  "Patrol > To Chase: no loaded code registers the event 'Alarm'. Register it in the game's code with "
                  "GE_REGISTER_AI_EVENT. A registration clears this row when the code reloads.",
                  "No loaded code registers the event 'Alarm'. Register it in the game's code with "
                  "GE_REGISTER_AI_EVENT. A registration clears this row when the code reloads."}));
}

// A task has no field that chooses it, so the refusal of an unregistered task offers to remove it or register it.
TEST_F(ScribePanelTests, ATaskThatNoLoadedCodeRegistersOffersToRemoveOrRegisterIt)
{
    AIECS::AiRegistry names;
    names.RegisterCondition(&TestNothing, "SeesEnemy");
    names.RegisterEventType("Alarm");
    std::string error;
    const std::shared_ptr<ScribeDocument> document = ScribeDocument::Open(File, &Undo, &names, error);
    ASSERT_NE(document, nullptr) << error;

    std::vector<std::string> refusals;
    for (const Row& row : document->BuildOutline())
        if (row.RowKind == Row::Kind::Task && !row.Refusal.empty())
            refusals.push_back(row.Refusal);
    ASSERT_FALSE(refusals.empty());
    EXPECT_EQ(refusals[0], "Patrol > Task Walk: no loaded code registers the task 'Walk'. Remove the task from the "
                           "program file, or register it in the game's code with GE_REGISTER_AI_TASK. A registration "
                           "clears this row when the code reloads.");
}

// A notice with names of several kinds lists the kinds, then the names in the same order.
TEST_F(ScribePanelLayoutTests, TheNoticeListsEachKindOfCauseThenItsNames)
{
    AIECS::AiRegistry names;
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &names);
    workspace->Open(File);
    UIManager manager{nullptr, &Assets()};
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    manager.SetRoot(std::move(owned));

    const std::string text = "1 condition, 1 event and 1 task are not registered: SeesEnemy, Alarm, Walk (4 rows). "
                             "Select a red row to see its fix.";
    EXPECT_EQ(NoticeTextAtWidth(manager, *panel, 600.0f, text), text);
}

// In a 360 px panel with a long input name, each outline row stays one line: its texts end before its badge, the
// badge ends inside the row, and the row's tooltip gives the whole text. The Input field's chevron stays in its field,
// and the name in the field ends in an ellipsis inside its box.
TEST_F(ScribePanelLayoutTests, ALongNameInANarrowPanelKeepsEachRowOnOneLine)
{
    constexpr const char* kLongName = "EnemyReinforcementsWithinRange";
    constexpr float kTolerance = 0.5f;
    std::string program = CanonicalProgram(kCommanderProgram);
    program.replace(program.find("Morale"), std::string_view("Morale").size(), kLongName);
    std::ofstream(File, std::ios::binary | std::ios::trunc) << program;
    const std::shared_ptr<ScribeWorkspace> workspace = std::make_shared<ScribeWorkspace>(&Undo, &Names);
    workspace->Open(File);
    InspectorLayoutTesting::InspectorLayoutFixture fixture;
    fixture.ViewportW = 360;
    fixture.ViewportH = 300;
    fixture.Assets = &Assets();
    fixture.ExtraSheets = {"Assets/UI/theme/widgets.css", "Assets/UI/panels/Scribe/ScribePanel.css"};
    std::unique_ptr<ScribePanel> owned = std::make_unique<ScribePanel>(workspace);
    ScribePanel* panel = owned.get();
    if (!fixture.Build(std::move(owned)))
        GTEST_SKIP() << fixture.Diagnostic;
    UIManager& manager = *fixture.Ui;

    UIElement* longRow = nullptr;
    ASSERT_TRUE(WaitUntil(
        [&manager, &longRow, panel]
        {
            manager.GetDispatcher()->Drain();
            manager.Update(1.0f / 60.0f, /*interactive=*/true);
            longRow = FindRowWithText(*panel, kLongName);
            return longRow != nullptr && longRow->GetLayoutWidth() > 0.0f;
        }));
    EXPECT_EQ(longRow->GetTooltip(), std::string("Consider ") + kLongName + " 0 to 1, logistic");

    std::vector<const UIElement*> rows;
    CollectRows(*panel, rows);
    ASSERT_GT(rows.size(), 10u);
    for (const UIElement* row : rows)
    {
        const float rowRight = row->GetLayoutX() + row->GetLayoutWidth();
        const float rowBottom = row->GetLayoutY() + row->GetLayoutHeight();
        float previousRight = row->GetLayoutX();
        for (const std::unique_ptr<UIElement>& child : row->GetChildren())
        {
            const std::string where =
                row->GetTooltip() + ", " + (child->GetClasses().empty() ? "" : child->GetClasses().front());
            EXPECT_GE(child->GetLayoutX() + kTolerance, previousRight) << where << " overlaps the text before it";
            EXPECT_LE(child->GetLayoutX() + child->GetLayoutWidth(), rowRight + kTolerance) << where;
            EXPECT_GE(child->GetLayoutY() + kTolerance, row->GetLayoutY()) << where << " is not on one line";
            EXPECT_LE(child->GetLayoutY() + child->GetLayoutHeight(), rowBottom + kTolerance)
                << where << " is not on one line";
            previousRight = child->GetLayoutX() + child->GetLayoutWidth();
        }
    }

    SendPointer(*longRow, kEventMouseDown, 0.0f);
    UIElement* chevron = nullptr;
    UIElement* dropdown = nullptr;
    ASSERT_TRUE(WaitUntil(
        [&manager, &chevron, &dropdown, panel]
        {
            manager.GetDispatcher()->Drain();
            manager.Update(1.0f / 60.0f, /*interactive=*/true);
            dropdown = FindFirstOfType<Dropdown>(*panel);
            chevron = dropdown ? InspectorLayoutTesting::FindByClass(dropdown, "dropdown-chevron") : nullptr;
            return chevron != nullptr && chevron->GetLayoutWidth() > 0.0f;
        }))
        << "no Input field with a chevron";
    const UIElement* field = dropdown->GetParent();
    EXPECT_LE(chevron->GetLayoutX() + chevron->GetLayoutWidth(),
              field->GetLayoutX() + field->GetLayoutWidth() + kTolerance)
        << "the Input field's chevron is cut";
    const UIElement* label = InspectorLayoutTesting::FindByClass(dropdown, "dropdown-header-label");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->GetResolvedStyle().Layout.Overflow, Overflow::Hidden) << "the name draws past its box";
    EXPECT_EQ(label->GetResolvedStyle().Visual.TextOverflow, TextOverflowMode::Ellipsis)
        << "the name is cut with no ellipsis";
}

} // namespace GameEngine::Editor
