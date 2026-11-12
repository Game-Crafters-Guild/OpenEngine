#pragma once

#include "Providers/SetupStatus.h"
#include "UI/UIElement.h"
#include "UI/UIManagerRef.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace GameEngine
{
namespace Platform
{
enum class SecretStoreStatus : uint8_t;
}
class AiAssistantApiKeyRow;
class Button;
class Dropdown;
class Foldout;
class IAgentProvider;
class Label;
class TextField;
class Toggle;
class UIManager;

namespace EditorUI
{
class SetupStepList;
}

/// Settings > AI Assistant: the assistant, its sign-in method, the connection's setup checks and
/// its Model and options, from AiAssistantSetup/AiAssistantSetupPage.uxml. The checks run on a thread
/// of their own when the page opens, on Check again and when the connection or an executable changes.
class AiAssistantSetupPage final : public UIElement
{
public:
    AiAssistantSetupPage();
    /// Cancels a check in flight and waits for its thread.
    ~AiAssistantSetupPage() override;

protected:
    void OnOwnerManagerChanged(UIManager* owner) override;

private:
    struct PendingCheck;

    bool Build(UIManager& ui);
    void ShowConnection();
    void ShowAssistantChoices(const std::string& assistant);
    void ShowMethodChoices(const std::string& providerId);
    void ShowExecutableRow(const std::string& providerId);
    void ShowOptions(const std::string& providerId);
    void ShowOptionsSummary(const std::string& providerId);
    /// Shows `status` as the chosen connection's checklist: the summary pill, the line beside
    /// it and one row per step. `keySaved` tells a saved Claude API key from one in the environment.
    void ShowStatus(const SetupStatus& status, bool keySaved);
    /// The steps of `status`, then the plan's steps that have not ended; the first of those
    /// runs while `running`.
    void ShowSteps(const SetupStatus& status, bool running);
    void StoreExecutable(const std::string& executable);

    /// Starts a check on the check thread; a `keyWrite` that is not empty runs first on that thread.
    void StartCheck(std::function<Platform::SecretStoreStatus()> keyWrite);
    static void RunCheck(std::shared_ptr<PendingCheck> pending, std::shared_ptr<IAgentProvider> provider,
                         std::string node, uint16_t port);
    /// Stops the check in flight and starts one that writes the key store first.
    void WriteKey(std::function<Platform::SecretStoreStatus()> keyWrite);
    /// Hands the end of a key write to the key row; a changed key makes the panel check again.
    void ShowKeyWriteResult(Platform::SecretStoreStatus status);
    void CancelCheck();
    /// Runs a new check when SetupGeneration() moved since the page last checked.
    void FollowSetup();
    void Refresh();
    void PollCheck();
    void StopRefresh();

    bool m_Built = false;
    UIElement* m_AssistantHost = nullptr;
    UIElement* m_MethodHost = nullptr;
    Label* m_MethodNeeds = nullptr;
    Label* m_SetupHeader = nullptr;
    UIElement* m_ExecutableRow = nullptr;
    Label* m_ExecutableLabel = nullptr;
    TextField* m_ExecutableField = nullptr;
    AiAssistantApiKeyRow* m_ApiKeyRow = nullptr;
    EditorUI::SetupStepList* m_Steps = nullptr;
    Foldout* m_Options = nullptr;
    UIElement* m_ModelRow = nullptr;
    Dropdown* m_ModelDropdown = nullptr;
    UIElement* m_EffortRow = nullptr;
    Dropdown* m_EffortDropdown = nullptr;
    UIElement* m_CodexModelRow = nullptr;
    TextField* m_CodexModelField = nullptr;
    Dropdown* m_ModeDropdown = nullptr;
    UIElement* m_NodeRow = nullptr;
    TextField* m_NodeField = nullptr;
    UIElement* m_DictationRows = nullptr;
    Toggle* m_HoldToSpeak = nullptr;
    Toggle* m_SendWhenMicStops = nullptr;

    /// What the choice fields show, so a choice made on the page does not rebuild the field
    /// the user is working in.
    std::string m_ShownAssistant;
    std::string m_ShownProvider;

    /// The steps of the check that runs or ran last, before any ends (IAgentProvider::SetupPlan).
    std::vector<SetupStep> m_Plan;
    std::shared_ptr<PendingCheck> m_Pending;
    /// How many steps of the running check the list shows.
    size_t m_ShownStepCount = 0;
    std::thread m_CheckThread;
    uint64_t m_SeenSetupGeneration = 0;
    uint64_t m_SeenProviderGeneration = 0;
    uint64_t m_SeenModelGeneration = 0;
    UIManagerRef m_RefreshManager;
    uint64_t m_RefreshToken = 0;
};
} // namespace GameEngine
