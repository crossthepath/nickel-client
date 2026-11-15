#pragma once
// PUBLIC's calls to the community API (https://nickelclient.com/api/community/v1), for the PUBLIC pages of the menu.
// Tests without the game: tests/public_client_checks.cpp, against a local server on 127.0.0.1 (the base URL and the
// host policy are options).
//
// - Every request runs on one background thread (net::Worker); the render thread only posts requests and reads
//   snapshots. A request first publishes a "loading" result, then its answer; both are immutable objects behind
//   shared_ptr, so a snapshot is a handful of pointer copies and the render thread never waits on the network.
// - Each kind's list, the detail, the source preview and the install download carry a generation: a newer request of
//   the same slot makes the older answer stale, and a stale answer is dropped (typing in the search box).
// - Integrity of files: the body must be at most 200 KB, its SHA-256 must equal the version's sha256 and the
//   Content-Digest header (required), and a strong ETag, when sent, must be "<sha256>". Otherwise nothing is kept.
// - Caching: JSON answers with an ETag are kept in memory (a few dozen) and asked again with If-None-Match; a 304 reuses
//   the kept body. When the network fails, a list or the revocation list falls back to the kept body (fromCache).
// - 429: the Retry-After (or the problem's retry_after) blocks every request of this client until it has elapsed;
//   requests made meanwhile fail at once with Status::RateLimited, without reaching the server.
// - No sign-in anywhere. Writes (Check, Publish, PublishVersion, Star, Report) send "Authorization: Install <secret>"
//   from the InstallIdentity (public_identity.hpp), read on the worker thread; reads never send it. Stars are
//   favourites: Star(id, true) is PUT /items/{id}/star, Star(id, false) DELETE; the list of what this install starred
//   is local (public_local.hpp). Publishing is final: there is no call that deletes, unlists or hides an item.
// - Nothing here reads or writes files; installing is public_installer.hpp. Stop() (or the destructor) cancels and
//   joins the worker: call it from the overlay's unload path, never from DllMain.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "https_client.hpp"
#include "json_value.hpp"
#include "public_identity.hpp"
#include "public_model.hpp"

namespace nickel::pub {

// What became of a request.
enum class Status : uint8_t {
    Ok,
    Loading,         // posted, no answer yet
    NoIdentity,      // a write without a usable install key (damaged or not creatable): nothing was sent
    Invalid,         // the caller's arguments (an id, a hash) or the base URL were refused before any request
    Offline,         // DNS, connection or TLS failure
    Timeout,
    TooLarge,        // the body was over its cap (or 413)
    Malformed,       // the answer is not the JSON the API documents
    DigestMismatch,  // a file whose bytes do not match its sha256 or Content-Digest: discarded
    RateLimited,     // 429, or a request made while a Retry-After runs
    Maintenance,     // 503
    BadRequest,      // 400 (an expired cursor: start from the first page again)
    Unauthorized,    // 401: the install key was refused
    Forbidden,       // 403: this install is blocked from writes
    NotFound,        // 404
    Gone,            // 410: removed or deleted (outcome.problem.goneState, reasonCategory)
    Conflict,        // 409 (a file already published, starring one's own item)
    Rejected,        // 422: the file or a field failed the checks (outcome.problem.errors says which)
    ServerError,     // 5xx and any other status
    Cancelled,       // Stop(), or a newer request of the same slot
};

inline const char* StatusName(Status status) {
    static const char* const kNames[] = {
            "ok",          "loading",     "no install key", "invalid",         "offline",
            "timeout",     "too large",   "malformed",      "digest mismatch", "rate limited",
            "maintenance", "bad request", "unauthorized",   "forbidden",       "not found",
            "gone",        "conflict",    "rejected",       "server error",    "cancelled"};
    const size_t index = static_cast<size_t>(status);
    return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "unknown";
}

struct Outcome {
    Status status = Status::Ok;
    int httpStatus = 0;
    int retryAfter = 0;          // seconds, for RateLimited and Maintenance
    bool notModified = false;    // a 304 answered with the kept body
    Problem problem;             // the problem body of an error answer (code, detail, request_id...)
    std::string message;         // what failed, for the log (a field name, a network error)

