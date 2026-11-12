#include "Setup/AiAssistantSetupPage.h"

#include "AgentSessionState.h"
#include "AgentStatusMessages.h"
#include "AiAssistantSettings.h"
#include "AssistantAttachment.h"
#include "AssistantMode.h"
#include "Providers/CancelToken.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CliSessionProvider.h"
#include "Providers/CodexSessionProvider.h"
#include "Providers/IAgentProvider.h"
#include "Providers/SetupStatus.h"
#include "Setup/AiAssistantApiKeyRow.h"

#include "Core/Engine.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Logger/Logger.h"
#include "Platform/SecretStore.h"
#include "Platform/Shell.h"
#include "Platform/SpeechInput.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/PictureChoiceField.h"
#include "UI/Controls/SetupStepList.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "ai-assistant:Editor/UI/controls/AiAssistantSetup/AiAssistantSetupPage.uxml";
constexpr const char* kStyleAssetPath = "Editor/UI/controls/AiAssistantSetup/AiAssistantSetupPage.css";
constexpr const char* kPackageAlias = "ai-assistant";
// The pill when a key is saved and the network stopped Key accepted; "Ready, limited" keeps the
// meaning "conversation only".
constexpr const char* kSavedNotCheckedStatus = "Saved, not checked";

constexpr const char* kModeTooltip = "What the assistant may do in the editor when a new conversation starts; the "
                                     "panel's mode control changes it, and a resumed session keeps its own.";
constexpr const char* kEffortTooltip = "How much a Claude (Subscription) turn reasons before it answers. The panel's "
                                       "model button changes it too.";
constexpr const char* kCodexModelTooltip =
    "The model a Codex (Subscription) turn asks for, as Codex names it; empty uses Codex's own default.";
constexpr const char* kHoldToSpeakTooltip = "Press and hold the microphone beside Send to dictate; releasing it "
                                            "stops. Off: a click starts dictation and a second click stops it.";
constexpr const char* kSendWhenMicStopsTooltip = "Stopping the microphone sends the dictated prompt once its last "
                                                 "words arrive. Typing while dictating never sends.";
// The executable fields' tooltips name each program's file on this platform.
#if defined(_WIN32)
constexpr const char* kClaudeExecutableTooltip = "The claude program: a name found on PATH or a full path to claude.exe.";
constexpr const char* kCodexExecutableTooltip =
    "The codex program: a name found on PATH or a full path to codex.exe, the native program (not codex.cmd).";
constexpr const char* kNodeExecutableTooltip = "The node program that runs the editor's tools for the assistant "
                                               "(Node 20 or later): a name found on PATH or a full path to node.exe.";
#else
constexpr const char* kClaudeExecutableTooltip = "The claude program: a name found on PATH or a full path to it.";
constexpr const char* kCodexExecutableTooltip = "The codex program: a name found on PATH or a full path to it.";
constexpr const char* kNodeExecutableTooltip = "The node program that runs the editor's tools for the assistant "
                                               "(Node 20 or later): a name found on PATH or a full path to it.";
#endif

// A status of one failed step, for a check that threw.
SetupStatus ThrownStatus(std::string found)
{
    return {.Steps = {{.Name = "Setup check",
                       .State = SetupStepState::Failed,
                       .Found = std::move(found),
                       .Fix = "Click Check again; the editor log has the details."}}};
}

EditorUI::SetupStepList::State ListState(SetupStepState state)
{
    switch (state)
    {
    case SetupStepState::Waiting:
        return EditorUI::SetupStepList::State::Waiting;
    case SetupStepState::Running:
        return EditorUI::SetupStepList::State::Running;
    case SetupStepState::Passed:
        return EditorUI::SetupStepList::State::Passed;
    case SetupStepState::Warning:
        return EditorUI::SetupStepList::State::Warning;
    case SetupStepState::Failed:
        return EditorUI::SetupStepList::State::Failed;
    }
    return EditorUI::SetupStepList::State::Waiting;
}

// `text` as a sentence: with a full stop at its end.
std::string Sentence(std::string text)
{
    if (!text.empty() && text.back() != '.')
        text += '.';
    return text;
}

