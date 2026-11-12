#include "Providers/CancelToken.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeApiStream.h"
#include "FakeApiKeyStore.h"
#include "RecordingTurnEvents.h"
#include "ScopedEnvironmentVariable.h"

#include "LogCapture.h"
#include "LoopbackServer.h"
#include "Platform/HttpClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
using namespace std::chrono_literals;

// A reply in the Messages API's documented stream shape, written by hand: an
// (empty) thinking block the mapper must skip, then a text block in two deltas.
constexpr std::string_view kRecordedReply =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_fixture\",\"type\":\"message\",\"role\":"
    "\"assistant\",\"model\":\"claude-opus-5-5\",\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":"
    "25,\"cache_creation_input_tokens\":0,\"cache_read_input_tokens\":5,\"output_tokens\":1}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"c2ln\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: ping\n"
    "data: {\"type\": \"ping\"}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\", world\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},"
    "\"usage\":{\"output_tokens\":12}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

const std::vector<std::string> kRecordedTurnLog = {"delta:Hello", "delta:, world", "finished:Hello, world"};

HttpClient::HttpResponse Completed()
{
    HttpClient::HttpResponse response;
    response.statusCode = 200;
    response.success = true;
    return response;
}

std::string ChunkedHead()
{
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n"
           "Connection: close\r\n\r\n";
}

// A provider that never touches the system's store; the models URL is unreachable unless given.
ClaudeApiProvider Provider(const FakeApiKeyStore& keys, std::string modelsUrl = "http://127.0.0.1:1/v1/models")
{
    return ClaudeApiProvider(keys.Store(), std::move(modelsUrl));
}

void SendModels(SocketHandle client)
{
    const std::string body = R"({"data":[{"type":"model","id":"claude-opus-5-5"}],"has_more":true})";
    LoopbackServer::Send(client, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
}

std::string Chunk(std::string_view bytes)
{
    std::ostringstream size;
    size << std::hex << bytes.size();
    return size.str() + "\r\n" + std::string(bytes) + "\r\n";
}
} // namespace

TEST(ClaudeApiProviderTests, RecordedReplyYieldsTheTextDeltasInOrderAndTheAccounting)
{
    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(kRecordedReply);
    stream.Finish(Completed());

    EXPECT_EQ(events.Log, kRecordedTurnLog);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.InputTokens, 30u);
    EXPECT_EQ(events.Result.OutputTokens, 12u);
    EXPECT_EQ(events.Result.ModelRoundTrips, 1u);
}

TEST(ClaudeApiProviderTests, ReplySplitAtEveryByteBoundaryYieldsTheSameTurn)
{
    for (size_t split = 0; split <= kRecordedReply.size(); ++split)
    {
        RecordingTurnEvents events;
        ClaudeApiStream stream(events);
        stream.Feed(kRecordedReply.substr(0, split));
        stream.Feed(kRecordedReply.substr(split));
        stream.Finish(Completed());
        ASSERT_EQ(events.Log, kRecordedTurnLog) << "split at byte " << split;
        ASSERT_EQ(events.Result.OutputTokens, 12u) << "split at byte " << split;
    }
}

TEST(ClaudeApiProviderTests, AnErrorEventEndsTheTurnWithItsMessage)
{
    const std::string_view reply =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Partial\"}}\n\n"
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"late\"}}\n\n";

    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(reply);
    stream.Finish(Completed());

    const std::vector<std::string> expected = {"delta:Partial", "finished:Partial"};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("overloaded_error: Overloaded"), std::string::npos) << events.Result.Error;
}

TEST(ClaudeApiProviderTests, AStreamThatEndsBeforeMessageStopFailsKeepingTheText)
{
    const std::string_view stopFrame = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    ASSERT_TRUE(kRecordedReply.ends_with(stopFrame));

    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(kRecordedReply.substr(0, kRecordedReply.size() - stopFrame.size()));
    stream.Finish(Completed());

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Text, "Hello, world");
    EXPECT_NE(events.Result.Error.find("ended before the reply finished"), std::string::npos) << events.Result.Error;
}

