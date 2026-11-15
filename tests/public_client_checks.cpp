// PUBLIC client core without the game: the model's validation (native/public_model.hpp), the API calls against a
// local HTTP server on 127.0.0.1 serving fixture JSON (native/public_client.hpp): reads, and the writes that carry the
// install key (check, publish, publish update, stars, reports); the install identity (native/public_identity.hpp), the
// local starred and published lists (native/public_local.hpp), and the installer on a temporary folder with a
// ScriptLibrary over that folder (native/public_installer.hpp). Nothing here reaches the internet or touches %APPDATA%.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include "public_client.hpp"
#include "public_identity.hpp"
#include "public_installer.hpp"
#include "public_local.hpp"
#include "script_files.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace nickel;
using namespace nickel::pub;

int failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++failed;
        std::printf("FAIL: %s\n", what);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Fixtures

const char* const kScriptId = "7k2m9x4qa1dz";
const char* const kGoneId = "abcdefghjkmn";
const char* const kTamperedId = "23456789abcd";
const char* const kNoDigestId = "pqrstvwxyz01";
const char* const kConfigId = "9x8w7v6t5s4r";
const char* const kDigestOnlyId = "badd1gest234";  // right sha256 and ETag, Content-Digest of other bytes
const char* const kEtagOnlyId = "badetag23456";    // right sha256 and Content-Digest, strong ETag of another hash
const std::string kScriptBytes = "print('hello')\n";
const std::string kConfigBytes = std::string("[menu]\r\nname=Caf\xe9\r\n[kill_aura]\r\nenabled=1\r\n");

std::string Sha(const std::string& bytes) {
    return net::Sha256Hex(bytes);
}

std::string CardJson(const std::string& id, const char* kind, const std::string& name, int number,
        const std::string& sha, const std::string& extra = std::string()) {
    return "{\"id\":\"" + id + "\",\"kind\":\"" + kind + "\",\"slug\":\"item-" + id + "\",\"name\":\"" + name
            + "\",\"summary\":\"A test item for PUBLIC.\",\"author\":{\"handle\":\"mash\",\"display_name\":\"Mash\","
              "\"kind\":\"user\",\"verified\":true},\"tags\":[\"pvp\",\"hud\"],\"license\":\"MIT\",\"featured\":false,"
              "\"latest_version\":{\"number\":"
            + std::to_string(number)
            + ",\"label\":\"1.2.0\",\"nickel_min\":\"1.1.0\",\"created_at\":\"2026-11-12T10:00:00.000Z\",\"size\":15,"
              "\"sha256\":\""
            + sha
            + "\",\"reviewed\":true,\"reviewed_at\":\"2026-11-12T14:30:00.000Z\"},"
              "\"capabilities\":[\"chat.print\",\"render\",\"future.thing\",\"chat.send\"],"
              "\"stats\":{\"downloads\":1240,\"favourites\":51,\"stars\":18,\"trending_score\":37.5},"
              "\"published_at\":\"2026-10-30T09:12:00.000Z\",\"updated_at\":\"2026-11-12T14:30:00.000Z\""
            + extra + "}";
}

std::string DetailJson() {
    return CardJson(kScriptId, "script", "Totem Reminder", 3, Sha(kScriptBytes),
            ",\"description\":\"Line one.\\nLine two.\",\"state\":\"published\",\"state_reason\":null,"
            "\"listed\":true,\"page_url\":\"https://nickelclient.com/public/script/item-7k2m9x4qa1dz\","
            "\"versions\":[{\"number\":3,\"label\":\"1.2.0\",\"created_at\":\"2026-11-12T10:00:00.000Z\","
            "\"size\":15,\"sha256\":\"" + Sha(kScriptBytes) + "\",\"reviewed\":true,\"reviewed_at\":null,"
            "\"changelog\":\"Fixed the border.\"}]");
}

// ---------------------------------------------------------------------------------------------------------------
// Model

void ModelChecks() {
    int64_t ms = 0;
    Check(ParseIsoTime("2026-11-15T18:00:00.000Z", &ms) && ms == 1794765600000LL, "ISO time with milliseconds");
    Check(ParseIsoTime("1970-01-01T00:00:00Z", &ms) && ms == 0, "ISO time without a fraction");
    Check(FormatIsoTime(1794765600123LL) == "2026-11-15T18:00:00.123Z", "ISO time written back");
    Check(!ParseIsoTime("2026-11-15T18:00:00+01:00", &ms), "only UTC times");
    Check(!ParseIsoTime("2026-02-30T00:00:00Z", &ms), "no 30 February");
    Check(!ParseIsoTime("2025-02-29T00:00:00Z", &ms) && ParseIsoTime("2024-02-29T00:00:00Z", &ms), "leap years");

    Check(ValidId("7k2m9x4qa1dz") && !ValidId("7k2m9x4qa1dZ") && !ValidId("7k2m9x4qa1dl") && !ValidId("short"),
            "public ids");
    Check(ValidSlug("totem-reminder") && !ValidSlug("ab") && !ValidSlug("Totem"), "slugs");
    Check(ValidHandle("mash") && !ValidHandle("1mash") && !ValidHandle("ma"), "handles");
    Check(ValidSha256(Sha("x")) && !ValidSha256("ABC"), "hashes");
    Check(ValidLabel("1.2.0-beta+3") && !ValidLabel("1.2 0") && !ValidLabel(std::string(33, '1')), "labels");

    Check(CompatibleWith("", "1.0.0") && CompatibleWith("1.1.0", "1.1.0") && CompatibleWith("1.1.0", "1.2.0-beta")
                    && !CompatibleWith("1.1.0", "1.0.9") && CompatibleWith("1.1.0", "2.0.0")
                    && !CompatibleWith("one", "1.1.0"),
            "nickel_min against the running version");
    Check(Sensitive(Capability::ChatSend) && !Sensitive(Capability::Render), "sensitive capabilities");

    json::Value value;
    Check(json::Parse(CardJson(kScriptId, "script", "Totem Reminder", 3, Sha(kScriptBytes)), &value), "card fixture");
    Card card;
    std::string error;
    Check(ParseCard(value, &card, &error), "a valid item summary parses");
    Check(card.id == kScriptId && card.kind == Kind::Script && card.name == "Totem Reminder"
                    && card.latest.number == 3 && card.latest.sha256 == Sha(kScriptBytes) && card.stats.stars == 18
                    && card.stats.downloads == 1240 && card.tags.size() == 2 && card.latest.reviewed,
            "card fields");
    Check(card.capabilities.size() == 3 && card.capabilities[2] == Capability::ChatSend,
            "unknown capabilities are dropped, known ones kept");
    Check(card.author.kind == AuthorKind::User && !card.author.verified, "only the official account is verified");

    const auto refused = [](const std::string& text) {
        json::Value parsed;
        Card unused;
        return json::Parse(text, &parsed) && !ParseCard(parsed, &unused);
    };
    Check(refused(CardJson(kScriptId, "script", std::string(61, 'n'), 3, Sha("x"))), "a name over 60 is refused");
    Check(refused(CardJson(kScriptId, "script", "Bad\\u0007Name", 3, Sha("x"))), "a control character is refused");
    Check(refused(CardJson(kScriptId, "script", "Name", 0, Sha("x"))), "version number 0 is refused");
    Check(refused(CardJson(kScriptId, "script", "Name", 3, "abc")), "a bad hash is refused");
    Check(refused(CardJson("NOT-AN-ID-12", "script", "Name", 3, Sha("x"))), "a bad id is refused");
    Check(refused(CardJson(kScriptId, "plugin", "Name", 3, Sha("x"))), "an unknown kind is refused");
    Check(refused("{\"id\":\"7k2m9x4qa1dz\"}"), "an item without its fields is refused");
    std::string longName;
    for (int i = 0; i < 60; ++i) {
        longName += "\xc3\xa9";  // e acute: 60 code points, 120 bytes
    }
    Check(!refused(CardJson(kScriptId, "script", longName, 3, Sha("x"))), "caps count code points, not bytes");

    // No accounts: an item published from the game may carry only its author's display name and no licence.
    std::string anonymous = CardJson(kScriptId, "script", "From the game", 1, Sha("x"));
    const size_t authorAt = anonymous.find("\"author\":{");
    anonymous.replace(authorAt, anonymous.find('}', authorAt) + 1 - authorAt, "\"author_name\":\"Steve\"");
    anonymous.replace(anonymous.find("\"license\":\"MIT\""), 15, "\"license\":null");
    Check(json::Parse(anonymous, &value) && ParseCard(value, &card) && card.author.displayName == "Steve"
                  && card.author.handle.empty() && card.license.empty() && !card.author.verified,
          "an item with a plain author name and no licence parses");
    std::string nameless = anonymous;
    nameless.replace(nameless.find("\"author_name\":\"Steve\""), 21, "\"author_name\":\"\"");
    Check(refused(nameless), "an item without any author name is refused");

    const std::string page = "{\"items\":[" + CardJson(kScriptId, "script", "One", 1, Sha("a")) + ","
            + CardJson(kScriptId, "script", "Repeated", 1, Sha("a")) + ","
            + CardJson(kConfigId, "config", std::string(70, 'x'), 1, Sha("b")) + "],\"next_cursor\":\"c2\","
            "\"total_estimate\":12}";
    Page parsed;
    Check(json::Parse(page, &value) && ParsePage(value, &parsed), "a page parses");
    Check(parsed.items.size() == 1 && parsed.dropped == 1 && parsed.nextCursor == "c2" && parsed.totalEstimate == 12,
            "a page drops invalid and repeated items");
    Check(json::Parse("{\"items\":{}}", &value) && !ParsePage(value, &parsed), "a page without an items array");

    std::vector<LookupEntry> entries;
    const std::string lookup = "{\"items\":[" + CardJson(kScriptId, "script", "One", 4, Sha("a"))
            + ",{\"id\":\"abcdefghjkmn\",\"state\":\"removed\",\"reason_category\":\"malicious\"}]}";
    Check(json::Parse(lookup, &value) && ParseLookup(value, {kScriptId, kGoneId}, &entries) && entries.size() == 2
                    && entries[0].found && !entries[1].found && entries[1].missing.state == MissingState::Removed
                    && entries[1].missing.reasonCategory == "malicious",
            "a lookup answer, item and missing item");
    Check(!ParseLookup(value, {kGoneId, kScriptId}, &entries), "a lookup answer in another order is refused");
    Check(!ParseLookup(value, {kScriptId}, &entries), "a lookup answer with another count is refused");

    Problem problem;
    Check(json::Parse("{\"type\":\"x\",\"title\":\"Gone.\",\"status\":410,\"code\":\"gone\",\"state\":\"removed\","
                      "\"reason_category\":\"malicious\",\"request_id\":\"01J\"}", &value),
            "problem fixture");
    problem = ParseProblem(value, 410);
    Check(problem.code == "gone" && problem.goneState == "removed" && problem.reasonCategory == "malicious"
                    && problem.requestId == "01J" && problem.Text() == "Gone.",
            "a 410 problem with its tombstone");

    Check(detail::CleanSearch("  auto \n  gg  ", 64) == "auto gg", "search text cleaned");
    Check(detail::CodePoints(detail::CleanSearch(std::string(100, 'q'), 64)) == 64, "search text cut to 64");
    Check(detail::PercentEncode("auto gg/&") == "auto%20gg%2F%26", "query encoding");
}

