#pragma once
// The overlay's shared HTTPS layer, for PUBLIC (community API) and Nickel AI (streamed answers).
// Tests without the game: tests/https_client_checks.cpp (policy, digest, SSE, and requests to a local test server).
//
// - Only hosts named in a HostPolicy are reached, over HTTPS on port 443. Plain HTTP is accepted only for 127.0.0.1,
//   and only when the policy allows it (the tests' local server). The URL is checked before any connection is made.
// - Redirects are never followed by WinHTTP. A caller may allow a few redirects to the same scheme, host and port;
//   one to another origin is refused (Error::RedirectRefused), so a compromised response cannot move the client to a
//   host the policy does not name.
// - Timeouts on every phase, a cap on the body size (checked against Content-Length first, then while reading), no
//   cookies, no automatic authentication, no decompression (the body is the bytes the Content-Digest covers).
// - Sha256 / ContentDigestMatches: integrity of downloaded files (Content-Digest of RFC 9530, sha256 of the API).
// - Worker: one background thread running jobs in order, so requests never block the render thread; every job gets
//   a CancelToken, and cancelling aborts its request in flight. Requests are asynchronous WinHTTP requests that the
//   calling thread waits on: Cancel only signals an event, and the thread that opened the request handle is the only
//   one that closes it (WinHTTP does not support closing a synchronous request from another thread).
// - Stream: a response in text/event-stream is delivered event by event through SseParser (sse_parser.hpp).
//
// Nothing here reads or writes files, and nothing runs in DllMain: the owner calls Worker::Stop from the overlay's
// unload path, where hooks are removed.
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sse_parser.hpp"