bool KeyNotChecked(const SetupStatus& status)
{
    const SetupStep* problem = status.FirstProblem();
    return problem && problem->Name == ClaudeApiProvider::kKeyAcceptedStep;
}

// The line beside a Ready pill: the last passed step's finding before the editor's tools (the
// login, or the API key), then the connection the panel uses, or what the warning limits: the
// editor's tools. A key the network stopped from being checked gets the one action instead.
std::string ReadySummary(const SetupStatus& status, std::string_view label)
{
    if (KeyNotChecked(status))
        return "Click Check again, or send a prompt, to check the key.";
    const SetupStep* shown = nullptr;
    for (const SetupStep& step : status.Steps)
        if (step.State == SetupStepState::Passed && step.Name != CliSessionProvider::kEditorToolsStep)
            shown = &step;
    std::string uses = "The AI Assistant panel uses " + std::string(label) + ".";
    if (status.FirstProblem())
        uses = "Conversation only: the editor's tools are off.";
    return shown ? Sentence(shown->Found) + " " + uses : uses;
}

std::string StepsToFix(size_t count)
{
    return count == 1 ? "1 step to fix." : std::to_string(count) + " steps to fix.";
}

void SetHidden(UIElement& element, bool hidden)
{
    if (hidden)
        element.AddClass("hidden");
    else
        element.RemoveClass("hidden");
}

bool IsSubscription(std::string_view providerId)
{
    return providerId == ClaudeSessionProvider::kId || providerId == CodexSessionProvider::kId;
}

// The session whose Editor tools step the page runs after `provider`'s own steps pass; null for
// a provider that cannot act in the editor.
CliSessionProvider* ToolsSession(IAgentProvider& provider)
{
    auto* session = dynamic_cast<CliSessionProvider*>(&provider);
    return session && provider.Capabilities().CanActWithTools ? session : nullptr;
}

// The line of a step that waits behind the failed step `failed`: never a second error for one cause.
std::string WaitsFor(const SetupStep& failed)
{
    return "Checked once the " + failed.Name + " step passes.";
}

// Opens the native file picker at the field's program when it names a file, else at the open
// project; writes the pick into the field and returns true, or false when cancelled.
bool BrowseInto(TextField& field)
{
    std::filesystem::path initial = EngineCore::GetInstance().GetWorkspaceRoot();
    const std::filesystem::path current(field.GetValue());
    std::error_code error;
    if (!current.empty() && std::filesystem::exists(current, error))
        initial = current;
    const std::filesystem::path picked = Platform::SelectFile(initial);
    if (picked.empty())
        return false;
    field.SetValue(picked.string());
    return true;
}

// Runs `commit` with the field's text when the user presses Enter or leaves the field.
void OnCommit(TextField& field, std::function<void(const std::string&)> commit)
{
    field.SetOnValueChanged(commit);
    field.RegisterEventHandler(kEventFocusOut, [&field, commit](UIEvent&) { commit(field.GetValue()); });
}
} // namespace

// Written by the check thread, read by PollCheck(). The page's destructor cancels the check and
// joins the thread, so a program that does not answer never outlives the page.
struct AiAssistantSetupPage::PendingCheck
{
    CancelToken Cancel;
    std::mutex Mutex;
    bool Done = false;
    /// The steps that have ended; the whole result once Done.
    SetupStatus Status;
    /// Whether a Claude API key is saved, read with a Claude (API key) check; else empty.
    std::optional<bool> KeySaved;
    /// The key store write that runs before the check; empty for a check alone. Set before the
    /// thread starts and read only by it.
    std::function<Platform::SecretStoreStatus()> KeyWrite;
    /// The status of KeyWrite once it has ended; PollCheck hands it to the key row once.
    std::optional<Platform::SecretStoreStatus> KeyWriteStatus;
};

AiAssistantSetupPage::AiAssistantSetupPage()
{
    AddClass("ai-setup-page-host");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, kPackageAlias);
}

AiAssistantSetupPage::~AiAssistantSetupPage()
{
    CancelCheck();
    StopRefresh();
}