// ---------------------------------------------------------------------------------------------------------------
// Local HTTP server (127.0.0.1, a free port), one thread per connection, every response with Connection: close

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

    std::string UrlFor(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }

    int Hits(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex_);
        int count = 0;
        for (const std::string& path : seen_) {
            count += path.compare(0, prefix.size(), prefix) == 0 ? 1 : 0;
        }
        return count;
    }

    std::string LastHeaders(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = seen_.size(); i > 0; --i) {
            if (seen_[i - 1].compare(0, prefix.size(), prefix) == 0) {
                return seenHeaders_[i - 1];
            }
        }
        return std::string();
    }

    // The headers of the last request whose path starts with `prefix` and does not contain `excluded`.
    std::string LastHeadersWithout(const std::string& prefix, const std::string& excluded) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = seen_.size(); i > 0; --i) {
            if (seen_[i - 1].compare(0, prefix.size(), prefix) == 0
                    && seen_[i - 1].find(excluded) == std::string::npos) {
                return seenHeaders_[i - 1];
            }
        }
        return std::string();
    }

    // The body of the last request whose "METHOD path" starts with `prefix`.
    std::string LastBody(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = seen_.size(); i > 0; --i) {
            if (seen_[i - 1].compare(0, prefix.size(), prefix) == 0) {
                return seenBodies_[i - 1];
            }
        }
        return std::string();
    }

    // Every "METHOD path" seen so far.
    std::vector<std::string> Seen() {
        std::lock_guard<std::mutex> lock(mutex_);
        return seen_;
    }

    std::string LastPath(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = seen_.size(); i > 0; --i) {
            if (seen_[i - 1].compare(0, prefix.size(), prefix) == 0) {
                return seen_[i - 1];
            }
        }
        return std::string();
    }

    std::atomic<bool> failConfigs{false};
    std::atomic<int> lookupLatest{3};
    std::string blockedInstall; // an "Install ..." header answered 403 (set before the requests)