    bool Ok() const { return status == Status::Ok; }
};

struct ClientConfig {
    std::string baseUrl = "https://nickelclient.com/api/community/v1";  // no trailing slash
    net::HostPolicy policy = net::HostPolicy{{"nickelclient.com"}, false};
    std::wstring userAgent = L"Nickel-Overlay";                          // "Nickel-Overlay/<version>"
    int timeoutMs = 15000;
    int defaultRetryAfterSeconds = 30;                                   // a 429 without Retry-After
    size_t cacheEntries = 32;
};

struct ListQuery {
    Kind kind = Kind::Script;
    Sort sort = Sort::Trending;
    std::string text;                      // search words; trimmed and cut to 64 code points
    std::vector<std::string> tags;         // up to 5 slugs: items having all of them
    std::vector<std::string> categories;   // configs, up to 5: configs changing all of them
    bool reviewedOnly = false;
    std::string compat;                    // the running Nickel version: only items it can run ("" for all)
    int limit = 24;                        // 1 to 50
};

struct ListResult {
    ListQuery query;
    Outcome outcome;
    std::vector<Card> items;               // every page loaded so far, without repeated ids
    std::string nextCursor;                // empty on the last page
    int64_t totalEstimate = -1;
    size_t dropped = 0;                    // items refused by validation
    bool loading = false;                  // the first page is on its way
    bool loadingMore = false;              // More() is on its way
    bool fromCache = false;                // the network failed: items are the last answer kept (outcome says why)
    int64_t fetchedAt = 0;                 // unix ms of the answer shown
    uint64_t generation = 0;
};

struct DetailResult {
    std::string id;
    Outcome outcome;
    Detail detail;
    bool loading = false;
    int64_t fetchedAt = 0;
    uint64_t generation = 0;
};

// A version file: the source preview (not counted) or the install download (counted as a download).
struct FileResult {
    std::string id;
    int number = 0;
    std::string sha256;                    // expected, and verified when outcome is Ok
    std::string bytes;                     // the file exactly as served; empty unless outcome is Ok
    bool install = false;                  // fetched from /file (counted) rather than /source
    Outcome outcome;
    bool loading = false;
    uint64_t generation = 0;
};

struct LookupResult {
    std::vector<std::string> ids;
    std::vector<LookupEntry> entries;      // one per id, in order, when outcome is Ok
    Outcome outcome;
    bool loading = false;
    int64_t fetchedAt = 0;
};

struct RevocationsResult {
    std::vector<Revocation> files;
    size_t dropped = 0;
    Outcome outcome;
    bool loading = false;
    bool fromCache = false;
    int64_t fetchedAt = 0;
};

struct StarResult {
    std::string id;
    bool starred = false; // what was asked
    Outcome outcome;
    bool loading = false;
};

// What a player publishes from OTHER -> Scripting or OTHER -> Configs (POST /items), or the file of a new version
// (POST /items/{id}/versions: only id, changelog, fileName and bytes are sent).
struct PublishRequest {
    Kind kind = Kind::Script;
    std::string name;
    std::string summary;
    std::string description;
    std::vector<std::string> tags; // slugs, up to 5
    std::string authorName;        // the display name set in Nickel, stored with the item
    std::string fileName;          // "<name>.luau" or "<name>.ini"
    std::string bytes;             // the file, sent in base64 exactly as read
    std::string changelog;         // new versions
    std::string ref;               // the caller's own reference (the local name), returned in the result
};

// The dry run of POST /items/check for a file.
struct CheckResult {
    Kind kind = Kind::Script;
    std::string fileName;
    std::string sentSha256; // of the bytes sent: the answer is about these bytes
    std::string ref;
    CheckSummary check; // when outcome is Ok
    Outcome outcome;
    bool loading = false;
};

struct PublishResult {
    Kind kind = Kind::Script;
    std::string ref;
    bool update = false;   // a new version of `itemId`
    std::string itemId;    // the item created, or the one updated
    int versionNumber = 0; // the version created (0 when the answer did not say)
    std::string state;     // the item's or version's state when sent ("published"...)
    std::string sentSha256;
    Outcome outcome;
    bool loading = false;
};

struct ReportResult {
    std::string id;
    Outcome outcome;
    bool loading = false;
};

// What the render thread reads. Every pointer may be null (nothing asked yet); the objects never change.
struct Snapshot {
    uint64_t version = 0;
    std::shared_ptr<const ListResult> lists[2];   // by Kind
    std::shared_ptr<const DetailResult> detail;
    std::shared_ptr<const FileResult> source;
    std::shared_ptr<const FileResult> file;
    std::shared_ptr<const LookupResult> lookup;
    std::shared_ptr<const RevocationsResult> revocations;
    std::shared_ptr<const LookupResult> starred; // the cards of the Starred tabs
    std::shared_ptr<const StarResult> star;
    std::shared_ptr<const CheckResult> check;
    std::shared_ptr<const PublishResult> publish;
    std::shared_ptr<const ReportResult> report;

    const ListResult* List(Kind kind) const { return lists[static_cast<size_t>(kind)].get(); }
};

namespace detail {

inline std::string PercentEncode(std::string_view text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                || c == '.' || c == '_' || c == '~';
        if (plain) {
            out += c;
        } else {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 15];
        }
    }
    return out;
}

// Search text: control characters become spaces, runs of spaces one space, trimmed, cut to `maxChars` code points.
inline std::string CleanSearch(std::string_view text, size_t maxChars) {
    std::string out;
    size_t chars = 0;
    bool space = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char u = static_cast<unsigned char>(text[i]);
        if (u < 0x20 || u == 0x7F || u == ' ') {
            space = !out.empty();
            continue;
        }
        const bool lead = (u & 0xC0) != 0x80;
        if (lead) {
            if (chars + (space ? 1 : 0) + 1 > maxChars) {
                break;
            }
            if (space) {
                out += ' ';
                ++chars;
                space = false;
            }
            ++chars;
        }
        out += static_cast<char>(u);
    }
    return out;
}

inline int64_t NowUnixMs() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const uint64_t ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return static_cast<int64_t>(ticks / 10000 - 11644473600000ULL);
}

// A file name the API accepts: 1 to 80 bytes, no path separator or control character, ending in `extension`.
inline bool ValidUploadName(std::string_view name, std::string_view extension) {
    if (name.size() <= extension.size() || name.size() > 80
        || name.compare(name.size() - extension.size(), extension.size(), extension) != 0) {
        return false;
    }
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '\\' || c == '/') {
            return false;
        }
    }
    return true;
}

inline json::Value FileJson(const std::string& name, const std::string& bytes) {
    json::Value file = json::Value::MakeObject();
    file.Set("name", json::Value::MakeString(name));
    file.Set("content",
             json::Value::MakeString(net::Base64Encode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())));
    file.Set("encoding", json::Value::MakeString("base64"));
    return file;
}

// What is wrong with a publish request before it is sent ("" when nothing is): the API's limits.
inline std::string PublishProblem(const PublishRequest& request, bool update) {
    const char* extension = request.kind == Kind::Script ? ".luau" : ".ini";
    if (!ValidUploadName(request.fileName, extension)) {
        return "file name";
    }
    if (request.bytes.empty()) {
        return "the file is empty";
    }
    if (request.bytes.size() > kMaxFileBytes) {
        return "the file is over 200 KB";
    }
    if (update) {
        return CodePoints(request.changelog) == 0
                               || !ValidText(request.changelog, kMaxChangelogChars, TextRule::MultiLine)
                       ? "changelog"
                       : "";
    }
    if (CodePoints(request.name) == 0 || !ValidText(request.name, kMaxNameChars, TextRule::OneLine)) {
        return "name";
    }
    if (CodePoints(request.summary) == 0 || !ValidText(request.summary, kMaxSummaryChars, TextRule::OneLine)) {
        return "summary";
    }
    if (!ValidText(request.description, kMaxDescriptionChars, TextRule::MultiLine)) {
        return "description";
    }
    if (request.tags.size() > kMaxTags) {
        return "tags";
    }
    for (const std::string& tag : request.tags) {
        if (!ValidTag(tag)) {
            return "tags";
        }
    }
    if (CodePoints(request.authorName) == 0
        || !ValidText(request.authorName, kMaxDisplayNameChars, TextRule::OneLine)) {
        return "author name";
    }
    return "";
}

}  // namespace detail