TEST(ClaudeApiProviderTests, AnUnknownEventIsSkippedWithoutReadingItsData)
{
    const std::string_view stopFrame = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    ASSERT_TRUE(kRecordedReply.ends_with(stopFrame));
    std::string reply(kRecordedReply.substr(0, kRecordedReply.size() - stopFrame.size()));
    reply += "event: future_event\ndata: not json\n\n";
    reply += stopFrame;

    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(reply);
    stream.Finish(Completed());

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(events.Log, kRecordedTurnLog);
}

TEST(ClaudeApiProviderTests, AReplyCutAtTheTokenLimitOrDeclinedEndsWithANote)
{
    const std::string_view endTurn = "\"stop_reason\":\"end_turn\"";
    const size_t at = kRecordedReply.find(endTurn);
    ASSERT_NE(at, std::string_view::npos);

    const std::pair<std::string, std::string> cases[] = {{"max_tokens", "16384-token limit"},
                                                         {"refusal", "declined"}};
    for (const auto& [stopReason, noteText] : cases)
    {
        std::string reply(kRecordedReply);
        reply.replace(at, endTurn.size(), "\"stop_reason\":\"" + stopReason + "\"");

        RecordingTurnEvents events;
        ClaudeApiStream stream(events);
        stream.Feed(reply);
        stream.Finish(Completed());

        EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << stopReason;
        EXPECT_EQ(events.Result.Text, "Hello, world") << stopReason;
        EXPECT_NE(events.Result.Note.find(noteText), std::string::npos) << stopReason << ": " << events.Result.Note;
    }

    RecordingTurnEvents complete;
    ClaudeApiStream stream(complete);
    stream.Feed(kRecordedReply);
    stream.Finish(Completed());
    EXPECT_TRUE(complete.Result.Note.empty()) << complete.Result.Note;
}

TEST(ClaudeApiProviderTests, TheKeyTravelsInTheXApiKeyHeaderAndNoOther)
{
    const std::string key = "sk-ant-dummy-header-test";
    const std::unordered_map<std::string, std::string> expected = {
        {"x-api-key", key},
        {"anthropic-version", "2023-06-01"},
        {"accept", "text/event-stream"},
    };

    const std::unordered_map<std::string, std::string> headers = ClaudeApiProvider::BuildRequestHeaders(key);

    EXPECT_EQ(headers, expected);
    for (const auto& [name, value] : headers)
    {
        if (name != "x-api-key")
        {
            EXPECT_EQ(value.find(key), std::string::npos) << name;
        }
    }
}

TEST(ClaudeApiProviderTests, AReplyOneByteOverTheCapFailsWithTheProvidersMessageAndKeepsTheText)
{
    const std::string_view delta =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"abcd\"}}\n\n";
    const std::string_view ending =
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":64}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    constexpr int kDeltaCount = 64;

    // The deltas, then comment lines the parser skips, then a complete ending,
    // in exactly one byte more than the cap: without the cap the turn succeeds.
    std::string body;
    std::string expectedText;
    for (int i = 0; i < kDeltaCount; ++i)
    {
        body += delta;
        expectedText += "abcd";
    }
    constexpr size_t kPaddingLine = 1024;
    size_t padding = ClaudeApiStream::kMaxResponseBytes + 1 - body.size() - ending.size();
    while (padding >= 2 * kPaddingLine)
    {
        body += ":" + std::string(kPaddingLine - 2, 'x') + "\n";
        padding -= kPaddingLine;
    }
    body += ":" + std::string(padding - 2, 'x') + "\n";
    body += ending;
    ASSERT_EQ(body.size(), ClaudeApiStream::kMaxResponseBytes + 1);

    LoopbackServer server([&](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, ChunkedHead() + Chunk(body) + "0\r\n\r\n");
    });
    RecordingTurnEvents events;
    CancelToken cancel;

    ClaudeApiStream::Run(server.Url(), {}, "{}", events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Text, expectedText);
    EXPECT_NE(events.Result.Error.find("passed 8 MiB"), std::string::npos) << events.Result.Error;
    EXPECT_EQ(events.Result.Error.find("PostOptions"), std::string::npos) << events.Result.Error;
}