private:
    struct Incoming {
        std::string method;
        std::string path;
        std::string headers;
        std::string body;
    };

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

    static bool ReadRequest(SOCKET client, Incoming* request) {
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
        request->headers = data.substr(0, headerEnd);
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

    static void Send(SOCKET client, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const int written = send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (written <= 0) {
                return;
            }
            sent += static_cast<size_t>(written);
        }
    }

    static std::string Reply(const char* status, const std::string& headers, const std::string& body) {
        return std::string("HTTP/1.1 ") + status + "\r\nConnection: close\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body;
    }

    static std::string Json(const char* status, const std::string& body, const std::string& extra = std::string()) {
        return Reply(status, "Content-Type: application/json\r\n" + extra, body);
    }

    static std::string ProblemReply(const char* status, int code, const std::string& name,
            const std::string& extra = std::string(), const std::string& headers = std::string()) {
        return Reply(status, "Content-Type: application/problem+json\r\n" + headers,
                "{\"type\":\"https://nickelclient.com/docs/api/errors#" + name + "\",\"title\":\"Problem " + name
                        + ".\",\"status\":" + std::to_string(code) + ",\"code\":\"" + name
                        + "\",\"request_id\":\"req-1\"" + extra + "}");
    }

    static std::string FileReply(const std::string& body, const std::string& digestOf, bool digest = true) {
        std::string headers = "Content-Type: text/plain; charset=utf-8\r\nETag: \"" + Sha(digestOf) + "\"\r\n";
        if (digest) {
            headers += "Content-Digest: " + net::ContentDigestFor(digestOf) + "\r\n";
        }
        return Reply("200 OK", headers, body);
    }

    static std::string HeaderValue(const std::string& headers, const std::string& name) {
        const size_t at = headers.find("\r\n" + name + ": ");
        if (at == std::string::npos) {
            return std::string();
        }
        const size_t start = at + name.size() + 4;
        return headers.substr(start, headers.find("\r\n", start) - start);
    }

    void Pause(int ms) const {
        for (int waited = 0; waited < ms && !stopping_; waited += 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void Serve(SOCKET client) {
        Incoming request;
        if (ReadRequest(client, &request)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                seen_.push_back(request.method + " " + request.path);
                seenHeaders_.push_back(request.headers);
                seenBodies_.push_back(request.body);
            }
            Send(client, Route(request));
        }
        shutdown(client, SD_SEND);
        closesocket(client);
    }

    std::string Route(const Incoming& request) {
        const std::string base = "/api/community/v1";
        if (request.path.compare(0, 3, "/rl") == 0) {
            return ProblemReply("429 Too Many Requests", 429, "rate_limited", ",\"retry_after\":7", "Retry-After: 7\r\n");
        }
        if (request.path.compare(0, base.size(), base) != 0) {
            return Reply("404 Not Found", "", "");
        }
        const std::string path = request.path.substr(base.size());
        const std::string tag = "W/\"list-" + std::to_string(std::hash<std::string>()(path)) + "\"";
        if (path.compare(0, 7, "/items?") == 0) {
            if (path.find("q=slow") != std::string::npos) {
                Pause(800);
            }
            if (path.find("expired") != std::string::npos && path.find("cursor=c2") != std::string::npos) {
                return ProblemReply("400 Bad Request", 400, "bad_request");  // the cursor expired
            }
            if (path.find("q=broken") != std::string::npos) {
                return Json("200 OK", "{\"items\": [" + CardJson(kScriptId, "script", "One", 1, Sha("a")));
            }
            if (path.find("kind=config") != std::string::npos && failConfigs) {
                return ProblemReply("500 Internal Server Error", 500, "server_error");
            }
            if (HeaderValue(request.headers, "If-None-Match") == tag) {
                return "HTTP/1.1 304 Not Modified\r\nConnection: close\r\nETag: " + tag + "\r\n\r\n";
            }
            std::string items;
            std::string cursor = "\"c2\"";
            if (path.find("cursor=c2") != std::string::npos) {
                items = CardJson(kConfigId, "script", "Second Page", 1, Sha("b")) + ","
                        + CardJson(kScriptId, "script", "Totem Reminder", 3, Sha(kScriptBytes));
                cursor = "null";
            } else if (path.find("kind=config") != std::string::npos) {
                items = CardJson(kConfigId, "config", "Bedwars Pack", 1, Sha(kConfigBytes),
                        ",\"categories\":[\"Combat\",\"Visuals\",\"Unknown\"],\"changes\":{\"modules\":12,"
                        "\"keybinds\":null}");
                cursor = "null";
            } else {
                items = CardJson(kScriptId, "script", "Totem Reminder", 3, Sha(kScriptBytes)) + ","
                        + CardJson(kGoneId, "script", std::string(80, 'x'), 1, Sha("x"));
            }
            return Json("200 OK",
                    "{\"items\":[" + items + "],\"next_cursor\":" + cursor + ",\"total_estimate\":3}",
                    "ETag: " + tag + "\r\nCache-Control: public, max-age=60\r\n");
        }
        if (path == std::string("/items/") + kScriptId) {
            return Json("200 OK", DetailJson());
        }
        if (path == std::string("/items/") + kGoneId) {
            return ProblemReply("410 Gone", 410, "gone",
                    ",\"state\":\"removed\",\"reason_category\":\"malicious\",\"id\":\"abcdefghjkmn\"");
        }
        if (path == std::string("/items/") + kTamperedId) {
            return Json("200 OK", "{\"id\":\"23456789abcd\",");
        }
        const std::string scriptFile = std::string("/items/") + kScriptId + "/versions/3/";
        if (path == scriptFile + "source" || path == scriptFile + "file") {
            return FileReply(kScriptBytes, kScriptBytes);
        }
        if (path == std::string("/items/") + kConfigId + "/versions/1/file") {
            return FileReply(kConfigBytes, kConfigBytes);
        }
        if (path == std::string("/items/") + kTamperedId + "/versions/1/source") {
            return FileReply(kScriptBytes + "os.exit()\n", kScriptBytes);  // digest of other bytes
        }
        if (path == std::string("/items/") + kNoDigestId + "/versions/1/file") {
            return FileReply(kScriptBytes, kScriptBytes, false);
        }
        if (path == std::string("/items/") + kDigestOnlyId + "/versions/1/file") {
            return Reply("200 OK",
                    "Content-Type: text/plain; charset=utf-8\r\nETag: \"" + Sha(kScriptBytes) + "\"\r\nContent-Digest: "
                            + net::ContentDigestFor("print('other')\n") + "\r\n",
                    kScriptBytes);
        }
        if (path == std::string("/items/") + kEtagOnlyId + "/versions/1/file") {
            return Reply("200 OK",
                    "Content-Type: text/plain; charset=utf-8\r\nETag: \"" + Sha("print('other')\n")
                            + "\"\r\nContent-Digest: " + net::ContentDigestFor(kScriptBytes) + "\r\n",
                    kScriptBytes);
        }
        if (path == "/items/lookup" && request.method == "POST") {
            json::Value body;
            if (!json::Parse(request.body, &body)) {
                return ProblemReply("400 Bad Request", 400, "bad_request");
            }
            std::string items;
            const json::Value& ids = body["ids"];
            for (size_t i = 0; i < ids.size(); ++i) {
                const std::string id = ids.At(i).AsString();
                items += i > 0 ? "," : "";
                if (id == kScriptId) {
                    items += CardJson(id, "script", "Totem Reminder", lookupLatest.load(), Sha(kScriptBytes));
                } else if (id == kGoneId) {
                    items += "{\"id\":\"" + id + "\",\"state\":\"removed\",\"reason_category\":\"malicious\"}";
                } else {
                    items += "{\"id\":\"" + id + "\",\"state\":\"not_found\"}";
                }
            }
            return Json("200 OK", "{\"items\":[" + items + "]}");
        }
        if (path == "/revocations") {
            return Json("200 OK",
                    "{\"files\":[{\"sha256\":\"" + Sha(kScriptBytes)
                            + "\",\"reason\":\"Sends your session to another server.\","
                              "\"revoked_at\":\"2026-11-14T10:00:00.000Z\"},{\"sha256\":\"bad\"}]}",
                    "ETag: \"rev-1\"\r\n");
        }
        const std::string install = HeaderValue(request.headers, "Authorization");
        const bool writes = request.method != "GET" && path != "/items/lookup";
        if (writes && !ValidInstallHeader(install)) {
            return ProblemReply("401 Unauthorized", 401, "unauthorized", "", "WWW-Authenticate: Install\r\n");
        }
        if (writes && !blockedInstall.empty() && install == blockedInstall) {
            return ProblemReply("403 Forbidden", 403, "install_blocked");
        }
        if (path.size() > 5 && path.compare(path.size() - 5, 5, "/star") == 0) {
            if (path == std::string("/items/") + kConfigId + "/star") {
                return ProblemReply("409 Conflict", 409, "conflict");
            }
            return Reply("204 No Content", "", "");
        }
        if (path == "/items/check" && request.method == "POST") {
            std::string bytes;
            if (!DecodeFile(request.body, &bytes)) {
                return ProblemReply("400 Bad Request", 400, "invalid");
            }
            if (bytes.find("require(") != std::string::npos) {
                return Json("200 OK",
                            "{\"ok\":false,\"warnings\":[],\"errors\":[{\"field\":\"file\",\"code\":\"require\","
                            "\"message\":\"require is not allowed.\",\"line\":1}]}");
            }
            return Json(
                    "200 OK",
                    "{\"ok\":true,\"sha256\":\"" + Sha(bytes) + "\",\"size\":" + std::to_string(bytes.size())
                            + ",\"capabilities\":[\"render\",\"chat.print\",\"future.thing\"],\"warnings\":[{\"code\":"
                              "\"chat\",\"message\":\"Prints in chat.\",\"line\":3}],\"errors\":[]}");
        }
        if (path == "/items" && request.method == "POST") {
            json::Value body;
            std::string bytes;
            if (!json::Parse(request.body, &body) || !body["name"].IsString() || !body["summary"].IsString()
                || !body["description"].IsString() || !body["tags"].IsArray() || !body["author_name"].IsString()
                || !DecodeFile(request.body, &bytes)) {
                return ProblemReply("400 Bad Request", 400, "invalid");
            }
            if (bytes.find("getfenv") != std::string::npos) {
                return ProblemReply("422 Unprocessable Content",
                                    422,
                                    "file_refused",
                                    ",\"errors\":[{\"field\":\"file\",\"code\":\"obfuscated\",\"message\":"
                                    "\"Obfuscated code is not allowed.\",\"line\":2}]");
            }
            std::lock_guard<std::mutex> lock(ownersMutex_);
            const std::string id = std::string("n3wq7p2x9k4") + "abcdefghjk"[owners_.size() % 10];
            owners_[id] = install;
            return Json("201 Created",
                        "{\"item\":{\"id\":\"" + id + "\",\"slug\":\"published-" + id
                                + "\",\"state\":\"published\"},\"version\":{\"number\":1,\"state\":\"published\"}}");
        }
        if (request.method == "POST" && path.size() == 7 + 12 + 9 && path.compare(0, 7, "/items/") == 0
            && path.compare(19, 9, "/versions") == 0) {
            const std::string id = path.substr(7, 12);
            json::Value body;
            std::string bytes;
            if (!json::Parse(request.body, &body) || !body["changelog"].IsString()
                || !DecodeFile(request.body, &bytes)) {
                return ProblemReply("400 Bad Request", 400, "invalid");
            }
            std::lock_guard<std::mutex> lock(ownersMutex_);
            const auto owner = owners_.find(id);
            if (owner == owners_.end()) {
                return ProblemReply("404 Not Found", 404, "not_found");
            }
            if (owner->second != install) {
                return ProblemReply("403 Forbidden", 403, "not_owner");
            }
            return Json("201 Created", "{\"version\":{\"number\":2,\"state\":\"published\"}}");
        }
        if (path == "/reports" && request.method == "POST") {
            json::Value body;
            if (!json::Parse(request.body, &body) || !body["item_id"].IsString() || !body["category"].IsString()) {
                return ProblemReply("400 Bad Request", 400, "invalid");
            }
            if (body["item_id"].AsString() == kGoneId) {
                return ProblemReply("409 Conflict", 409, "already_reported");
            }
            return Json("202 Accepted", "{\"id\":\"r1\"}");
        }
        return ProblemReply("404 Not Found", 404, "not_found");
    }

    // "Install " and 43 characters of base64url (32 bytes).
    static bool ValidInstallHeader(const std::string& value) {
        if (value.size() != 8 + 43 || value.compare(0, 8, "Install ") != 0) {
            return false;
        }
        for (size_t i = 8; i < value.size(); ++i) {
            const char c = value[i];
            const bool ok =
                    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    // The bytes of a request's file { name, content, encoding: "base64" }.
    static bool DecodeFile(const std::string& text, std::string* bytes) {
        json::Value body;
        if (!json::Parse(text, &body)) {
            return false;
        }
        const json::Value& file = body["file"];
        return file.IsObject() && file["name"].IsString() && file["encoding"].IsString()
               && file["encoding"].AsString() == "base64" && file["content"].IsString()
               && net::Base64Decode(file["content"].AsString(), bytes);
    }

    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::thread> handlers_;
    std::vector<std::string> seen_;
    std::vector<std::string> seenHeaders_;
    std::vector<std::string> seenBodies_;
    std::mutex ownersMutex_;
    std::map<std::string, std::string> owners_; // item id -> the Install header that published it
};

ClientConfig LoopbackConfig(const TestServer& server, const std::string& base = "/api/community/v1") {
    ClientConfig config;
    config.baseUrl = server.UrlFor(base);
    config.policy.httpsHosts = {"nickelclient.com"};
    config.policy.allowLoopbackHttp = true;
    config.userAgent = L"Nickel-Overlay/test";
    config.timeoutMs = 5000;
    return config;
}

// Waits until `done(snapshot)` holds (true) or 10 s pass (false).
bool WaitFor(const PublicClient& client, const std::function<bool(const Snapshot&)>& done) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        if (done(client.Snap())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool ListDone(const Snapshot& snapshot, Kind kind) {
    const ListResult* list = snapshot.List(kind);
    return list != nullptr && !list->loading && !list->loadingMore;
}

void ListChecks(TestServer& server) {
    PublicClient client(LoopbackConfig(server));
    Check(client.Usable(), "a loopback base URL is usable under a loopback policy");
    ListQuery query;
    query.kind = Kind::Script;
    query.sort = Sort::Top;
    query.text = "  auto  gg ";
    query.tags = {"pvp"};
    query.compat = "1.1.0-beta";
    Check(client.List(query), "a list is posted");
    Check(client.Snap().List(Kind::Script) != nullptr && client.Snap().List(Kind::Script)->loading
                    || ListDone(client.Snap(), Kind::Script),
            "a list shows loading at once");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Script); }), "the list arrives");
    Snapshot snapshot = client.Snap();
    const ListResult* list = snapshot.List(Kind::Script);
    Check(list != nullptr && list->outcome.Ok() && list->items.size() == 1 && list->dropped == 1
                    && list->nextCursor == "c2" && list->totalEstimate == 3 && !list->fromCache,
            "the first page, with the invalid item dropped");
    Check(server.LastPath("GET /api/community/v1/items?")
                    == "GET /api/community/v1/items?kind=script&sort=top&q=auto%20gg&tag=pvp&compat=1.1.0&limit=24",
            "the list query string");
    Check(server.LastHeaders("GET /api/community/v1/items?").find("User-Agent: Nickel-Overlay/test")
                    != std::string::npos,
            "the user agent names the overlay");
    Check(server.LastHeaders("GET /api/community/v1/items?").find("Authorization") == std::string::npos,
            "no token on public reads");

    // ETag: the same query again is answered 304 and shows the kept body.
    Check(client.List(query), "the same list again");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Script); }), "the second answer");
    snapshot = client.Snap();
    list = snapshot.List(Kind::Script);
    Check(server.LastHeaders("GET /api/community/v1/items?").find("If-None-Match: W/\"list-") != std::string::npos,
            "If-None-Match sent with the kept ETag");
    Check(list->outcome.Ok() && list->outcome.notModified && list->items.size() == 1, "304 reuses the kept list");

    // More: the next page is appended without repeated ids.
    Check(client.More(Kind::Script), "the next page is posted");
    Check(!client.More(Kind::Script), "only one next page at a time");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Script); }), "the next page arrives");
    snapshot = client.Snap();
    list = snapshot.List(Kind::Script);
    Check(list->outcome.Ok() && list->items.size() == 2 && list->items[1].name == "Second Page"
                    && list->nextCursor.empty(),
            "pages are merged, repeated ids dropped");
    Check(!client.More(Kind::Script), "no page after the last");

    // Configs: categories and changes; then the server fails and the kept list is shown.
    ListQuery configs;
    configs.kind = Kind::Config;
    configs.categories = {"Combat"};
    Check(client.List(configs), "a config list");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Config); }), "the config list arrives");
    snapshot = client.Snap();
    list = snapshot.List(Kind::Config);
    Check(list->outcome.Ok() && list->items.size() == 1 && list->items[0].categories.size() == 2
                    && list->items[0].changedModules == 12 && list->items[0].changedKeybinds == -1,
            "config cards: known categories, module changes");
    Check(snapshot.List(Kind::Script) != nullptr && snapshot.List(Kind::Script)->items.size() == 2,
            "each kind keeps its own list");
    server.failConfigs = true;
    Check(client.List(configs), "the config list again, server failing");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Config); }), "the failure arrives");
    snapshot = client.Snap();
    list = snapshot.List(Kind::Config);
    Check(list->outcome.status == Status::ServerError && list->fromCache && list->items.size() == 1
                    && list->outcome.problem.code == "server_error" && list->outcome.problem.requestId == "req-1",
            "a failed list falls back to the kept answer, with the problem");
    server.failConfigs = false;

    // Malformed JSON is refused.
    ListQuery broken = query;
    broken.text = "broken";
    Check(client.List(broken), "a list answered with broken JSON");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Script); }), "the broken answer");
    snapshot = client.Snap();
    Check(snapshot.List(Kind::Script)->outcome.status == Status::Malformed
                    && snapshot.List(Kind::Script)->items.empty(),
            "malformed JSON is refused");

    // A newer list makes the older answer stale: only the newer one is shown.
    ListQuery slow = query;
    slow.text = "slow";
    ListQuery fast = query;
    fast.text = "fast";
    Check(client.List(slow) && client.List(fast), "two lists in a row");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Script); }), "the newer list arrives");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    snapshot = client.Snap();
    Check(snapshot.List(Kind::Script)->query.text == "fast" && snapshot.List(Kind::Script)->outcome.Ok(),
            "the stale answer is dropped");

    // Arguments refused before any request.
    ListQuery bad = query;
    bad.tags = {"NOT A TAG"};
    Check(!client.List(bad) && client.Snap().List(Kind::Script)->outcome.status == Status::Invalid,
            "an invalid tag is refused locally");
    ListQuery categoriesOnScripts = query;
    categoriesOnScripts.categories = {"Combat"};
    Check(!client.List(categoriesOnScripts), "categories only for configs");
}