namespace nickel::net {

enum class Error : uint8_t {
    None,
    BadUrl,            // not an absolute http(s) URL this layer accepts
    HostNotAllowed,    // the policy does not name this scheme, host and port
    BadRequest,        // a method or header that could inject other headers
    Cancelled,
    Timeout,
    Connect,           // name not resolved, or the connection was refused
    Tls,               // certificate or TLS failure
    TooLarge,          // the body is larger than Request::maxBytes
    RedirectRefused,   // a redirect to another origin
    TooManyRedirects,
    NotEventStream,    // Stream: a 2xx response that is not text/event-stream
    StreamFailed,      // Stream: an event larger than the parser's limit
    Network,           // any other WinHTTP failure (systemError says which)
};

inline const char* ErrorName(Error error) {
    switch (error) {
        case Error::None:
            return "none";
        case Error::BadUrl:
            return "bad url";
        case Error::HostNotAllowed:
            return "host not allowed";
        case Error::BadRequest:
            return "bad request";
        case Error::Cancelled:
            return "cancelled";
        case Error::Timeout:
            return "timeout";
        case Error::Connect:
            return "cannot connect";
        case Error::Tls:
            return "tls failure";
        case Error::TooLarge:
            return "response too large";
        case Error::RedirectRefused:
            return "redirect refused";
        case Error::TooManyRedirects:
            return "too many redirects";
        case Error::NotEventStream:
            return "not an event stream";
        case Error::StreamFailed:
            return "event stream failed";
        case Error::Network:
            return "network error";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------------------------------------------
// URLs and host policy (no network)

struct Url {
    std::string scheme;  // "https" or "http"
    std::string host;    // lower case
    uint16_t port = 0;
    std::string target;  // path and query, starting with '/'; the fragment is dropped
};

namespace detail {

inline char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

inline bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && EqualsNoCase(text.substr(0, prefix.size()), prefix);
}

// Host names only (no IP literal other than dotted IPv4, no IPv6, no IDN): labels of [a-z0-9-], not empty.
inline bool ValidHost(std::string_view host) {
    if (host.empty() || host.size() > 253) {
        return false;
    }
    size_t labelLength = 0;
    for (const char c : host) {
        if (c == '.') {
            if (labelLength == 0) {
                return false;
            }
            labelLength = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
        if (++labelLength > 63) {
            return false;
        }
    }
    return labelLength > 0;
}

inline bool ParsePort(std::string_view text, uint16_t* port) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (value == 0 || value > 65535) {
        return false;
    }
    *port = static_cast<uint16_t>(value);
    return true;
}

} // namespace detail

// Absolute http or https URL, printable ASCII only (percent-encode the rest), no user name or password, no IPv6.
inline bool ParseUrl(std::string_view text, Url* out) {
    if (text.empty() || text.size() > 2048) {
        return false;
    }
    for (const char c : text) {
        if (c <= 0x20 || c >= 0x7F || c == '\\') {
            return false;
        }
    }
    Url url;
    if (detail::StartsWithNoCase(text, "https://")) {
        url.scheme = "https";
        url.port = 443;
        text.remove_prefix(8);
    } else if (detail::StartsWithNoCase(text, "http://")) {
        url.scheme = "http";
        url.port = 80;
        text.remove_prefix(7);
    } else {
        return false;
    }
    const size_t authorityEnd = text.find_first_of("/?#");
    const std::string_view authority = text.substr(0, authorityEnd);
    std::string_view rest = authorityEnd == std::string_view::npos ? std::string_view() : text.substr(authorityEnd);
    if (authority.find_first_of("@[]") != std::string_view::npos) {
        return false;
    }
    const size_t colon = authority.find(':');
    std::string_view host = authority.substr(0, colon);
    if (colon != std::string_view::npos && !detail::ParsePort(authority.substr(colon + 1), &url.port)) {
        return false;
    }
    url.host.reserve(host.size());
    for (const char c : host) {
        url.host += detail::Lower(c);
    }
    if (!detail::ValidHost(url.host)) {
        return false;
    }
    rest = rest.substr(0, rest.find('#'));
    url.target = rest.empty() || rest[0] != '/' ? "/" + std::string(rest) : std::string(rest);
    *out = std::move(url);
    return true;
}

struct HostPolicy {
    std::vector<std::string> httpsHosts;  // exact host names (no wildcard, no subdomain), reached on port 443 only
    bool allowLoopbackHttp = false;       // http://127.0.0.1:<port>, for the tests' local server
};

inline bool Allowed(const HostPolicy& policy, const Url& url) {
    if (url.scheme == "http") {
        return policy.allowLoopbackHttp && url.host == "127.0.0.1";
    }
    if (url.scheme != "https" || url.port != 443) {
        return false;
    }
    for (const std::string& host : policy.httpsHosts) {
        if (detail::EqualsNoCase(url.host, host)) {
            return true;
        }
    }
    return false;
}

// Parses and checks a URL against the policy: Error::None, BadUrl or HostNotAllowed.
inline Error CheckUrl(const HostPolicy& policy, std::string_view text, Url* out) {
    if (!ParseUrl(text, out)) {
        return Error::BadUrl;
    }
    return Allowed(policy, *out) ? Error::None : Error::HostNotAllowed;
}

inline bool SameOrigin(const Url& a, const Url& b) {
    return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

// Target of a redirect from `base`: an absolute path ("/x") or an absolute URL. Relative and scheme-relative
// references are refused, as are other origins (returns false).
inline bool ResolveSameOriginRedirect(const Url& base, std::string_view location, Url* out) {
    if (!location.empty() && location[0] == '/' && (location.size() < 2 || location[1] != '/')) {
        const std::string absolute = base.scheme + "://" + base.host + ":" + std::to_string(base.port)
                + std::string(location);
        return ParseUrl(absolute, out) && SameOrigin(base, *out);
    }
    return ParseUrl(location, out) && SameOrigin(base, *out);
}

// Header names are tokens; values are printable ASCII or tab (no CR or LF, so no header can be injected).
inline bool ValidHeader(std::string_view name, std::string_view value) {
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        const bool token = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        if (!token) {
            return false;
        }
    }
    for (const char c : value) {
        if ((c < 0x20 && c != '\t') || c >= 0x7F) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// SHA-256 and Content-Digest (RFC 9530)

using Sha256Digest = std::array<uint8_t, 32>;

inline bool Sha256(std::string_view data, Sha256Digest* out) {
    if (data.size() > 0xFFFFFFFFu) {
        return false;
    }
    auto* input = reinterpret_cast<PUCHAR>(const_cast<char*>(data.data()));
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
            nullptr,
            0,
            input,
            static_cast<ULONG>(data.size()),
            out->data(),
            static_cast<ULONG>(out->size())));
}

inline std::string HexLower(const uint8_t* bytes, size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        text += kHex[bytes[i] >> 4];
        text += kHex[bytes[i] & 0xF];
    }
    return text;
}

// Lower-case hex SHA-256 of data, as the API's sha256 fields and file ETags write it; empty on failure.
inline std::string Sha256Hex(std::string_view data) {
    Sha256Digest digest{};
    return Sha256(data, &digest) ? HexLower(digest.data(), digest.size()) : std::string();
}

inline std::string Base64Encode(const uint8_t* bytes, size_t size) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16)
                | (i + 1 < size ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0)
                | (i + 2 < size ? static_cast<uint32_t>(bytes[i + 2]) : 0);
        text += kAlphabet[(chunk >> 18) & 0x3F];
        text += kAlphabet[(chunk >> 12) & 0x3F];
        text += i + 1 < size ? kAlphabet[(chunk >> 6) & 0x3F] : '=';
        text += i + 2 < size ? kAlphabet[chunk & 0x3F] : '=';
    }
    return text;
}

