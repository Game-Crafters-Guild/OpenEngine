#include "Platform/HttpClient.h"
#include "HttpHeaderRules.h"
#include "LoopbackServer.h"
#include "Types/StringUtils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace
{

using namespace std::chrono_literals;

std::string OkHead(size_t contentLength)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(contentLength) + "\r\nConnection: close\r\n\r\n";
}

TEST(HttpClientTests, PostJsonRefusesAResponseOneByteOverTheCap)
{
    const std::string payload = R"({"text":"sixteen"})";
    LoopbackServer server([&](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, OkHead(payload.size()) + payload);
    });

    HttpClient::PostOptions options;
    options.MaxResponseBytes = payload.size();
    const HttpClient::HttpResponse atCap = HttpClient::PostJson(server.Url(), {}, "{}", options);
    EXPECT_TRUE(atCap.success) << atCap.error;
    EXPECT_EQ(atCap.body, payload);

    options.MaxResponseBytes = payload.size() - 1;
    const HttpClient::HttpResponse overCap = HttpClient::PostJson(server.Url(), {}, "{}", options);
    EXPECT_FALSE(overCap.success);
    EXPECT_TRUE(overCap.overflowed);
    EXPECT_TRUE(overCap.body.empty());
    EXPECT_NE(overCap.error.find("(" + std::to_string(payload.size() - 1) + " bytes)"), std::string::npos)
        << overCap.error;
}

TEST(HttpClientTests, StreamRefusesAChunkedBodyOneByteOverTheCap)
{
    // A chunked reply declares no length, so only the received-byte count can
    // enforce the cap: the case of a server-sent event stream.
    const std::string payload = R"({"text":"sixteen"})";
    LoopbackServer server([&](LoopbackServer&, SocketHandle client) {
        std::ostringstream chunkSize;
        chunkSize << std::hex << payload.size();
        LoopbackServer::Send(client, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                     "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
                                         chunkSize.str() + "\r\n" + payload + "\r\n0\r\n\r\n");
    });

    HttpClient::PostOptions options;
    options.MaxResponseBytes = payload.size() - 1;
    std::string streamed;
    const HttpClient::HttpResponse response = HttpClient::PostJsonStream(
        server.Url(), {}, "{}", options, [&](std::string_view chunk) { streamed += chunk; });

    EXPECT_FALSE(response.success);
    EXPECT_TRUE(response.overflowed);
    EXPECT_LE(streamed.size(), options.MaxResponseBytes);
    EXPECT_NE(response.error.find("(" + std::to_string(payload.size() - 1) + " bytes)"), std::string::npos)
        << response.error;
}

TEST(HttpClientTests, StreamRethrowsAChunkCallbackExceptionAndTheNextRequestWorks)
{
    LoopbackServer server([](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, OkHead(2) + "{}");
    });

    EXPECT_THROW(HttpClient::PostJsonStream(server.Url(), {}, "{}", {},
                                            [](std::string_view) { throw std::runtime_error("caller failed"); }),
                 std::runtime_error);

    const HttpClient::HttpResponse next = HttpClient::PostJson(server.Url(), {}, "{}", {});
    EXPECT_TRUE(next.success) << next.error;
    EXPECT_EQ(next.body, "{}");
}

TEST(HttpClientTests, CancellingMidTransferReturnsCancelledWithoutABody)
{
    // The flag is set only after the client has consumed the partial body and
    // the server has gone quiet, so only the idle-time check can notice it.
    LoopbackServer server([](LoopbackServer& self, SocketHandle client) {
        LoopbackServer::Send(client, OkHead(1000) + R"({"partial":)");
        self.WaitForSignals(1, 30s);
    });

    std::atomic<bool> cancellation{false};
    std::promise<void> firstChunkReceived;
    std::atomic<bool> anyChunkReceived{false};
    HttpClient::PostOptions options;
    options.Timeout = 20s;
    options.Cancellation = &cancellation;
    const auto start = std::chrono::steady_clock::now();
    std::future<HttpClient::HttpResponse> request = std::async(std::launch::async, [&] {
        return HttpClient::PostJsonStream(server.Url(), {}, "{}", options, [&](std::string_view) {
            if (!anyChunkReceived.exchange(true))
                firstChunkReceived.set_value();
        });
    });

    ASSERT_EQ(firstChunkReceived.get_future().wait_for(10s), std::future_status::ready);
    cancellation = true;
    ASSERT_EQ(request.wait_for(10s), std::future_status::ready) << "cancellation did not stop the transfer";
    const HttpClient::HttpResponse response = request.get();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(response.cancelled) << response.error;
    EXPECT_FALSE(response.success);
    EXPECT_TRUE(response.body.empty()) << response.body;
    EXPECT_LT(elapsed, options.Timeout);
}