TEST(ClaudeApiProviderTests, TheEndpointIsTheFixedHttpsMessagesApi)
{
    EXPECT_EQ(ClaudeApiProvider::kEndpoint, "https://api.anthropic.com/v1/messages");
    EXPECT_EQ(ClaudeApiProvider::kModelsEndpoint, "https://api.anthropic.com/v1/models?limit=1");
    const ClaudeApiProvider provider = Provider(FakeApiKeyStore{});
    EXPECT_FALSE(provider.Capabilities().OwnsTranscript);
    EXPECT_TRUE(provider.Capabilities().NeedsKey);
}

TEST(ClaudeApiProviderTests, RequestBodyStreamsTheSystemPromptHistoryAndUserTurn)
{
    const std::vector<ConversationMessage> history = {
        {.Role = MessageRole::User, .Text = "first"},
        {.Role = MessageRole::Assistant, .Text = ""},
        {.Role = MessageRole::Assistant, .Text = "reply"},
    };
    AgentTurnRequest request;
    request.SystemPrompt = "Be brief.";
    request.History = history;
    request.UserText = "second";

    const nlohmann::json body = nlohmann::json::parse(ClaudeApiProvider::BuildRequestBody(request));

    EXPECT_EQ(body["model"], "claude-opus-5-5");
    EXPECT_EQ(body["max_tokens"], 16384);
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["system"], "Be brief.");
    const nlohmann::json expectedMessages = nlohmann::json::parse(
        R"([{"role":"user","content":"first"},{"role":"assistant","content":"reply"},{"role":"user","content":"second"}])");
    EXPECT_EQ(body["messages"], expectedMessages);
    EXPECT_FALSE(body.contains("temperature"));
}

TEST(ClaudeApiProviderTests, WithoutAKeyTheTurnIsRefusedNamingTheSettingsPage)
{
    const ScopedEnvironmentVariable noKey("ANTHROPIC_API_KEY", nullptr);
    AgentTurnRequest request;
    request.UserText = "Hello";
    RecordingTurnEvents events;
    CancelToken cancel;

    Provider(FakeApiKeyStore{}).RunTurn(request, events, cancel);

    EXPECT_EQ(events.FinishedCount, 1);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_TRUE(events.Result.NotReady);
    EXPECT_EQ(events.Result.Error, "No Claude API key is saved. Open Settings > AI Assistant, paste a key from the "
                                   "Claude Console and click Save.");
}

TEST(ClaudeApiProviderTests, TheEnvironmentKeyPassesBothStepsAndNoFieldOrLogLineShowsIt)
{
    constexpr const char* kKey = "fixture-anthropic-key";
    const ScopedEnvironmentVariable key("ANTHROPIC_API_KEY", kKey);
    LoopbackServer models([](LoopbackServer&, SocketHandle client) { SendModels(client); });
    std::vector<std::string> lines;
    SetupStatus status;
    {
        const TestLog::ScopedLogCapture capture(&lines);
        Logger::Log::Info("capture is live");
        status = Provider(FakeApiKeyStore{}, models.Url("/v1/models")).CheckSetup({}, SetupSurface::SettingsPage, {});
    }

    ASSERT_EQ(status.Steps.size(), 2u);
    EXPECT_EQ(status.Steps[0].Name, "API key");
    EXPECT_EQ(status.Steps[0].State, SetupStepState::Passed);
    EXPECT_EQ(status.Steps[0].Found, "Uses ANTHROPIC_API_KEY from the environment instead of a saved key.");
    EXPECT_EQ(status.Steps[1].Name, "Key accepted");
    EXPECT_EQ(status.Steps[1].State, SetupStepState::Passed);
    EXPECT_EQ(status.Steps[1].Found, "The Claude API accepted the key.");
    EXPECT_TRUE(status.Ready());
    EXPECT_NE(models.LastRequest().find(std::string("x-api-key: ") + kKey), std::string::npos);
    for (const SetupStep& step : status.Steps)
        for (const std::string* field : {&step.Name, &step.Found, &step.Fix, &step.Program, &step.FirstOutput})
            EXPECT_EQ(field->find(kKey), std::string::npos) << *field;
    ASSERT_EQ(TestLog::CountLinesContaining(lines, "capture is live"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "AI Assistant setup: "), 2u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, std::string_view(kKey).substr(0, 8)), 0u);
}

