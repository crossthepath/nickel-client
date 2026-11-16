// Nickel AI's protocol client (native/ai_client.hpp) without the game and without the internet: a small SSE server on
// 127.0.0.1 (a free port) plays the AI service, one scripted answer per test message.
// - streamed text, the turn event (ids, quota), the conversation id sent back on the next turn;
// - tool_use -> AwaitingTools -> SubmitToolResults (and its refusals) -> the same turn continued;
// - 429 quota_exceeded / rate_limited with Retry-After (Send blocked meanwhile), 401, 403, 400;
// - malformed and oversized events, unknown events ignored, a stream without done, an error event, caps on text;
// - the quota after a failed turn: the error event's (a refunded turn), else GET /status once;
// - Cancel in the middle of a stream; Stop; the host policy and NICKEL_AI_LOCAL_SERVER rule;
// - a step left pending on the server (Cancel while tools wait, a failed tool-results post) and its 409 conflict;
// - the install key reaches the server's Authorization header and nowhere else (log lines, snapshots).
// winsock2.h before windows.h (included by https_client.hpp), or windows.h pulls in the old winsock.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "ai_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace nickel;
using namespace nickel::ai;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

const std::string kKey = "Install q8Zr3vJ0x9VdT6mYkHw2LbQeNcAsPfUi7GoR1tXyE4z";

class FakeIdentity final : public IdentitySource {
public:
    bool Authorization(std::string* header) override {
        ++calls;
        if (!available) {
            return false;
        }
        *header = kKey;
        return true;
    }

    std::atomic<int> calls{0};
    bool available = true;
};

// ---------------------------------------------------------------------------------------------------------------
// Local SSE server

struct Seen {
    std::string method;
    std::string path;
    std::string headers;
    std::string body;
};

class TestServer {
public:
    bool Start() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int size = sizeof(address);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener_, 16) != 0
                || getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return false;
        }
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this] { Accept(); });
        return true;
    }

    void Stop() {
        stopping_ = true;
        if (listener_ != INVALID_SOCKET) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (acceptor_.joinable()) {
            acceptor_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::thread& handler : handlers_) {
            handler.join();
        }
        handlers_.clear();
        WSACleanup();
    }

    std::string Origin() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::vector<Seen> Requests() {
        std::lock_guard<std::mutex> lock(seenMutex_);
        return seen_;
    }

    size_t RequestCount() {
        std::lock_guard<std::mutex> lock(seenMutex_);
        return seen_.size();
    }