void AiAssistantSetupPage::OnOwnerManagerChanged(UIManager* owner)
{
    StopRefresh();
    if (!owner)
        return;
    if (!m_Built && !Build(*owner))
        return;
    m_RefreshManager = UIManagerRef(owner);
    m_RefreshToken = owner->RegisterPeriodicRefresh([this] { Refresh(); });
    // The checks run when the page opens, so its first state is a result, not "Not checked".
    StartCheck({});
}

bool AiAssistantSetupPage::Build(UIManager& ui)
{
    if (!ui.InstantiateLayoutChildrenFromAssetPath(this, kLayoutAssetPath))
    {
        Logger::Log::Error("AI Assistant: {} did not instantiate; check that the package's assets are staged.",
                           kLayoutAssetPath);
        return false;
    }
    m_AssistantHost = FindById("AiSetupAssistantHost"_sid);
    m_MethodHost = FindById("AiSetupMethodHost"_sid);
    m_MethodNeeds = dynamic_cast<Label*>(FindById("AiSetupMethodNeeds"_sid));
    m_SetupHeader = dynamic_cast<Label*>(FindById("AiSetupHeader"_sid));
    m_ExecutableRow = FindById("AiSetupExecutableRow"_sid);
    m_ExecutableLabel = dynamic_cast<Label*>(FindById("AiSetupExecutableLabel"_sid));
    m_ExecutableField = dynamic_cast<TextField*>(FindById("AiSetupExecutableField"_sid));
    auto* executableBrowse = dynamic_cast<Button*>(FindById("AiSetupExecutableBrowse"_sid));
    UIElement* stepsHost = FindById("AiSetupStepsHost"_sid);
    UIElement* apiKeyHost = FindById("AiSetupApiKeyHost"_sid);
    m_Options = dynamic_cast<Foldout*>(FindById("AiSetupOptions"_sid));
    m_ModelRow = FindById("AiSetupModelRow"_sid);
    m_ModelDropdown = dynamic_cast<Dropdown*>(FindById("AiSetupModel"_sid));
    m_EffortRow = FindById("AiSetupEffortRow"_sid);
    m_EffortDropdown = dynamic_cast<Dropdown*>(FindById("AiSetupEffort"_sid));
    m_CodexModelRow = FindById("AiSetupCodexModelRow"_sid);
    m_CodexModelField = dynamic_cast<TextField*>(FindById("AiSetupCodexModel"_sid));
    m_ModeDropdown = dynamic_cast<Dropdown*>(FindById("AiSetupMode"_sid));
    m_NodeRow = FindById("AiSetupNodeRow"_sid);
    m_NodeField = dynamic_cast<TextField*>(FindById("AiSetupNode"_sid));
    auto* nodeBrowse = dynamic_cast<Button*>(FindById("AiSetupNodeBrowse"_sid));
    m_DictationRows = FindById("AiSetupDictationRows"_sid);
    m_HoldToSpeak = dynamic_cast<Toggle*>(FindById("AiSetupHoldToSpeak"_sid));
    m_SendWhenMicStops = dynamic_cast<Toggle*>(FindById("AiSetupSendWhenMicStops"_sid));
    if (!m_AssistantHost || !m_MethodHost || !m_MethodNeeds || !m_SetupHeader || !m_ExecutableRow || !m_ExecutableLabel || !m_ExecutableField || !executableBrowse ||
        !stepsHost || !apiKeyHost || !m_Options || !m_ModelRow || !m_ModelDropdown || !m_EffortRow || !m_EffortDropdown ||
        !m_CodexModelRow || !m_CodexModelField || !m_ModeDropdown || !m_NodeRow || !m_NodeField || !nodeBrowse ||
        !m_DictationRows || !m_HoldToSpeak || !m_SendWhenMicStops)
    {
        Logger::Log::Error("AI Assistant: {} is missing one of its controls.", kLayoutAssetPath);
        RemoveAllChildren();
        return false;
    }

    auto apiKeyRow = std::make_unique<AiAssistantApiKeyRow>(
        [this](AiAssistantApiKeyRow::KeyWrite keyWrite) { WriteKey(std::move(keyWrite)); });
    if (!apiKeyRow->Build(ui))
    {
        RemoveAllChildren();
        return false;
    }
    m_ApiKeyRow = apiKeyRow.get();
    apiKeyHost->AddChild(std::move(apiKeyRow));

    auto steps = std::make_unique<EditorUI::SetupStepList>();
    m_Steps = steps.get();
    stepsHost->AddChild(std::move(steps));

    m_Steps->SetCheckAgainTooltip("Checks the connection again the way the first prompt does: the program, the "
                                  "login and the editor's tools.");
    m_Steps->SetOnCheckAgain([this] {
        AiAssistantSettings::CheckSetupAgain();
        FollowSetup();
    });
    OnCommit(*m_ExecutableField, [this](const std::string& value) { StoreExecutable(value); });
    executableBrowse->SetTooltip("Choose the program.");
    executableBrowse->SetOnClick([this](UIEvent&) {
        if (BrowseInto(*m_ExecutableField))
            StoreExecutable(m_ExecutableField->GetValue());
    });

    m_ModelDropdown->SetOnValueChanged(
        [](const std::string& model) { AiAssistantSettings::SetModel(AiAssistantSettings::Provider(), model); });
    m_EffortRow->SetTooltip(kEffortTooltip);
    m_EffortDropdown->SetOnValueChanged([](const std::string& effort)
                                        { AiAssistantSettings::SetEffort(ClaudeSessionProvider::kId, effort); });
    m_CodexModelRow->SetTooltip(kCodexModelTooltip);
    OnCommit(*m_CodexModelField,
             [](const std::string& model) { AiAssistantSettings::SetModel(CodexSessionProvider::kId, model); });

    std::vector<Dropdown::Option> modes;
    for (const AssistantModeChoice& choice : AssistantModeChoices())
        modes.push_back({.value = std::string(choice.Id), .label = std::string(choice.Label)});
    m_ModeDropdown->SetOptions(modes);
    m_ModeDropdown->SetTooltip(kModeTooltip);
    m_ModeDropdown->SetOnValueChanged([](const std::string& id) {
        if (const std::optional<AssistantMode> mode = ParseAssistantMode(id))
            AiAssistantSettings::SetLastMode(*mode);
    });

    m_NodeField->SetTooltip(kNodeExecutableTooltip);
    OnCommit(*m_NodeField, [](const std::string& node) { AiAssistantSettings::SetNodeExecutable(node); });
    nodeBrowse->SetTooltip("Choose the program.");
    nodeBrowse->SetOnClick([this](UIEvent&) {
        if (BrowseInto(*m_NodeField))
            AiAssistantSettings::SetNodeExecutable(m_NodeField->GetValue());
    });

    m_HoldToSpeak->SetTooltip(kHoldToSpeakTooltip);
    m_HoldToSpeak->SetOnValueChanged([](const bool& hold) { AiAssistantSettings::SetHoldToSpeak(hold); });
    m_SendWhenMicStops->SetTooltip(kSendWhenMicStopsTooltip);
    m_SendWhenMicStops->SetOnValueChanged([](const bool& send) { AiAssistantSettings::SetSendWhenMicStops(send); });
    if (!Platform::SpeechInput::IsAvailable())
        m_DictationRows->AddClass("hidden");

    m_Built = true;
    m_SeenProviderGeneration = AiAssistantSettings::ProviderGeneration();
    m_SeenModelGeneration = AiAssistantSettings::ModelGeneration();
    m_SeenSetupGeneration = AiAssistantSettings::SetupGeneration();
    ShowConnection();
    return true;
}