// Strict standard base64 with padding; false on any other character or a bad length.
inline bool Base64Decode(std::string_view text, std::string* out) {
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        return c == '/' ? 63 : -1;
    };
    if (text.size() % 4 != 0) {
        return false;
    }
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int padding = last ? (text[i + 3] == '=') + (text[i + 2] == '=' && text[i + 3] == '=') : 0;
        uint32_t chunk = 0;
        for (int j = 0; j < 4 - padding; ++j) {
            const int digit = value(text[i + j]);
            if (digit < 0) {
                return false;
            }
            chunk |= static_cast<uint32_t>(digit) << (18 - 6 * j);
        }
        bytes += static_cast<char>((chunk >> 16) & 0xFF);
        if (padding < 2) {
            bytes += static_cast<char>((chunk >> 8) & 0xFF);
        }
        if (padding < 1) {
            bytes += static_cast<char>(chunk & 0xFF);
        }
    }
    *out = std::move(bytes);
    return true;
}

// Content-Digest header value for body: "sha-256=:<base64>:".
inline std::string ContentDigestFor(std::string_view body) {
    Sha256Digest digest{};
    if (!Sha256(body, &digest)) {
        return std::string();
    }
    return "sha-256=:" + Base64Encode(digest.data(), digest.size()) + ":";
}

// True when the header (a structured-field dictionary such as "sha-256=:...:, sha-512=:...:") has a sha-256 entry
// equal to the body's hash. A header without sha-256 does not match: the client requires that algorithm.
inline bool ContentDigestMatches(std::string_view header, std::string_view body) {
    Sha256Digest digest{};
    if (!Sha256(body, &digest)) {
        return false;
    }
    while (!header.empty()) {
        const size_t comma = header.find(',');
        std::string_view entry = header.substr(0, comma);
        header = comma == std::string_view::npos ? std::string_view() : header.substr(comma + 1);
        while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) {
            entry.remove_prefix(1);
        }
        const size_t equals = entry.find('=');
        if (equals == std::string_view::npos || !detail::EqualsNoCase(entry.substr(0, equals), "sha-256")) {
            continue;
        }
        std::string_view value = entry.substr(equals + 1);
        if (value.size() < 2 || value[0] != ':') {
            return false;
        }
        const size_t end = value.find(':', 1);
        if (end == std::string_view::npos) {
            return false;
        }
        std::string decoded;
        return Base64Decode(value.substr(1, end - 1), &decoded) && decoded.size() == digest.size()
                && std::memcmp(decoded.data(), digest.data(), digest.size()) == 0;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Requests and responses

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<Header> headers;  // e.g. {"Content-Type", "application/json"}
    std::string body;
    size_t maxBytes = 256 * 1024;  // body cap; in Stream, the cap on the whole stream
    int timeoutMs = 15000;         // resolve, connect, send, and each receive (Stream: the longest silence);
                                   // WinHTTP checks it in steps of about 4 s, so it may fire up to 4 s late
    int maxRedirects = 0;          // same-origin redirects followed by Send; 0: a 3xx is returned as is
};