TEST(ClaudeApiProviderTests, ASavedKeyWorksWithoutTheEnvironmentVariable)
{
    const ScopedEnvironmentVariable noKey("ANTHROPIC_API_KEY", nullptr);
    FakeApiKeyStore keys;
    keys.Data->Saved = "fixture-saved-key";
    LoopbackServer models([](LoopbackServer&, SocketHandle client) { SendModels(client); });

    const SetupStatus status =
        Provider(keys, models.Url("/v1/models")).CheckSetup({}, SetupSurface::SettingsPage, {});

    ASSERT_EQ(status.Steps.size(), 2u);
    EXPECT_EQ(status.Steps[0].Found, "A key is saved in the system's credential store.");
    EXPECT_EQ(status.Steps[1].State, SetupStepState::Passed);
    EXPECT_EQ(keys.Data->LastName, "GameEngine/Editor/ai-assistant/anthropic-api-key");
    EXPECT_NE(models.LastRequest().find("x-api-key: fixture-saved-key"), std::string::npos);
}

// A set variable is read first, so the store (a Keychain prompt on macOS) is not read at all.
TEST(ClaudeApiProviderTests, TheEnvironmentVariableWinsOverASavedKey)
{
    const ScopedEnvironmentVariable key("ANTHROPIC_API_KEY", "fixture-environment-key");
    FakeApiKeyStore keys;
    keys.Data->Saved = "fixture-saved-key";
    LoopbackServer models([](LoopbackServer&, SocketHandle client) { SendModels(client); });

    Provider(keys, models.Url("/v1/models")).CheckSetup({}, SetupSurface::SettingsPage, {});

    EXPECT_EQ(keys.Data->Reads, 0);
    EXPECT_NE(models.LastRequest().find("x-api-key: fixture-environment-key"), std::string::npos);
    EXPECT_EQ(models.LastRequest().find("fixture-saved-key"), std::string::npos);
}

TEST(ClaudeApiProviderTests, WithoutAKeyOnlyTheKeyStepRunsAndItsFixNamesTheSurfacesControls)
{
    const ScopedEnvironmentVariable noKey("ANTHROPIC_API_KEY", nullptr);
    std::vector<std::string> published;
    const SetupStatus page = Provider(FakeApiKeyStore{}).CheckSetup(
        {}, SetupSurface::SettingsPage, [&published](const SetupStep& step) { published.push_back(step.Name); });
    const SetupStatus panel = Provider(FakeApiKeyStore{}).CheckSetup({}, SetupSurface::Panel, {});

    EXPECT_EQ(published, std::vector<std::string>{"API key"});
    ASSERT_EQ(page.Steps.size(), 1u);
    EXPECT_EQ(page.Steps[0].State, SetupStepState::Failed);
    EXPECT_EQ(page.Steps[0].Fix, "Paste a key from the Claude Console and click Save.");
    ASSERT_EQ(panel.Steps.size(), 1u);
    EXPECT_EQ(panel.Steps[0].Fix, "Open Settings > AI Assistant, paste a key from the Claude Console and click Save.");
    EXPECT_FALSE(page.Ready());
}

TEST(ClaudeApiProviderTests, WithoutACredentialStoreTheFixIsTheEnvironmentVariable)
{
    const ScopedEnvironmentVariable noKey("ANTHROPIC_API_KEY", nullptr);
    FakeApiKeyStore keys;
    keys.Data->Refuse = Platform::SecretStoreStatus::NoStore;

    const SetupStatus status = Provider(keys).CheckSetup({}, SetupSurface::SettingsPage, {});

    ASSERT_EQ(status.Steps.size(), 1u);
    EXPECT_EQ(status.Steps[0].Found,
              "No Claude API key: this system has no credential store.");
    EXPECT_EQ(status.Steps[0].Fix, "Set ANTHROPIC_API_KEY in the environment, then start the editor again.");
}

