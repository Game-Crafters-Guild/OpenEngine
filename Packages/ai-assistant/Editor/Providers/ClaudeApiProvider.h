#pragma once

#include "ApiKeyStore.h"
#include "IAgentProvider.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{
/// The Claude Messages API, streamed. Stateless: every turn resends the system
/// prompt, the history and the new user message. The key comes from the ApiKeyStore
/// when the turn starts (the environment variable, else the saved key) and goes only
/// to the fixed endpoint below: there is no endpoint setting.
class ClaudeApiProvider final : public IAgentProvider
{
public:
    /// The provider's id (Id()).
    static constexpr std::string_view kId = "claude-api";
    /// The only URL this provider posts to.
    static constexpr std::string_view kEndpoint = "https://api.anthropic.com/v1/messages";
    /// The URL the Key accepted step reads: one model, so the answer is small.
    static constexpr std::string_view kModelsEndpoint = "https://api.anthropic.com/v1/models?limit=1";
    /// The model a turn uses when AgentTurnRequest::Model is empty.
    static constexpr std::string_view kDefaultModel = "claude-opus-5-5";
    /// The request's max_tokens: the longest reply a turn can produce.
    static constexpr int kMaxReplyTokens = 16384;
    /// The setup steps' names.
    static constexpr std::string_view kApiKeyStep = "API key";
    static constexpr std::string_view kKeyAcceptedStep = "Key accepted";

    /// `modelsUrl` is the URL the Key accepted step reads (kModelsEndpoint in the editor).
    ClaudeApiProvider(ApiKeyStore keys, std::string modelsUrl);

    std::string_view Id() const override { return kId; }
    ProviderCapabilities Capabilities() const override;

    /// Two steps. API key: Passed when the environment variable is set or a key is saved.
    /// Key accepted: one GET of the models URL with the key; Failed on 401, Warning when the
    /// API cannot be reached. No field and no log line holds the key's value.
    SetupStatus CheckSetup(const CancelToken& cancel, SetupSurface surface, const SetupStepSink& onStep) const override;
    std::vector<SetupStep> SetupPlan() const override;

    /// Reads the key once: refuses with the API key step's finding and fix when there is
    /// none; otherwise streams the turn from kEndpoint (ClaudeApiStream) with it.
    void RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel) override;

    /// The request body: model, max_tokens, stream, the system prompt when there
    /// is one, and messages = the history (messages with empty text skipped, as
    /// the API refuses them) followed by the user's message.
    static std::string BuildRequestBody(const AgentTurnRequest& request);

    /// The request headers: x-api-key = `key`, anthropic-version and accept
    /// (an event stream). The key goes in that one header and nowhere else.
    static std::unordered_map<std::string, std::string> BuildRequestHeaders(std::string_view key);

private:
    ApiKeyStore m_Keys;
    std::string m_ModelsUrl;
};
} // namespace GameEngine
