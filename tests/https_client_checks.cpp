// The shared HTTPS layer (native/https_client.hpp, native/sse_parser.hpp) without the game and without the internet:
// - the URL and host policy, redirect targets and header checks (pure);
// - SHA-256, base64 and Content-Digest (RFC 9530 vectors);
// - the Server-Sent Events parser, fed whole, byte by byte and in odd chunks;
// - real WinHTTP requests to a small HTTP server this test starts on 127.0.0.1 (a free port): bodies, headers,
//   size caps, redirects, timeouts, cancellation, the worker queue, and streaming.
// Nothing leaves the machine: the policy names no real host the tests connect to, and refused URLs fail before any
// connection is made.
// winsock2.h before windows.h (included by https_client.hpp), or windows.h pulls in the old winsock.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "https_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace nickel::net;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

using Clock = std::chrono::steady_clock;

long long MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// ---------------------------------------------------------------------------------------------------------------
// URL and host policy

bool Parses(const char* text) {
    Url url;
    return ParseUrl(text, &url);
}

void UrlParsing() {
    Url url;
    Check(ParseUrl("https://NickelClient.com/api/v1/items?sort=top#reviews", &url) && url.scheme == "https"
                    && url.host == "nickelclient.com" && url.port == 443 && url.target == "/api/v1/items?sort=top",
            "an https URL: host in lower case, port 443, fragment dropped");
    Check(ParseUrl("https://nickelclient.com", &url) && url.target == "/", "no path means /");
    Check(ParseUrl("https://nickelclient.com?q=1", &url) && url.target == "/?q=1", "a query without a path");
    Check(ParseUrl("http://127.0.0.1:8080/x", &url) && url.scheme == "http" && url.port == 8080, "an explicit port");

    Check(!Parses("ftp://nickelclient.com/"), "another scheme is refused");
    Check(!Parses("//nickelclient.com/"), "a scheme-relative URL is refused");
    Check(!Parses("https://"), "no host is refused");
    Check(!Parses("https://user:pass@nickelclient.com/"), "a user name is refused");
    Check(!Parses("https://evil.com\\@nickelclient.com/"), "a backslash is refused");
    Check(!Parses("https://nickelclient.com:0/") && !Parses("https://nickelclient.com:65536/")
                    && !Parses("https://nickelclient.com:44a/"),
            "a bad port is refused");
    Check(!Parses("https://[::1]/"), "an IPv6 literal is refused");
    Check(!Parses("https://nickel client.com/") && !Parses("https://nickelclient.com/a b"), "spaces are refused");
    Check(!Parses("https://nickelclient.com/\xC3\xA9"), "non-ASCII characters are refused (percent-encode them)");
    Check(!Parses("https://a..b/") && !Parses("https://.com/") && !Parses("https://nickelclient.com./"),
            "empty host labels are refused");
    Check(!Parses("https://nickel_client.com/") && !Parses("https://nickel%2eclient.com/"),
            "other characters in the host are refused");
    Check(!Parses(("https://nickelclient.com/" + std::string(3000, 'a')).c_str()), "a very long URL is refused");
}

void HostAllowlist() {
    HostPolicy policy;
    policy.httpsHosts = {"nickelclient.com"};
    Url url;
    const auto verdict = [&](const char* text) { return CheckUrl(policy, text, &url); };
    Check(verdict("https://nickelclient.com/api/v1/items") == Error::None, "the named host over https is allowed");
    Check(verdict("https://NICKELCLIENT.COM/") == Error::None, "host names compare without case");
    Check(verdict("https://nickelclient.com:8443/") == Error::HostNotAllowed, "another port is refused");
    Check(verdict("https://api.nickelclient.com/") == Error::HostNotAllowed, "a subdomain is not the host");
    Check(verdict("https://nickelclient.com.evil.com/") == Error::HostNotAllowed, "a longer name is not the host");
    Check(verdict("https://evil.com/") == Error::HostNotAllowed, "another host is refused");
    Check(verdict("http://nickelclient.com/") == Error::HostNotAllowed, "plain http to the host is refused");
    Check(verdict("http://127.0.0.1:8080/") == Error::HostNotAllowed, "loopback http is refused unless allowed");
    Check(verdict("not a url") == Error::BadUrl, "text that is not a URL");
    policy.allowLoopbackHttp = true;
    Check(verdict("http://127.0.0.1:8080/") == Error::None, "loopback http when the policy allows it");
    Check(verdict("http://localhost:8080/") == Error::HostNotAllowed, "only 127.0.0.1, not a name for it");
    Check(verdict("http://127.0.0.2:8080/") == Error::HostNotAllowed, "only 127.0.0.1, not the loopback range");
    Check(verdict("https://127.0.0.1/") == Error::HostNotAllowed, "https to 127.0.0.1 needs the allowlist");
}