class PublicClient {
public:
    // identity: the install key for writes (nullptr: every write ends as Status::NoIdentity).
    explicit PublicClient(ClientConfig config = ClientConfig(), InstallIdentity* identity = nullptr)
        : config_(std::move(config)), identity_(identity),
          http_(net::ClientOptions{config_.userAgent, config_.policy}) {
        while (!config_.baseUrl.empty() && config_.baseUrl.back() == '/') {
            config_.baseUrl.pop_back();
        }
        net::Url url;
        usable_ = net::CheckUrl(config_.policy, config_.baseUrl + "/items", &url) == net::Error::None;
    }

    ~PublicClient() { Stop(); }

    PublicClient(const PublicClient&) = delete;
    PublicClient& operator=(const PublicClient&) = delete;

    // False when the base URL is not allowed by the host policy: every request then ends as Status::Invalid.
    bool Usable() const { return usable_; }

    // Cancels the request in flight and the queued ones, and joins the worker. Requests made afterwards are refused.
    void Stop() { worker_.Stop(); }

    // Bumped by every published result: copy a Snapshot only when it changed.
    uint64_t Version() const { return version_.load(); }

    Snapshot Snap() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    // ---- requests (any thread, never blocking; each returns false when it was refused at once, and the slot's
    // result then says why)

    // The first page of a kind's list; replaces that kind's list (the older answer, if late, is dropped).
    bool List(const ListQuery& query) { return StartList(query, 0); }

    // The next page of a kind's list, appended to it. False when there is no next page or a page is on its way.
    // A newer List() cancels it. An expired cursor (400) starts the list again from its first page, asked without
    // If-None-Match so that a fresh cursor comes back, unless a newer List() replaced the list meanwhile.
    bool More(Kind kind) {
        const size_t slot = static_cast<size_t>(kind);
        std::shared_ptr<const ListResult> current;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current = snapshot_.lists[slot];
            if (!current || current->loading || current->loadingMore || current->nextCursor.empty()
                    || current->fromCache) {
                return false;
            }
            generation = listGeneration_[slot];
            auto placeholder = std::make_shared<ListResult>(*current);
            placeholder->loadingMore = true;
            snapshot_.lists[slot] = placeholder;
            Bump();
        }
        std::string path;
        ListPath(current->query, current->nextCursor, &path);
        auto token = worker_.Post([this, slot, generation, current, path](net::CancelToken& cancel) {
            ListResult fetched;
            FetchPage(cancel, path, &fetched, false);
            if (fetched.outcome.status == Status::BadRequest) {
                std::string first;
                if (ListPath(current->query, std::string(), &first)) {
                    Forget(config_.baseUrl + first);
                }
                StartList(current->query, generation);
                return;
            }
            auto result = std::make_shared<ListResult>(*current);
            result->loadingMore = false;
            result->outcome = fetched.outcome;
            if (fetched.outcome.Ok()) {
                for (Card& card : fetched.items) {
                    bool repeated = false;
                    for (const Card& seen : result->items) {
                        repeated = repeated || seen.id == card.id;
                    }
                    if (!repeated) {
                        result->items.push_back(std::move(card));
                    }
                }
                result->nextCursor = fetched.nextCursor;
                result->dropped += fetched.dropped;
                result->fetchedAt = fetched.fetchedAt;
            }
            PublishList(slot, generation, std::move(result));
        });
        if (!token) {
            auto result = std::make_shared<ListResult>(*current);
            result->outcome.status = Status::Cancelled;
            PublishList(slot, generation, std::move(result));
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (listGeneration_[slot] == generation) {
            listToken_[slot] = token;  // a newer List() cancels this page
        }
        return true;
    }

private:
    // List(); with onlyIfGeneration != 0, nothing happens unless the kind's list is still at that generation (a late
    // More restarting its list must not replace a newer one).
    bool StartList(const ListQuery& query, uint64_t onlyIfGeneration) {
        ListQuery clean = query;
        clean.text = detail::CleanSearch(query.text, kMaxQueryChars);
        clean.limit = clean.limit < 1 ? 1 : (clean.limit > static_cast<int>(kMaxPageItems) ? 50 : clean.limit);
        const size_t slot = static_cast<size_t>(clean.kind);
        auto placeholder = std::make_shared<ListResult>();
        placeholder->query = clean;
        placeholder->loading = true;
        std::string path;
        const bool valid = usable_ && ListPath(clean, std::string(), &path);
        uint64_t generation = 0;
        std::shared_ptr<net::CancelToken> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (onlyIfGeneration != 0 && listGeneration_[slot] != onlyIfGeneration) {
                return false;
            }
            generation = ++listGeneration_[slot];
            placeholder->generation = generation;
            previous = listToken_[slot];
            snapshot_.lists[slot] = placeholder;
            Bump();
        }
        if (previous) {
            previous->Cancel();
        }
        if (!valid) {
            return Refuse(placeholder, [this, slot, generation](std::shared_ptr<ListResult> result) {
                PublishList(slot, generation, std::move(result));
            });
        }
        auto token = worker_.Post([this, slot, generation, clean, path](net::CancelToken& cancel) {
            auto result = std::make_shared<ListResult>();
            result->query = clean;
            result->generation = generation;
            FetchPage(cancel, path, result.get(), true);
            PublishList(slot, generation, std::move(result));
        });
        if (!token) {
            return Refuse(placeholder, [this, slot, generation](std::shared_ptr<ListResult> result) {
                PublishList(slot, generation, std::move(result));
            }, Status::Cancelled);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (listGeneration_[slot] == generation) {
            listToken_[slot] = token;
        }
        return true;
    }

public:
    // ItemDetail of one item (also unlisted ones). 410: Status::Gone with the tombstone in outcome.problem.
    bool Detail(const std::string& id) {
        auto placeholder = std::make_shared<DetailResult>();
        placeholder->id = id;
        placeholder->loading = true;
        const uint64_t generation = PublishPlaceholder(&snapshot_.detail, &detailGeneration_, placeholder);
        auto publish = [this, generation](std::shared_ptr<DetailResult> result) {
            PublishSlot(&snapshot_.detail, &detailGeneration_, generation, std::move(result));
        };
        if (!usable_ || !ValidId(id)) {
            return Refuse(placeholder, publish);
        }
        auto token = worker_.Post([this, id, generation, publish](net::CancelToken& cancel) {
            auto result = std::make_shared<DetailResult>();
            result->id = id;
            result->generation = generation;
            Fetched fetched = Fetch(cancel, "GET", "/items/" + id, std::string(), false, kMaxJsonBytes, true);
            result->outcome = fetched.outcome;
            if (fetched.outcome.Ok()) {
                json::Value value;
                std::string error;
                if (!ParseJson(fetched, &value, &result->outcome) || !ParseDetail(value, &result->detail, &error)) {
                    Malformed(&result->outcome, error);
                    result->detail = pub::Detail();
                } else if (result->detail.card.id != id) {
                    Malformed(&result->outcome, "answers another id");
                    result->detail = pub::Detail();
                }
                result->fetchedAt = detail::NowUnixMs();
            }
            publish(std::move(result));
        });
        return token ? true : Refuse(placeholder, publish, Status::Cancelled);
    }

