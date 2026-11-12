#include "ClaudeApiProvider.h"

#include "CancelToken.h"
#include "ClaudeApiStream.h"

#include "Logger/Logger.h"
#include "Platform/HttpClient.h"

#include <chrono>
#include <cstddef>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
constexpr const char* kApiVersion = "2023-06-01";
// A models list of one entry is a few hundred bytes; the cap bounds a misbehaving answer.
constexpr size_t kMaxModelsResponseBytes = size_t{64} * 1024;
// The models list answers in well under a second; a check that waits longer reports the network.
constexpr std::chrono::milliseconds kKeyCheckTimeout = std::chrono::seconds(15);
constexpr int kHttpUnauthorized = 401;

std::string Variable()
{
    return std::string(ApiKeyStore::kVariable);
}

// The fix for a missing key names the controls of `surface`: the page has the key field.
std::string PasteKeyFix(SetupSurface surface)
{
    return surface == SetupSurface::SettingsPage
               ? "Paste a key from the Claude Console and click Save."
               : "Open Settings > AI Assistant, paste a key from the Claude Console and click Save.";
}

// The API key step for `key`; the step never holds the key's value.
// The step names the environment variable only when it is set, or when it is the only way to give a key.
SetupStep ApiKeyStep(const ApiKeyStore::Key& key, SetupSurface surface)
{
    SetupStep step{.Name = std::string(ClaudeApiProvider::kApiKeyStep)};
    if (key.From == ApiKeyStore::Source::Environment)
    {
        step.State = SetupStepState::Passed;
        step.Found = "Uses " + Variable() + " from the environment instead of a saved key.";
        return step;
    }
    if (key.From == ApiKeyStore::Source::Saved)
    {
        step.State = SetupStepState::Passed;
        step.Found = "A key is saved in the system's credential store.";
        return step;
    }
    step.State = SetupStepState::Failed;
    if (key.StoreStatus == Platform::SecretStoreStatus::NoStore)
    {
        step.Found = "No Claude API key: this system has no credential store.";
        step.Fix = "Set " + Variable() + " in the environment, then start the editor again.";
        return step;
    }
    if (key.StoreStatus == Platform::SecretStoreStatus::Failed)
        step.Found = "No Claude API key: the system's credential store refused the read.";
    else
        step.Found = "No Claude API key is saved.";
    step.Fix = PasteKeyFix(surface);
    return step;
}

// The fix for a key the API refused: the key comes from the variable or from the saved key.
std::string RefusedKeyFix(const ApiKeyStore::Key& key, SetupSurface surface)
{
    if (key.From == ApiKeyStore::Source::Environment)
        return "Set a valid key in " + Variable() + ", then start the editor again.";
    return surface == SetupSurface::SettingsPage
               ? "Paste a new key from the Claude Console and click Save."
               : "Open Settings > AI Assistant, paste a new key from the Claude Console and click Save.";
}

// The Key accepted step: one GET of `modelsUrl` with the key, stopped by `cancel`. Only 401 fails:
// a network error or another status says nothing about the key, so it warns and a turn may still run.
SetupStep KeyAcceptedStep(const ApiKeyStore::Key& key, const std::string& modelsUrl, SetupSurface surface,
                          const CancelToken& cancel)
{
    SetupStep step{.Name = std::string(ClaudeApiProvider::kKeyAcceptedStep)};
    std::unordered_map<std::string, std::string> headers = ClaudeApiProvider::BuildRequestHeaders(key.Value);
    headers.erase("accept");
    const HttpClient::HttpResponse response = HttpClient::Get(
        modelsUrl, headers,
        {.MaxResponseBytes = kMaxModelsResponseBytes, .Timeout = kKeyCheckTimeout, .Cancellation = &cancel.Flag()});
    if (response.success)
    {
        step.State = SetupStepState::Passed;
        step.Found = "The Claude API accepted the key.";
        return step;
    }
    if (response.statusCode == kHttpUnauthorized)
    {
        step.State = SetupStepState::Failed;
        step.Found = "The Claude API refused the key (HTTP 401).";
        step.Fix = RefusedKeyFix(key, surface);
        return step;
    }
    step.State = SetupStepState::Warning;
    step.Found = response.statusCode == 0
                     ? "The Claude API could not be reached (" + response.error + "), so the key is not checked."
                     : "The Claude API answered HTTP " + std::to_string(response.statusCode) +
                           ", so the key is not checked.";
    step.Fix = "Check the network, then click Check again.";
    return step;
}