// An expired cursor (400) restarts the list from a fresh first page; a late 400 never replaces a newer list.
void CursorChecks(TestServer& server) {
    PublicClient client(LoopbackConfig(server));
    const std::string items = "GET /api/community/v1/items?";
    ListQuery query;
    query.kind = Kind::Script;
    query.text = "expired";
    Check(client.List(query), "a list whose cursor will expire");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Script); }), "its first page");
    Check(client.List(query), "the same list again, kept with its ETag");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Script); }) && client.Snap()
                    .List(Kind::Script)->outcome.notModified,
            "the first page is answered 304");
    const int cursorHits = server.Hits(items);
    Check(client.More(Kind::Script), "the next page with an expired cursor");
    Check(WaitFor(client,
                  [](const Snapshot& s) {
                      return ListDone(s, Kind::Script) && s.List(Kind::Script)->generation > 2;
                  }),
            "the list starts again");
    Snapshot snapshot = client.Snap();
    const ListResult* list = snapshot.List(Kind::Script);
    Check(server.Hits(items) == cursorHits + 2 && server.LastPath(items).find("cursor=c2") == std::string::npos,
            "a 400 on the cursor page asks for the first page again");
    Check(server.LastHeadersWithout(items, "cursor=").find("If-None-Match") == std::string::npos,
            "the restarted first page is asked without If-None-Match");
    Check(list->outcome.Ok() && !list->outcome.notModified && list->nextCursor == "c2" && list->query.text == "expired",
            "a fresh first page with a fresh cursor, so page 2 can be asked again");

    // A newer List() while that More is in flight: the late 400 must not bring the old query back.
    ListQuery slow = query;
    slow.text = "slowexpired";
    Check(client.List(slow), "a slow list whose cursor will expire");
    Check(WaitFor(client, [](const Snapshot& s) { return ListDone(s, Kind::Script); }), "the slow first page");
    Check(client.More(Kind::Script), "its next page, slow and expired");
    ListQuery newer = query;
    newer.text = "newer";
    Check(client.List(newer), "the player searches again meanwhile");
    Check(WaitFor(client,
                  [](const Snapshot& s) {
                      return ListDone(s, Kind::Script) && s.List(Kind::Script)->query.text == "newer";
                  }),
            "the newer list arrives");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    snapshot = client.Snap();
    Check(snapshot.List(Kind::Script)->query.text == "newer" && snapshot.List(Kind::Script)->outcome.Ok()
                    && ListDone(snapshot, Kind::Script),
            "a late expired cursor never replaces the newer search");
}

void DetailAndFileChecks(TestServer& server) {
    PublicClient client(LoopbackConfig(server));
    Check(client.Detail(kScriptId), "a detail is posted");
    Check(WaitFor(client, [](const Snapshot& s) { return s.detail && !s.detail->loading; }), "the detail arrives");
    Snapshot snapshot = client.Snap();
    Check(snapshot.detail->outcome.Ok() && snapshot.detail->detail.card.id == kScriptId
                    && snapshot.detail->detail.description == "Line one.\nLine two."
                    && snapshot.detail->detail.versions.size() == 1
                    && snapshot.detail->detail.versions[0].changelog == "Fixed the border."
                    && snapshot.detail->detail.pageUrl == "https://nickelclient.com/public/script/item-7k2m9x4qa1dz",
            "detail fields");

    Check(client.Detail(kGoneId), "a removed item");
    Check(WaitFor(client, [](const Snapshot& s) { return s.detail && !s.detail->loading; }), "the 410 arrives");
    snapshot = client.Snap();
    Check(snapshot.detail->outcome.status == Status::Gone && snapshot.detail->outcome.problem.goneState == "removed"
                    && snapshot.detail->outcome.problem.reasonCategory == "malicious",
            "410 gives the tombstone");

    Check(client.Detail(kTamperedId), "a truncated detail");
    Check(WaitFor(client, [](const Snapshot& s) { return s.detail && !s.detail->loading; }), "the bad detail arrives");
    Check(client.Snap().detail->outcome.status == Status::Malformed, "a truncated detail is refused");
    Check(!client.Detail("../../etc") && client.Snap().detail->outcome.status == Status::Invalid,
            "an invalid id never reaches the server");

    // Files: hash and Content-Digest checked.
    Check(client.Source(kScriptId, 3, Sha(kScriptBytes)), "a source preview");
    Check(WaitFor(client, [](const Snapshot& s) { return s.source && !s.source->loading; }), "the source arrives");
    snapshot = client.Snap();
    Check(snapshot.source->outcome.Ok() && snapshot.source->bytes == kScriptBytes && !snapshot.source->install,
            "the source with its hash checked");
    Check(server.LastPath("GET /api/community/v1/items/7k2m9x4qa1dz/versions/3/")
                    == "GET /api/community/v1/items/7k2m9x4qa1dz/versions/3/source",
            "previews use /source (not counted)");

    Check(client.File(kScriptId, 3, Sha(kScriptBytes)), "an install download");
    Check(WaitFor(client, [](const Snapshot& s) { return s.file && !s.file->loading; }), "the file arrives");
    snapshot = client.Snap();
    Check(snapshot.file->outcome.Ok() && snapshot.file->bytes == kScriptBytes && snapshot.file->install
                    && server.LastPath("GET /api/community/v1/items/7k2m9x4qa1dz/versions/3/")
                            == "GET /api/community/v1/items/7k2m9x4qa1dz/versions/3/file",
            "installs use /file (counted)");

    Check(client.File(kScriptId, 3, Sha("other bytes")), "a download expecting another hash");
    Check(WaitFor(client, [](const Snapshot& s) { return s.file && !s.file->loading; }), "the mismatch arrives");
    Check(client.Snap().file->outcome.status == Status::DigestMismatch && client.Snap().file->bytes.empty(),
            "a file that does not match its sha256 is discarded");

    Check(client.Source(kTamperedId, 1, Sha(kScriptBytes)), "a tampered file");
    Check(WaitFor(client, [](const Snapshot& s) { return s.source && !s.source->loading; }), "the tampered answer");
    Check(client.Snap().source->outcome.status == Status::DigestMismatch && client.Snap().source->bytes.empty(),
            "a body that does not match its Content-Digest is discarded");

    Check(client.File(kNoDigestId, 1, Sha(kScriptBytes)), "a file without Content-Digest");
    Check(WaitFor(client, [](const Snapshot& s) { return s.file && !s.file->loading; }), "the undigested answer");
    Check(client.Snap().file->outcome.status == Status::DigestMismatch, "Content-Digest is required");

    Check(client.File(kDigestOnlyId, 1, Sha(kScriptBytes)), "a file whose Content-Digest names other bytes");
    Check(WaitFor(client, [](const Snapshot& s) { return s.file && !s.file->loading; }), "the digest-only answer");
    Check(client.Snap().file->outcome.status == Status::DigestMismatch && client.Snap().file->bytes.empty(),
            "bytes matching sha256 and ETag are still discarded when Content-Digest does not match");
    Check(client.File(kEtagOnlyId, 1, Sha(kScriptBytes)), "a file whose strong ETag names another hash");
    Check(WaitFor(client, [](const Snapshot& s) { return s.file && !s.file->loading; }), "the ETag-only answer");
    Check(client.Snap().file->outcome.status == Status::DigestMismatch && client.Snap().file->bytes.empty(),
            "bytes matching sha256 and Content-Digest are still discarded when a strong ETag names another hash");

    Check(!client.File(kScriptId, 3, "not-a-hash"), "a download needs the expected hash");
}

void LookupAndRevocationChecks(TestServer& server) {
    PublicClient client(LoopbackConfig(server));
    Check(client.Lookup({kScriptId, kGoneId, kConfigId, kScriptId}), "a lookup");
    Check(WaitFor(client, [](const Snapshot& s) { return s.lookup && !s.lookup->loading; }), "the lookup arrives");
    Snapshot snapshot = client.Snap();
    Check(snapshot.lookup->outcome.Ok() && snapshot.lookup->entries.size() == 3 && snapshot.lookup->entries[0].found
                    && snapshot.lookup->entries[0].card.latest.number == 3 && !snapshot.lookup->entries[1].found
                    && snapshot.lookup->entries[1].missing.state == MissingState::Removed
                    && snapshot.lookup->entries[2].missing.state == MissingState::NotFound,
            "lookup entries in order, ids sent once");
    Check(server.LastHeaders("POST /api/community/v1/items/lookup").find("Content-Type: application/json")
                    != std::string::npos,
            "the lookup body is JSON");

    Check(client.Revocations(), "revocations");
    Check(WaitFor(client, [](const Snapshot& s) { return s.revocations && !s.revocations->loading; }),
            "the revocations arrive");
    snapshot = client.Snap();
    Check(snapshot.revocations->outcome.Ok() && snapshot.revocations->files.size() == 1
                    && snapshot.revocations->dropped == 1 && snapshot.revocations->files[0].sha256 == Sha(kScriptBytes),
            "revocations, an invalid entry dropped");
}

void RateLimitChecks(TestServer& server) {
    PublicClient client(LoopbackConfig(server, "/rl/api/community/v1"));
    Check(client.Detail(kScriptId), "a request that is rate limited");
    Check(WaitFor(client, [](const Snapshot& s) { return s.detail && !s.detail->loading; }), "the 429 arrives");
    Snapshot snapshot = client.Snap();
    Check(snapshot.detail->outcome.status == Status::RateLimited && snapshot.detail->outcome.retryAfter == 7
                    && snapshot.detail->outcome.problem.retryAfter == 7,
            "429 with retry_after");
    const int hits = server.Hits("GET /rl");
    Check(client.Lookup({kScriptId}), "another request while blocked");
    Check(WaitFor(client, [](const Snapshot& s) { return s.lookup && !s.lookup->loading; }), "the blocked answer");
    snapshot = client.Snap();
    Check(snapshot.lookup->outcome.status == Status::RateLimited && snapshot.lookup->outcome.retryAfter > 0
                    && snapshot.lookup->outcome.retryAfter <= 7 && server.Hits("GET /rl") == hits
                    && server.Hits("POST /rl") == 0,
            "requests wait for Retry-After without reaching the server");
}