    // The source of a version for reading (GET .../versions/{n}/source, not counted), checked against sha256.
    bool Source(const std::string& id, int number, const std::string& sha256) {
        return FetchFile(&snapshot_.source, &sourceGeneration_, id, number, sha256, false);
    }

    // The file of a version to install (GET .../versions/{n}/file, counted as a download), checked against sha256.
    // The installer writes FileResult::bytes as they are.
    bool File(const std::string& id, int number, const std::string& sha256) {
        return FetchFile(&snapshot_.file, &fileGeneration_, id, number, sha256, true);
    }

    // The latest state of installed items (POST /items/lookup, in chunks of 100, without the install key): an update
    // when latest_version.number is greater than the installed number.
    bool Lookup(std::vector<std::string> ids) {
        return StartLookup(&snapshot_.lookup, &lookupGeneration_, std::move(ids));
    }

    // The cards of the items this install starred (the Starred tabs), the same call in its own slot.
    bool LookupStarred(std::vector<std::string> ids) {
        return StartLookup(&snapshot_.starred, &starredGeneration_, std::move(ids));
    }

    // The files revoked as malicious (GET /revocations); the installer holds the matching scripts.
    bool Revocations() {
        auto placeholder = std::make_shared<RevocationsResult>();
        placeholder->loading = true;
        const uint64_t generation = PublishPlaceholder(&snapshot_.revocations, &revocationGeneration_, placeholder);
        auto publish = [this, generation](std::shared_ptr<RevocationsResult> result) {
            PublishSlot(&snapshot_.revocations, &revocationGeneration_, generation, std::move(result));
        };
        if (!usable_) {
            return Refuse(placeholder, publish);
        }
        auto token = worker_.Post([this, publish](net::CancelToken& cancel) {
            auto result = std::make_shared<RevocationsResult>();
            Fetched fetched = Fetch(cancel, "GET", "/revocations", std::string(), false, kMaxRevocationBytes, true);
            result->outcome = fetched.outcome;
            if (!fetched.outcome.Ok() && UseCacheFor(fetched.outcome.status) && !fetched.cachedBody.empty()) {
                fetched.body = fetched.cachedBody;
                result->fromCache = true;
            }
            if (fetched.outcome.Ok() || result->fromCache) {
                json::Value value;
                std::string error;
                Outcome parsed = result->outcome;
                if (!ParseJson(fetched, &value, &parsed, result->fromCache)
                        || !ParseRevocations(value, &result->files, &result->dropped, &error)) {
                    Malformed(&result->outcome, error.empty() ? parsed.message : error);
                    result->files.clear();
                    result->fromCache = false;
                }
                result->fetchedAt = result->fromCache ? fetched.cachedAt : detail::NowUnixMs();
            }
            publish(std::move(result));
        });
        return token ? true : Refuse(placeholder, publish, Status::Cancelled);
    }

    // Stars (true) or unstars (false) an item for this install: one star per install and item, starring again
    // changes nothing. Status::NoIdentity without a usable install key, Status::Conflict for one's own item.
    bool Star(const std::string& id, bool starred) {
        auto placeholder = std::make_shared<StarResult>();
        placeholder->id = id;
        placeholder->starred = starred;
        placeholder->loading = true;
        const uint64_t generation = PublishPlaceholder(&snapshot_.star, &starGeneration_, placeholder);
        auto publish = [this, generation](std::shared_ptr<StarResult> result) {
            PublishSlot(&snapshot_.star, &starGeneration_, generation, std::move(result));
        };
        if (!usable_ || !ValidId(id)) {
            return Refuse(placeholder, publish);
        }
        auto token = worker_.Post([this, id, starred, publish](net::CancelToken& cancel) {
            auto result = std::make_shared<StarResult>();
            result->id = id;
            result->starred = starred;
            Fetched fetched = Fetch(cancel, starred ? "PUT" : "DELETE", "/items/" + id + "/star", std::string(), true,
                    16 * 1024, false);
            result->outcome = fetched.outcome;
            publish(std::move(result));
        });
        return token ? true : Refuse(placeholder, publish, Status::Cancelled);
    }