void AiAssistantSetupPage::ShowConnection()
{
    const std::string provider = AiAssistantSettings::Provider();
    const AiAssistantSettings::ProviderChoice& choice = AiAssistantSettings::ProviderChoiceOf(provider);
    if (choice.Assistant != m_ShownAssistant)
        ShowAssistantChoices(std::string(choice.Assistant));
    if (provider != m_ShownProvider)
        ShowMethodChoices(provider);
    m_MethodNeeds->SetText(std::string(choice.MethodNeeds));
    m_SetupHeader->SetText("Setup for " + std::string(choice.Label));
    ShowExecutableRow(provider);
    SetHidden(*m_ApiKeyRow, provider != ClaudeApiProvider::kId);
    ShowOptions(provider);
    m_Plan.clear();
    m_Steps->SetSteps({});
    m_Steps->SetSummary(EditorUI::SetupStepList::Summary::NotChecked, kNotCheckedStatus, {});
}

void AiAssistantSetupPage::ShowAssistantChoices(const std::string& assistant)
{
    m_ShownAssistant = assistant;
    m_AssistantHost->RemoveAllChildren();
    std::vector<EditorUI::PictureChoiceField::Choice> tiles;
    size_t selected = 0;
    for (const AiAssistantSettings::AssistantChoice& choice : AiAssistantSettings::AssistantChoices())
    {
        if (choice.Name == assistant)
            selected = tiles.size();
        tiles.push_back({.Label = std::string(choice.Name), .Description = std::string(choice.Line)});
    }
    auto field = std::make_unique<EditorUI::PictureChoiceField>(std::move(tiles), selected);
    field->SetOnSelected([this](size_t index) {
        m_ShownAssistant = std::string(AiAssistantSettings::AssistantChoices()[index].Name);
        AiAssistantSettings::SetAssistant(m_ShownAssistant);
    });
    m_AssistantHost->AddChild(std::move(field));
}