TEST(HttpClientTests, TimeoutEndsARequestTheServerNeverAnswers)
{
    LoopbackServer server([](LoopbackServer& self, SocketHandle) { self.WaitForSignals(1, 30s); });

    HttpClient::PostOptions options;
    options.Timeout = 300ms;
    const auto start = std::chrono::steady_clock::now();
    const HttpClient::HttpResponse response = HttpClient::PostJson(server.Url(), {}, "{}", options);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(response.success);
    EXPECT_FALSE(response.cancelled);
    EXPECT_NE(response.error.find("timed out after 300 ms"), std::string::npos) << response.error;
    EXPECT_LT(elapsed, 10s);
}

// The server accepts and never answers, so only the idle-time check can notice the flag; the
// stall limit alone would hold the call for 30 s.
TEST(HttpClientTests, GetReturnsCancelledWithinASecondWhileTheServerNeverAnswers)
{
    LoopbackServer server([](LoopbackServer& self, SocketHandle) { self.WaitForSignals(1, 60s); });

    std::atomic<bool> cancellation{false};
    std::future<HttpClient::HttpResponse> request = std::async(std::launch::async, [&] {
        return HttpClient::Get(server.Url(), {}, {.Cancellation = &cancellation});
    });
    const auto sent = std::chrono::steady_clock::now();
    while (server.RequestCount() == 0 && std::chrono::steady_clock::now() - sent < 10s)
        std::this_thread::sleep_for(10ms);
    ASSERT_EQ(server.RequestCount(), 1);

    const auto start = std::chrono::steady_clock::now();
    cancellation = true;
    ASSERT_EQ(request.wait_for(5s), std::future_status::ready) << "cancellation did not stop the request";
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const HttpClient::HttpResponse response = request.get();

    EXPECT_TRUE(response.cancelled) << response.error;
    EXPECT_FALSE(response.success);
    EXPECT_LT(elapsed, 2s);
}

TEST(HttpClientTests, GetTimeoutEndsARequestTheServerNeverAnswers)
{
    LoopbackServer server([](LoopbackServer& self, SocketHandle) { self.WaitForSignals(1, 60s); });

    const auto start = std::chrono::steady_clock::now();
    const HttpClient::HttpResponse response = HttpClient::Get(server.Url(), {}, {.Timeout = 300ms});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(response.success);
    EXPECT_FALSE(response.cancelled);
    EXPECT_NE(response.error.find("timed out after 300 ms"), std::string::npos) << response.error;
    EXPECT_LT(elapsed, 10s);
}

TEST(HttpClientTests, StreamDeliversEachChunkBeforeTheNextIsSent)
{
    const std::vector<std::string> parts = {"data: one\n\n", "data: two\n\n", "data: three\n\n"};
    std::string fullBody;
    for (const std::string& part : parts)
        fullBody += part;

    // The server sends the next part only once the client has seen the
    // previous one: a client that buffered the body would leave it waiting.
    std::atomic<bool> serverWaitTimedOut{false};
    LoopbackServer server([&](LoopbackServer& self, SocketHandle client) {
        LoopbackServer::Send(client, OkHead(fullBody.size()));
        for (size_t i = 0; i < parts.size(); ++i)
        {
            LoopbackServer::Send(client, parts[i]);
            if (!self.WaitForSignals(static_cast<int>(i) + 1, 2s))
                serverWaitTimedOut = true;
        }
    });

    std::vector<std::string> chunks;
    std::string streamed;
    const HttpClient::HttpResponse response =
        HttpClient::PostJsonStream(server.Url(), {}, R"({"stream":true})", {}, [&](std::string_view chunk) {
            chunks.emplace_back(chunk);
            streamed += chunk;
            // One signal per completed part, however the transport split it.
            size_t partEnd = 0;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                partEnd += parts[i].size();
                if (streamed.size() == partEnd)
                    server.Signal();
            }
        });

    EXPECT_TRUE(response.success) << response.error;
    EXPECT_TRUE(response.body.empty());
    EXPECT_FALSE(serverWaitTimedOut) << "a part reached the client only after the server moved on";
    EXPECT_GE(chunks.size(), parts.size());
    EXPECT_EQ(streamed, fullBody);
}