void RedirectTargets() {
    Url base;
    ParseUrl("https://nickelclient.com/api/v1/items/1/download", &base);
    Url next;
    Check(ResolveSameOriginRedirect(base, "/files/a.luau", &next) && next.host == "nickelclient.com"
                    && next.target == "/files/a.luau",
            "an absolute path stays on the same origin");
    Check(ResolveSameOriginRedirect(base, "https://nickelclient.com/files/b", &next), "the same origin, written out");
    Check(!ResolveSameOriginRedirect(base, "https://evil.com/files/a.luau", &next), "another host is refused");
    Check(!ResolveSameOriginRedirect(base, "//evil.com/x", &next), "a scheme-relative target is refused");
    Check(!ResolveSameOriginRedirect(base, "http://nickelclient.com/x", &next), "a scheme change is refused");
    Check(!ResolveSameOriginRedirect(base, "https://nickelclient.com:444/x", &next), "a port change is refused");
    Check(!ResolveSameOriginRedirect(base, "files/a", &next), "a relative path is refused");
}

void HeaderChecks() {
    Check(ValidHeader("Authorization", "Bearer abc.def"), "an ordinary header");
    Check(ValidHeader("If-None-Match", "\"e3b0\""), "quotes in a value");
    Check(!ValidHeader("X-Test", "a\r\nInjected: 1"), "CR LF in a value is refused");
    Check(!ValidHeader("X-Test", std::string("a\0b", 3)), "NUL in a value is refused");
    Check(!ValidHeader("Bad Name", "x") && !ValidHeader("", "x") && !ValidHeader("A:B", "x"), "bad header names");
    Check(!ValidHeader("X-Name", "caf\xC3\xA9"), "non-ASCII values are refused");
}

// ---------------------------------------------------------------------------------------------------------------
// SHA-256 and Content-Digest

void Digests() {
    Check(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    Check(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");

    const char* const plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* const encoded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    bool vectors = true;
    for (int i = 0; i < 7; ++i) {
        const std::string text = plain[i];
        std::string decoded;
        vectors &= Base64Encode(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == encoded[i];
        vectors &= Base64Decode(encoded[i], &decoded) && decoded == text;
    }
    Check(vectors, "base64 test vectors of RFC 4648, both ways");
    std::string decoded;
    Check(!Base64Decode("Zm9", &decoded) && !Base64Decode("Zm=v", &decoded) && !Base64Decode("Z===", &decoded)
                    && !Base64Decode("Zm9v!A==", &decoded),
            "malformed base64 is refused");

    // RFC 9530 section 2, example body {"hello": "world"}.
    const std::string body = "{\"hello\": \"world\"}";
    Check(ContentDigestFor(body) == "sha-256=:X48E9qOokqqrvdts8nOJRJN3OWDUoyWxBf7kbu9DBPE=:",
            "Content-Digest of the RFC 9530 example");
    Check(ContentDigestMatches("sha-256=:X48E9qOokqqrvdts8nOJRJN3OWDUoyWxBf7kbu9DBPE=:", body), "a matching digest");
    Check(ContentDigestMatches("sha-512=:AAAA:, sha-256=:X48E9qOokqqrvdts8nOJRJN3OWDUoyWxBf7kbu9DBPE=:;p=1", body),
            "sha-256 found among other algorithms, with a parameter");
    Check(!ContentDigestMatches("sha-256=:X48E9qOokqqrvdts8nOJRJN3OWDUoyWxBf7kbu9DBPE=:", body + " "),
            "a changed body does not match");
    Check(!ContentDigestMatches("sha-512=:AAAA:", body), "no sha-256 entry does not match");
    Check(!ContentDigestMatches("sha-256=X48E9qOokqqrvdts8nOJRJN3OWDUoyWxBf7kbu9DBPE=", body),
            "a value that is not a byte sequence does not match");
    Check(!ContentDigestMatches("sha-256=:AAAA:", body), "a digest of the wrong length does not match");
}

// ---------------------------------------------------------------------------------------------------------------
// Server-Sent Events parser

std::vector<SseEvent> FeedAll(const std::vector<std::string>& chunks, size_t limit = 1024 * 1024) {
    SseParser parser(limit);
    std::vector<SseEvent> events;
    for (const std::string& chunk : chunks) {
        parser.Feed(chunk, [&](const SseEvent& event) {
            events.push_back(event);
            return true;
        });
    }
    return events;
}

std::vector<SseEvent> FeedBytes(const std::string& stream) {
    std::vector<std::string> chunks;
    for (const char c : stream) {
        chunks.emplace_back(1, c);
    }
    return FeedAll(chunks);
}

bool SameEvents(const std::vector<SseEvent>& a, const std::vector<SseEvent>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].type != b[i].type || a[i].data != b[i].data || a[i].id != b[i].id) {
            return false;
        }
    }
    return true;
}