struct Response {
    Error error = Error::None;
    unsigned long systemError = 0;  // WinHTTP or Win32 code behind Error::Network and the others
    int status = 0;
    std::vector<Header> headers;
    std::string body;               // empty after Stream's events, and when the cap was exceeded
    std::string url;                // the URL that answered (after redirects)

    bool Ok() const { return error == Error::None && status >= 200 && status < 300; }

    // First header with that name (case-insensitive), or nullptr.
    const std::string* FindHeader(std::string_view name) const {
        for (const Header& header : headers) {
            if (detail::EqualsNoCase(header.name, name)) {
                return &header.value;
            }
        }
        return nullptr;
    }
};

// Cancels one request, one Worker job, or every request sharing the token. Cancel may be called from any thread, at
// any time, more than once: it only sets a flag and an event. The thread running the request waits on that event
// next to WinHTTP's completion, then closes its own request handle, which aborts the operation in flight; a step it
// would start next fails at once with Error::Cancelled. The token must outlive the requests that use it.
class CancelToken {
public:
    CancelToken() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~CancelToken() {
        if (event_ != nullptr) {
            CloseHandle(event_);
        }
    }

    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    void Cancel() {
        cancelled_.store(true);
        if (event_ != nullptr) {
            SetEvent(event_);
        }
    }

    bool Cancelled() const { return cancelled_.load(); }

private:
    friend class HttpsClient;

    std::atomic<bool> cancelled_{false};
    HANDLE event_;  // manual reset; nullptr if it could not be created (a step in flight then ends at its timeout)
};

namespace detail {

inline std::wstring Widen(std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    }
    return wide;
}

inline std::string Narrow(std::wstring_view text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), size, narrow.data(), length, nullptr, nullptr);
    }
    return narrow;
}

inline std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// "HTTP/1.1 200 OK\r\nName: value\r\n..." into headers (the status line is skipped).
inline std::vector<Header> SplitRawHeaders(std::string_view raw) {
    std::vector<Header> headers;
    bool statusLine = true;
    while (!raw.empty()) {
        const size_t end = raw.find("\r\n");
        const std::string_view line = raw.substr(0, end);
        raw = end == std::string_view::npos ? std::string_view() : raw.substr(end + 2);
        if (statusLine) {
            statusLine = false;
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            continue;
        }
        headers.push_back({std::string(Trim(line.substr(0, colon))), std::string(Trim(line.substr(colon + 1)))});
    }
    return headers;
}

inline Error FromWinHttp(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_OPERATION_CANCELLED:
            return Error::Cancelled;
        case ERROR_WINHTTP_TIMEOUT:
            return Error::Timeout;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return Error::Connect;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
            return Error::Tls;
        default:
            return Error::Network;
    }
}

inline bool IsRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// What one asynchronous request shares with WinHTTP's callback threads. It has two references: the thread running
// the request, and WinHTTP from the moment the callback is set until the handle's closing notification, so whichever
// is last frees it. The request body and the read buffer live here because WinHTTP may use them until the request
// handle has closed.
struct AsyncCall {
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto reset: one completion per operation
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> references{1};
    DWORD error = 0;  // of the last completion, written before `completed` is set (0: it succeeded)
    DWORD bytes = 0;  // DATA_AVAILABLE: the bytes available; READ_COMPLETE: the bytes read
    std::string body;
    std::vector<char> buffer = std::vector<char>(16 * 1024);

    AsyncCall() = default;
    AsyncCall(const AsyncCall&) = delete;
    AsyncCall& operator=(const AsyncCall&) = delete;

    ~AsyncCall() {
        if (completed != nullptr) {
            CloseHandle(completed);
        }
        if (closed != nullptr) {
            CloseHandle(closed);
        }
    }

    void Release() {
        if (references.fetch_sub(1) == 1) {
            delete this;
        }
    }
};