void AiAssistantSetupPage::ShowMethodChoices(const std::string& providerId)
{
    m_ShownProvider = providerId;
    m_MethodHost->RemoveAllChildren();
    const std::vector<AiAssistantSettings::ProviderChoice> methods =
        AiAssistantSettings::MethodChoices(AiAssistantSettings::ProviderChoiceOf(providerId).Assistant);
    // An assistant with one method draws no tiles: its method is one line.
    if (methods.size() < 2)
    {
        auto line = std::make_unique<Label>();
        if (!methods.empty())
            line->SetText(std::string(methods.front().Method) + ": " + std::string(methods.front().MethodLine));
        line->AddClass("ai-setup-method-line");
        m_MethodHost->AddChild(std::move(line));
        return;
    }
    std::vector<EditorUI::PictureChoiceField::Choice> tiles;
    size_t selected = 0;
    for (const AiAssistantSettings::ProviderChoice& method : methods)
    {
        if (method.Id == providerId)
            selected = tiles.size();
        tiles.push_back({.Label = std::string(method.Method), .Description = std::string(method.MethodLine)});
    }
    auto field = std::make_unique<EditorUI::PictureChoiceField>(std::move(tiles), selected);
    field->SetOnSelected([this, methods](size_t index) {
        m_ShownProvider = std::string(methods[index].Id);
        AiAssistantSettings::SetProvider(m_ShownProvider);
    });
    m_MethodHost->AddChild(std::move(field));
}

void AiAssistantSetupPage::ShowExecutableRow(const std::string& providerId)
{
    if (!IsSubscription(providerId))
    {
        m_ExecutableRow->AddClass("hidden");
        return;
    }
    m_ExecutableRow->RemoveClass("hidden");
    const bool claude = providerId == ClaudeSessionProvider::kId;
    m_ExecutableLabel->SetText(claude ? "Claude Code executable" : "Codex executable");
    const char* tooltip = claude ? kClaudeExecutableTooltip : kCodexExecutableTooltip;
    m_ExecutableLabel->SetTooltip(tooltip);
    m_ExecutableField->SetTooltip(tooltip);
    m_ExecutableField->SetValue(claude ? AiAssistantSettings::ClaudeExecutable()
                                       : AiAssistantSettings::CodexExecutable());
}

void AiAssistantSetupPage::StoreExecutable(const std::string& executable)
{
    const std::string provider = AiAssistantSettings::Provider();
    if (provider == ClaudeSessionProvider::kId)
        AiAssistantSettings::SetClaudeExecutable(executable);
    else if (provider == CodexSessionProvider::kId)
        AiAssistantSettings::SetCodexExecutable(executable);
}