void SseFormat() {
    auto events = FeedAll({"data: hello\n\n"});
    Check(events.size() == 1 && events[0].type == "message" && events[0].data == "hello" && events[0].id.empty(),
            "one event, type message by default");

    events = FeedAll({"event: delta\ndata: line 1\ndata: line 2\nid: 7\n\n"});
    Check(events.size() == 1 && events[0].type == "delta" && events[0].data == "line 1\nline 2" && events[0].id == "7",
            "event type, data lines joined with LF, id");

    const std::string mixed = "data: a\r\n\r\ndata: b\r\rdata: c\n\n";
    events = FeedAll({mixed});
    Check(events.size() == 3 && events[0].data == "a" && events[1].data == "b" && events[2].data == "c",
            "CRLF, CR and LF line ends");
    Check(SameEvents(FeedBytes(mixed), events), "the same events when fed one byte at a time");
    Check(SameEvents(FeedAll({"data: a\r", "\n\r", "\ndata: b\r", "\rdata: c\n", "\n"}), events),
            "a CRLF split across chunks is one line end");

    events = FeedAll({": keep-alive\n\n:another\ndata: x\n\n"});
    Check(events.size() == 1 && events[0].data == "x", "comments are ignored");
    events = FeedAll({"data\n\n"});
    Check(events.size() == 1 && events[0].data.empty(), "a field name alone has an empty value");
    events = FeedAll({"data:  two spaces\n\n"});
    Check(events.size() == 1 && events[0].data == " two spaces", "only one space after the colon is removed");
    events = FeedAll({"event: ignored\n\ndata: y\n\n"});
    Check(events.size() == 1 && events[0].type == "message", "an event without data is not dispatched and resets type");
    events = FeedAll({"unknown: 1\ndata: z\n\n"});
    Check(events.size() == 1 && events[0].data == "z", "unknown fields are ignored");
    events = FeedAll({"id: 5\ndata: a\n\ndata: b\n\n"});
    Check(events.size() == 2 && events[1].id == "5", "the last event id carries over to later events");
    events = FeedAll({std::string("id: 9\0x\ndata: a\n\n", 17)});
    Check(events.size() == 1 && events[0].id.empty(), "an id containing NUL is ignored");
    events = FeedAll({"data: open"});
    Check(events.empty(), "an event the stream never ended is not dispatched");

    events = FeedAll({"\xEF\xBB", "\xBF" "data: bom\n\n"});
    Check(events.size() == 1 && events[0].data == "bom", "a leading byte order mark is skipped, even when split");
    events = FeedAll({"data: a\n\n\xEF\xBB\xBF" "data: b\n\n"});
    Check(events.size() == 1, "a byte order mark later in the stream is not skipped");

    SseParser retry;
    retry.Feed("retry: 1500\n\nretry: 12a\n\n", [](const SseEvent&) { return true; });
    Check(retry.RetryMs() == 1500, "retry with digits only");

    SseParser stopping;
    int seen = 0;
    const bool more = stopping.Feed("data: 1\n\ndata: 2\n\n", [&](const SseEvent&) {
        ++seen;
        return false;
    });
    Check(!more && seen == 1 && !stopping.Failed(), "the callback can stop the stream");

    SseParser limited(16);
    const bool accepted = limited.Feed("data: " + std::string(40, 'x') + "\n\n", [](const SseEvent&) { return true; });
    Check(!accepted && limited.Failed(), "an event larger than the limit stops the parser");
    SseParser manyLines(16);
    manyLines.Feed("data: 1234567\ndata: 1234567\ndata: 1234567\n\n", [](const SseEvent&) { return true; });
    Check(manyLines.Failed(), "the limit counts every data line of one event");
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
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
                || listen(listener_, 16) != 0
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

    // Waits up to ms, returning early when the server stops.
    void Pause(int ms) const {
        for (int waited = 0; waited < ms && !stopping_; waited += 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    static std::string HeaderValue(const std::string& headers, const std::string& name) {
        const size_t at = headers.find("\r\n" + name + ": ");
        if (at == std::string::npos) {
            return std::string();
        }
        const size_t start = at + name.size() + 4;
        return headers.substr(start, headers.find("\r\n", start) - start);
    }

    void Serve(SOCKET client) {
        Incoming request;
        if (ReadRequest(client, &request)) {
            Route(client, request);
        }
        shutdown(client, SD_SEND);
        closesocket(client);
    }

    void Route(SOCKET client, const Incoming& request) {
        const std::string& path = request.path;
        if (path == "/json") {
            const std::string body = "{\"ok\":true,\"stars\":42}";
            Send(client,
                    Reply("200 OK",
                            "Content-Type: application/json\r\nContent-Digest: " + ContentDigestFor(body)
                                    + "\r\nETag: \"" + Sha256Hex(body) + "\"\r\n",
                            body));
        } else if (path == "/echo") {
            const std::string body = request.method + " " + HeaderValue(request.headers, "X-Test") + " " + request.body;
            Send(client, Reply("200 OK", "Content-Type: text/plain\r\n", body));
        } else if (path == "/agent") {
            Send(client, Reply("200 OK", "", HeaderValue(request.headers, "User-Agent")));
        } else if (path == "/big") {
            Send(client, Reply("200 OK", "", std::string(300000, 'x')));
        } else if (path == "/unsized") {
            Send(client, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + std::string(5000, 'y'));
        } else if (path == "/redirect-same") {
            Send(client, Reply("302 Found", "Location: /json\r\n", ""));
        } else if (path == "/redirect-loop") {
            Send(client, Reply("302 Found", "Location: /redirect-loop\r\n", ""));
        } else if (path == "/redirect-other") {
            Send(client, Reply("302 Found", "Location: http://example.com/json\r\n", ""));
        } else if (path == "/see-other") {
            Send(client, Reply("303 See Other", "Location: /echo\r\n", ""));
        } else if (path == "/slower") {
            Send(client, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 4\r\n\r\n");
            Pause(15000);
            Send(client, "late");
        } else if (path == "/slow") {
            Send(client, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 4\r\n\r\n");
            Pause(5000);
            Send(client, "late");
        } else if (path == "/events") {
            Send(client, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/event-stream\r\n\r\n");
            const char* const parts[] = {": keep-alive\r\n\r\n",
                    "event: delta\r\ndata: Hel",
                    "lo\r\n\r",
                    "\nevent: delta\ndata: line 1\ndata: line 2\nid: 7\n\n",
                    "retry: 1500\n\n",
                    "event: done\ndata: {\"stop\":\"end_turn\"}\n\n"};
            for (const char* part : parts) {
                Send(client, part);
                Pause(30);
            }
        } else if (path == "/events-then-wait") {
            Send(client,
                    "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/event-stream\r\n\r\n"
                    "data: first\n\ndata: second\n\n");
            Pause(5000);
        } else if (path == "/events-big") {
            Send(client, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/event-stream\r\n\r\n");
            for (int i = 0; i < 20; ++i) {
                Send(client, "data: " + std::string(500, 'z') + "\n\n");
            }
        } else if (path == "/problem") {
            Send(client,
                    Reply("404 Not Found",
                            "Content-Type: application/problem+json\r\n",
                            "{\"code\":\"not_found\",\"title\":\"Not found\"}"));
        } else {
            Send(client, Reply("404 Not Found", "", ""));
        }
    }

    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::thread> handlers_;
};

ClientOptions LoopbackOptions() {
    ClientOptions options;
    options.userAgent = L"Nickel-Overlay/test";
    options.policy.httpsHosts = {"nickelclient.com"};
    options.policy.allowLoopbackHttp = true;
    return options;
}

Request Get(const std::string& url) {
    Request request;
    request.url = url;
    request.timeoutMs = 5000;
    return request;
}

void Requests(const TestServer& server, HttpsClient& client) {
    Response response = client.Send(Get(server.UrlFor("/json")));
    Check(response.Ok() && response.status == 200 && response.body == "{\"ok\":true,\"stars\":42}",
            "a GET and its body");
    const std::string* type = response.FindHeader("content-type");
    Check(type != nullptr && *type == "application/json", "response headers, looked up without case");
    const std::string* digest = response.FindHeader("Content-Digest");
    Check(digest != nullptr && ContentDigestMatches(*digest, response.body), "the body matches its Content-Digest");
    const std::string* etag = response.FindHeader("ETag");
    Check(etag != nullptr && *etag == "\"" + Sha256Hex(response.body) + "\"", "the body matches its sha256 ETag");

    Request post = Get(server.UrlFor("/echo"));
    post.method = "POST";
    post.body = "{\"a\":1}";
    post.headers = {{"X-Test", "yes"}, {"Content-Type", "application/json"}};
    response = client.Send(post);
    Check(response.Ok() && response.body == "POST yes {\"a\":1}", "a POST with a body and a header");

    response = client.Send(Get(server.UrlFor("/agent")));
    Check(response.Ok() && response.body == "Nickel-Overlay/test", "the user agent of the client options");

    Request injected = Get(server.UrlFor("/echo"));
    injected.headers = {{"X-Test", "a\r\nX-Other: b"}};
    Check(client.Send(injected).error == Error::BadRequest, "a header with CR LF is refused before sending");
    Request badMethod = Get(server.UrlFor("/echo"));
    badMethod.method = "GET /x";
    Check(client.Send(badMethod).error == Error::BadRequest, "a malformed method is refused");

    Check(client.Send(Get("https://evil.example/")).error == Error::HostNotAllowed,
            "a host outside the policy is refused without connecting");
    Check(client.Send(Get("http://localhost:" + server.UrlFor("").substr(17))).error == Error::HostNotAllowed,
            "plain http to a name is refused");

    response = client.Send(Get(server.UrlFor("/missing")));
    Check(response.error == Error::None && response.status == 404 && !response.Ok(),
            "a 404 is a response, not an error");
}

void SizeCaps(const TestServer& server, HttpsClient& client) {
    Request request = Get(server.UrlFor("/big"));
    request.maxBytes = 1000;
    Response response = client.Send(request);
    Check(response.error == Error::TooLarge && response.body.empty(), "a Content-Length above the cap is refused");
    request = Get(server.UrlFor("/unsized"));
    request.maxBytes = 1000;
    response = client.Send(request);
    Check(response.error == Error::TooLarge && response.body.empty(), "a body without length is cut at the cap");
    request.maxBytes = 5000;
    response = client.Send(request);
    Check(response.Ok() && response.body.size() == 5000, "a body exactly at the cap is accepted");
}

void Redirects(const TestServer& server, HttpsClient& client) {
    Response response = client.Send(Get(server.UrlFor("/redirect-same")));
    const std::string* location = response.FindHeader("Location");
    Check(response.error == Error::None && response.status == 302 && location != nullptr && *location == "/json",
            "without maxRedirects a redirect is returned as is (never followed by WinHTTP)");

    Request follow = Get(server.UrlFor("/redirect-same"));
    follow.maxRedirects = 2;
    response = client.Send(follow);
    Check(response.Ok() && response.body == "{\"ok\":true,\"stars\":42}" && response.url == server.UrlFor("/json"),
            "a same-origin redirect is followed when allowed");

    follow.url = server.UrlFor("/redirect-other");
    response = client.Send(follow);
    Check(response.error == Error::RedirectRefused && response.status == 302, "a redirect to another host is refused");

    follow.url = server.UrlFor("/redirect-loop");
    response = client.Send(follow);
    Check(response.error == Error::TooManyRedirects, "a redirect loop stops at maxRedirects");

    Request post = Get(server.UrlFor("/see-other"));
    post.method = "POST";
    post.body = "payload";
    post.maxRedirects = 1;
    post.headers = {{"X-Test", "kept"}};
    response = client.Send(post);
    Check(response.Ok() && response.body == "GET kept ", "303 continues as a GET without the body");
}

void TimeoutsAndCancel(const TestServer& server, HttpsClient& client) {
    // WinHTTP checks its timeouts in steps of about 4 seconds: a 300 ms timeout fires after up to 4 s.
    Request silent = Get(server.UrlFor("/slower"));
    silent.timeoutMs = 300;
    Clock::time_point start = Clock::now();
    Response response = client.Send(silent);
    Check(response.error == Error::Timeout && MillisecondsSince(start) < 9000, "a silent server times out");

    Request slow = Get(server.UrlFor("/slow"));
    slow.timeoutMs = 10000;
    CancelToken token;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        token.Cancel();
    });
    start = Clock::now();
    response = client.Send(slow, &token);
    canceller.join();
    Check(response.error == Error::Cancelled && MillisecondsSince(start) < 3000,
            "cancelling from another thread aborts the request in flight");
    Check(client.Send(Get(server.UrlFor("/json")), &token).error == Error::Cancelled,
            "a cancelled token refuses the next request at once");
}

void Streaming(const TestServer& server, HttpsClient& client) {
    std::vector<SseEvent> events;
    Response response = client.Stream(Get(server.UrlFor("/events")), [&](const SseEvent& event) {
        events.push_back(event);
        return true;
    });
    Check(response.Ok() && events.size() == 3, "three events from a stream sent in pieces");
    Check(events.size() == 3 && events[0].type == "delta" && events[0].data == "Hello"
                    && events[1].data == "line 1\nline 2" && events[1].id == "7" && events[2].type == "done"
                    && events[2].data == "{\"stop\":\"end_turn\"}",
            "event types, joined data and ids survive chunk boundaries");

    events.clear();
    const Clock::time_point start = Clock::now();
    response = client.Stream(Get(server.UrlFor("/events-then-wait")), [&](const SseEvent& event) {
        events.push_back(event);
        return false;
    });
    Check(response.error == Error::None && events.size() == 1 && MillisecondsSince(start) < 3000,
            "the callback stops the stream at once, without waiting for the server");

    Request big = Get(server.UrlFor("/events-big"));
    big.maxBytes = 4000;
    response = client.Stream(big, [](const SseEvent&) { return true; });
    Check(response.error == Error::TooLarge, "the cap covers the whole stream");

    response = client.Stream(Get(server.UrlFor("/json")), [](const SseEvent&) { return true; });
    Check(response.error == Error::NotEventStream && response.status == 200, "a 2xx that is not an event stream");

    response = client.Stream(Get(server.UrlFor("/problem")), [](const SseEvent&) { return true; });
    Check(response.error == Error::None && response.status == 404
                    && response.body.find("not_found") != std::string::npos,
            "an error response keeps its problem+json body");
}

void WorkerQueue(const TestServer& server, HttpsClient& client) {
    std::mutex mutex;
    std::vector<int> order;
    std::vector<Error> errors;
    {
        Worker worker;
        for (int i = 0; i < 3; ++i) {
            worker.Post([&, i](CancelToken& token) {
                const Response response = client.Send(Get(server.UrlFor("/json")), &token);
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(i);
                errors.push_back(response.error);
            });
        }
        const Clock::time_point start = Clock::now();
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (order.size() == 3 || MillisecondsSince(start) > 5000) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    Check(order == std::vector<int>({0, 1, 2}), "jobs run once each, in the order they were posted");
    Check(errors == std::vector<Error>(3, Error::None), "requests from the worker thread succeed");

    Error slowResult = Error::None;
    Error queuedResult = Error::None;
    bool queuedRan = false;
    Worker worker;
    Request slow = Get(server.UrlFor("/slow"));
    slow.timeoutMs = 10000;
    const auto running = worker.Post([&](CancelToken& token) { slowResult = client.Send(slow, &token).error; });
    const auto queued = worker.Post([&](CancelToken& token) {
        queuedRan = true;
        queuedResult = client.Send(Get(server.UrlFor("/json")), &token).error;
    });
    queued->Cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const Clock::time_point start = Clock::now();
    worker.Stop();
    Check(MillisecondsSince(start) < 3000, "Stop aborts the running request and joins quickly");
    Check(slowResult == Error::Cancelled, "the running job sees Cancelled");
    Check(queuedRan && queuedResult == Error::Cancelled, "a job cancelled while queued still runs once, cancelled");
    Check(running != nullptr && worker.Post([](CancelToken&) {}) == nullptr, "no job is accepted after Stop");
}

} // namespace

int main() {
    UrlParsing();
    HostAllowlist();
    RedirectTargets();
    HeaderChecks();
    Digests();
    SseFormat();

    TestServer server;
    const bool started = server.Start();
    Check(started, "the local test server listens on 127.0.0.1");
    if (started) {
        HttpsClient client(LoopbackOptions());
        Requests(server, client);
        SizeCaps(server, client);
        Redirects(server, client);
        TimeoutsAndCancel(server, client);
        Streaming(server, client);
        WorkerQueue(server, client);
    }
    server.Stop();
    std::printf("%s (%d failed)\n",
            failed == 0 ? "all https client checks passed" : "https client checks failed",
            failed);
    return failed == 0 ? 0 : 1;
}