void PolicyChecks() {
    ClientConfig config;
    config.baseUrl = "https://example.com/api/community/v1";
    PublicClient client(config);
    Check(!client.Usable(), "a base URL outside the policy is not usable");
    Check(!client.Revocations() && client.Snap().revocations->outcome.status == Status::Invalid,
            "nothing is requested from a host outside the policy");
    ClientConfig plain;
    plain.baseUrl = "http://nickelclient.com/api/community/v1";
    Check(!PublicClient(plain).Usable(), "plain HTTP is refused for the real host");
    Check(PublicClient().Usable(), "the default base URL is usable");

    PublicClient noKey(config);
    Check(!noKey.Star(kScriptId, true) && noKey.Snap().star->outcome.status == Status::Invalid,
          "a write to a host outside the policy is refused first");
}

// ---------------------------------------------------------------------------------------------------------------
// Installer, on a temporary folder

// A ScriptLibrary over a folder, with the engine's own file code (script_files.hpp): what the installer sees in game.
class FolderLibrary : public overlay::ScriptLibrary {
public:
    explicit FolderLibrary(std::wstring folder) : folder_(std::move(folder)), held_(folder_) {}

    std::vector<overlay::ScriptInfo> List() const override {
        std::vector<overlay::ScriptInfo> scripts;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((folder_ + L"\\*.luau").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            return scripts;
        }
        do {
            std::wstring stem = data.cFileName;
            stem.resize(stem.size() - 5);
            overlay::ScriptInfo info;
            info.name = net::detail::Narrow(stem);
            info.file = folder_ + L"\\" + data.cFileName;
            std::lock_guard<std::recursive_mutex> lock(guard_);
            info.held = held_.Contains(info.name);
            scripts.push_back(info);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return scripts;
    }

    std::wstring Folder() const override { return folder_; }

    bool ReadSource(const std::string& name, overlay::ScriptSource& source) const override {
        return overlay::ReadScriptSource(overlay::ScriptFilePath(folder_, name), source);
    }

    std::vector<overlay::CompileError> Compile(const std::string& source) const override {
        if (source.find("@@error@@") != std::string::npos) {
            return {{1, "Expected 'end'"}};
        }
        return {};
    }

    bool HasBackup(const std::string& name) const override { return overlay::ScriptHasBackup(folder_, name); }

    overlay::ScriptFileResult WriteSource(const std::string& name, const std::string& source,
            const std::string& expectedSha256) override {
        return overlay::WriteScriptFile(Store(), name, Known(name), source, expectedSha256);
    }

    overlay::ScriptFileResult Remove(const std::string& name, const std::string& expectedSha256) override {
        return overlay::RemoveScriptFile(Store(), name, Known(name), expectedSha256);
    }

    overlay::ScriptFileResult RestoreBackup(const std::string& name, const std::string& expectedSha256) override {
        return overlay::RestoreScriptBackup(Store(), name, Known(name), expectedSha256);
    }

    bool Hold(const std::string& name) override {
        if (!Known(name)) {
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(guard_);
        return held_.Add(name);
    }

    bool Start(const std::string& name) override {
        if (!Known(name)) {
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(guard_);
        return held_.Remove(name);
    }

    bool Stop(const std::string& name) override { return Known(name); }

    bool Held(const std::string& name) const {
        std::lock_guard<std::recursive_mutex> lock(guard_);
        return held_.Contains(name);
    }

private:
    bool Known(const std::string& name) const { return overlay::FindScript(List(), name) >= 0; }

    overlay::ScriptFolderFiles Store() const { return {folder_, files_, guard_, held_}; }

    std::wstring folder_;
    mutable std::mutex files_;
    mutable std::recursive_mutex guard_;
    mutable overlay::ScriptHeldNames held_;
};

void DeleteTree(const std::wstring& folder) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring path = folder + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(path);
            } else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(folder.c_str());
}

std::wstring TemporaryRoot() {
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    std::wstring root = std::wstring(buffer) + L"nickel-public-checks-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(GetTickCount64());
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\scripts").c_str(), nullptr);
    return root;
}

std::string ReadText(const std::wstring& path) {
    std::string bytes;
    nickel::pub::detail::ReadAll(path, &bytes, 1024 * 1024);
    return bytes;
}

void WriteText(const std::wstring& path, const std::string& bytes) {
    nickel::pub::detail::WriteAll(path, bytes);
}

Package ScriptPackage(const std::string& id, const std::string& name, int number, const std::string& bytes) {
    Package package;
    package.kind = Kind::Script;
    package.id = id;
    package.versionNumber = number;
    package.label = "1." + std::to_string(number) + ".0";
    package.name = name;
    package.sha256 = Sha(bytes);
    package.bytes = bytes;
    return package;
}

int CountFiles(const std::wstring& pattern) {
    int count = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            ++count;
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return count;
}

void InstallerScriptChecks(const std::wstring& root) {
    FolderLibrary library(root + L"\\scripts");
    InstallerOptions options;
    options.root = root;
    options.scripts = &library;
    options.now = [] { return int64_t(1794765600000LL); };
    Installer installer(options);
    const std::wstring scripts = root + L"\\scripts";

    // Fresh install: written through the library, held, recorded.
    InstallOutcome outcome = installer.Install(ScriptPackage(kScriptId, "Auto GG", 3, kScriptBytes));
    Check(outcome.status == InstallStatus::Done && outcome.name == "Auto GG", "a script installs");
    Check(ReadText(scripts + L"\\Auto GG.luau") == kScriptBytes, "the script file holds the bytes as served");
    Check(library.Held("Auto GG"), "an installed script is held (never auto-started)");
    const std::string sidecar = ReadText(scripts + L"\\.public\\installed.json");
    Check(sidecar.find("\"id\":\"7k2m9x4qa1dz\"") != std::string::npos
                    && sidecar.find("\"version_number\":3") != std::string::npos
                    && sidecar.find("\"sha256\":\"" + Sha(kScriptBytes) + "\"") != std::string::npos
                    && sidecar.find("\"file\":\"Auto GG.luau\"") != std::string::npos
                    && sidecar.find("\"installed_at\":\"2026-11-15T18:00:00.000Z\"") != std::string::npos,
            "the provenance sidecar has id, version_number, sha256, file, installed_at");
    Check(sidecar.find("held") == std::string::npos && sidecar.find("started") == std::string::npos,
            "the sidecar stores no running state");
    InstallRecord record;
    Check(installer.Find(Kind::Script, kScriptId, &record) && record.versionNumber == 3, "the record is found");

    Installer reloaded(options);
    Check(reloaded.Records(Kind::Script).size() == 1 && reloaded.Records(Kind::Script)[0].file == "Auto GG.luau",
            "records survive a restart");

    // Bad packages are refused before any write.
    Package tampered = ScriptPackage("abcdefghjkmn", "Tampered", 1, "print(1)\n");
    tampered.bytes = "print(2)\n";
    Check(installer.Install(tampered).status == InstallStatus::BadPackage
                    && !nickel::pub::detail::FileExists(scripts + L"\\Tampered.luau"),
            "bytes that do not match their hash are not installed");
    Check(installer.Install(ScriptPackage("abcdefghjkmn", "Nul", 1, std::string("a\0b", 3))).status
                    == InstallStatus::BadPackage,
            "a NUL byte is refused");
    Check(installer.Install(ScriptPackage("abcdefghjkmn", "Latin", 1, "x = '\xe9'\n")).status
                    == InstallStatus::BadPackage,
            "a script that is not UTF-8 is refused");

    // Name conflicts with a local script that did not come from PUBLIC.
    WriteText(scripts + L"\\Mine.luau", "print('mine')\n");
    Package other = ScriptPackage("abcdefghjkmn", "Mine", 1, "print('public')\n");
    outcome = installer.Install(other);
    Check(outcome.status == InstallStatus::NameConflict && outcome.name == "Mine"
                    && ReadText(scripts + L"\\Mine.luau") == "print('mine')\n",
            "a local file of that name is a conflict, untouched");
    outcome = installer.Install(other, OnConflict::KeepBoth);
    Check(outcome.status == InstallStatus::Done && outcome.name == "Mine (2)"
                    && ReadText(scripts + L"\\Mine (2).luau") == "print('public')\n" && library.Held("Mine (2)"),
            "Keep both writes the next free name, held");
    Check(installer.Uninstall(Kind::Script, "abcdefghjkmn").status == InstallStatus::Done
                    && !nickel::pub::detail::FileExists(scripts + L"\\Mine (2).luau"),
            "uninstall removes the file");
    outcome = installer.Install(other, OnConflict::Replace);
    Check(outcome.status == InstallStatus::Done && ReadText(scripts + L"\\Mine.luau") == "print('public')\n"
                    && library.HasBackup("Mine"),
            "Replace keeps the local file as the engine's backup");
    Check(installer.Install(ScriptPackage("pqrstvwxyz01", "Mine", 1, "print(3)\n"), OnConflict::Replace).status
                    == InstallStatus::NameConflict,
            "Replace never takes the file of another PUBLIC item");

    // Reinstalling the same id is an update, not a conflict.
    library.Start("Auto GG");
    Check(!library.Held("Auto GG"), "the player started the script");
    const std::string v4 = "print('hello v4')\n";
    outcome = installer.Install(ScriptPackage(kScriptId, "Auto GG", 4, v4));
    Check(outcome.status == InstallStatus::Done && outcome.record.versionNumber == 4
                    && ReadText(scripts + L"\\Auto GG.luau") == v4,
            "the same id again updates in place");
    Check(library.Held("Auto GG"), "an updated script is held again until Start");

    // Local edits block updates and uninstalls unless the player confirms.
    WriteText(scripts + L"\\Auto GG.luau", "print('my edit')\n");
    const std::string v5 = "print('hello v5')\n";
    Check(installer.Update(ScriptPackage(kScriptId, "Auto GG", 5, v5)).status == InstallStatus::Modified
                    && ReadText(scripts + L"\\Auto GG.luau") == "print('my edit')\n",
            "an edited script is not overwritten");
    std::vector<LocalState> states = installer.States(Kind::Script);
    Check(states.size() == 2 && states[0].state == Local::Modified, "local edits show as Modified");
    outcome = installer.Update(ScriptPackage(kScriptId, "Auto GG", 5, v5), true);
    Check(outcome.status == InstallStatus::Done && ReadText(scripts + L"\\Auto GG.luau") == v5
                    && ReadText(overlay::ScriptBackupPath(scripts, "Auto GG")) == "print('my edit')\n",
            "Update, back up my edits: the edited file becomes the backup");
    Check(installer.Update(ScriptPackage("23456789abcd", "Nope", 1, v5)).status == InstallStatus::NotInstalled,
            "an update needs an installed id");

    // Compile errors are reported with the install, which still succeeds (held).
    outcome = installer.Install(ScriptPackage("23456789abcd", "Broken", 1, "@@error@@\n"));
    Check(outcome.status == InstallStatus::Done && outcome.compileErrors.size() == 1 && library.Held("Broken"),
            "a script that does not compile installs held, with its errors");

    // A byte order mark is dropped by the library: the record keeps both hashes.
    const std::string bom = "\xef\xbb\xbfprint('bom')\n";
    outcome = installer.Install(ScriptPackage("9x8w7v6t5s4r", "Bom", 1, bom));
    Check(outcome.status == InstallStatus::Done && outcome.record.sha256 == Sha("print('bom')\n")
                    && outcome.record.downloadSha256 == Sha(bom),
            "the record has the hash as written and as served");
    Check(installer.States(Kind::Script)[3].state == Local::Installed, "a BOM install is not seen as an edit");

    // Lookups: updates, skipped versions, withdrawn and removed items.
    LookupEntry newer;
    newer.id = kScriptId;
    newer.found = true;
    newer.card.latest.number = 6;
    std::vector<LookupEntry> lookup = {newer};
    states = installer.States(Kind::Script, &lookup);
    Check(states[0].id == kScriptId && states[0].state == Local::UpdateAvailable, "a newer version is an update");
    Check(installer.SkipVersion(Kind::Script, kScriptId, 6)
                    && installer.States(Kind::Script, &lookup)[0].state == Local::Installed,
            "a skipped version shows no update");
    lookup[0].card.latest.number = 2;
    Check(installer.States(Kind::Script, &lookup)[0].state == Local::Withdrawn, "an older latest version is withdrawn");
    lookup[0].found = false;
    lookup[0].missing.state = MissingState::Deleted;
    Check(installer.States(Kind::Script, &lookup)[0].state == Local::Removed, "a deleted item is removed");

    // Revocation: the script is held again, and shows Revoked.
    library.Start("Auto GG");
    std::vector<Revocation> revocations(1);
    revocations[0].sha256 = Sha(v5);
    revocations[0].reason = "Steals sessions.";
    std::vector<RevokedInstall> revoked = installer.ApplyRevocations(revocations);
    Check(revoked.size() == 1 && revoked[0].id == kScriptId && revoked[0].held && revoked[0].reason == "Steals sessions."
                    && library.Held("Auto GG"),
            "a revoked script is held");
    Check(installer.States(Kind::Script)[0].state == Local::Revoked, "a revoked script shows Revoked");
    revocations[0].sha256 = Sha("print('mine')\n");
    Check(installer.ApplyRevocations(revocations).empty() && installer.States(Kind::Script)[0].state != Local::Revoked,
            "a revocation that no longer lists the file clears it");

    // Uninstall: refuses an edited file unless confirmed; the record goes.
    WriteText(scripts + L"\\Auto GG.luau", "print('edited again')\n");
    Check(installer.Uninstall(Kind::Script, kScriptId).status == InstallStatus::Modified
                    && nickel::pub::detail::FileExists(scripts + L"\\Auto GG.luau"),
            "an edited script is not removed silently");
    Check(installer.Uninstall(Kind::Script, kScriptId, true).status == InstallStatus::Done
                    && !nickel::pub::detail::FileExists(scripts + L"\\Auto GG.luau")
                    && !installer.Find(Kind::Script, kScriptId, nullptr) && !library.Held("Auto GG"),
            "Remove anyway removes it and drops the record");
    Check(installer.Uninstall(Kind::Script, kScriptId).status == InstallStatus::NotInstalled,
            "uninstalling twice says not installed");
    DeleteFileW((scripts + L"\\Broken.luau").c_str());
    Check(installer.Uninstall(Kind::Script, "23456789abcd").status == InstallStatus::Done,
            "a file already gone only drops the record");
}

void InstallerConfigChecks(const std::wstring& root) {
    InstallerOptions options;
    options.root = root;
    Installer installer(options);
    const std::wstring configs = root + L"\\configs";
    Package config;
    config.kind = Kind::Config;
    config.id = kConfigId;
    config.versionNumber = 1;
    config.label = "1.0.0";
    config.name = "Bedwars: Pack?";
    config.sha256 = Sha(kConfigBytes);
    config.bytes = kConfigBytes;

    InstallOutcome outcome = installer.Install(config);
    Check(outcome.status == InstallStatus::Done && outcome.name == "Bedwars Pack", "a config installs, name cleaned");
    Check(ReadText(configs + L"\\Bedwars Pack.ini") == kConfigBytes, "configs are written byte for byte");
    Check(CountFiles(configs + L"\\.public\\tmp\\*.tmp") == 0, "no temporary file is left");
    Check(ReadText(configs + L"\\.public\\installed.json").find("\"file\":\"Bedwars Pack.ini\"") != std::string::npos,
            "the config sidecar");

    WriteText(configs + L"\\Mine.ini", "[menu]\r\n");
    Package other = config;
    other.id = "abcdefghjkmn";
    other.name = "Mine";
    Check(installer.Install(other).status == InstallStatus::NameConflict
                    && ReadText(configs + L"\\Mine.ini") == "[menu]\r\n",
            "a config conflict leaves the local file");
    Check(installer.Install(other, OnConflict::Replace).status == InstallStatus::Done
                    && ReadText(configs + L"\\Mine.ini") == kConfigBytes
                    && CountFiles(configs + L"\\.public\\backup\\Mine-*.ini") == 1,
            "Replace moves the local config to the backup folder first");

    WriteText(configs + L"\\Bedwars Pack.ini", "[menu]\r\nedited=1\r\n");
    Package v2 = config;
    v2.versionNumber = 2;
    v2.bytes = kConfigBytes + "[hud]\r\n";
    v2.sha256 = Sha(v2.bytes);
    Check(installer.Update(v2).status == InstallStatus::Modified, "an edited config is not updated silently");
    Check(installer.Update(v2, true).status == InstallStatus::Done
                    && ReadText(configs + L"\\Bedwars Pack.ini") == v2.bytes
                    && CountFiles(configs + L"\\.public\\backup\\Bedwars Pack-*.ini") == 1,
            "a confirmed update backs the edited config up");

    std::vector<Revocation> revocations(1);
    revocations[0].sha256 = v2.sha256;
    std::vector<RevokedInstall> revoked = installer.ApplyRevocations(revocations);
    Check(revoked.size() == 1 && revoked[0].kind == Kind::Config && !revoked[0].held,
            "a revoked config is reported (configs never load by themselves)");

    Check(installer.Uninstall(Kind::Config, kConfigId).status == InstallStatus::Done
                    && !nickel::pub::detail::FileExists(configs + L"\\Bedwars Pack.ini")
                    && CountFiles(configs + L"\\.public\\backup\\Bedwars Pack-*.ini") == 2,
            "uninstalling a config moves it to the backup folder");
}

// An install whose sidecar cannot be saved is taken back without losing the player's files; Keep both skips names
// owned by other PUBLIC records.
void InstallerRollbackChecks(const std::wstring& root) {
    FolderLibrary library(root + L"\\scripts");
    InstallerOptions options;
    options.root = root;
    options.scripts = &library;
    options.now = [] { return int64_t(1794765600000LL); };
    Installer installer(options);
    const std::wstring scripts = root + L"\\scripts";
    const std::wstring configs = root + L"\\configs";
    const auto holdOpen = [](const std::wstring& path) {
        // Readers allowed, no FILE_SHARE_DELETE: moving the new sidecar over it fails.
        return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                nullptr);
    };