const char* StateWord(SetupStepState state)
{
    switch (state)
    {
    case SetupStepState::Passed:
        return "passed";
    case SetupStepState::Warning:
        return "warning";
    case SetupStepState::Failed:
        return "failed";
    default:
        return "not run";
    }
}

// One editor log line per step for support, as CliSessionProvider writes; never the key's value.
void LogStep(const SetupStep& step)
{
    Logger::Log::Info("AI Assistant setup: {}: {} ({})", step.Name, StateWord(step.State), step.Found);
}
} // namespace

ClaudeApiProvider::ClaudeApiProvider(ApiKeyStore keys, std::string modelsUrl)
    : m_Keys(std::move(keys)), m_ModelsUrl(std::move(modelsUrl))
{
}

ProviderCapabilities ClaudeApiProvider::Capabilities() const
{
    return {.Streams = true, .OwnsTranscript = false, .NeedsKey = true, .CanActWithTools = false};
}

SetupStatus ClaudeApiProvider::CheckSetup(const CancelToken& cancel, SetupSurface surface,
                                          const SetupStepSink& onStep) const
{
    SetupStatus status;
    const ApiKeyStore::Key key = m_Keys.Read();
    SetupStep keyStep = ApiKeyStep(key, surface);
    LogStep(keyStep);
    if (onStep)
        onStep(keyStep);
    const bool hasKey = keyStep.State == SetupStepState::Passed;
    status.Steps.push_back(std::move(keyStep));
    if (!hasKey || cancel.IsCancelled())
        return status;

    SetupStep accepted = KeyAcceptedStep(key, m_ModelsUrl, surface, cancel);
    LogStep(accepted);
    if (onStep)
        onStep(accepted);
    status.Steps.push_back(std::move(accepted));
    return status;
}

std::vector<SetupStep> ClaudeApiProvider::SetupPlan() const
{
    return {{.Name = std::string(kApiKeyStep),
             .Found = "Looks for the key the editor uses."},
            {.Name = std::string(kKeyAcceptedStep), .Found = "Asks the Claude API whether it accepts the key."}};
}

// A turn logs the key step only when it refuses: a turn that runs adds no line.
void ClaudeApiProvider::RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel)
{
    const ApiKeyStore::Key key = m_Keys.Read();
    if (const SetupStep step = ApiKeyStep(key, SetupSurface::Panel); step.State == SetupStepState::Failed)
    {
        LogStep(step);
        TurnResult result;
        result.Outcome = TurnOutcome::Failed;
        result.NotReady = true;
        result.Error = step.Found + " " + step.Fix;
        events.OnFinished(result);
        return;
    }
    ClaudeApiStream::Run(std::string(kEndpoint), BuildRequestHeaders(key.Value), BuildRequestBody(request), events,
                         cancel);
}

std::unordered_map<std::string, std::string> ClaudeApiProvider::BuildRequestHeaders(std::string_view key)
{
    return {
        {"x-api-key", std::string(key)},
        {"anthropic-version", kApiVersion},
        {"accept", "text/event-stream"},
    };
}

std::string ClaudeApiProvider::BuildRequestBody(const AgentTurnRequest& request)
{
    nlohmann::json messages = nlohmann::json::array();
    for (const ConversationMessage& message : request.History)
    {
        if (message.Text.empty())
            continue;
        messages.push_back({{"role", message.Role == MessageRole::User ? "user" : "assistant"},
                            {"content", message.Role == MessageRole::User ? message.SentText()
                                                                          : message.Text}});
    }
    messages.push_back({{"role", "user"}, {"content", request.UserText}});

    nlohmann::json body = {
        {"model", request.Model.empty() ? std::string(kDefaultModel) : request.Model},
        {"max_tokens", kMaxReplyTokens},
        {"stream", true},
        {"messages", std::move(messages)},
    };
    if (!request.SystemPrompt.empty())
        body["system"] = request.SystemPrompt;
    // Invalid UTF-8 in typed text becomes U+FFFD instead of throwing out of the turn.
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
} // namespace GameEngine