inline void CALLBACK OnRequestStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
    AsyncCall* call = reinterpret_cast<AsyncCall*>(context);
    if (call == nullptr) {
        return;
    }
    switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            call->error = 0;
            call->bytes = 0;
            SetEvent(call->completed);
            break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            call->error = 0;
            call->bytes = info != nullptr ? *static_cast<const DWORD*>(info) : 0;
            SetEvent(call->completed);
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            call->error = 0;
            call->bytes = length;
            SetEvent(call->completed);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
            const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
            call->error = result != nullptr && result->dwError != 0 ? result->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
            SetEvent(call->completed);
            break;
        }
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            SetEvent(call->closed);
            call->Release();  // WinHTTP's reference: nothing is called back for this handle after this one
            break;
        default:
            break;
    }
}

} // namespace detail

struct ClientOptions {
    std::wstring userAgent = L"Nickel-Overlay";
    HostPolicy policy;
};

// One asynchronous WinHTTP session. Send and Stream block the calling thread until the response (or the stream) has
// ended: call them from a Worker job, never from the render thread. Both may be called from several threads at once.
class HttpsClient {
public:
    static constexpr size_t kMaxEventBytes = 1024 * 1024;  // one SSE event, whatever the stream's cap
    static constexpr DWORD kCloseWaitMs = 10000;           // for WinHTTP's closing notification of a request

    explicit HttpsClient(ClientOptions options) : options_(std::move(options)) {
        session_ = WinHttpOpen(options_.userAgent.c_str(),
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                WINHTTP_FLAG_ASYNC);
        if (session_ != nullptr) {
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
            DWORD withTls13 = protocols | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            if (WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &withTls13, sizeof(withTls13))) {
                return;
            }
#endif
            WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
    }

    ~HttpsClient() {
        if (session_ != nullptr) {
            WinHttpCloseHandle(session_);
        }
    }

    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;

    const HostPolicy& policy() const { return options_.policy; }

    // A whole response in memory, following at most request.maxRedirects same-origin redirects.
    Response Send(const Request& request, CancelToken* cancel = nullptr) {
        Response response;
        Url url;
        response.error = CheckUrl(options_.policy, request.url, &url);
        if (response.error != Error::None) {
            return response;
        }
        Request hop = request;
        for (int redirects = 0;; ++redirects) {
            response = Exchange(hop, url, cancel, nullptr);
            if (response.error != Error::None || !detail::IsRedirect(response.status) || request.maxRedirects == 0) {
                return response;
            }
            if (redirects >= request.maxRedirects) {
                response.error = Error::TooManyRedirects;
                return response;
            }
            const std::string* location = response.FindHeader("Location");
            Url next;
            if (location == nullptr || !ResolveSameOriginRedirect(url, *location, &next)) {
                response.error = Error::RedirectRefused;
                return response;
            }
            // 303, and 301/302 after a POST, continue as a GET without a body (what browsers do).
            const bool postToGet = (response.status == 301 || response.status == 302) && hop.method == "POST";
            if (response.status == 303 || postToGet) {
                hop.method = "GET";
                hop.body.clear();
            }
            url = std::move(next);
        }
    }

    // A text/event-stream response: onEvent(const SseEvent&) is called for each event as it arrives and returns
    // false to stop reading (the response then has no error). Redirects are not followed. A response that is not a
    // 2xx keeps its body (a problem+json error), with the same cap.
    template <typename OnEvent>
    Response Stream(const Request& request, OnEvent&& onEvent, CancelToken* cancel = nullptr) {
        Response response;
        Url url;
        response.error = CheckUrl(options_.policy, request.url, &url);
        if (response.error != Error::None) {
            return response;
        }
        SseParser parser((std::min)(request.maxBytes, kMaxEventBytes));
        std::function<bool(Response&, std::string_view)> sink = [&](Response& partial, std::string_view chunk) {
            if (partial.status < 200 || partial.status >= 300) {
                partial.body.append(chunk.data(), chunk.size());
                return true;
            }
            if (!parser.Feed(chunk, onEvent)) {
                if (parser.Failed()) {
                    partial.error = Error::StreamFailed;
                }
                return false;
            }
            return true;
        };
        return Exchange(request, url, cancel, &sink);
    }

private:
    struct InternetHandle {
        HINTERNET handle = nullptr;