// Only a 401 says the key is wrong: another answer or no answer warns, and a turn may still run.
TEST(ClaudeApiProviderTests, KeyAcceptedFailsOnlyOn401AndWarnsWhenTheApiCannotSay)
{
    const ScopedEnvironmentVariable noKey("ANTHROPIC_API_KEY", nullptr);
    FakeApiKeyStore keys;
    keys.Data->Saved = "fixture-saved-key";
    const auto answer = [](std::string head) {
        return [head](LoopbackServer&, SocketHandle client) { LoopbackServer::Send(client, head); };
    };
    LoopbackServer refused(answer("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
    LoopbackServer busy(answer("HTTP/1.1 529 Overloaded\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));

    const SetupStatus refusedStatus = Provider(keys, refused.Url()).CheckSetup({}, SetupSurface::SettingsPage, {});
    const SetupStatus busyStatus = Provider(keys, busy.Url()).CheckSetup({}, SetupSurface::SettingsPage, {});
    const SetupStatus unreachable = Provider(keys).CheckSetup({}, SetupSurface::SettingsPage, {});

    ASSERT_EQ(refusedStatus.Steps.size(), 2u);
    EXPECT_EQ(refusedStatus.Steps[1].State, SetupStepState::Failed);
    EXPECT_EQ(refusedStatus.Steps[1].Found, "The Claude API refused the key (HTTP 401).");
    EXPECT_EQ(refusedStatus.Steps[1].Fix, "Paste a new key from the Claude Console and click Save.");
    EXPECT_FALSE(refusedStatus.Ready());
    ASSERT_EQ(busyStatus.Steps.size(), 2u);
    EXPECT_EQ(busyStatus.Steps[1].State, SetupStepState::Warning);
    EXPECT_EQ(busyStatus.Steps[1].Found, "The Claude API answered HTTP 529, so the key is not checked.");
    EXPECT_TRUE(busyStatus.Ready());
    ASSERT_EQ(unreachable.Steps.size(), 2u);
    EXPECT_EQ(unreachable.Steps[1].State, SetupStepState::Warning);
    EXPECT_EQ(unreachable.Steps[1].Found.rfind("The Claude API could not be reached (", 0), 0u)
        << unreachable.Steps[1].Found;
}

TEST(ClaudeApiProviderTests, TheStoreSavesRemovesAndRefusesAnEmptyKey)
{
    const ScopedEnvironmentVariable noKey("ANTHROPIC_API_KEY", nullptr);
    FakeApiKeyStore fake;
    const ApiKeyStore keys = fake.Store();

    EXPECT_EQ(keys.Save(""), Platform::SecretStoreStatus::Failed);
    EXPECT_FALSE(keys.HasSavedKey());
    EXPECT_EQ(keys.Save("fixture-saved-key"), Platform::SecretStoreStatus::Done);
    EXPECT_TRUE(keys.HasSavedKey());
    EXPECT_EQ(keys.Read().From, ApiKeyStore::Source::Saved);
    EXPECT_EQ(keys.Remove(), Platform::SecretStoreStatus::Done);
    EXPECT_EQ(keys.Remove(), Platform::SecretStoreStatus::NotFound);
    EXPECT_EQ(keys.Read().From, ApiKeyStore::Source::None);
    EXPECT_EQ(keys.Read().StoreStatus, Platform::SecretStoreStatus::NotFound);
}

TEST(ClaudeApiProviderTests, RecordedReplyStreamedFromALoopbackServerYieldsTheSameTurn)
{
    // Three uneven pieces, so event boundaries and chunk boundaries disagree.
    LoopbackServer server([](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, ChunkedHead());
        LoopbackServer::Send(client, Chunk(kRecordedReply.substr(0, 101)));
        LoopbackServer::Send(client, Chunk(kRecordedReply.substr(101, 700)));
        LoopbackServer::Send(client, Chunk(kRecordedReply.substr(801)));
        LoopbackServer::Send(client, "0\r\n\r\n");
    });
    RecordingTurnEvents events;
    CancelToken cancel;

    ClaudeApiStream::Run(server.Url("/v1/messages"), {{"anthropic-version", "2023-06-01"}}, "{}", events, cancel);

    EXPECT_EQ(events.Log, kRecordedTurnLog);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(server.RequestCount(), 1);
}

TEST(ClaudeApiProviderTests, ARefusedRequestFailsWithTheApiMessageAndTheFix)
{
    const std::string errorBody =
        R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})";
    LoopbackServer server([&](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: " +
                                         std::to_string(errorBody.size()) + "\r\nConnection: close\r\n\r\n" +
                                         errorBody);
    });
    RecordingTurnEvents events;
    CancelToken cancel;

    ClaudeApiStream::Run(server.Url(), {}, "{}", events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("HTTP 401, authentication_error: invalid x-api-key"), std::string::npos)
        << events.Result.Error;
    EXPECT_NE(events.Result.Error.find("Check the key in Settings > AI Assistant, or ANTHROPIC_API_KEY when it is set."),
              std::string::npos)
        << events.Result.Error;
}