void AiAssistantSetupPage::ShowOptions(const std::string& providerId)
{
    const std::span<const AiAssistantSettings::Choice> models = AiAssistantSettings::ModelChoices(providerId);
    SetHidden(*m_ModelRow, models.empty());
    if (!models.empty())
    {
        std::vector<Dropdown::Option> options;
        for (const AiAssistantSettings::Choice& model : models)
            options.push_back({.value = std::string(model.Id),
                               .label = AiAssistantSettings::ModelLabel(providerId, model.Id)});
        const std::string stored = AiAssistantSettings::Model(providerId);
        const auto found = std::find_if(options.begin(), options.end(),
                                        [&stored](const Dropdown::Option& option) { return option.value == stored; });
        m_ModelDropdown->SetOptions(options, found == options.end() ? 0 : static_cast<int>(found - options.begin()));
    }

    const std::span<const AiAssistantSettings::Choice> efforts = AiAssistantSettings::EffortChoices(providerId);
    SetHidden(*m_EffortRow, efforts.empty());
    if (!efforts.empty())
    {
        std::vector<Dropdown::Option> options;
        const std::string stored = AiAssistantSettings::Effort(providerId);
        int selected = 0;
        for (const AiAssistantSettings::Choice& effort : efforts)
        {
            if (effort.Id == stored)
                selected = static_cast<int>(options.size());
            options.push_back({.value = std::string(effort.Id), .label = std::string(effort.Label)});
        }
        m_EffortDropdown->SetOptions(options, selected);
    }

    const bool codex = providerId == CodexSessionProvider::kId;
    SetHidden(*m_CodexModelRow, !codex);
    if (codex)
        m_CodexModelField->SetValue(AiAssistantSettings::Model(providerId));

    m_ModeDropdown->SetSelectedValue(std::string(DescribeAssistantMode(AiAssistantSettings::LastMode()).Id));
    // Node runs the editor's tools, which only a session that signs in through its program uses.
    SetHidden(*m_NodeRow, !IsSubscription(providerId));
    m_NodeField->SetValue(AiAssistantSettings::NodeExecutable());
    m_HoldToSpeak->SetValueWithoutNotify(AiAssistantSettings::HoldToSpeak());
    m_SendWhenMicStops->SetValueWithoutNotify(AiAssistantSettings::SendWhenMicStops());
    ShowOptionsSummary(providerId);
}

void AiAssistantSetupPage::ShowOptionsSummary(const std::string& providerId)
{
    std::string summary = "Model and options: ";
    if (const std::string model = AiAssistantSettings::Model(providerId); !model.empty())
        summary += AiAssistantSettings::ModelLabel(providerId, model);
    else
        summary += "Codex's own model";
    if (const std::string effort = AiAssistantSettings::Effort(providerId); !effort.empty())
        for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::EffortChoices(providerId))
            if (choice.Id == effort)
                summary += " \xC2\xB7 " + std::string(choice.Label) + " effort";
    summary += " \xC2\xB7 new conversations: " +
               std::string(DescribeAssistantMode(AiAssistantSettings::LastMode()).Label);
    m_Options->SetTitle(summary);
}

void AiAssistantSetupPage::ShowStatus(const SetupStatus& status, bool keySaved)
{
    if (!m_Built)
        return;
    ShowSteps(status, /*running=*/false);
    const std::string_view label = AiAssistantSettings::ProviderChoiceOf(AiAssistantSettings::Provider()).Label;
    if (!status.Ready())
    {
        const size_t count = static_cast<size_t>(std::count_if(
            status.Steps.begin(), status.Steps.end(),
            [](const SetupStep& step) { return step.State == SetupStepState::Failed; }));
        m_Steps->SetSummary(EditorUI::SetupStepList::Summary::NotReady, kNotReadyStatus, StepsToFix(count));
        return;
    }
    if (status.FirstProblem())
        m_Steps->SetSummary(EditorUI::SetupStepList::Summary::ReadyLimited,
                            keySaved && KeyNotChecked(status) ? kSavedNotCheckedStatus : kReadyLimitedStatus,
                            ReadySummary(status, label));
    else
        m_Steps->SetSummary(EditorUI::SetupStepList::Summary::Ready, kReadyStatus, ReadySummary(status, label));
}