private:
    void Accept() {
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            handlers_.emplace_back([this, client] { Serve(client); });
        }
    }

    static bool ReadRequest(SOCKET client, Seen* request) {
        std::string data;
        char buffer[4096];
        size_t headerEnd = std::string::npos;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                return false;
            }
            data.append(buffer, got);
        }
        const size_t firstSpace = data.find(' ');
        const size_t secondSpace = data.find(' ', firstSpace + 1);
        request->method = data.substr(0, firstSpace);
        request->path = data.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        request->headers = data.substr(0, headerEnd) + "\r\n";
        size_t length = 0;
        const size_t lengthAt = request->headers.find("Content-Length: ");
        if (lengthAt != std::string::npos) {
            length = std::stoul(request->headers.substr(lengthAt + 16));
        }
        request->body = data.substr(headerEnd + 4);
        while (request->body.size() < length) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                return false;
            }
            request->body.append(buffer, got);
        }
        return true;
    }

    static bool Send(SOCKET client, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const int written = send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (written <= 0) {
                return false;
            }
            sent += static_cast<size_t>(written);
        }
        return true;
    }

    static std::string Reply(const char* status, const std::string& headers, const std::string& body) {
        return std::string("HTTP/1.1 ") + status + "\r\nConnection: close\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body;
    }

    static std::string Event(const std::string& name, const std::string& data) {
        return "event: " + name + "\ndata: " + data + "\n\n";
    }

    void Pause(int ms) const {
        for (int waited = 0; waited < ms && !stopping_; waited += 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void Serve(SOCKET client) {
        Seen request;
        if (ReadRequest(client, &request)) {
            {
                std::lock_guard<std::mutex> lock(seenMutex_);
                seen_.push_back(request);
            }
            Route(client, request);
        }
        shutdown(client, SD_SEND);
        closesocket(client);
    }

    static const char* SseHead() {
        return "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n\r\n";
    }

    static std::string TurnEvent(const std::string& turn, const std::string& conversation, int used) {
        return Event("turn",
                "{\"turn_id\":\"" + turn + "\",\"conversation_id\":\"" + conversation + "\",\"quota\":{\"used\":"
                        + std::to_string(used) + ",\"limit\":25,\"reset_at\":\"2026-11-17T00:00:00Z\"}}");
    }

    static std::string Done(const char* reason) { return Event("done", std::string("{\"stop_reason\":\"") + reason + "\"}"); }

    // Sends the parts with a short pause between them, so the client sees them as separate reads.
    void Stream(SOCKET client, const std::vector<std::string>& parts) {
        if (!Send(client, SseHead())) {
            return;
        }
        for (const std::string& part : parts) {
            if (!Send(client, part)) {
                return;
            }
            Pause(15);
        }
    }

    void Route(SOCKET client, const Seen& request) {
        const std::string prefix = "/api/ai/v1";
        if (request.method == "GET" && request.path == prefix + "/status") {
            // the service's count, as GET /status reports it (a failed turn already given back)
            Send(client, Reply("200 OK", "Content-Type: application/json\r\n",
                                 "{\"enabled\":true,\"quota\":{\"used\":6,\"limit\":25,\"reset_at\":"
                                 "\"2026-11-17T00:00:00Z\"},\"prompt_version\":\"v1\"}"));
            return;
        }
        if (request.method != "POST" || request.path.rfind(prefix, 0) != 0) {
            Send(client, Reply("404 Not Found", "", ""));
            return;
        }
        const std::string path = request.path.substr(prefix.size());
        // Steps left waiting for tool results, like the service: a new turn in that conversation is a 409 conflict.
        if (path == "/turns/t-pend/tool-results") {
            SetBusy("c-pend", false);
            Stream(client, {Event("text", "{\"delta\":\"done\"}"), Done("end_turn")});
            return;
        }
        if (path == "/turns/t-pfail/tool-results") {
            Send(client, Reply("503 Service Unavailable", "Content-Type: application/problem+json\r\n",
                                 "{\"code\":\"service_unavailable\",\"title\":\"Nickel AI is busy.\"}"));
            return;
        }
        if (path == "/turns/t-pslow/tool-results") {
            Pause(1500);
            Stream(client, {Event("text", "{\"delta\":\"late\"}"), Done("end_turn")});
            return;
        }
        if (path == "/turns/t-tools/tool-results") {
            Stream(client, {Event("text", "{\"delta\":\"Found \"}"), ": keep-alive\n\n",
                                   Event("text", "{\"delta\":\"Fly.\"}"), Done("end_turn")});
            return;
        }
        if (path == "/turns/t-again/tool-results") {
            Stream(client, {TurnEvent("t-again", "c-1", 3), Event("text", "{\"delta\":\"ok\"}"), Done("end_turn")});
            return;
        }
        if (path == "/turns/t-other/tool-results") {
            Stream(client, {TurnEvent("t-different", "c-1", 3), Done("end_turn")});
            return;
        }
        if (path != "/turns") {
            Send(client, Reply("404 Not Found", "Content-Type: application/problem+json\r\n",
                                 "{\"code\":\"not_found\",\"title\":\"Not found\"}"));
            return;
        }
        json::Value body;
        json::Parse(request.body, &body);
        const std::string message = body["message"].AsString();
        if (body["conversation_id"].IsString() && Busy(body["conversation_id"].AsString())) {
            Send(client, Reply("409 Conflict", "Content-Type: application/problem+json\r\n",
                                 "{\"code\":\"conflict\",\"title\":\"A turn is already running in this "
                                 "conversation.\"}"));
            return;
        }
        if (message == "pending" || message == "pending-fail" || message == "pending-slow") {
            const std::string turn = message == "pending" ? "t-pend" : message == "pending-fail" ? "t-pfail" : "t-pslow";
            const std::string conversation =
                    message == "pending" ? "c-pend" : message == "pending-fail" ? "c-pfail" : "c-pslow";
            SetBusy(conversation, true);
            Stream(client, {TurnEvent(turn, conversation, 2),
                                   Event("tool_use",
                                           "{\"id\":\"tu_1\",\"name\":\"set_module_enabled\",\"input\":{"
                                           "\"module_id\":\"fly\",\"enabled\":true}}"),
                                   Done("tool_use")});
        } else if (message == "busy-next") {
            // A conversation the service will find busy at the next turn (a step pending from an earlier request).
            SetBusy("c-busy", true);
            Stream(client, {TurnEvent("t-busy", "c-busy", 1), Event("text", "{\"delta\":\"ok\"}"), Done("end_turn")});
        } else if (message == "dot-turn-id") {
            Stream(client, {Event("turn", "{\"turn_id\":\"..\",\"conversation_id\":\"c-1\"}"), Done("end_turn")});
        } else if (message == "hello" || message.rfind("Bonjour", 0) == 0) {
            Stream(client, {TurnEvent("t-hello", "c-1", 1), Event("text", "{\"delta\":\"Bon\"}"),
                                   Event("text", "{\"delta\":\"jour \\u00e9\"}"), Done("end_turn")});
        } else if (message == "tools" || message == "again" || message == "other") {
            const std::string turn = message == "tools" ? "t-tools" : message == "again" ? "t-again" : "t-other";
            Stream(client, {TurnEvent(turn, "c-1", 2), Event("text", "{\"delta\":\"Let me look.\"}"),
                                   Event("tool_use",
                                           "{\"id\":\"tu_1\",\"name\":\"search_modules\",\"input\":{\"query\":\"fly\","
                                           "\"category\":\"movement\"}}"),
                                   Event("tool_use", "{\"id\":\"tu_2\",\"name\":\"get_client_status\",\"input\":{}}"),
                                   Done("tool_use")});
        } else if (message == "quota") {
            Send(client, Reply("429 Too Many Requests",
                                 "Content-Type: application/problem+json\r\nRetry-After: 2\r\n",
                                 "{\"type\":\"x\",\"status\":429,\"code\":\"quota_exceeded\",\"title\":\"The daily "
                                 "Nickel AI limit is reached.\",\"quota\":{\"used\":25,\"limit\":25,"
                                 "\"reset_at\":\"2026-11-17T00:00:00Z\"}}"));
        } else if (message == "rate") {
            Send(client, Reply("429 Too Many Requests", "Content-Type: application/problem+json\r\n",
                                 "{\"code\":\"rate_limited\",\"title\":\"Slow down.\"}"));
        } else if (message == "unauthorized") {
            Send(client, Reply("401 Unauthorized", "Content-Type: application/problem+json\r\n",
                                 "{\"code\":\"unauthorized\",\"title\":\"Unknown install.\"}"));
        } else if (message == "blocked") {
            Send(client, Reply("403 Forbidden", "Content-Type: application/problem+json\r\n",
                                 "{\"code\":\"blocked\",\"title\":\"This install is blocked.\"}"));
        } else if (message == "invalid") {
            Send(client, Reply("400 Bad Request", "Content-Type: application/problem+json\r\n",
                                 "{\"code\":\"invalid\",\"title\":\"The request is not valid.\"}"));
        } else if (message == "not-json") {
            Stream(client, {TurnEvent("t-x", "c-1", 1), Event("text", "{\"delta\":"), Done("end_turn")});
        } else if (message == "text-first") {
            Stream(client, {Event("text", "{\"delta\":\"hi\"}"), Done("end_turn")});
        } else if (message == "bad-tool") {
            Stream(client, {TurnEvent("t-x", "c-1", 1),
                                   Event("tool_use", "{\"id\":\"tu_1\",\"name\":\"search_modules\",\"input\":\"fly\"}"),
                                   Done("tool_use")});
        } else if (message == "bad-tool-id") {
            Stream(client, {TurnEvent("t-x", "c-1", 1),
                                   Event("tool_use", "{\"id\":\"../x\",\"name\":\"search_modules\",\"input\":{}}"),
                                   Done("tool_use")});
        } else if (message == "bad-turn-id") {
            Stream(client, {Event("turn", "{\"turn_id\":\"a/b\",\"conversation_id\":\"c-1\"}"), Done("end_turn")});
        } else if (message == "unknown-event") {
            Stream(client, {TurnEvent("t-u", "c-1", 1), Event("thinking", "{}"), Event("tool_progress", "not json"),
                                   Event("text", "{\"delta\":\"fine\"}"), Done("end_turn")});
        } else if (message == "no-done") {
            Stream(client, {TurnEvent("t-x", "c-1", 1), Event("text", "{\"delta\":\"cut\"}")});
        } else if (message == "error-event") {
            Stream(client, {TurnEvent("t-x", "c-1", 1), Event("text", "{\"delta\":\"partial\"}"),
                                   Event("error",
                                           "{\"code\":\"model_unavailable\",\"message\":\"Nickel AI cannot answer "
                                           "right now.\"}"),
                                   Done("end_turn")});
        } else if (message == "error-refund") {
            // the service of today: the turn counted the message (used 5), failed before any output and gave it back
            Stream(client, {TurnEvent("t-r", "c-1", 5),
                                   Event("error",
                                           "{\"code\":\"model_unavailable\",\"message\":\"Busy.\",\"retry_after\":1,"
                                           "\"quota\":{\"used\":4,\"limit\":25,"
                                           "\"reset_at\":\"2026-11-17T00:00:00Z\"}}")});
        } else if (message == "error-old") {
            // an older service: no quota in the error event; GET /status says 6
            Stream(client, {TurnEvent("t-o", "c-1", 7),
                                   Event("error", "{\"code\":\"model_unavailable\",\"message\":\"Busy.\"}")});
        } else if (message == "big-event") {
            Stream(client, {TurnEvent("t-x", "c-1", 1),
                                   Event("text", "{\"delta\":\"" + std::string(6000, 'z') + "\"}"), Done("end_turn")});
        } else if (message == "long-text") {
            std::vector<std::string> parts = {TurnEvent("t-long", "c-1", 1)};
            for (int i = 0; i < 10; ++i) {
                parts.push_back(Event("text", "{\"delta\":\"\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\"}"));  // 10 bytes
            }
            parts.push_back(Done("end_turn"));
            Stream(client, parts);
        } else if (message == "done-without-tools") {
            Stream(client, {TurnEvent("t-x", "c-1", 1), Done("tool_use")});
        } else if (message == "bad-reason") {
            Stream(client, {TurnEvent("t-x", "c-1", 1), Done("finished")});
        } else if (message == "repeat-tool") {
            Stream(client, {TurnEvent("t-x", "c-1", 1),
                                   Event("tool_use", "{\"id\":\"tu_1\",\"name\":\"get_module\",\"input\":{\"id\":\"a\"}}"),
                                   Event("tool_use", "{\"id\":\"tu_1\",\"name\":\"get_module\",\"input\":{\"id\":\"b\"}}"),
                                   Done("tool_use")});
        } else if (message == "max-tokens") {
            Stream(client, {TurnEvent("t-x", "c-1", 1), Event("text", "{\"delta\":\"long\"}"), Done("max_tokens")});
        } else if (message == "plain-json") {
            Send(client, Reply("200 OK", "Content-Type: application/json\r\n", "{}"));
        } else if (message == "slow") {
            if (!Send(client, SseHead()) || !Send(client, TurnEvent("t-slow", "c-1", 1))
                    || !Send(client, Event("text", "{\"delta\":\"partial\"}"))) {
                return;
            }
            Pause(4000);
            Send(client, Event("text", "{\"delta\":\" late\"}"));
            Send(client, Done("end_turn"));
        } else {
            Stream(client, {TurnEvent("t-echo", "c-1", 1), Event("text", "{\"delta\":\"echo\"}"), Done("end_turn")});
        }
    }

    void SetBusy(const std::string& conversation, bool busy) {
        std::lock_guard<std::mutex> lock(seenMutex_);
        busy_[conversation] = busy;
    }

    bool Busy(const std::string& conversation) {
        std::lock_guard<std::mutex> lock(seenMutex_);
        const auto it = busy_.find(conversation);
        return it != busy_.end() && it->second;
    }

    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::thread> handlers_;
    std::mutex seenMutex_;
    std::vector<Seen> seen_;
    std::map<std::string, bool> busy_;
};

// ---------------------------------------------------------------------------------------------------------------
// Helpers

struct Fixture {
    TestServer& server;
    FakeIdentity identity;
    std::mutex logMutex;
    std::vector<std::string> logLines;
    ClientConfig config;

    explicit Fixture(TestServer& s) : server(s) {
        UseLocalTestServer(config, server.Origin());
        config.client = ClientInfo{"1.2.0-beta", "1.26.63", "fr"};
        config.timeoutMs = 10000;
        config.defaultRetryAfterSeconds = 1;
        config.log = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(logMutex);
            logLines.push_back(line);
        };
    }
};