    // The dry run of the publish checks for a file (POST /items/check): shown before Publish. ref: returned as is.
    bool
    Check(Kind kind, const std::string& fileName, const std::string& bytes, const std::string& ref = std::string()) {
        auto placeholder = std::make_shared<CheckResult>();
        placeholder->kind = kind;
        placeholder->fileName = fileName;
        placeholder->sentSha256 = net::Sha256Hex(bytes);
        placeholder->ref = ref;
        placeholder->loading = true;
        const uint64_t generation = PublishPlaceholder(&snapshot_.check, &checkGeneration_, placeholder);
        auto publish = [this, generation](std::shared_ptr<CheckResult> result) {
            PublishSlot(&snapshot_.check, &checkGeneration_, generation, std::move(result));
        };
        const bool valid = detail::ValidUploadName(fileName, kind == Kind::Script ? ".luau" : ".ini") && !bytes.empty()
                           && bytes.size() <= kMaxFileBytes;
        if (!usable_ || !valid) {
            return Refuse(placeholder, publish);
        }
        json::Value body = json::Value::MakeObject();
        body.Set("kind", json::Value::MakeString(KindName(kind)));
        body.Set("file", detail::FileJson(fileName, bytes));
        auto token = worker_.Post([this, placeholder, publish, text = json::Serialize(body)](net::CancelToken& cancel) {
            auto result = std::make_shared<CheckResult>(*placeholder);
            result->loading = false;
            Fetched fetched = Fetch(cancel, "POST", "/items/check", text, true, kMaxJsonBytes, false);
            result->outcome = fetched.outcome;
            json::Value value;
            std::string error;
            if (fetched.outcome.Ok()
                && (!ParseJson(fetched, &value, &result->outcome)
                    || !ParseCheckSummary(value, &result->check, &error))) {
                Malformed(&result->outcome, error);
            }
            publish(std::move(result));
        });
        return token ? true : Refuse(placeholder, publish, Status::Cancelled);
    }

    // Publishes a new item with its first version (POST /items): final and immediate once the server's checks pass.
    bool Publish(const PublishRequest& request) { return StartPublish(request, std::string()); }

    // A new version of an item this install owns (POST /items/{id}/versions, with its changelog). The server refuses
    // it for any other install.
    bool PublishVersion(const std::string& id, const PublishRequest& request) {
        return StartPublish(request, id.empty() ? std::string("-") : id);
    }

    // Reports an item to the moderators (POST /reports): category is one of malicious, stolen, spam, inappropriate,
    // broken, rules. Anonymous: the install key only lets the server limit how often one install reports.
    bool Report(const std::string& id, const std::string& category, const std::string& details) {
        auto placeholder = std::make_shared<ReportResult>();
        placeholder->id = id;
        placeholder->loading = true;
        const uint64_t generation = PublishPlaceholder(&snapshot_.report, &reportGeneration_, placeholder);
        auto publish = [this, generation](std::shared_ptr<ReportResult> result) {
            PublishSlot(&snapshot_.report, &reportGeneration_, generation, std::move(result));
        };
        if (!usable_ || !ValidId(id) || !KnownReasonCategory(category)
            || !detail::ValidText(details, kMaxReportChars, detail::TextRule::MultiLine)) {
            return Refuse(placeholder, publish);
        }
        json::Value body = json::Value::MakeObject();
        body.Set("item_id", json::Value::MakeString(id));
        body.Set("category", json::Value::MakeString(category));
        body.Set("details", json::Value::MakeString(details));
        auto token = worker_.Post([this, id, publish, text = json::Serialize(body)](net::CancelToken& cancel) {
            auto result = std::make_shared<ReportResult>();
            result->id = id;
            Fetched fetched = Fetch(cancel, "POST", "/reports", text, true, 16 * 1024, false);
            result->outcome = fetched.outcome;
            publish(std::move(result));
        });
        return token ? true : Refuse(placeholder, publish, Status::Cancelled);
    }

private:
    struct Fetched {
        Outcome outcome;
        std::string body;
        std::string contentType;
        std::string cachedBody;   // the kept body for that URL, when there is one (offline fallback)
        int64_t cachedAt = 0;
        net::Response response;
    };

    struct CacheEntry {
        std::string url;
        std::string etag;
        std::string body;
        std::string contentType;
        int64_t fetchedAt = 0;
    };

    static bool UseCacheFor(Status status) {
        return status == Status::Offline || status == Status::Timeout || status == Status::ServerError
                || status == Status::Maintenance || status == Status::RateLimited;
    }

    static void Malformed(Outcome* outcome, const std::string& error) {
        if (outcome->status == Status::Ok) {
            outcome->status = Status::Malformed;
        }
        if (outcome->message.empty()) {
            outcome->message = error.empty() ? "malformed answer" : error;
        }
    }

    void Bump() {
        snapshot_.version = version_.fetch_add(1) + 1;
    }

    template <typename Result>
    uint64_t PublishPlaceholder(std::shared_ptr<const Result>* slot, uint64_t* generation,
            const std::shared_ptr<Result>& placeholder) {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t current = ++*generation;
        *slot = placeholder;
        Bump();
        return current;
    }

    template <typename Result>
    void PublishSlot(std::shared_ptr<const Result>* slot, const uint64_t* generation, uint64_t expected,
            std::shared_ptr<Result> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (*generation != expected) {
            return;  // a newer request of this slot was made: this answer is stale
        }
        *slot = std::move(result);
        Bump();
    }

    void PublishList(size_t slot, uint64_t generation, std::shared_ptr<ListResult> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (listGeneration_[slot] != generation) {
            return;
        }
        snapshot_.lists[slot] = std::move(result);
        Bump();
    }

    // Publishes a copy of the placeholder that is no longer loading, with `status`.
    template <typename Result, typename Publish>
    static bool Refuse(const std::shared_ptr<Result>& placeholder, Publish&& publish, Status status = Status::Invalid) {
        auto result = std::make_shared<Result>(*placeholder);
        result->loading = false;
        result->outcome.status = status;
        result->outcome.message = status == Status::Invalid ? "refused before any request" : "the client is stopped";
        publish(std::move(result));
        return false;
    }

    static bool ListPath(const ListQuery& query, const std::string& cursor, std::string* path) {
        std::string target = "/items?kind=" + std::string(KindName(query.kind)) + "&sort=" + SortName(query.sort);
        if (!query.text.empty()) {
            target += "&q=" + detail::PercentEncode(query.text);
        }
        if (query.tags.size() > kMaxTags || query.categories.size() > kMaxTags) {
            return false;
        }
        for (const std::string& tag : query.tags) {
            if (!ValidTag(tag)) {
                return false;
            }
            target += "&tag=" + tag;
        }
        for (const std::string& category : query.categories) {
            if (!KnownCategory(category) || query.kind != Kind::Config) {
                return false;
            }
            target += "&category=" + category;
        }
        if (query.reviewedOnly) {
            target += "&reviewed=true";
        }
        if (!query.compat.empty()) {
            int parts[3] = {};
            if (!ParseNickelVersion(query.compat, parts)) {
                return false;
            }
            target += "&compat=" + std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "."
                    + std::to_string(parts[2]);
        }
        target += "&limit=" + std::to_string(query.limit);
        if (!cursor.empty()) {
            if (!ValidCursor(cursor)) {
                return false;
            }
            target += "&cursor=" + detail::PercentEncode(cursor);
        }
        *path = std::move(target);
        return true;
    }