TEST(HttpClientTests, StreamBuffersANon2xxBodyInsteadOfStreamingIt)
{
    const std::string errorBody = R"({"error":"overloaded"})";
    LoopbackServer server([&](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
                                     "Content-Length: " + std::to_string(errorBody.size()) +
                                         "\r\nConnection: close\r\n\r\n" + errorBody);
    });

    size_t chunkCount = 0;
    const HttpClient::HttpResponse response =
        HttpClient::PostJsonStream(server.Url(), {}, "{}", {}, [&](std::string_view) { ++chunkCount; });

    EXPECT_EQ(response.statusCode, 500);
    EXPECT_FALSE(response.success);
    EXPECT_EQ(response.body, errorBody);
    EXPECT_EQ(chunkCount, 0u);
}

TEST(HttpClientTests, StreamTimeoutLimitsSilenceNotDuration)
{
    static constexpr size_t kSteadyBytes = 15;
    static constexpr auto kByteInterval = 100ms;
    LoopbackServer steady([](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, OkHead(kSteadyBytes));
        for (size_t i = 0; i < kSteadyBytes; ++i)
        {
            LoopbackServer::Send(client, "x");
            std::this_thread::sleep_for(kByteInterval);
        }
    });
    LoopbackServer silent([](LoopbackServer& self, SocketHandle client) {
        LoopbackServer::Send(client, OkHead(kSteadyBytes));
        self.WaitForSignals(1, 30s);
    });

    HttpClient::PostOptions options;
    options.Timeout = 500ms;
    std::string streamed;
    const auto appendChunk = [&](std::string_view chunk) { streamed += chunk; };

    const auto steadyStart = std::chrono::steady_clock::now();
    const HttpClient::HttpResponse steadyResponse =
        HttpClient::PostJsonStream(steady.Url(), {}, "{}", options, appendChunk);
    const auto steadyElapsed = std::chrono::steady_clock::now() - steadyStart;

    EXPECT_TRUE(steadyResponse.success) << steadyResponse.error;
    EXPECT_EQ(streamed.size(), kSteadyBytes);
    EXPECT_GT(steadyElapsed, options.Timeout);

    const auto silentStart = std::chrono::steady_clock::now();
    const HttpClient::HttpResponse silentResponse =
        HttpClient::PostJsonStream(silent.Url(), {}, "{}", options, appendChunk);
    const auto silentElapsed = std::chrono::steady_clock::now() - silentStart;

    EXPECT_FALSE(silentResponse.success);
    EXPECT_NE(silentResponse.error.find("timed out"), std::string::npos) << silentResponse.error;
    EXPECT_LT(silentElapsed, 10s);
}