    Check(installer.Install(ScriptPackage(kScriptId, "Auto GG", 3, kScriptBytes)).status == InstallStatus::Done,
            "a first install writes the script sidecar");
    WriteText(scripts + L"\\Mine.luau", "print('mine')\n");
    HANDLE sidecar = holdOpen(installer.SidecarPath(Kind::Script));
    Check(sidecar != INVALID_HANDLE_VALUE, "the script sidecar is held open");
    InstallOutcome outcome =
            installer.Install(ScriptPackage("abcdefghjkmn", "Mine", 1, "print('public')\n"), OnConflict::Replace);
    Check(outcome.status == InstallStatus::Failed && ReadText(scripts + L"\\Mine.luau") == "print('mine')\n"
                    && !installer.Find(Kind::Script, "abcdefghjkmn", nullptr),
            "a failed save after Replace puts the player's script back");
    Check(ReadText(overlay::ScriptBackupPath(scripts, "Mine")) == "print('public')\n",
            "the backup then holds the PUBLIC file, not the player's script");
    outcome = installer.Install(ScriptPackage("pqrstvwxyz01", "Fresh", 1, "print('fresh')\n"));
    Check(outcome.status == InstallStatus::Failed && !nickel::pub::detail::FileExists(scripts + L"\\Fresh.luau"),
            "a failed save after a fresh install removes the new script");
    if (sidecar != INVALID_HANDLE_VALUE) {
        CloseHandle(sidecar);
    }
    Check(installer.Records(Kind::Script).size() == 1, "failed installs leave no record");

    Package config;
    config.kind = Kind::Config;
    config.id = kConfigId;
    config.versionNumber = 1;
    config.label = "1.0.0";
    config.name = "Bedwars Pack";
    config.sha256 = Sha(kConfigBytes);
    config.bytes = kConfigBytes;
    Check(installer.Install(config).status == InstallStatus::Done, "a first config writes the config sidecar");
    WriteText(configs + L"\\Mine.ini", "[menu]\r\n");
    Package other = config;
    other.id = "abcdefghjkmn";
    other.name = "Mine";
    sidecar = holdOpen(installer.SidecarPath(Kind::Config));
    Check(sidecar != INVALID_HANDLE_VALUE, "the config sidecar is held open");
    Check(installer.Install(other, OnConflict::Replace).status == InstallStatus::Failed
                    && ReadText(configs + L"\\Mine.ini") == "[menu]\r\n"
                    && CountFiles(configs + L"\\.public\\backup\\Mine-*.ini") == 0,
            "a failed save after Replace moves the player's config back in place");
    if (sidecar != INVALID_HANDLE_VALUE) {
        CloseHandle(sidecar);
    }

    // The file of a PUBLIC record is gone, but its record still owns the name.
    DeleteFileW((scripts + L"\\Auto GG.luau").c_str());
    outcome = installer.Install(ScriptPackage("23456789abcd", "Auto GG", 1, "print('second')\n"), OnConflict::KeepBoth);
    Check(outcome.status == InstallStatus::Done && outcome.name == "Auto GG (2)"
                    && ReadText(scripts + L"\\Auto GG (2).luau") == "print('second')\n",
            "Keep both skips a name another PUBLIC record owns");
}