using Snapshot = std::shared_ptr<const Conversation>;

Snapshot WaitFor(const AssistantClient& client, const std::function<bool(const Conversation&)>& done, int ms = 8000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        Snapshot snap = client.Snap();
        if (done(*snap) || std::chrono::steady_clock::now() >= until) {
            return snap;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool Settled(const Conversation& c) {
    return c.status == Status::Idle || c.status == Status::Failed || c.status == Status::AwaitingTools;
}

Snapshot SendAndWait(AssistantClient& client, const std::string& message) {
    if (!client.Send(message)) {
        return client.Snap();
    }
    return WaitFor(client, [](const Conversation& c) { return Settled(c); });
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string HeaderValue(const std::string& headers, const std::string& name) {
    const size_t at = headers.find("\r\n" + name + ": ");
    if (at == std::string::npos) {
        return std::string();
    }
    const size_t start = at + name.size() + 4;
    return headers.substr(start, headers.find("\r\n", start) - start);
}

// Every string a snapshot holds, for the key leak check.
std::string AllText(const Conversation& c) {
    std::string all = c.conversationId + c.turnId + c.error.code + c.error.message + c.stopReason + c.quota.resetAt;
    for (const auto& message : c.messages) {
        all += message->text + message->turnId;
        for (const ToolUse& use : message->toolUses) {
            all += use.id + use.name + use.inputJson;
        }
    }
    for (const ToolUse& use : c.pendingToolUses) {
        all += use.id + use.name + use.inputJson;
    }
    return all;
}

// ---------------------------------------------------------------------------------------------------------------
// Pure checks (no request)

void PureChecks() {
    ClientConfig config;
    Check(config.baseUrl == "https://nickelclient.com/api/ai/v1" && config.policy.httpsHosts.size() == 1
                    && config.policy.httpsHosts[0] == "nickelclient.com" && !config.policy.allowLoopbackHttp,
            "the default client reaches nickelclient.com over HTTPS only");
    Check(UseLocalTestServer(config, "http://127.0.0.1:8794") && config.baseUrl == "http://127.0.0.1:8794/api/ai/v1"
                    && config.policy.allowLoopbackHttp && config.policy.httpsHosts.empty(),
            "a loopback origin with a port is accepted, and only loopback is allowed then");
    ClientConfig other;
    const char* const refused[] = {"http://localhost:8794", "https://127.0.0.1:8794", "http://127.0.0.1:",
            "http://127.0.0.1:99999", "http://127.0.0.1:0", "http://127.0.0.1:80/x", "http://127.0.0.2:80",
            "http://example.com:80", "http://127.0.0.1:80@evil.com"};
    bool all = true;
    for (const char* origin : refused) {
        all &= !UseLocalTestServer(other, origin);
    }
    Check(all && other.baseUrl == "https://nickelclient.com/api/ai/v1", "other local server values are refused");

    Check(ai::detail::Utf8Prefix("ab\xC3\xA9" "c", 3) == 2 && ai::detail::Utf8Prefix("ab\xC3\xA9" "c", 4) == 4,
            "text is cut on a code point boundary");
    Check(ai::detail::ValidId("t_01-A.b") && !ai::detail::ValidId("") && !ai::detail::ValidId("a/b")
                    && !ai::detail::ValidId("a b") && !ai::detail::ValidId(std::string(129, 'a')),
            "ids are path-safe and bounded");
    Check(!ai::detail::ValidId(".") && !ai::detail::ValidId("..") && !ai::detail::ValidId(".x")
                    && !ai::detail::ValidId("-x") && !ai::detail::ValidId("_x") && ai::detail::ValidId("9a..b"),
            "ids start with a letter or a digit, so never a dot segment");

    const ToolResult declined = AssistantClient::MakeErrorResult("tu_1", "declined", "The player said no.");
    json::Value parsed;
    Check(declined.isError && json::Parse(declined.content, &parsed) && parsed["error"].AsString() == "declined"
                    && parsed["message"].AsString() == "The player said no.",
            "an error result is {error, message} with is_error");

    // A client whose base URL the policy refuses never sends.
    ClientConfig refusedConfig;
    refusedConfig.baseUrl = "http://127.0.0.1:1/api/ai/v1";
    FakeIdentity identity;
    AssistantClient refusedClient(refusedConfig, &identity);
    Check(!refusedClient.Usable() && !refusedClient.Send("hello")
                    && refusedClient.Snap()->error.kind == ErrorKind::Invalid && identity.calls == 0,
            "a base URL outside the policy is refused before any request");
}

// ---------------------------------------------------------------------------------------------------------------
// Network checks

void StreamedText(TestServer& server) {
    Fixture f(server);
    AssistantClient client(f.config, &f.identity);
    Check(client.Usable(), "the local server is usable");

    Check(!client.Send("") && !client.Send("   \n") && !client.Send(std::string(4001, 'a'))
                    && !client.Send("bad \xC3(") && client.Snap()->messages.empty(),
            "empty, blank, too long or invalid UTF-8 messages are refused without a change");
    const size_t before = server.RequestCount();
    Check(client.Version() == 0, "no snapshot was published by refusals");

    std::string fourThousand;
    for (int i = 0; i < 4000; ++i) {
        fourThousand += "\xC3\xA9";
    }
    Snapshot snap = SendAndWait(client, "Bonjour" + fourThousand.substr(14));
    Check(snap->status == Status::Idle && snap->error.Ok(), "a 4000 code point message is accepted");
    Check(server.RequestCount() == before + 1, "it was one request");

    snap = SendAndWait(client, "hello");
    Check(snap->status == Status::Idle && snap->stopReason == "end_turn", "a streamed turn ends idle with end_turn");
    Check(snap->conversationId == "c-1" && snap->turnId == "t-hello", "the turn event gives the turn and conversation ids");
    Check(snap->quota.used == 1 && snap->quota.limit == 25 && snap->quota.resetAt == "2026-11-17T00:00:00Z",
            "the turn event gives the quota");
    const auto& messages = snap->messages;
    Check(messages.size() == 4 && messages[2]->role == Role::User && messages[2]->text == "hello"
                    && messages[2]->turnId == "t-hello",
            "the player's message is kept and tied to its turn");
    Check(messages[3]->role == Role::Assistant && messages[3]->text == "Bonjour \xC3\xA9" && !messages[3]->streaming
                    && !messages[3]->truncated && !messages[3]->interrupted,
            "the deltas are joined into one answer");
    Check(snap->turns.size() == 2 && snap->turns[1].id == "t-hello" && snap->turns[1].steps == 1
                    && snap->turns[1].stopReason == "end_turn",
            "turns are recorded");

    const std::vector<Seen> requests = server.Requests();
    const Seen& first = requests[before];
    const Seen& second = requests[before + 1];
    json::Value firstBody;
    json::Value secondBody;
    json::Parse(first.body, &firstBody);
    json::Parse(second.body, &secondBody);
    Check(first.method == "POST" && first.path == "/api/ai/v1/turns", "a new turn is POST /turns");
    Check(firstBody["conversation_id"].IsNull() && firstBody.Find("conversation_id") != nullptr,
            "the first turn sends conversation_id null");
    Check(secondBody["conversation_id"].AsString() == "c-1" && secondBody["message"].AsString() == "hello",
            "the next turn sends the conversation id back");
    Check(secondBody["client"]["nickel_version"].AsString() == "1.2.0-beta"
                    && secondBody["client"]["game_version"].AsString() == "1.26.63"
                    && secondBody["client"]["language"].AsString() == "fr" && secondBody.size() == 3,
            "the client info is sent, and nothing else");
    Check(HeaderValue(second.headers, "Authorization") == kKey, "the install key is sent as Authorization: Install");
    Check(HeaderValue(second.headers, "Accept") == "text/event-stream"
                    && Contains(HeaderValue(second.headers, "Content-Type"), "application/json"),
            "Accept and Content-Type headers");

    // The history is for display: a new conversation forgets it and sends null again.
    client.NewConversation();
    snap = client.Snap();
    Check(snap->messages.empty() && snap->conversationId.empty() && snap->quota.used == 1,
            "a new conversation clears the history and keeps the quota");
    SendAndWait(client, "hello");
    json::Value third;
    json::Parse(server.Requests().back().body, &third);
    Check(third["conversation_id"].IsNull(), "after a new conversation, conversation_id is null again");

    // Language change between turns.
    client.SetClientInfo(ClientInfo{"1.2.0-beta", "1.26.63", "en"});
    SendAndWait(client, "hello");
    json::Parse(server.Requests().back().body, &third);
    Check(third["client"]["language"].AsString() == "en", "SetClientInfo changes the next turn's language");
    Check(third["client"]["memory"].IsNull(), "no memory field until the Settings toggle is known");

    // Nickel AI memory (Settings): sent with the next turns, on or off.
    client.SetMemory(true);
    SendAndWait(client, "hello");
    json::Parse(server.Requests().back().body, &third);
    Check(third["client"]["memory"].AsString() == "on", "SetMemory(true) sends memory on");
    client.SetMemory(false);
    SendAndWait(client, "hello");
    json::Parse(server.Requests().back().body, &third);
    Check(third["client"]["memory"].AsString() == "off", "SetMemory(false) sends memory off");

    bool leak = false;
    {
        std::lock_guard<std::mutex> lock(f.logMutex);
        for (const std::string& line : f.logLines) {
            leak |= Contains(line, kKey.substr(8)) || Contains(line, "Install ");
        }
        Check(!f.logLines.empty(), "requests are logged");
    }
    Check(!leak, "no log line holds the install key");
    Check(!Contains(AllText(*client.Snap()), kKey.substr(8)), "no snapshot holds the install key");
}

void ToolFlow(TestServer& server) {
    Fixture f(server);
    AssistantClient client(f.config, &f.identity);
    Snapshot snap = SendAndWait(client, "tools");
    Check(snap->status == Status::AwaitingTools && snap->stopReason == "tool_use", "done tool_use awaits the tools");
    Check(snap->pendingToolUses.size() == 2 && snap->pendingToolUses[0].id == "tu_1"
                    && snap->pendingToolUses[0].name == "search_modules"
                    && snap->pendingToolUses[0].input["query"].AsString() == "fly"
                    && snap->pendingToolUses[0].inputJson == "{\"query\":\"fly\",\"category\":\"movement\"}"
                    && snap->pendingToolUses[1].name == "get_client_status" && snap->pendingToolUses[1].input.IsObject(),
            "the tool calls are pending with their inputs");
    Check(snap->messages.size() == 2 && snap->messages[1]->text == "Let me look."
                    && snap->messages[1]->toolUses.size() == 2,
            "the answer shows its text and tool calls");
    Check(snap->messages[1]->toolUses[0].id == "tu_1" && snap->messages[1]->toolUses[0].name == "search_modules"
                    && snap->messages[1]->toolUses[0].inputJson.empty() && !snap->messages[1]->toolUses[0].input.IsObject(),
            "the history keeps the id and name of a call, not its input (copied with every text delta)");
    Check(!client.Send("another question"), "Send is refused while tools are awaited");

    const size_t before = server.RequestCount();
    const ToolResult one{"tu_1", "{\"modules\":[{\"id\":\"fly\",\"name\":\"Fly\"}]}", false};
    const ToolResult two{"tu_2", "{\"nickel_version\":\"1.2.0\",\"in_world\":true}", false};
    Check(client.SubmitToolResults("t-other", {one, two}) == SubmitError::WrongTurn, "another turn id is refused");
    Check(client.SubmitToolResults("t-tools", {one}) == SubmitError::Missing, "a missing result is refused");
    Check(client.SubmitToolResults("t-tools", {one, two, ToolResult{"tu_9", "{}", false}}) == SubmitError::Unknown,
            "a result for an unknown id is refused");
    Check(client.SubmitToolResults("t-tools", {one, one, two}) == SubmitError::Duplicate, "two results for one id");
    Check(client.SubmitToolResults(
                  "t-tools", {one, ToolResult{"tu_2", "\"" + std::string(kMaxToolResultBytes, 'a') + "\"", false}})
                    == SubmitError::ContentTooLarge,
            "a result over 16 KB is refused");
    Check(client.SubmitToolResults("t-tools", {one, ToolResult{"tu_2", "not json", false}})
                    == SubmitError::ContentNotJson,
            "a result that is not JSON text is refused");
    Check(server.RequestCount() == before && client.Snap()->status == Status::AwaitingTools,
            "refused results send nothing and keep the tools pending");

    const ToolResult declined = AssistantClient::MakeErrorResult("tu_2", "declined", "The player refused.");
    Check(client.SubmitToolResults("t-tools", {declined, one}) == SubmitError::None, "every result, in any order");
    Check(client.SubmitToolResults("t-tools", {declined, one}) == SubmitError::NotAwaiting,
            "results are posted once");
    snap = WaitFor(client, [](const Conversation& c) { return Settled(c); });
    Check(snap->status == Status::Idle && snap->stopReason == "end_turn", "the continued turn ends");
    Check(snap->messages.size() == 3 && snap->messages[2]->text == "Found Fly." && snap->messages[2]->turnId == "t-tools",
            "the continuation streams into a new answer of the same turn (no turn event needed)");
    Check(snap->turns.size() == 1 && snap->turns[0].steps == 2 && snap->turns[0].toolUses == 2,
            "one turn, two steps");

    const Seen post = server.Requests().back();
    json::Value body;
    json::Parse(post.body, &body);
    const json::Value& results = body["results"];
    Check(post.path == "/api/ai/v1/turns/t-tools/tool-results" && body.size() == 1 && results.size() == 2,
            "the results go to /turns/{turn_id}/tool-results");
    Check(results.At(0)["tool_use_id"].AsString() == "tu_2" && results.At(0)["is_error"].AsBool()
                    && results.At(0)["content"].AsString() == declined.content
                    && results.At(1)["tool_use_id"].AsString() == "tu_1" && !results.At(1)["is_error"].AsBool(true)
                    && results.At(1)["content"].AsString() == one.content,
            "each result carries tool_use_id, content as JSON text and is_error");
    Check(HeaderValue(post.headers, "Authorization") == kKey, "the results are authorized too");

    // A continuation that repeats its turn event; and one whose turn event names another turn.
    snap = SendAndWait(client, "again");
    client.SubmitToolResults("t-again", {one, two});
    snap = WaitFor(client, [](const Conversation& c) { return Settled(c); });
    Check(snap->status == Status::Idle && snap->messages.back()->text == "ok" && snap->quota.used == 3
                    && snap->turns.back().steps == 2,
            "a continuation may repeat the turn event of the same turn");
    SendAndWait(client, "other");
    client.SubmitToolResults("t-other", {one, two});
    snap = WaitFor(client, [](const Conversation& c) { return Settled(c); });
    Check(snap->status == Status::Failed && snap->error.kind == ErrorKind::Malformed,
            "a continuation answering for another turn is malformed");
    Check(snap->conversationId == "c-1",
            "a continuation that failed after the server answered keeps the conversation (that step is over there)");

    // Cancel while tools are pending drops them.
    SendAndWait(client, "tools");
    client.Cancel();
    snap = client.Snap();
    Check(snap->status == Status::Idle && snap->pendingToolUses.empty() && snap->error.kind == ErrorKind::Cancelled,
            "Cancel drops the pending tool calls");
    Check(client.SubmitToolResults("t-tools", {one, two}) == SubmitError::NotAwaiting, "and refuses their results");
}

void ErrorAnswers(TestServer& server) {
    {
        Fixture f(server);
        AssistantClient client(f.config, &f.identity);
        Snapshot snap = SendAndWait(client, "quota");
        Check(snap->status == Status::Failed && snap->error.kind == ErrorKind::QuotaExceeded
                        && snap->error.httpStatus == 429 && snap->error.code == "quota_exceeded"
                        && snap->error.retryAfter == 2 && snap->error.message == "The daily Nickel AI limit is reached.",
                "429 quota_exceeded with Retry-After");
        Check(snap->quota.used == 25 && snap->quota.limit == 25, "the problem's quota is kept");
        const size_t before = server.RequestCount();
        Check(!client.Send("hello") && server.RequestCount() == before, "Send is refused without a request while Retry-After runs");
        snap = client.Snap();
        Check(snap->status == Status::Failed && snap->error.kind == ErrorKind::QuotaExceeded
                        && snap->error.code == "quota_exceeded" && snap->error.httpStatus == 429
                        && snap->error.message == "The daily Nickel AI limit is reached." && snap->error.retryAfter > 0
                        && snap->error.retryAfter <= 2,
                "a Send refused during the wait repeats the daily quota error, with the time left");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        snap = SendAndWait(client, "hello");
        Check(snap->status == Status::Idle && snap->error.Ok(), "Send works again once Retry-After has elapsed");
    }
    {
        Fixture f(server);
        AssistantClient client(f.config, &f.identity);
        Snapshot snap = SendAndWait(client, "rate");
        Check(snap->error.kind == ErrorKind::RateLimited && snap->error.retryAfter == 1 && client.RateLimitRemaining() > 0,
                "429 rate_limited without Retry-After waits the default time");
    }
    struct Case {
        const char* message;
        ErrorKind kind;
        int status;
        const char* what;
    };
    const Case cases[] = {
            {"unauthorized", ErrorKind::Unauthorized, 401, "401 is Unauthorized"},
            {"blocked", ErrorKind::Blocked, 403, "403 is Blocked"},
            {"invalid", ErrorKind::Invalid, 400, "400 is Invalid"},
            {"plain-json", ErrorKind::Malformed, 200, "a 200 that is not an event stream is malformed"},
    };
    Fixture f(server);
    AssistantClient client(f.config, &f.identity);
    for (const Case& c : cases) {
        Snapshot snap = SendAndWait(client, c.message);
        Check(snap->status == Status::Failed && snap->error.kind == c.kind && snap->error.httpStatus == c.status, c.what);
    }
    Snapshot snap = client.Snap();
    Check(snap->messages.size() == 4 && snap->messages.back()->role == Role::User,
            "a failed request keeps the player's message, with no answer");
    Check(client.RateLimitRemaining() == 0, "only 429 blocks later requests");
}

void MalformedStreams(TestServer& server) {
    struct Case {
        const char* message;
        ErrorKind kind;
        const char* what;
    };
    const Case cases[] = {
            {"not-json", ErrorKind::Malformed, "event data that is not JSON stops the stream"},
            {"text-first", ErrorKind::Malformed, "text before the turn event"},
            {"bad-tool", ErrorKind::Malformed, "a tool_use whose input is not an object"},
            {"bad-tool-id", ErrorKind::Malformed, "a tool_use id that is not path-safe"},
            {"bad-turn-id", ErrorKind::Malformed, "a turn id that is not path-safe"},
            {"dot-turn-id", ErrorKind::Malformed, "a turn id that is a dot segment"},
            {"no-done", ErrorKind::Interrupted, "a stream that ends without done is interrupted"},
            {"big-event", ErrorKind::TooLarge, "an event over the cap"},
            {"done-without-tools", ErrorKind::Malformed, "done tool_use without tool calls"},
            {"bad-reason", ErrorKind::Malformed, "done with an unknown stop_reason"},
            {"repeat-tool", ErrorKind::Malformed, "a repeated tool_use id"},
    };
    Fixture f(server);
    f.config.maxEventBytes = 4096;
    f.config.maxAnswerBytes = 25;
    AssistantClient client(f.config, &f.identity);
    for (const Case& c : cases) {
        Snapshot snap = SendAndWait(client, c.message);
        Check(snap->status == Status::Failed && snap->error.kind == c.kind && snap->pendingToolUses.empty(), c.what);
    }
    Snapshot snap = SendAndWait(client, "no-done");
    Check(snap->messages.back()->text == "cut" && snap->messages.back()->interrupted
                    && !snap->messages.back()->streaming,
            "the text of an interrupted answer stays, marked interrupted");

    snap = SendAndWait(client, "error-event");
    Check(snap->status == Status::Failed && snap->error.kind == ErrorKind::Server
                    && snap->error.code == "model_unavailable"
                    && snap->error.message == "Nickel AI cannot answer right now.",
            "an error event fails the turn with its code and message");
    Check(snap->messages.back()->text == "partial" && snap->messages.back()->interrupted,
            "events after an error event are not read");

    snap = SendAndWait(client, "unknown-event");
    Check(snap->status == Status::Idle && snap->messages.back()->text == "fine", "unknown events are ignored");

    snap = SendAndWait(client, "long-text");
    Check(snap->status == Status::Idle && snap->messages.back()->truncated && snap->messages.back()->text.size() == 24
                    && ai::detail::Utf8Chars(snap->messages.back()->text) == 12,
            "an answer over the text cap is cut on a code point and marked truncated");

    snap = SendAndWait(client, "max-tokens");
    Check(snap->status == Status::Idle && snap->stopReason == "max_tokens" && snap->turns.back().stopReason == "max_tokens",
            "max_tokens ends the turn with its reason");
}

size_t StatusRequests(TestServer& server) {
    size_t count = 0;
    for (const Seen& seen : server.Requests()) {
        count += seen.method == "GET" && seen.path == "/api/ai/v1/status";
    }
    return count;
}

// A turn that fails before any output is given back by the service: the quota line must go back too ("24 left today"
// stayed after "Nickel AI is busy right now").
void QuotaAfterFailure(TestServer& server) {
    Fixture f(server);
    AssistantClient client(f.config, &f.identity);
    size_t before = StatusRequests(server);
    Snapshot snap = SendAndWait(client, "error-refund");
    Check(snap->status == Status::Failed && snap->error.code == "model_unavailable" && snap->quota.used == 4
                    && snap->quota.limit == 25 && snap->quota.resetAt == "2026-11-17T00:00:00Z",
            "an error event's quota replaces the turn event's (the refunded count)");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check(StatusRequests(server) == before && client.Snap()->quota.used == 4,
            "with the quota in the error event, GET /status is not asked");

    before = StatusRequests(server);
    snap = SendAndWait(client, "error-old");
    Check(snap->status == Status::Failed && snap->error.code == "model_unavailable", "an error event without quota");
    snap = WaitFor(client, [](const Conversation& c) { return c.quota.used == 6; });
    Check(snap->quota.used == 6 && snap->quota.limit == 25 && snap->status == Status::Failed
                    && snap->error.code == "model_unavailable" && StatusRequests(server) == before + 1,
            "without it, GET /status is asked once after the failure and its quota is shown");
    bool keyed = false;
    for (const Seen& seen : server.Requests()) {
        if (seen.method == "GET" && seen.path == "/api/ai/v1/status") {
            keyed = HeaderValue(seen.headers, "Authorization") == kKey;
        }
    }
    Check(keyed, "GET /status carries the install key like the turns");

    before = StatusRequests(server);
    snap = SendAndWait(client, "hello");
    Check(snap->status == Status::Idle && snap->quota.used == 1, "the next turn's quota is shown again");
    snap = SendAndWait(client, "invalid");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check(snap->status == Status::Failed && StatusRequests(server) == before && client.Snap()->quota.used == 1,
            "a refusal before the stream (no turn event) does not ask GET /status");
}

void CancelMidStream(TestServer& server) {
    Fixture f(server);
    AssistantClient client(f.config, &f.identity);
    Check(client.Send("slow"), "a slow turn starts");
    Snapshot snap = WaitFor(client, [](const Conversation& c) {
        return !c.messages.empty() && c.messages.back()->role == Role::Assistant && c.messages.back()->text == "partial";
    });
    Check(snap->status == Status::Streaming && snap->messages.back()->streaming, "the first text is visible mid-stream");
    const auto start = std::chrono::steady_clock::now();
    client.Cancel();
    snap = client.Snap();
    Check(snap->status == Status::Idle && snap->error.kind == ErrorKind::Cancelled
                    && snap->messages.back()->text == "partial" && snap->messages.back()->interrupted
                    && !snap->messages.back()->streaming,
            "Cancel ends the turn at once, keeping the text received");
    Check(snap->conversationId == "c-1", "Cancel of a streamed step keeps the conversation (the server ends that step)");
    Check(client.Send("hello"), "a new message can be sent right after Cancel");
    snap = WaitFor(client, [](const Conversation& c) { return Settled(c); });
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    Check(snap->status == Status::Idle && snap->messages.back()->text == "Bonjour \xC3\xA9" && elapsed < 3000,
            "the next turn runs without waiting for the cancelled stream to end");
    bool late = false;
    for (const auto& message : snap->messages) {
        late |= Contains(message->text, "late");
    }
    Check(!late, "nothing of the cancelled stream arrives after Cancel");

    client.Stop();
    Check(!client.Send("hello"), "nothing is sent after Stop");
}

// The service keeps a conversation busy while a step waits for tool results (409 conflict for a new turn), until the
// step expires. The client must never stay stuck on such a conversation.
void PendingSteps(TestServer& server) {
    Fixture f(server);
    AssistantClient client(f.config, &f.identity);
    const auto lastBody = [&server]() {  // of the last POST (a failed turn may be followed by GET /status)
        json::Value body;
        const std::vector<Seen> seen = server.Requests();
        for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
            if (it->method == "POST") {
                json::Parse(it->body, &body);
                break;
            }
        }
        return body;
    };

    // Cancel while the tools wait: nothing is posted, the conversation is dropped, the next message works.
    Snapshot snap = SendAndWait(client, "pending");
    Check(snap->status == Status::AwaitingTools && snap->conversationId == "c-pend", "a step waits for its tools");
    size_t before = server.RequestCount();
    client.Cancel();
    snap = client.Snap();
    Check(snap->status == Status::Idle && snap->error.kind == ErrorKind::Cancelled && snap->conversationId.empty()
                    && snap->messages.size() == 2 && server.RequestCount() == before,
            "Cancel while tools wait posts nothing, keeps the history and drops the server conversation");
    snap = SendAndWait(client, "hello");
    Check(lastBody()["conversation_id"].IsNull() && snap->status == Status::Idle && snap->error.Ok(),
            "the next message opens a new conversation instead of meeting a 409");

    // A 409 conflict on a new turn drops the conversation, so only that one message fails.
    client.NewConversation();
    snap = SendAndWait(client, "busy-next");
    Check(snap->status == Status::Idle && snap->conversationId == "c-busy", "a conversation the server will find busy");
    snap = SendAndWait(client, "hello");
    Check(lastBody()["conversation_id"].AsString() == "c-busy" && snap->status == Status::Failed
                    && snap->error.httpStatus == 409 && snap->error.code == "conflict"
                    && snap->error.message == "A turn is already running in this conversation."
                    && snap->conversationId.empty(),
            "a 409 conflict fails the message with the server's text and drops the conversation");
    snap = SendAndWait(client, "hello");
    Check(lastBody()["conversation_id"].IsNull() && snap->status == Status::Idle && snap->error.Ok(),
            "the message after a 409 works in a new conversation");

    // A tool-results post that fails before the server answers: the step may still wait there.
    snap = SendAndWait(client, "pending-fail");
    Check(snap->status == Status::AwaitingTools && snap->conversationId == "c-pfail", "another step waits for its tools");
    const ToolResult on{"tu_1", "{\"ok\":true,\"previous\":false}", false};
    Check(client.SubmitToolResults("t-pfail", {on}) == SubmitError::None, "its results are posted");
    snap = WaitFor(client, [](const Conversation& c) { return Settled(c); });
    Check(snap->status == Status::Failed && snap->error.kind == ErrorKind::Server && snap->error.httpStatus == 503
                    && snap->conversationId.empty() && snap->pendingToolUses.empty(),
            "a failed tool-results post drops the conversation");
    snap = SendAndWait(client, "hello");
    Check(lastBody()["conversation_id"].IsNull() && snap->status == Status::Idle && snap->error.Ok(),
            "and the next message works in a new conversation");

    // Cancel while the tool-results post has no answer yet: same.
    snap = SendAndWait(client, "pending-slow");
    Check(client.SubmitToolResults("t-pslow", {on}) == SubmitError::None, "results posted to a slow server");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the post reaches the server, which holds it
    Check(client.Snap()->status == Status::Sending, "the post waits for the server's answer");
    client.Cancel();
    snap = client.Snap();
    Check(snap->status == Status::Idle && snap->conversationId.empty(),
            "Cancel before the server answered the tool results drops the conversation");

    // The step that was answered frees its conversation on the server (the fake models the service).
    client.NewConversation();
    SendAndWait(client, "pending");
    client.SubmitToolResults("t-pend", {on});
    snap = WaitFor(client, [](const Conversation& c) { return Settled(c); });
    Check(snap->status == Status::Idle && snap->conversationId == "c-pend" && snap->messages.back()->text == "done",
            "answered tool results end the step and keep the conversation");
    snap = SendAndWait(client, "hello");
    Check(lastBody()["conversation_id"].AsString() == "c-pend" && snap->status == Status::Idle,
            "the same conversation goes on");
}

void Identity(TestServer& server) {
    Fixture f(server);
    f.identity.available = false;
    AssistantClient client(f.config, &f.identity);
    const size_t before = server.RequestCount();
    Snapshot snap = SendAndWait(client, "hello");
    Check(snap->status == Status::Failed && snap->error.kind == ErrorKind::NoIdentity && server.RequestCount() == before,
            "without an install key nothing is sent");
    AssistantClient anonymous(f.config, nullptr);
    snap = SendAndWait(anonymous, "hello");
    Check(snap->error.kind == ErrorKind::NoIdentity && server.RequestCount() == before, "no identity source at all");

    // The real source over PUBLIC's InstallIdentity creates the key on the first AI request.
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"nickel_ai_client_checks_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\public").c_str(), nullptr);
    const std::wstring path = pub::InstallKeyPath(root);
    {
        pub::InstallIdentity install(path);
        InstallIdentitySource source(install);
        Check(install.State() == pub::IdentityState::Missing, "no key before the first AI request");
        Fixture g(server);
        AssistantClient real(g.config, &source);
        snap = SendAndWait(real, "hello");
        std::string header;
        install.Authorization(&header);
        Check(snap->status == Status::Idle && install.State() == pub::IdentityState::Ready
                        && HeaderValue(server.Requests().back().headers, "Authorization") == header
                        && header.size() == 8 + pub::kInstallKeyChars,
                "the install key is created on the first AI request and sent");
        bool leak = false;
        {
            std::lock_guard<std::mutex> lock(g.logMutex);
            for (const std::string& line : g.logLines) {
                leak |= Contains(line, header.substr(8));
            }
        }
        Check(!leak && !Contains(AllText(*real.Snap()), header.substr(8)), "the real key is not logged either");
    }
    DeleteFileW(path.c_str());
    RemoveDirectoryW((root + L"\\public").c_str());
    RemoveDirectoryW(root.c_str());
}

}  // namespace

int main() {
    PureChecks();
    TestServer server;
    if (!server.Start()) {
        std::printf("FAIL the local test server did not start\n");
        return 1;
    }
    StreamedText(server);
    ToolFlow(server);
    ErrorAnswers(server);
    MalformedStreams(server);
    QuotaAfterFailure(server);
    CancelMidStream(server);
    PendingSteps(server);
    Identity(server);
    server.Stop();
    std::printf("%s: %d failed\n", failed == 0 ? "PASS" : "FAIL", failed);
    return failed == 0 ? 0 : 1;
}