        ~InternetHandle() {
            if (handle != nullptr) {
                WinHttpCloseHandle(handle);
            }
        }
    };

    // An asynchronous request handle, owned by the thread running the request: only that thread closes it, which
    // aborts an operation in flight, and it then waits for WinHTTP's closing notification so that no callback for this
    // request runs after the exchange has returned.
    struct AsyncRequest {
        HINTERNET handle = nullptr;
        detail::AsyncCall* call = new detail::AsyncCall();
        bool watched = false;

        AsyncRequest() = default;
        AsyncRequest(const AsyncRequest&) = delete;
        AsyncRequest& operator=(const AsyncRequest&) = delete;

        ~AsyncRequest() {
            if (handle != nullptr) {
                WinHttpCloseHandle(handle);
                if (watched) {
                    WaitForSingleObject(call->closed, kCloseWaitMs);
                }
            }
            call->Release();
        }

        // Routes the handle's completions to `call`; WinHTTP then holds a reference until the handle closes.
        bool Watch() {
            DWORD_PTR context = reinterpret_cast<DWORD_PTR>(call);
            if (!WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
                return false;
            }
            call->references.fetch_add(1);
            const DWORD notifications = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES;
            if (WinHttpSetStatusCallback(handle, detail::OnRequestStatus, notifications, 0)
                    == WINHTTP_INVALID_STATUS_CALLBACK) {
                call->references.fetch_sub(1);
                return false;
            }
            watched = true;
            return true;
        }
    };

    // Starts one asynchronous operation and waits for its completion, or for the token. Error::Cancelled as soon as
    // the token is cancelled: the caller then returns, and closing the handle aborts the operation.
    template <typename Start>
    static Error Step(const CancelToken& token, detail::AsyncCall* call, DWORD* code, Start&& start) {
        if (token.Cancelled()) {
            return Error::Cancelled;
        }
        if (!start()) {
            *code = GetLastError();
            return token.Cancelled() ? Error::Cancelled : detail::FromWinHttp(*code);
        }
        const HANDLE events[2] = {call->completed, token.event_};
        const DWORD count = token.event_ != nullptr ? 2 : 1;
        const DWORD woke = WaitForMultipleObjects(count, events, FALSE, INFINITE);
        if (woke == WAIT_OBJECT_0) {
            if (call->error == 0) {
                return Error::None;
            }
            *code = call->error;
            return token.Cancelled() ? Error::Cancelled : detail::FromWinHttp(call->error);
        }
        if (woke == WAIT_OBJECT_0 + 1) {
            return Error::Cancelled;
        }
        *code = GetLastError();
        return Error::Network;
    }

    static Response Failure(Error error, DWORD code = 0) {
        Response response;
        response.error = error;
        response.systemError = code;
        return response;
    }

    static Response LastFailure(const CancelToken& token) {
        const DWORD code = GetLastError();
        return Failure(token.Cancelled() ? Error::Cancelled : detail::FromWinHttp(code), code);
    }

    static bool BuildHeaders(const Request& request, std::wstring* out) {
        for (const Header& header : request.headers) {
            if (!ValidHeader(header.name, header.value)) {
                return false;
            }
            *out += detail::Widen(header.name) + L": " + detail::Widen(header.value) + L"\r\n";
        }
        return true;
    }

    static bool ValidMethod(std::string_view method) {
        if (method.empty() || method.size() > 16) {
            return false;
        }
        for (const char c : method) {
            if (c < 'A' || c > 'Z') {
                return false;
            }
        }
        return true;
    }

    static void Configure(HINTERNET request, const Url& url, int timeoutMs) {
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
        DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
        if (url.scheme == "http") {
            // The loopback test server: never through a proxy.
            WINHTTP_PROXY_INFO direct{};
            direct.dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
            WinHttpSetOption(request, WINHTTP_OPTION_PROXY, &direct, sizeof(direct));
        }
        const int timeout = timeoutMs > 0 ? timeoutMs : 15000;
        WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
    }