    // One page into `result` (outcome, items, cursor); with `fallback`, a network failure uses the kept body.
    void FetchPage(net::CancelToken& cancel, const std::string& path, ListResult* result, bool fallback) {
        Fetched fetched = Fetch(cancel, "GET", path, std::string(), false, kMaxJsonBytes, true);
        result->outcome = fetched.outcome;
        if (!fetched.outcome.Ok()) {
            if (!fallback || !UseCacheFor(fetched.outcome.status) || fetched.cachedBody.empty()) {
                return;
            }
            fetched.body = fetched.cachedBody;
            result->fromCache = true;
        }
        json::Value value;
        Page page;
        std::string error;
        Outcome parsed = result->outcome;
        if (!ParseJson(fetched, &value, &parsed, result->fromCache) || !ParsePage(value, &page, &error)) {
            Malformed(&result->outcome, error.empty() ? parsed.message : error);
            result->fromCache = false;
            return;
        }
        result->items = std::move(page.items);
        result->nextCursor = std::move(page.nextCursor);
        result->totalEstimate = page.totalEstimate;
        result->dropped = page.dropped;
        result->fetchedAt = result->fromCache ? fetched.cachedAt : detail::NowUnixMs();
    }

    bool FetchFile(std::shared_ptr<const FileResult>* slot, uint64_t* generationSlot, const std::string& id,
            int number, const std::string& sha256, bool install) {
        auto placeholder = std::make_shared<FileResult>();
        placeholder->id = id;
        placeholder->number = number;
        placeholder->sha256 = sha256;
        placeholder->install = install;
        placeholder->loading = true;
        const uint64_t generation = PublishPlaceholder(slot, generationSlot, placeholder);
        auto publish = [this, slot, generationSlot, generation](std::shared_ptr<FileResult> result) {
            PublishSlot(slot, generationSlot, generation, std::move(result));
        };
        if (!usable_ || !ValidId(id) || number < 1 || !ValidSha256(sha256)) {
            return Refuse(placeholder, publish);
        }
        auto token = worker_.Post([this, id, number, sha256, install, generation, publish](net::CancelToken& cancel) {
            auto result = std::make_shared<FileResult>();
            result->id = id;
            result->number = number;
            result->sha256 = sha256;
            result->install = install;
            result->generation = generation;
            const std::string path = "/items/" + id + "/versions/" + std::to_string(number) + (install ? "/file"
                                                                                                      : "/source");
            Fetched fetched = Fetch(cancel, "GET", path, std::string(), false, kMaxFileBytes, false, "*/*");
            result->outcome = fetched.outcome;
            if (fetched.outcome.Ok()) {
                const std::string* digest = fetched.response.FindHeader("Content-Digest");
                const std::string* etag = fetched.response.FindHeader("ETag");
                const bool etagOk = etag == nullptr || etag->compare(0, 2, "W/") == 0 || *etag == "\"" + sha256 + "\"";
                if (digest == nullptr || !net::ContentDigestMatches(*digest, fetched.body)
                        || net::Sha256Hex(fetched.body) != sha256 || !etagOk) {
                    result->outcome.status = Status::DigestMismatch;
                    result->outcome.message = digest == nullptr ? "no Content-Digest" : "the bytes do not match";
                } else {
                    result->bytes = std::move(fetched.body);
                }
            }
            publish(std::move(result));
        });
        return token ? true : Refuse(placeholder, publish, Status::Cancelled);
    }

    bool
    StartLookup(std::shared_ptr<const LookupResult>* slot, uint64_t* generationSlot, std::vector<std::string> ids) {
        std::vector<std::string> unique;
        bool valid = usable_;
        for (const std::string& id : ids) {
            valid = valid && ValidId(id);
            bool repeated = false;
            for (const std::string& seen : unique) {
                repeated = repeated || seen == id;
            }
            if (!repeated) {
                unique.push_back(id);
            }
        }
        auto placeholder = std::make_shared<LookupResult>();
        placeholder->ids = unique;
        placeholder->loading = true;
        const uint64_t generation = PublishPlaceholder(slot, generationSlot, placeholder);
        auto publish = [this, slot, generationSlot, generation](std::shared_ptr<LookupResult> result) {
            PublishSlot(slot, generationSlot, generation, std::move(result));
        };
        if (!valid) {
            return Refuse(placeholder, publish);
        }
        auto token = worker_.Post([this, unique, publish](net::CancelToken& cancel) {
            auto result = std::make_shared<LookupResult>();
            result->ids = unique;
            for (size_t start = 0; start < unique.size(); start += kMaxLookupIds) {
                const size_t end = (std::min)(unique.size(), start + kMaxLookupIds);
                std::vector<std::string> chunk(unique.begin() + static_cast<std::ptrdiff_t>(start),
                                               unique.begin() + static_cast<std::ptrdiff_t>(end));
                json::Value list = json::Value::MakeArray();
                for (const std::string& id : chunk) {
                    list.Push(json::Value::MakeString(id));
                }
                json::Value body = json::Value::MakeObject();
                body.Set("ids", std::move(list));
                Fetched fetched =
                        Fetch(cancel, "POST", "/items/lookup", json::Serialize(body), false, kMaxJsonBytes, false);
                result->outcome = fetched.outcome;
                std::vector<LookupEntry> entries;
                json::Value value;
                std::string error;
                if (fetched.outcome.Ok()
                    && (!ParseJson(fetched, &value, &result->outcome)
                        || !ParseLookup(value, chunk, &entries, &error))) {
                    Malformed(&result->outcome, error);
                }
                if (!result->outcome.Ok()) {
                    result->entries.clear();
                    break;
                }
                for (LookupEntry& entry : entries) {
                    result->entries.push_back(std::move(entry));
                }
            }
            result->fetchedAt = detail::NowUnixMs();
            publish(std::move(result));
        });
        return token ? true : Refuse(placeholder, publish, Status::Cancelled);
    }