void SidecarChecks() {
    const std::string text = "{\"format\":1,\"items\":["
            "{\"id\":\"7k2m9x4qa1dz\",\"version_number\":2,\"sha256\":\"" + Sha("a") + "\",\"file\":\"A.luau\","
            "\"installed_at\":\"2026-11-15T12:00:00Z\",\"skipped_version\":null},"
            "{\"id\":\"7k2m9x4qa1dz\",\"version_number\":3,\"sha256\":\"" + Sha("a") + "\",\"file\":\"B.luau\","
            "\"installed_at\":\"2026-11-15T12:00:00Z\"},"
            "{\"id\":\"abcdefghjkmn\",\"version_number\":1,\"sha256\":\"" + Sha("a") + "\",\"file\":\"..\\\\x.luau\","
            "\"installed_at\":\"2026-11-15T12:00:00Z\"},"
            "{\"id\":\"pqrstvwxyz01\",\"version_number\":1,\"sha256\":\"" + Sha("a") + "\",\"file\":\"C.ini\","
            "\"installed_at\":\"2026-11-15T12:00:00Z\"},"
            "{\"id\":\"23456789abcd\",\"version_number\":0,\"sha256\":\"" + Sha("a") + "\",\"file\":\"D.luau\","
            "\"installed_at\":\"2026-11-15T12:00:00Z\"}]}";
    const std::vector<InstallRecord> records = ParseSidecar(text, ".luau");
    Check(records.size() == 1 && records[0].file == "A.luau" && records[0].versionNumber == 2,
            "sidecar entries: repeated ids, paths, other extensions and bad numbers dropped");
    Check(ParseSidecar("not json", ".luau").empty(), "an unreadable sidecar has no records");
    Check(ParseSidecar(FormatSidecar(records), ".luau").size() == 1, "a sidecar reads back");

    Check(nickel::pub::detail::CleanStem("  a/b:c  ", "7k2m9x4qa1dz") == "abc", "file names lose refused characters");
    Check(nickel::pub::detail::CleanStem("CON", "7k2m9x4qa1dz") == "public-7k2m9x4qa1dz", "device names fall back");
    Check(nickel::pub::detail::CleanStem("...", "7k2m9x4qa1dz") == "public-7k2m9x4qa1dz", "empty names fall back");

    InstallRecord record;
    record.versionNumber = 3;
    LookupEntry entry;
    entry.found = true;
    entry.card.latest.number = 3;
    Check(DecideLocal(record, &entry, false, false) == Local::Installed, "same version: installed");
    Check(DecideLocal(record, nullptr, true, false) == Local::Revoked, "revoked first");
    entry.found = false;
    entry.missing.state = MissingState::Hidden;
    Check(DecideLocal(record, &entry, false, true) == Local::Unavailable, "hidden: unavailable");
    entry.missing.state = MissingState::NotFound;
    Check(DecideLocal(record, &entry, false, false) == Local::NotListed, "not found: not listed");
}

// ---------------------------------------------------------------------------------------------------------------
// Install identity, starred and published lists (temporary folder)

std::string FileText(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void IdentityChecks(const std::wstring& root) {
    const std::wstring path = InstallKeyPath(root);
    Check(path == root + L"\\public\\install.key", "the key lives in <root>\\public\\install.key");
    {
        InstallIdentity identity(path);
        std::string fingerprint;
        Check(identity.State() == IdentityState::Missing && !identity.Fingerprint(&fingerprint) && !Exists(path),
              "no key before the first write, and reading it creates none");
        std::string header;
        Check(identity.Authorization(&header) && Exists(path), "the first write creates the key");
        Check(header.size() == 8 + 43 && header.compare(0, 8, "Install ") == 0
                      && header.find_first_of("+/= ", 8) == std::string::npos,
              "Authorization: Install <43 characters of base64url>");
        std::string secret;
        Check(detail::FromBase64Url(header.substr(8), &secret) && secret.size() == 32, "a 32-byte secret");
        const std::string text = FileText(path);
        Check(text == header.substr(8) + "\n", "the file holds the secret as base64url text");
        Check(identity.Fingerprint(&fingerprint) && fingerprint == net::Sha256Hex(secret) && fingerprint.size() == 64
                      && fingerprint.find(header.substr(8)) == std::string::npos,
              "the fingerprint is SHA-256 of the secret, not the secret");
        std::string again;
        Check(identity.Authorization(&again) && again == header, "the same key on every write");

        InstallIdentity reloaded(path);
        std::string other;
        Check(reloaded.State() == IdentityState::Ready && reloaded.Authorization(&other) && other == header
                      && FileText(path) == text,
              "the key persists: a new identity on the same file reads it, never replaces it");

        InstallIdentity second(root + L"\\second\\install.key");
        std::string secondHeader;
        Check(second.Authorization(&secondHeader) && secondHeader != header, "another install gets another secret");
    }
    {
        const std::wstring damaged = root + L"\\damaged.key";
        WriteText(damaged, "not a key\n");
        InstallIdentity identity(damaged);
        std::string header;
        Check(identity.State() == IdentityState::Damaged && !identity.Authorization(&header) && header.empty(),
              "a damaged key refuses writes");
        Check(FileText(damaged) == "not a key\n", "a damaged key is never overwritten");
        WriteText(damaged, std::string(43, '+'));
        InstallIdentity plusSigns(damaged);
        Check(plusSigns.State() == IdentityState::Damaged, "only the url-safe alphabet is a key");
    }
}

void LocalListChecks(const std::wstring& root) {
    const std::wstring starredPath = StarredPath(root);
    {
        StarredList starred(starredPath);
        Check(starred.Ids(Kind::Script).empty() && !starred.Has(Kind::Script, kScriptId), "no stars at first");
        Check(starred.Set(Kind::Script, kScriptId, true) && starred.Set(Kind::Script, kGoneId, true)
                      && starred.Set(Kind::Config, kConfigId, true),
              "stars are saved");
        Check(!starred.Set(Kind::Script, "NOT-AN-ID", true), "an invalid id is refused");
        Check(starred.Ids(Kind::Script).size() == 2 && starred.Ids(Kind::Script)[0] == kGoneId
                      && starred.Has(Kind::Config, kConfigId) && !starred.Has(Kind::Script, kConfigId),
              "ids per kind, newest first");
        starred.Set(Kind::Script, kScriptId, true);
        Check(starred.Ids(Kind::Script).size() == 2 && starred.Ids(Kind::Script)[0] == kScriptId,
              "starring again moves it first, once");
        starred.Set(Kind::Script, kGoneId, false);
    }
    {
        StarredList reloaded(starredPath);
        Check(reloaded.Ids(Kind::Script).size() == 1 && reloaded.Has(Kind::Script, kScriptId)
                      && reloaded.Has(Kind::Config, kConfigId) && !reloaded.Has(Kind::Script, kGoneId),
              "the starred list persists in starred.json");
    }
    WriteText(starredPath, "{\"format\":1,\"script\":[\"7k2m9x4qa1dz\",\"BAD\",\"7k2m9x4qa1dz\"],\"config\":5}");
    StarredList messy(starredPath);
    Check(messy.Ids(Kind::Script).size() == 1 && messy.Ids(Kind::Config).empty(),
          "invalid and repeated entries are dropped");

    const std::wstring publishedPath = PublishedPath(root);
    const std::string owner = Sha("owner");
    const std::string stranger = Sha("stranger");
    {
        PublishedList published(publishedPath);
        PublishedRecord record;
        record.kind = Kind::Script;
        record.name = "Auto GG";
        record.id = kScriptId;
        record.owner = owner;
        record.version = 1;
        record.publishedAt = "2026-11-15T18:00:00.000Z";
        Check(published.Remember(record), "a published item is remembered");
        Check(published.Find(Kind::Script, "auto gg", owner) != nullptr, "found by name (case ignored) and owner");
        Check(published.Find(Kind::Script, "Auto GG", stranger) == nullptr
                      && published.Find(Kind::Script, "Auto GG", "") == nullptr,
              "another install does not own it");
        Check(published.Find(Kind::Config, "Auto GG", owner) == nullptr, "a config of the same name is another item");
        record.version = 2;
        published.Remember(record);
        Check(published.Records().size() == 1 && published.Find(Kind::Script, "Auto GG", owner)->version == 2,
              "remembering again replaces the entry");
        PublishedRecord bad = record;
        bad.owner = "secret";
        Check(!published.Remember(bad), "an owner that is not a fingerprint is refused");
    }
    PublishedList reloaded(publishedPath);
    Check(reloaded.Find(Kind::Script, "Auto GG", owner) != nullptr
                  && reloaded.Find(Kind::Script, "Auto GG", owner)->id == kScriptId,
          "published.json persists");
}

// ---------------------------------------------------------------------------------------------------------------
// Writes with the install key, against the local server

template <typename Done>
bool WaitSlot(const PublicClient& client, Done done) {
    return WaitFor(client, [&](const Snapshot& s) { return done(s); });
}

void StarChecks(TestServer& server, const std::wstring& root) {
    {
        PublicClient client(LoopbackConfig(server));
        const int before = server.Hits("PUT");
        Check(client.Star(kScriptId, true), "a star without an identity is posted");
        Check(WaitSlot(client, [](const Snapshot& s) { return s.star && !s.star->loading; }), "the star answer");
        Check(client.Snap().star->outcome.status == Status::NoIdentity && server.Hits("PUT") == before,
              "without an install key: no request");
    }
    InstallIdentity identity(InstallKeyPath(root));
    PublicClient client(LoopbackConfig(server), &identity);
    Check(client.Star(kScriptId, true), "a star");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.star && !s.star->loading; }), "the star answer");
    Check(client.Snap().star->outcome.Ok() && client.Snap().star->starred, "starred");
    std::string header;
    identity.Authorization(&header);
    Check(server.LastHeaders("PUT /api/community/v1/items/7k2m9x4qa1dz/star").find("Authorization: " + header)
                  != std::string::npos,
          "the star carries the install key");

    Check(client.Star(kScriptId, false), "an unstar");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.star && !s.star->loading && !s.star->starred; }),
          "the unstar answer");
    Check(client.Snap().star->outcome.Ok() && server.Hits("DELETE /api/community/v1/items/7k2m9x4qa1dz/star") == 1,
          "unstar is DELETE");

    Check(client.Star(kConfigId, true), "starring one's own item");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.star && !s.star->loading && s.star->id == kConfigId; }),
          "the conflict answer");
    Check(client.Snap().star->outcome.status == Status::Conflict, "409 for one's own item");

    // Reads never send the key.
    Check(client.Detail(kScriptId), "a read");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.detail && !s.detail->loading; }), "the read answer");
    Check(server.LastHeaders("GET /api/community/v1/items/7k2m9x4qa1dz").find("Authorization") == std::string::npos,
          "reads carry no key");
    Check(client.Lookup({kScriptId}), "a lookup");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.lookup && !s.lookup->loading; }), "the lookup answer");
    Check(server.LastHeaders("POST /api/community/v1/items/lookup").find("Authorization") == std::string::npos,
          "lookups carry no key");

    // The Starred tabs' lookup has its own slot.
    Check(client.LookupStarred({kScriptId, kGoneId}), "the starred lookup");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.starred && !s.starred->loading; }), "its answer");
    const Snapshot snapshot = client.Snap();
    Check(snapshot.starred->outcome.Ok() && snapshot.starred->entries.size() == 2 && snapshot.starred->entries[0].found
                  && !snapshot.starred->entries[1].found && snapshot.lookup->ids.size() == 1,
          "starred cards come in their own slot");

    // A blocked install: 403.
    server.blockedInstall = header;
    Check(client.Star(kScriptId, true), "a star of a blocked install");
    Check(WaitSlot(client,
                   [](const Snapshot& s) {
                       return s.star && !s.star->loading && s.star->id == kScriptId && s.star->starred;
                   }),
          "the 403 answer");
    Check(client.Snap().star->outcome.status == Status::Forbidden, "a blocked install is Forbidden");
    server.blockedInstall.clear();
}