    static void ReadStatusAndHeaders(HINTERNET request, Response* response) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &size,
                WINHTTP_NO_HEADER_INDEX);
        response->status = static_cast<int>(status);
        DWORD bytes = 0;
        WinHttpQueryHeaders(request,
                WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX,
                WINHTTP_NO_OUTPUT_BUFFER,
                &bytes,
                WINHTTP_NO_HEADER_INDEX);
        if (bytes == 0) {
            return;
        }
        std::wstring raw(bytes / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(request,
                    WINHTTP_QUERY_RAW_HEADERS_CRLF,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    raw.data(),
                    &bytes,
                    WINHTTP_NO_HEADER_INDEX)) {
            raw.resize(bytes / sizeof(wchar_t));
            response->headers = detail::SplitRawHeaders(detail::Narrow(raw));
        }
    }

    static bool DeclaredTooLarge(const Response& response, size_t maxBytes) {
        const std::string* length = response.FindHeader("Content-Length");
        if (length == nullptr) {
            return false;
        }
        unsigned long long declared = 0;
        for (const char c : *length) {
            if (c < '0' || c > '9') {
                return false;
            }
            declared = declared * 10 + static_cast<unsigned long long>(c - '0');
            if (declared > maxBytes) {
                return true;
            }
        }
        return false;
    }

    // Reads the body: into response->body, or chunk by chunk into sink (which may stop the read, or set an error).
    static void ReadBody(const AsyncRequest& exchange,
            const Request& options,
            const CancelToken& token,
            std::function<bool(Response&, std::string_view)>* sink,
            Response* response) {
        detail::AsyncCall* call = exchange.call;
        size_t total = 0;
        for (;;) {
            // Wait for what has arrived, then read only that: a read of a whole buffer would wait for it to fill,
            // which holds back the events of a stream.
            DWORD code = 0;
            Error error = Step(token, call, &code, [&] { return WinHttpQueryDataAvailable(exchange.handle, nullptr); });
            if (error == Error::None) {
                const DWORD available = call->bytes;
                if (available == 0) {
                    return;
                }
                const DWORD wanted = (std::min)(available, static_cast<DWORD>(call->buffer.size()));
                error = Step(token, call, &code, [&] {
                    return WinHttpReadData(exchange.handle, call->buffer.data(), wanted, nullptr);
                });
            }
            if (error != Error::None) {
                response->error = error;
                response->systemError = code;
                response->body.clear();
                return;
            }
            const DWORD got = call->bytes;
            if (got == 0) {
                return;
            }
            total += got;
            if (total > options.maxBytes) {
                response->error = Error::TooLarge;
                response->body.clear();
                return;
            }
            const std::string_view chunk(call->buffer.data(), got);
            if (sink == nullptr) {
                response->body.append(chunk.data(), chunk.size());
            } else if (!(*sink)(*response, chunk)) {
                return;
            }
        }
    }

    // One request and its response, for an origin already checked against the policy.
    Response Exchange(const Request& request,
            const Url& url,
            CancelToken* cancel,
            std::function<bool(Response&, std::string_view)>* sink) {
        std::wstring headers;
        if (!ValidMethod(request.method) || !BuildHeaders(request, &headers) || request.body.size() > 0xFFFFFFFFu) {
            return Failure(Error::BadRequest);
        }
        if (session_ == nullptr) {
            return Failure(Error::Network, ERROR_INVALID_HANDLE);
        }
        CancelToken local;
        const CancelToken& token = cancel != nullptr ? *cancel : local;
        if (token.Cancelled()) {
            return Failure(Error::Cancelled);
        }
        InternetHandle connection{WinHttpConnect(session_, detail::Widen(url.host).c_str(), url.port, 0)};
        if (connection.handle == nullptr) {
            return LastFailure(token);
        }
        AsyncRequest exchange;  // declared after the connection, so the request handle closes first
        exchange.handle = WinHttpOpenRequest(connection.handle,
                detail::Widen(request.method).c_str(),
                detail::Widen(url.target).c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                url.scheme == "https" ? WINHTTP_FLAG_SECURE : 0);
        if (exchange.handle == nullptr || exchange.call->completed == nullptr || exchange.call->closed == nullptr
                || !exchange.Watch()) {
            return LastFailure(token);
        }
        Configure(exchange.handle, url, request.timeoutMs);
        detail::AsyncCall* call = exchange.call;
        call->body = request.body;
        const DWORD bodySize = static_cast<DWORD>(call->body.size());
        void* body = bodySize > 0 ? call->body.data() : WINHTTP_NO_REQUEST_DATA;
        DWORD code = 0;
        Error error = Step(token, call, &code, [&] {
            return WinHttpSendRequest(exchange.handle,
                    headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                    headers.empty() ? 0 : static_cast<DWORD>(-1L),
                    body,
                    bodySize,
                    bodySize,
                    reinterpret_cast<DWORD_PTR>(call));
        });
        if (error == Error::None) {
            error = Step(token, call, &code, [&] { return WinHttpReceiveResponse(exchange.handle, nullptr); });
        }
        if (error != Error::None) {
            return Failure(error, code);
        }
        Response response;
        response.url = url.scheme + "://" + url.host + ":" + std::to_string(url.port) + url.target;
        ReadStatusAndHeaders(exchange.handle, &response);
        // A HEAD, 1xx, 204 or 304 has no body, whatever length it declares.
        const bool bodiless = request.method == "HEAD" || response.status < 200 || response.status == 204
                || response.status == 304;
        if (!bodiless && DeclaredTooLarge(response, request.maxBytes)) {
            response.error = Error::TooLarge;
            return response;
        }
        if (sink != nullptr && response.status >= 200 && response.status < 300) {
            const std::string* type = response.FindHeader("Content-Type");
            if (type == nullptr || !detail::StartsWithNoCase(*type, "text/event-stream")) {
                response.error = Error::NotEventStream;
                return response;
            }
        }
        ReadBody(exchange, request, token, sink, &response);
        return response;
    }

    ClientOptions options_;
    HINTERNET session_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------------------