    // POST /items (updateOf empty) or POST /items/{updateOf}/versions.
    bool StartPublish(const PublishRequest& request, const std::string& updateOf) {
        const bool update = !updateOf.empty();
        auto placeholder = std::make_shared<PublishResult>();
        placeholder->kind = request.kind;
        placeholder->ref = request.ref;
        placeholder->update = update;
        placeholder->itemId = update ? updateOf : std::string();
        placeholder->sentSha256 = net::Sha256Hex(request.bytes);
        placeholder->loading = true;
        const uint64_t generation = PublishPlaceholder(&snapshot_.publish, &publishGeneration_, placeholder);
        auto publish = [this, generation](std::shared_ptr<PublishResult> result) {
            PublishSlot(&snapshot_.publish, &publishGeneration_, generation, std::move(result));
        };
        const std::string problem = detail::PublishProblem(request, update);
        if (!usable_ || !problem.empty() || (update && !ValidId(updateOf))) {
            auto refused = std::make_shared<PublishResult>(*placeholder);
            refused->loading = false;
            refused->outcome.status = Status::Invalid;
            refused->outcome.message = problem.empty() ? "refused before any request" : problem;
            publish(std::move(refused));
            return false;
        }
        json::Value body = json::Value::MakeObject();
        if (update) {
            body.Set("changelog", json::Value::MakeString(request.changelog));
        } else {
            body.Set("kind", json::Value::MakeString(KindName(request.kind)));
            body.Set("name", json::Value::MakeString(request.name));
            body.Set("summary", json::Value::MakeString(request.summary));
            body.Set("description", json::Value::MakeString(request.description));
            json::Value tags = json::Value::MakeArray();
            for (const std::string& tag : request.tags) {
                tags.Push(json::Value::MakeString(tag));
            }
            body.Set("tags", std::move(tags));
            body.Set("author_name", json::Value::MakeString(request.authorName));
        }
        body.Set("file", detail::FileJson(request.fileName, request.bytes));
        const std::string path = update ? "/items/" + updateOf + "/versions" : std::string("/items");
        auto token = worker_.Post(
                [this, placeholder, publish, path, text = json::Serialize(body)](net::CancelToken& cancel) {
                    auto result = std::make_shared<PublishResult>(*placeholder);
                    result->loading = false;
                    Fetched fetched = Fetch(cancel, "POST", path, text, true, kMaxJsonBytes, false);
                    result->outcome = fetched.outcome;
                    json::Value value;
                    if (fetched.outcome.Ok()) {
                        if (!ParseJson(fetched, &value, &result->outcome) || !ReadPublished(value, result.get())) {
                            Malformed(&result->outcome, "the published item");
                        }
                    }
                    publish(std::move(result));
                });
        return token ? true : Refuse(placeholder, publish, Status::Cancelled);
    }

    // The answer of a publish: { item, version } or the item itself; a version answer { version } or the version.
    static bool ReadPublished(const json::Value& value, PublishResult* result) {
        if (!value.IsObject()) {
            return false;
        }
        const json::Value* item = value.Find("item");
        const json::Value* version = value.Find("version");
        const json::Value& itemObject = item != nullptr && item->IsObject() ? *item : value;
        const json::Value& versionObject = version != nullptr && version->IsObject() ? *version : value;
        std::string ignored;
        if (!result->update) {
            std::string id;
            const detail::Reader read{itemObject, &ignored, ""};
            if (!read.Pattern("id", &id, true, ValidId)) {
                return false;
            }
            result->itemId = id;
        }
        const json::Value* number = versionObject.Find("number");
        int64_t n = 0;
        if (number != nullptr && number->GetInt64(&n) && n >= 1 && n <= 1000000) {
            result->versionNumber = static_cast<int>(n);
        } else if (!result->update) {
            result->versionNumber = 1;
        }
        const json::Value* state = (result->update ? versionObject : itemObject).Find("state");
        if (state != nullptr && state->IsString()
            && detail::ValidText(state->AsString(), 16, detail::TextRule::OneLine)) {
            result->state = state->AsString();
        }
        return true;
    }

    // Seconds left of a Retry-After, or 0.
    int RateLimitRemaining() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (now >= blockedUntil_) {
            return 0;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(blockedUntil_ - now).count();
        return static_cast<int>((left + 999) / 1000);
    }

    void BlockFor(int seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        if (until > blockedUntil_) {
            blockedUntil_ = until;
        }
    }

    static Status FromNet(net::Error error) {
        switch (error) {
            case net::Error::Cancelled:
                return Status::Cancelled;
            case net::Error::Timeout:
                return Status::Timeout;
            case net::Error::TooLarge:
                return Status::TooLarge;
            case net::Error::BadUrl:
            case net::Error::HostNotAllowed:
            case net::Error::BadRequest:
                return Status::Invalid;
            case net::Error::Connect:
            case net::Error::Tls:
            case net::Error::Network:
                return Status::Offline;
            default:
                return Status::ServerError;
        }
    }

    static Status FromHttp(int status) {
        switch (status) {
            case 400:
                return Status::BadRequest;
            case 401:
                return Status::Unauthorized;
            case 403:
                return Status::Forbidden;
            case 404:
                return Status::NotFound;
            case 409:
                return Status::Conflict;
            case 410:
                return Status::Gone;
            case 422:
                return Status::Rejected;
            case 413:
                return Status::TooLarge;
            case 429:
                return Status::RateLimited;
            case 503:
                return Status::Maintenance;
            default:
                return Status::ServerError;
        }
    }

    static int RetryAfterHeader(const net::Response& response) {
        const std::string* header = response.FindHeader("Retry-After");
        if (header == nullptr || header->empty() || header->size() > 6) {
            return 0;
        }
        int seconds = 0;
        for (const char c : *header) {
            if (c < '0' || c > '9') {
                return 0;
            }
            seconds = seconds * 10 + (c - '0');
        }
        return seconds;
    }