TEST(HttpClientTests, HeadersWithLineBreaksAreRefusedBeforeAnythingIsSent)
{
    LoopbackServer server([](LoopbackServer&, SocketHandle client) {
        LoopbackServer::Send(client, OkHead(2) + "{}");
    });

    const HttpClient::HttpResponse badName =
        HttpClient::PostJson(server.Url(), {{"X-Test\r\nX-Injected", "value"}}, "{}", {});
    const HttpClient::HttpResponse badValue =
        HttpClient::PostJson(server.Url(), {{"X-Test", "value\r\nX-Injected: yes"}}, "{}", {});
    const HttpClient::HttpResponse badGet = HttpClient::Get(server.Url(), {{"X-Test", "value\r\nX-Injected: yes"}}, {});
    const HttpClient::HttpResponse badForm =
        HttpClient::PostForm(server.Url(), {{"field", "value"}}, {{"X-Test", "value\r\nX-Injected: yes"}});

    EXPECT_FALSE(badName.success);
    EXPECT_FALSE(badName.error.empty());
    EXPECT_FALSE(badValue.success);
    EXPECT_FALSE(badValue.error.empty());
    EXPECT_FALSE(badGet.success);
    EXPECT_FALSE(badGet.error.empty());
    EXPECT_FALSE(badForm.success);
    EXPECT_FALSE(badForm.error.empty());
    EXPECT_EQ(server.RequestCount(), 0);
}

TEST(HttpClientTests, CredentialHeadersGoOnlyOverHttpsOrToLoopback)
{
    const std::unordered_map<std::string, std::string> authorization = {{"Authorization", "Bearer key"}};
    const std::unordered_map<std::string, std::string> apiKey = {{"x-api-key", "key"}};

    EXPECT_FALSE(ValidateHeaders("http://api.example.com/v1/messages", authorization).empty());
    EXPECT_FALSE(ValidateHeaders("http://api.example.com/v1/messages", apiKey).empty());
    EXPECT_FALSE(ValidateHeaders("http://127.0.0.1.example.com/", apiKey).empty());
    EXPECT_FALSE(ValidateHeaders("http://localhost@example.com/", apiKey).empty());
    EXPECT_FALSE(ValidateHeaders("http://127.0.0.999/", apiKey).empty());
    EXPECT_FALSE(ValidateHeaders(std::string("http://example.com\0@localhost/", 30), apiKey).empty());
    for (const char* name : {"X-Api-Key ", " X-Api-Key", "X-Api-Key\t", "X-Api-Key\xC2\xA0"})
        EXPECT_FALSE(ValidateHeaders("http://example.com/", {{name, "key"}}).empty()) << '"' << name << '"';
    for (const char* name : {"Api-Key", "X-Goog-Api-Key", "Proxy-Authorization", "Cookie"})
        EXPECT_FALSE(ValidateHeaders("http://example.com/", {{name, "key"}}).empty()) << name;

    EXPECT_TRUE(ValidateHeaders("https://api.example.com/v1/messages", authorization).empty());
    EXPECT_TRUE(ValidateHeaders("HTTPS://api.example.com/v1/messages", apiKey).empty());
    EXPECT_TRUE(ValidateHeaders("http://127.0.0.1:11434/api/chat", apiKey).empty());
    EXPECT_TRUE(ValidateHeaders("http://localhost:8080/", authorization).empty());
    EXPECT_TRUE(ValidateHeaders("http://127.1/", apiKey).empty());
    EXPECT_TRUE(ValidateHeaders("http://[::1]:8080/", authorization).empty());
    EXPECT_TRUE(ValidateHeaders("http://api.example.com/", {{"Accept", "text/event-stream"}}).empty());
}