// Background worker

// One thread, started by the first Post, running jobs in the order they were posted. Each job receives its own
// CancelToken (also returned by Post): a cancelled job still runs, once, so its owner always hears back, and its
// requests fail at once with Error::Cancelled. Results go back through whatever the job captures (for example a
// mutex-protected slot the render thread reads), never through the worker.
class Worker {
public:
    using Job = std::function<void(CancelToken&)>;

    Worker() = default;
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    ~Worker() { Stop(); }

    // Queues a job; nullptr (and the job is dropped) once Stop was called, or for an empty job.
    std::shared_ptr<CancelToken> Post(Job job) {
        if (!job) {
            return nullptr;
        }
        auto token = std::make_shared<CancelToken>();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return nullptr;
            }
            queue_.emplace_back(token, std::move(job));
            if (!thread_.joinable()) {
                thread_ = std::thread([this] { Run(); });
                threadId_ = thread_.get_id();
            }
        }
        wake_.notify_one();
        return token;
    }

    // Cancels every queued job and the running one, lets them finish, and joins the thread. Not from DllMain (the
    // loader lock would deadlock the join). The worker cannot be restarted. From a job, Stop only cancels: the thread
    // ends after that job, and the destructor (or a Stop from another thread) joins it. Several threads may call
    // Stop at once; each returns once the thread has ended.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            for (auto& entry : queue_) {
                entry.first->Cancel();
            }
            if (running_) {
                running_->Cancel();
            }
            if (threadId_ == std::this_thread::get_id()) {
                return;
            }
        }
        wake_.notify_one();
        // Post no longer touches thread_ once stopping_ is set, so only this mutex guards the join.
        std::lock_guard<std::mutex> join(joinMutex_);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Jobs queued and not started yet.
    size_t Pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;  // stopping, and every job has run
            }
            auto entry = std::move(queue_.front());
            queue_.pop_front();
            running_ = entry.first;
            lock.unlock();
            try {
                entry.second(*entry.first);
            } catch (...) {
                // An exception leaving this thread would end the game; the job's owner hears nothing back.
            }
            lock.lock();
            running_.reset();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<std::shared_ptr<CancelToken>, Job>> queue_;
    std::shared_ptr<CancelToken> running_;
    std::mutex joinMutex_;
    std::thread thread_;
    std::thread::id threadId_;
    bool stopping_ = false;
};

} // namespace nickel::net