void AiAssistantSetupPage::ShowSteps(const SetupStatus& status, bool running)
{
    const auto failed = std::find_if(status.Steps.begin(), status.Steps.end(),
                                     [](const SetupStep& step) { return step.State == SetupStepState::Failed; });
    const bool anyFailed = failed != status.Steps.end();
    std::vector<EditorUI::SetupStepList::Step> rows;
    for (const SetupStep& step : status.Steps)
    {
        EditorUI::SetupStepList::Step row{
            .Name = step.Name, .StepState = ListState(step.State), .Found = step.Found, .Fix = step.Fix};
        if (step.State == SetupStepState::Waiting && row.Found.empty() && anyFailed)
            row.Found = WaitsFor(*failed);
        rows.push_back(std::move(row));
    }
    // The plan's steps that have not ended: the next one runs while the check does; the rest wait.
    for (size_t index = status.Steps.size(); index < m_Plan.size(); ++index)
    {
        const SetupStep& planned = m_Plan[index];
        const bool next = running && !anyFailed && index == status.Steps.size();
        rows.push_back({.Name = planned.Name,
                        .StepState = next ? EditorUI::SetupStepList::State::Running
                                          : EditorUI::SetupStepList::State::Waiting,
                        .Found = anyFailed ? WaitsFor(*failed) : planned.Found});
    }
    m_Steps->SetSteps(rows);
}

void AiAssistantSetupPage::WriteKey(std::function<Platform::SecretStoreStatus()> keyWrite)
{
    CancelCheck();
    StartCheck(std::move(keyWrite));
    // With no provider no check starts, and nothing runs the write.
    if (!m_Pending)
        m_ApiKeyRow->ShowWriteResult(Platform::SecretStoreStatus::Failed);
}

void AiAssistantSetupPage::StartCheck(std::function<Platform::SecretStoreStatus()> keyWrite)
{
    if (!m_Built || m_Pending)
        return;
    // The previous check has ended (PollCheck cleared m_Pending); reap its thread.
    if (m_CheckThread.joinable())
        m_CheckThread.join();

    std::shared_ptr<IAgentProvider> provider = AgentSessionState::Get().Provider(AiAssistantSettings::Provider());
    if (!provider)
        return;

    m_Steps->SetSummary(EditorUI::SetupStepList::Summary::Checking, kCheckingSetupStatus,
                        "Running the checks: a few seconds.");
    m_Plan = provider->SetupPlan();
    if (ToolsSession(*provider))
        m_Plan.push_back(CliSessionProvider::PlannedToolsStep());
    ShowSteps({}, /*running=*/true);
    m_ShownStepCount = 0;
    m_Pending = std::make_shared<PendingCheck>();
    m_Pending->KeyWrite = std::move(keyWrite);
    // A thread of its own, never a JobSystem job: the check waits on child processes for up to
    // their time limits. CheckSetup returns once the destructor cancels it.
    m_CheckThread = std::thread(RunCheck, m_Pending, std::move(provider), AiAssistantSettings::NodeExecutable(),
                                Editor::EditorDebugPort());
}

// An exception that escapes a thread function terminates the process, so whatever the check
// throws becomes the page's finding instead.
void AiAssistantSetupPage::RunCheck(std::shared_ptr<PendingCheck> pending, std::shared_ptr<IAgentProvider> provider,
                                    std::string node, uint16_t port)
{
    // Each step that ends joins the steps the page shows while the check runs.
    const SetupStepSink publish = [&pending](const SetupStep& step) {
        std::lock_guard lock(pending->Mutex);
        pending->Status.Steps.push_back(step);
    };
    SetupStatus status;
    try
    {
        // The write runs even when the check is cancelled, so a key saved just before the page
        // closes is kept. A store call can block on a prompt, so it never runs on the UI thread.
        if (pending->KeyWrite)
        {
            const Platform::SecretStoreStatus written = pending->KeyWrite();
            pending->KeyWrite = nullptr;
            std::lock_guard lock(pending->Mutex);
            pending->KeyWriteStatus = written;
        }
        status = provider->CheckSetup(pending->Cancel, SetupSurface::SettingsPage, publish);
        // The panel's acting check, so the page and the panel agree.
        if (CliSessionProvider* session = ToolsSession(*provider); session && status.Ready())
        {
            status.Steps.push_back(CliSessionProvider::ToolsStep(
                node, AssistantAttachment::ActingRefusal(*session, node, port, SetupSurface::SettingsPage,
                                                         pending->Cancel)));
            publish(status.Steps.back());
        }
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AI Assistant: the setup check for '{}' threw: {}", provider->Id(), e.what());
        status = ThrownStatus("The check stopped with an error: " + std::string(e.what()) + ".");
    }
    catch (...)
    {
        Logger::Log::Error("AI Assistant: the setup check for '{}' threw a non-standard exception", provider->Id());
        status = ThrownStatus("The check stopped with an unknown error.");
    }
    std::optional<bool> keySaved;
    if (provider->Id() == ClaudeApiProvider::kId)
        keySaved = AgentSessionState::Get().ApiKeys().HasSavedKey();
    std::lock_guard lock(pending->Mutex);
    // A write that threw leaves KeyWrite set: the row must still leave its writing state.
    if (pending->KeyWrite)
        pending->KeyWriteStatus = Platform::SecretStoreStatus::Failed;
    pending->Status = std::move(status);
    pending->KeySaved = keySaved;
    pending->Done = true;
}