TEST(HttpClientTests, PostJsonReturnsARedirectInsteadOfFollowingIt)
{
    LoopbackServer server([](LoopbackServer& self, SocketHandle client) {
        LoopbackServer::Send(client, "HTTP/1.1 307 Temporary Redirect\r\nLocation: " + self.Url("/elsewhere") +
                                         "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    });

    const HttpClient::HttpResponse response =
        HttpClient::PostJson(server.Url(), {{"X-Api-Key", "key"}}, "{}", {});

    EXPECT_EQ(response.statusCode, 307) << response.error;
    EXPECT_FALSE(response.success);
    EXPECT_EQ(server.RequestCount(), 1);
}

std::string RedirectTo(const std::string& location)
{
    return "HTTP/1.1 302 Found\r\nLocation: " + location + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

void SendOk(SocketHandle client)
{
    LoopbackServer::Send(client, OkHead(2) + "{}");
}

// "localhost" and "127.0.0.1" are two origins: the target answers on both.
TEST(HttpClientTests, GetDropsACredentialHeaderOnARedirectToAnotherOrigin)
{
    LoopbackServer target([](LoopbackServer&, SocketHandle client) { SendOk(client); });
    const std::string otherOrigin = "http://localhost:" + target.Url().substr(std::string("http://127.0.0.1:").size());
    LoopbackServer origin([&otherOrigin](LoopbackServer&, SocketHandle client) { LoopbackServer::Send(client, RedirectTo(otherOrigin)); });

    const HttpClient::HttpResponse response =
        HttpClient::Get(origin.Url(), {{"X-Api-Key", "throwaway"}, {"Accept", "application/json"}}, {});

    EXPECT_EQ(response.statusCode, 200) << response.error;
    ASSERT_EQ(target.RequestCount(), 1);
    const std::string forwarded = ToLowerAscii(target.LastRequest());
    EXPECT_EQ(forwarded.find("x-api-key"), std::string::npos);
    EXPECT_NE(forwarded.find("accept: application/json"), std::string::npos);
}

TEST(HttpClientTests, GetKeepsACredentialHeaderOnARedirectToTheSameOrigin)
{
    LoopbackServer server([](LoopbackServer& self, SocketHandle client) {
        if (self.RequestCount() == 1)
            LoopbackServer::Send(client, RedirectTo(self.Url("/elsewhere")));
        else
            SendOk(client);
    });

    const HttpClient::HttpResponse response = HttpClient::Get(server.Url(), {{"X-Api-Key", "throwaway"}}, {});

    EXPECT_EQ(response.statusCode, 200) << response.error;
    EXPECT_EQ(server.RequestCount(), 2);
    EXPECT_NE(ToLowerAscii(server.LastRequest()).find("x-api-key: throwaway"), std::string::npos);
}

// The origin is the scheme (https or not), the host and the port. A '#', a '?' or an '@' in
// the authority must not make another host read as the first one.
TEST(HttpClientTests, SameOriginComparesTheSchemeTheHostAndThePort)
{
    constexpr std::string_view api = "https://api.example.com/v1/models";
    EXPECT_FALSE(IsSameOrigin(api, "http://api.example.com/v1/models")) << "a downgrade to http";
    EXPECT_FALSE(IsSameOrigin("http://api.example.com/", api)) << "an upgrade to https";
    EXPECT_FALSE(IsSameOrigin(api, "https://api.example.com:8443/v1/models")) << "another port";
    EXPECT_FALSE(IsSameOrigin(api, "https://evil.test#@api.example.com/"));
    EXPECT_FALSE(IsSameOrigin(api, "https://evil.test?@api.example.com/"));
    EXPECT_FALSE(IsSameOrigin(api, "https://api.example.com.evil.test/")) << "a host suffix";
    EXPECT_FALSE(IsSameOrigin(api, "https://api.example.com@evil.test/")) << "user information";
    EXPECT_TRUE(IsSameOrigin(api, "HTTPS://API.Example.com/other")) << "case and path do not change the origin";
}

TEST(HttpClientTests, GetStopsAfterFiveRedirects)
{
    LoopbackServer server(
        [](LoopbackServer& self, SocketHandle client) { LoopbackServer::Send(client, RedirectTo(self.Url("/again"))); });

    const HttpClient::HttpResponse response = HttpClient::Get(server.Url(), {}, {});

    EXPECT_FALSE(response.success);
    EXPECT_FALSE(response.error.empty());
    EXPECT_EQ(server.RequestCount(), 6) << "the first request and five redirects";
}

TEST(HttpClientTests, PostFormWithACredentialReturnsARedirectInsteadOfFollowingIt)
{
    LoopbackServer server([](LoopbackServer& self, SocketHandle client) {
        LoopbackServer::Send(client, RedirectTo(self.Url("/elsewhere")));
    });

    const HttpClient::HttpResponse response =
        HttpClient::PostForm(server.Url(), {{"field", "value"}}, {{"Authorization", "Bearer throwaway"}});

    EXPECT_EQ(response.statusCode, 302) << response.error;
    EXPECT_FALSE(response.success);
    EXPECT_EQ(server.RequestCount(), 1);
}

} // namespace
} // namespace GameEngine