    // The body as JSON: an application/json answer that parses within the size cap.
    static bool ParseJson(const Fetched& fetched, json::Value* value, Outcome* outcome, bool fromCache = false) {
        const std::string& type = fromCache ? std::string("application/json") : fetched.contentType;
        if (!net::detail::StartsWithNoCase(type, "application/json")) {
            Malformed(outcome, "not application/json");
            return false;
        }
        json::Limits limits;
        limits.maxBytes = kMaxRevocationBytes;
        json::ParseError error;
        if (!json::Parse(fetched.body, value, &error, limits)) {
            Malformed(outcome, "invalid JSON: " + error.message);
            return false;
        }
        return true;
    }

    // One request to the API; `cacheable` GETs keep their ETag and body and send If-None-Match.
    Fetched Fetch(net::CancelToken& cancel, const char* method, const std::string& path, const std::string& body,
            bool authorized, size_t maxBytes, bool cacheable, const char* accept = "application/json") {
        Fetched fetched;
        net::Request request;
        request.method = method;
        request.url = config_.baseUrl + path;
        request.maxBytes = maxBytes;
        request.timeoutMs = config_.timeoutMs;
        request.headers.push_back({"Accept", accept});
        CacheEntry kept;
        if (cacheable) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const CacheEntry& entry : cache_) {
                if (entry.url == request.url) {
                    kept = entry;
                }
            }
        }
        fetched.cachedBody = kept.body;
        fetched.cachedAt = kept.fetchedAt;
        if (const int wait = RateLimitRemaining(); wait > 0) {
            fetched.outcome.status = Status::RateLimited;
            fetched.outcome.retryAfter = wait;
            fetched.outcome.message = "waiting for Retry-After";
            return fetched;
        }
        if (authorized) {
            std::string header;
            if (identity_ == nullptr || !identity_->Authorization(&header)) {
                fetched.outcome.status = Status::NoIdentity;
                fetched.outcome.message = "no install key";
                return fetched;
            }
            request.headers.push_back({"Authorization", std::move(header)});
        }
        if (!body.empty()) {
            request.headers.push_back({"Content-Type", "application/json"});
            request.body = body;
        }
        if (!kept.etag.empty()) {
            request.headers.push_back({"If-None-Match", kept.etag});
        }
        fetched.response = http_.Send(request, &cancel);
        const net::Response& response = fetched.response;
        fetched.outcome.httpStatus = response.status;
        if (response.error != net::Error::None) {
            fetched.outcome.status = FromNet(response.error);
            fetched.outcome.message = net::ErrorName(response.error);
            return fetched;
        }
        if (const std::string* type = response.FindHeader("Content-Type")) {
            fetched.contentType = *type;
        }
        if (response.status == 304) {
            if (!cacheable || kept.body.empty()) {
                fetched.outcome.status = Status::Malformed;
                fetched.outcome.message = "304 without a kept answer";
                return fetched;
            }
            fetched.body = kept.body;
            fetched.contentType = kept.contentType;
            fetched.outcome.notModified = true;
            fetched.cachedAt = detail::NowUnixMs();
            Remember(kept.url, kept.etag, kept.body, kept.contentType);
            return fetched;
        }
        if (response.status >= 200 && response.status < 300) {
            fetched.body = response.body;
            const std::string* etag = response.FindHeader("ETag");
            if (cacheable && etag != nullptr && !etag->empty() && etag->size() <= 200
                    && net::ValidHeader("If-None-Match", *etag)) {
                Remember(request.url, *etag, response.body, fetched.contentType);
            }
            return fetched;
        }
        fetched.outcome.status = FromHttp(response.status);
        json::Value problem;
        if (json::Parse(response.body, &problem)) {
            fetched.outcome.problem = ParseProblem(problem, response.status);
        } else {
            fetched.outcome.problem.status = response.status;
        }
        fetched.outcome.message = "HTTP " + std::to_string(response.status);
        if (response.status == 429 || response.status == 503) {
            int wait = RetryAfterHeader(response);
            if (wait <= 0) {
                wait = fetched.outcome.problem.retryAfter;
            }
            if (wait <= 0 && response.status == 429) {
                wait = config_.defaultRetryAfterSeconds;
            }
            wait = wait > 3600 ? 3600 : wait;
            fetched.outcome.retryAfter = wait;
            if (response.status == 429 && wait > 0) {
                BlockFor(wait);
            }
        }
        return fetched;
    }

    // Drops the kept answer of `url`, so its next request goes without If-None-Match.
    void Forget(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < cache_.size(); ++i) {
            if (cache_[i].url == url) {
                cache_.erase(cache_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    void Remember(const std::string& url, const std::string& etag, const std::string& body,
            const std::string& contentType) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < cache_.size(); ++i) {
            if (cache_[i].url == url) {
                cache_.erase(cache_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (config_.cacheEntries == 0) {
            return;
        }
        while (cache_.size() >= config_.cacheEntries) {
            cache_.erase(cache_.begin());  // the oldest
        }
        cache_.push_back({url, etag, body, contentType, detail::NowUnixMs()});
    }

    ClientConfig config_;
    InstallIdentity* identity_;
    net::HttpsClient http_;
    bool usable_ = false;

    mutable std::mutex mutex_;
    Snapshot snapshot_;
    std::atomic<uint64_t> version_{0};
    uint64_t listGeneration_[2] = {0, 0};
    std::shared_ptr<net::CancelToken> listToken_[2];
    uint64_t detailGeneration_ = 0;
    uint64_t sourceGeneration_ = 0;
    uint64_t fileGeneration_ = 0;
    uint64_t lookupGeneration_ = 0;
    uint64_t revocationGeneration_ = 0;
    uint64_t starredGeneration_ = 0;
    uint64_t starGeneration_ = 0;
    uint64_t checkGeneration_ = 0;
    uint64_t publishGeneration_ = 0;
    uint64_t reportGeneration_ = 0;
    std::vector<CacheEntry> cache_;
    std::chrono::steady_clock::time_point blockedUntil_{};

    net::Worker worker_;  // last: destroyed (and joined) first
};

}  // namespace nickel::pub