// A write that ended before the check was cancelled still reaches the row, so its buttons turn on.
void AiAssistantSetupPage::CancelCheck()
{
    if (m_Pending)
        m_Pending->Cancel.Cancel();
    if (m_CheckThread.joinable())
        m_CheckThread.join();
    if (m_Pending && m_Pending->KeyWriteStatus)
        ShowKeyWriteResult(*m_Pending->KeyWriteStatus);
    m_Pending.reset();
}

void AiAssistantSetupPage::ShowKeyWriteResult(Platform::SecretStoreStatus status)
{
    m_ApiKeyRow->ShowWriteResult(status);
    if (status != Platform::SecretStoreStatus::Done && status != Platform::SecretStoreStatus::NotFound)
        return;
    // The panel checks the key again; this page's check after the write already runs.
    AiAssistantSettings::CheckSetupAgain();
    m_SeenSetupGeneration = AiAssistantSettings::SetupGeneration();
}

void AiAssistantSetupPage::Refresh()
{
    if (const uint64_t generation = AiAssistantSettings::ProviderGeneration(); generation != m_SeenProviderGeneration)
    {
        m_SeenProviderGeneration = generation;
        ShowConnection();
    }
    if (const uint64_t generation = AiAssistantSettings::ModelGeneration(); generation != m_SeenModelGeneration)
    {
        m_SeenModelGeneration = generation;
        ShowOptions(AiAssistantSettings::Provider());
    }
    FollowSetup();
    PollCheck();
}

void AiAssistantSetupPage::FollowSetup()
{
    const uint64_t generation = AiAssistantSettings::SetupGeneration();
    if (generation == m_SeenSetupGeneration)
        return;
    m_SeenSetupGeneration = generation;
    // A check of the previous connection or program answers for the wrong one: stop it.
    CancelCheck();
    StartCheck({});
}

void AiAssistantSetupPage::PollCheck()
{
    if (!m_Pending)
        return;
    SetupStatus status;
    bool done = false;
    std::optional<bool> keySaved;
    std::optional<Platform::SecretStoreStatus> keyWriteStatus;
    {
        std::lock_guard lock(m_Pending->Mutex);
        keyWriteStatus = std::exchange(m_Pending->KeyWriteStatus, std::nullopt);
        if (!keyWriteStatus && !m_Pending->Done && m_Pending->Status.Steps.size() == m_ShownStepCount)
            return;
        done = m_Pending->Done;
        status = m_Pending->Status;
        keySaved = m_Pending->KeySaved;
    }
    if (keyWriteStatus)
        ShowKeyWriteResult(*keyWriteStatus);
    m_ShownStepCount = status.Steps.size();
    if (!done)
    {
        // The steps so far, under the Checking pill.
        ShowSteps(status, /*running=*/true);
        return;
    }
    m_Pending.reset();
    if (keySaved)
        m_ApiKeyRow->ShowSaved(*keySaved);
    ShowStatus(status, keySaved.value_or(false));
}

void AiAssistantSetupPage::StopRefresh()
{
    if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
        manager->UnregisterPeriodicRefresh(m_RefreshToken);
    m_RefreshManager = UIManagerRef();
    m_RefreshToken = 0;
}
} // namespace GameEngine