TEST(ClaudeApiProviderTests, CancellingMidStreamStopsTheTurnWithTheTextSoFar)
{
    // The server sends the first delta, then goes quiet until the test ends:
    // only the cancel token can end the turn.
    const std::string_view firstDelta =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n";
    LoopbackServer server([&](LoopbackServer& self, SocketHandle client) {
        LoopbackServer::Send(client, ChunkedHead() + Chunk(firstDelta));
        self.WaitForSignals(1, 30s);
    });

    class CancelOnFirstDelta final : public RecordingTurnEvents
    {
    public:
        explicit CancelOnFirstDelta(CancelToken& token) : m_Token(token) {}
        void OnTextDelta(std::string_view text) override
        {
            RecordingTurnEvents::OnTextDelta(text);
            m_Token.Cancel();
        }

    private:
        CancelToken& m_Token;
    };
    CancelToken cancel;
    CancelOnFirstDelta events(cancel);

    const auto start = std::chrono::steady_clock::now();
    ClaudeApiStream::Run(server.Url(), {}, "{}", events, cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const std::vector<std::string> expected = {"delta:Hello", "finished:Hello"};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Stopped);
    EXPECT_TRUE(events.Result.Error.empty()) << events.Result.Error;
    EXPECT_LT(elapsed, 10s);
}

namespace
{
std::string SseFrame(std::string_view type, std::string_view data)
{
    return "event: " + std::string(type) + "\ndata: " + std::string(data) + "\n\n";
}
} // namespace

// Text blocks of one reply (around a tool_use block) are joined by a paragraph break, never
// run together (issue #3435); a block that opens the reply or adds no text adds no break.
TEST(ClaudeApiProviderTests, TextBlocksOfOneReplyAreSeparatedByAParagraphBreak)
{
    const std::string reply =
        SseFrame("message_start", "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":1}}}") +
        SseFrame("content_block_start",
                 "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}") +
        SseFrame("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                                        "\"text_delta\",\"text\":\"Creating the lantern.\"}}") +
        SseFrame("content_block_start", "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
                                        "\"tool_use\",\"id\":\"toolu_1\",\"name\":\"create_entity\",\"input\":{}}}") +
        SseFrame("content_block_start",
                 "{\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}") +
        SseFrame("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":"
                                        "\"text_delta\",\"text\":\"It is lit.\"}}") +
        SseFrame("content_block_start",
                 "{\"type\":\"content_block_start\",\"index\":3,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}") +
        SseFrame("message_stop", "{\"type\":\"message_stop\"}");
    RecordingTurnEvents events;
    ClaudeApiStream stream(events);
    stream.Feed(reply);
    stream.Finish(Completed());

    const std::vector<std::string> expected = {"delta:Creating the lantern.", "delta:\n\nIt is lit.",
                                               "finished:Creating the lantern.\n\nIt is lit."};
    EXPECT_EQ(events.Log, expected);
}
} // namespace GameEngine