void PublishChecks(TestServer& server, const std::wstring& root) {
    InstallIdentity identity(InstallKeyPath(root + L"\\author"));
    PublicClient client(LoopbackConfig(server), &identity);
    const std::string script = "-- Auto GG\nlocal mod = nickel.module(\"Auto GG\")\nprint('gg')\n";

    // The check: a dry run, with the file in base64.
    Check(client.Check(Kind::Script, "Auto GG.luau", script, "Auto GG"), "a check");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.check && !s.check->loading; }), "the check answer");
    Snapshot snapshot = client.Snap();
    Check(snapshot.check->outcome.Ok() && snapshot.check->check.ok && snapshot.check->check.sha256 == Sha(script)
                  && snapshot.check->sentSha256 == Sha(script) && snapshot.check->ref == "Auto GG",
          "the check passed for these bytes");
    Check(snapshot.check->check.capabilities.size() == 2 && snapshot.check->check.warnings.size() == 1
                  && snapshot.check->check.warnings[0].line == 3,
          "capabilities and warnings of the check");
    json::Value body;
    Check(json::Parse(server.LastBody("POST /api/community/v1/items/check"), &body)
                  && body["kind"].AsString() == "script" && body["file"]["name"].AsString() == "Auto GG.luau"
                  && body["file"]["encoding"].AsString() == "base64",
          "the check body");
    Check(server.LastHeaders("POST /api/community/v1/items/check").find("Authorization: Install ") != std::string::npos,
          "the check carries the install key");

    Check(client.Check(Kind::Script, "Bad.luau", "local x = require(\"y\")\n"), "a check of a refused file");
    Check(WaitSlot(client,
                   [](const Snapshot& s) { return s.check && !s.check->loading && s.check->fileName == "Bad.luau"; }),
          "its answer");
    snapshot = client.Snap();
    Check(snapshot.check->outcome.Ok() && !snapshot.check->check.ok && snapshot.check->check.errors.size() == 1
                  && snapshot.check->check.errors[0].code == "require",
          "a refused file: not ok, with its error and line");
    Check(!client.Check(Kind::Script, "Nope.ini", script), "a script must be a .luau file");
    Check(!client.Check(Kind::Config, "big.ini", std::string(kMaxFileBytes + 1, 'x')), "over 200 KB is refused");

    // Publish: a new item, final.
    PublishRequest request;
    request.kind = Kind::Script;
    request.name = "Auto GG";
    request.summary = "Says gg after a win.";
    request.description = "Line one.\nLine two.";
    request.tags = {"pvp", "chat"};
    request.authorName = "Mash";
    request.fileName = "Auto GG.luau";
    request.bytes = script;
    request.ref = "Auto GG";
    Check(client.Publish(request), "a publish");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.publish && !s.publish->loading; }), "the publish answer");
    snapshot = client.Snap();
    const std::string published = snapshot.publish->itemId;
    Check(snapshot.publish->outcome.Ok() && ValidId(published) && snapshot.publish->versionNumber == 1
                  && snapshot.publish->state == "published" && !snapshot.publish->update
                  && snapshot.publish->ref == "Auto GG",
          "201: the new item's id, version 1, published");
    std::string decoded;
    Check(json::Parse(server.LastBody("POST /api/community/v1/items"), &body) && body["kind"].AsString() == "script"
                  && body["name"].AsString() == "Auto GG" && body["summary"].AsString() == "Says gg after a win."
                  && body["description"].AsString() == "Line one.\nLine two." && body["tags"].size() == 2
                  && body["author_name"].AsString() == "Mash"
                  && net::Base64Decode(body["file"]["content"].AsString(), &decoded) && decoded == script,
          "the publish body: fields, author name and the exact bytes");
    Check(server.LastHeaders("POST /api/community/v1/items").find("Authorization: Install ") != std::string::npos,
          "the publish carries the install key");

    PublishRequest refused = request;
    refused.authorName.clear();
    const int posts = server.Hits("POST /api/community/v1/items");
    Check(!client.Publish(refused) && client.Snap().publish->outcome.status == Status::Invalid
                  && client.Snap().publish->outcome.message == "author name",
          "an author name is required");
    refused = request;
    refused.tags = {"Not A Tag"};
    Check(!client.Publish(refused), "invalid tags are refused");
    refused = request;
    refused.name = std::string(61, 'n');
    Check(!client.Publish(refused) && server.Hits("POST /api/community/v1/items") == posts,
          "a name over 60 is refused before any request");

    PublishRequest obfuscated = request;
    obfuscated.bytes = "local f = getfenv(1)\nprint(f)\n";
    obfuscated.ref = "Obfuscated";
    Check(client.Publish(obfuscated), "a publish the server refuses");
    Check(WaitSlot(
                  client,
                  [](const Snapshot& s) { return s.publish && !s.publish->loading && s.publish->ref == "Obfuscated"; }),
          "the 422 answer");
    snapshot = client.Snap();
    Check(snapshot.publish->outcome.status == Status::Rejected && snapshot.publish->outcome.problem.errors.size() == 1
                  && snapshot.publish->outcome.problem.errors[0].code == "obfuscated"
                  && snapshot.publish->outcome.problem.errors[0].line == 2,
          "422: rejected, with the refused line");

    // Publish update: only the owner install.
    PublishRequest update;
    update.kind = Kind::Script;
    update.fileName = "Auto GG.luau";
    update.bytes = script + "-- v2\n";
    update.changelog = "Waits a random delay.";
    update.ref = "Auto GG";
    Check(!client.PublishVersion(published, PublishRequest()), "a version needs its file and changelog");
    Check(client.PublishVersion(published, update), "a publish update");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.publish && !s.publish->loading && s.publish->update; }),
          "the update answer");
    snapshot = client.Snap();
    Check(snapshot.publish->outcome.Ok() && snapshot.publish->itemId == published
                  && snapshot.publish->versionNumber == 2,
          "the owner's update: version 2 of the same item");
    Check(json::Parse(server.LastBody("POST /api/community/v1/items/" + published + "/versions"), &body)
                  && body["changelog"].AsString() == "Waits a random delay." && body.Find("name") == nullptr
                  && net::Base64Decode(body["file"]["content"].AsString(), &decoded) && decoded == update.bytes,
          "the update body: changelog and file only");

    InstallIdentity strangerIdentity(InstallKeyPath(root + L"\\stranger"));
    PublicClient stranger(LoopbackConfig(server), &strangerIdentity);
    Check(stranger.PublishVersion(published, update), "an update from another install");
    Check(WaitSlot(stranger, [](const Snapshot& s) { return s.publish && !s.publish->loading; }), "its answer");
    Check(stranger.Snap().publish->outcome.status == Status::Forbidden, "another install cannot publish an update");

    // Reports: anonymous, with the install key only for rate limits.
    Check(client.Report(kScriptId, "malicious", "Sends my chat to a webhook."), "a report");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.report && !s.report->loading; }), "the report answer");
    Check(client.Snap().report->outcome.Ok(), "202: reported");
    Check(json::Parse(server.LastBody("POST /api/community/v1/reports"), &body)
                  && body["item_id"].AsString() == kScriptId && body["category"].AsString() == "malicious"
                  && body["details"].AsString() == "Sends my chat to a webhook.",
          "the report body");
    Check(!client.Report(kScriptId, "boring", ""), "an unknown category is refused");
    Check(client.Report(kGoneId, "spam", ""), "a second report");
    Check(WaitSlot(client, [](const Snapshot& s) { return s.report && !s.report->loading && s.report->id == kGoneId; }),
          "its answer");
    Check(client.Snap().report->outcome.status == Status::Conflict, "409: already reported");
}

// Publishing is final: the client has no request that deletes, unlists or hides an item.
void NoDeleteChecks(TestServer& server) {
    bool onlyStars = true;
    for (const std::string& seen : server.Seen()) {
        if (seen.compare(0, 7, "DELETE ") == 0) {
            onlyStars = onlyStars && seen.size() > 5 && seen.compare(seen.size() - 5, 5, "/star") == 0;
        }
        onlyStars = onlyStars && seen.find("unlist") == std::string::npos && seen.find("PATCH") != 0;
    }
    Check(onlyStars, "the only DELETE requests are unstars; nothing unlists or edits an item");
    std::ifstream file(std::string(NICKEL_SOURCE_DIR) + "/native/public_client.hpp", std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    const std::string source = text.str();
    size_t deletes = 0;
    for (size_t at = source.find("\"DELETE\""); at != std::string::npos; at = source.find("\"DELETE\"", at + 1)) {
        ++deletes;
    }
    Check(!source.empty() && deletes == 1 && source.find("\"PATCH\"") == std::string::npos
                  && source.find("/unlist") == std::string::npos && source.find("/me/") == std::string::npos,
          "public_client.hpp: DELETE only for stars, no PATCH, no unlist, no account endpoint");
}

}  // namespace

int main() {
    ModelChecks();
    PolicyChecks();
    SidecarChecks();
    TestServer server;
    if (!server.Start()) {
        std::printf("FAIL: the local test server did not start\n");
        return 1;
    }
    ListChecks(server);
    CursorChecks(server);
    DetailAndFileChecks(server);
    LookupAndRevocationChecks(server);
    const std::wstring writesRoot = TemporaryRoot();
    StarChecks(server, writesRoot);
    PublishChecks(server, writesRoot);
    NoDeleteChecks(server);
    RateLimitChecks(server);
    server.Stop();
    DeleteTree(writesRoot);

    const std::wstring localRoot = TemporaryRoot();
    IdentityChecks(localRoot);
    LocalListChecks(localRoot);
    DeleteTree(localRoot);

    const std::wstring root = TemporaryRoot();
    InstallerScriptChecks(root);
    InstallerConfigChecks(root);
    DeleteTree(root);
    const std::wstring rollbackRoot = TemporaryRoot();
    InstallerRollbackChecks(rollbackRoot);
    DeleteTree(rollbackRoot);

    if (failed == 0) {
        std::printf("public_client_checks: all passed\n");
        return 0;
    }
    std::printf("public_client_checks: %d failed\n", failed);
    return 1;
}
