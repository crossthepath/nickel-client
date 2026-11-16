#pragma once
// PUBLIC data as the overlay uses it: item cards, details, versions, lookups, revocations and API problems, parsed
// from the community API's JSON (https://nickelclient.com/api/community/v1, website repo docs/public/api.md and
// openapi.yaml) with strict validation and length caps. No network, no files, no ImGui, no game access:
// tests/public_client_checks.cpp.
//
// Validation is strict because the text comes from the network and is drawn in the game:
// - ids, slugs, handles, tags, labels and hashes must match the API's patterns;
// - text fields are capped (in code points, as the API counts them) and one-line fields hold no control character;
// - numbers must be integers in range; times must be ISO 8601 UTC ("2026-11-15T18:00:00.000Z").
// A list item that fails validation is dropped (never drawn) and counted; a document whose shape is wrong is refused.
// Unknown fields are ignored, and unknown enum values of open lists (capabilities, categories) are dropped, as the API
// asks of clients (api.md, section 1).
//
// Stars are local favourites only (public_local.hpp): the API has no star, no vote and no public star count. There
// are no accounts: authors are display names stored per item at publish time.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "json_value.hpp"

namespace nickel::pub {

// ---------------------------------------------------------------------------------------------------------------
// Limits (api.md, section 1.5)

constexpr size_t kMaxNameChars = 60;
constexpr size_t kMaxSummaryChars = 140;
constexpr size_t kMaxDescriptionChars = 4000;
constexpr size_t kMaxChangelogChars = 2000;
constexpr size_t kMaxLabelChars = 32;
constexpr size_t kMaxDisplayNameChars = 32;
constexpr size_t kMaxTags = 5;
constexpr size_t kMaxPageItems = 50;
constexpr size_t kMaxDetailVersions = 10;
constexpr size_t kMaxLookupIds = 100;
constexpr size_t kMaxRevocations = 10000;
constexpr size_t kMaxReasonChars = 300;
constexpr size_t kMaxProblemTextChars = 500;
constexpr size_t kMaxReportChars = 1000;
constexpr size_t kMaxScanNotes = 50; // warnings or errors of a check kept
constexpr size_t kMaxCursorBytes = 512;
constexpr size_t kMaxQueryChars = 64;
constexpr size_t kMaxFileBytes = 200 * 1024;   // one version file, after base64 decoding on the server
constexpr size_t kMaxJsonBytes = 256 * 1024;   // a list, a detail, a lookup answer
constexpr size_t kMaxRevocationBytes = 1024 * 1024;

// ---------------------------------------------------------------------------------------------------------------
// Enums

enum class Kind : uint8_t { Script, Config };

inline const char* KindName(Kind kind) {
    return kind == Kind::Script ? "script" : "config";
}

inline bool ParseKind(std::string_view text, Kind* out) {
    if (text == "script") {
        *out = Kind::Script;
        return true;
    }
    if (text == "config") {
        *out = Kind::Config;
        return true;
    }
    return false;
}

// The three rankings of the PUBLIC pages, as the API names them: Popular (most downloaded), Trending (recent
// downloads, time-decayed), Recent (newest published). The Starred tab is local (public_local.hpp), not a ranking.
enum class Sort : uint8_t { Trending, Popular, Recent };

inline const char* SortName(Sort sort) {
    switch (sort) {
        case Sort::Popular:
            return "popular";
        case Sort::Recent:
            return "recent";
        case Sort::Trending:
        default:
            return "trending";
    }
}

enum class AuthorKind : uint8_t { User, Legacy, Official };

// What a script's code can do (auth-and-security.md, section 6.3).
enum class Capability : uint8_t {
    Render,
    Input,
    Notify,
    ChatPrint,
    ChatSend,
    ChatCommand,
    ChatRead,
    Storage,
    ModulesRead,
    ModulesControl,
    Movement,
    Combat,
    WorldEdit,
    Inventory,
    Container,
    Pathing,
    Packets,
    Commands,
    Dynamic,
};

inline const char* CapabilityName(Capability capability) {
    static const char* const kNames[] = {"render", "input", "notify", "chat.print", "chat.send", "chat.command",
            "chat.read", "storage", "modules.read", "modules.control", "movement", "combat", "world.edit", "inventory",
            "container", "pathing", "packets", "commands", "dynamic"};
    const size_t index = static_cast<size_t>(capability);
    return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "";
}

inline bool ParseCapability(std::string_view text, Capability* out) {
    for (int i = 0; i <= static_cast<int>(Capability::Dynamic); ++i) {
        if (text == CapabilityName(static_cast<Capability>(i))) {
            *out = static_cast<Capability>(i);
            return true;
        }
    }
    return false;
}

// The capabilities the API marks in bold: shown in the sensitive style before an install.
constexpr bool Sensitive(Capability capability) {
    return capability == Capability::ChatSend || capability == Capability::ChatCommand
            || capability == Capability::ModulesControl || capability == Capability::Dynamic;
}

// Module categories a config changes.
inline bool KnownCategory(std::string_view text) {
    return text == "Combat" || text == "Movement" || text == "Player" || text == "Visuals" || text == "Utility"
            || text == "HUD";
}

// State of an item that a lookup or a 410 reports as no longer available.
enum class MissingState : uint8_t { Hidden, Removed, Deleted, NotFound };

inline bool ParseMissingState(std::string_view text, MissingState* out) {
    if (text == "hidden") {
        *out = MissingState::Hidden;
    } else if (text == "removed") {
        *out = MissingState::Removed;
    } else if (text == "deleted") {
        *out = MissingState::Deleted;
    } else if (text == "not_found") {
        *out = MissingState::NotFound;
    } else {
        return false;
    }
    return true;
}

// Why moderation removed an item (reason_category of a 410 or a lookup).
inline bool KnownReasonCategory(std::string_view text) {
    return text == "malicious" || text == "stolen" || text == "spam" || text == "inappropriate" || text == "broken"
            || text == "rules";
}

// The reason of a report (POST /reports "reason", ReportReason in the API): the list of KnownReasonCategory with
// "other" instead of "rules".
inline bool KnownReportReason(std::string_view text) {
    return text == "malicious" || text == "broken" || text == "stolen" || text == "spam" || text == "inappropriate"
            || text == "other";
}

// What the client knows about an installed item, joined from the install record, the lookup answer and the
// revocation list (public_installer.hpp, LocalStateOf).
enum class Local : uint8_t {
    None,             // not installed
    Installed,        // the installed version is the latest
    UpdateAvailable,  // a newer approved version exists (and was not skipped)
    Modified,         // the file on disk is not the file as installed (local edits)
    Withdrawn,        // the installed version is newer than the latest: it was withdrawn or removed
    Removed,          // deleted by its author or removed by moderation; the file is kept
    Unavailable,      // hidden for now
    NotListed,        // the API no longer knows the id
    Revoked,          // the file is on the revocation list: held, never loaded again by itself
};

// ---------------------------------------------------------------------------------------------------------------
// Data

struct Author {
    std::string handle;
    std::string displayName;
    AuthorKind kind = AuthorKind::User;
    bool verified = false;  // only the official Nickel account
};

struct VersionRef {
    int number = 0;              // the version's identity within its item (from 1)
    std::string label;           // free text shown as v<label>; empty: "version <number>"
    std::string nickelMin;       // empty: any Nickel version
    int64_t createdAt = 0;       // unix ms
    uint32_t size = 0;           // bytes of the file
    std::string sha256;          // of the file, 64 lower-case hex
    bool reviewed = false;       // a moderator read and approved it
    int64_t reviewedAt = 0;      // unix ms, 0 when not reviewed
    std::string changelog;       // ItemDetail.versions only
};

// ItemSummary.stats: { downloads, trending_score }. Other keys are ignored.
struct Stats {
    uint64_t downloads = 0;
    double trendingScore = 0;    // the server's time-decayed recent downloads (Trending sorts on it)
};

struct Card {
    std::string id;
    Kind kind = Kind::Script;
    std::string slug;
    std::string name;
    std::string summary;
    Author author;
    std::vector<std::string> tags;            // slugs
    std::string license;                      // the API's license id (MIT, Apache-2.0, ...)
    bool featured = false;
    VersionRef latest;
    std::vector<Capability> capabilities;     // scripts
    std::vector<std::string> categories;      // configs
    int changedModules = -1;                  // configs, -1 when unknown
    int changedKeybinds = -1;                 // configs, -1 when unknown (null in v1)
    Stats stats;
    int64_t publishedAt = 0;                  // unix ms
    int64_t updatedAt = 0;                    // unix ms, time of the last approved version
};

struct Detail {
    Card card;
    std::string description;                  // plain text, up to 4 000 code points
    std::vector<VersionRef> versions;         // up to 10, newest first, with their changelog
    std::string state;                        // "published" for the public
    std::string stateReason;
    bool listed = true;
    std::string pageUrl;                      // https://nickelclient.com/..., empty when not on that site
};

struct Page {
    std::vector<Card> items;
    std::string nextCursor;                   // empty on the last page
    int64_t totalEstimate = -1;               // -1 when not sent
    size_t dropped = 0;                       // items refused by validation
};

struct Missing {
    std::string id;
    MissingState state = MissingState::NotFound;
    std::string reasonCategory;               // for Removed; empty otherwise
};

// One answer of POST /items/lookup: the item's summary, or why it is missing.
struct LookupEntry {
    std::string id;
    bool found = false;
    Card card;                                // when found
    Missing missing;                          // when not found
};

struct Revocation {
    std::string sha256;
    std::string reason;
    int64_t revokedAt = 0;
};

// One warning or error of the server's file checks (CheckResult, Problem errors[]): what and where.
struct ScanNote {
    std::string field;   // "file", "name", "tags[1]"... ("" when not sent)
    std::string code;    // stable, for programs: obfuscated, require, too_long...
    std::string message; // for the player
    int line = 0;        // 1-based line of the file, 0 when not about a line
};

// The answer of POST /items/check: the dry run of the publish checks (scan of a script, filter of a config).
struct CheckSummary {
    bool ok = false;    // no error: the file can be published
    std::string sha256; // of the file as it would be stored ("" when not sent)
    uint32_t size = 0;
    std::vector<Capability> capabilities; // scripts
    std::vector<ScanNote> warnings;
    std::vector<ScanNote> errors;
};

// An RFC 9457 problem (application/problem+json), read at the top level.
struct Problem {
    int status = 0;
    std::string code;                         // stable, for programs: rate_limited, gone, not_found...
    std::string title;
    std::string detail;
    std::string requestId;
    int retryAfter = 0;                       // seconds (429, 503)
    std::string goneState;                    // 410: "removed" or "deleted"
    std::string reasonCategory;               // 410 removed: malicious, stolen, ...
    std::vector<ScanNote> errors;             // 400 / 422: the fields or file lines that were refused

    // The text to show: detail, else title.
    const std::string& Text() const { return detail.empty() ? title : detail; }
};

// ---------------------------------------------------------------------------------------------------------------
// Text, patterns and times

namespace detail {

inline size_t CodePoints(std::string_view text) {
    size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

inline bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

inline bool IsLowerAlnum(char c) {
    return (c >= 'a' && c <= 'z') || IsDigit(c);
}

enum class TextRule : uint8_t { OneLine, MultiLine };

// Text from a parsed JSON string (valid UTF-8 already): within the cap, and without control characters (a multi-line
// field may hold tabs and line breaks).
inline bool ValidText(std::string_view text, size_t maxChars, TextRule rule) {
    if (CodePoints(text) > maxChars) {
        return false;
    }
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == 0x7F) {
            return false;
        }
        if (u < 0x20) {
            const bool allowed = rule == TextRule::MultiLine && (c == '\n' || c == '\r' || c == '\t');
            if (!allowed) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace detail

// 12-character public id: digits and lower-case letters without i, l, o, u.
inline bool ValidId(std::string_view id) {
    if (id.size() != 12) {
        return false;
    }
    for (const char c : id) {
        if (!detail::IsLowerAlnum(c) || c == 'i' || c == 'l' || c == 'o' || c == 'u') {
            return false;
        }
    }
    return true;
}

inline bool ValidSlug(std::string_view slug) {
    if (slug.size() < 3 || slug.size() > 48) {
        return false;
    }
    for (const char c : slug) {
        if (!detail::IsLowerAlnum(c) && c != '-') {
            return false;
        }
    }
    return true;
}

inline bool ValidHandle(std::string_view handle) {
    if (handle.size() < 3 || handle.size() > 24 || handle[0] < 'a' || handle[0] > 'z') {
        return false;
    }
    for (const char c : handle) {
        if (!detail::IsLowerAlnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

inline bool ValidTag(std::string_view tag) {
    if (tag.size() < 2 || tag.size() > 24) {
        return false;
    }
    for (const char c : tag) {
        if (!detail::IsLowerAlnum(c) && c != '-') {
            return false;
        }
    }
    return true;
}

inline bool ValidSha256(std::string_view text) {
    if (text.size() != 64) {
        return false;
    }
    for (const char c : text) {
        if (!detail::IsDigit(c) && !(c >= 'a' && c <= 'f')) {
            return false;
        }
    }
    return true;
}

// Version label: up to 32 of [0-9A-Za-z.+-].
inline bool ValidLabel(std::string_view label) {
    if (label.size() > kMaxLabelChars) {
        return false;
    }
    for (const char c : label) {
        const bool ok = detail::IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '+'
                || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// License id (MIT, Apache-2.0, GPL-3.0-only, CC0-1.0, NOASSERTION...): 1 to 32 of [0-9A-Za-z.+-].
inline bool ValidLicense(std::string_view license) {
    return !license.empty() && ValidLabel(license);
}

// A cursor is opaque but must be safe to put in a URL after encoding: printable ASCII without spaces.
inline bool ValidCursor(std::string_view cursor) {
    if (cursor.empty() || cursor.size() > kMaxCursorBytes) {
        return false;
    }
    for (const char c : cursor) {
        if (c <= 0x20 || c >= 0x7F) {
            return false;
        }
    }
    return true;
}

// "major.minor.patch" at the start of text (anything after is ignored, "1.1.0-beta" reads 1.1.0).
inline bool ParseNickelVersion(std::string_view text, int out[3]) {
    size_t at = 0;
    for (int part = 0; part < 3; ++part) {
        if (part > 0) {
            if (at >= text.size() || text[at] != '.') {
                return false;
            }
            ++at;
        }
        const size_t start = at;
        long value = 0;
        while (at < text.size() && detail::IsDigit(text[at]) && at - start < 6) {
            value = value * 10 + (text[at] - '0');
            ++at;
        }
        if (at == start) {
            return false;
        }
        out[part] = static_cast<int>(value);
    }
    return true;
}

// True when a version needing nickelMin runs on Nickel `running`. An empty nickelMin runs anywhere; one that cannot
// be read does not (never offer an install the client cannot judge).
inline bool CompatibleWith(std::string_view nickelMin, std::string_view running) {
    if (nickelMin.empty()) {
        return true;
    }
    int need[3] = {};
    int have[3] = {};
    if (!ParseNickelVersion(nickelMin, need) || !ParseNickelVersion(running, have)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (have[i] != need[i]) {
            return have[i] > need[i];
        }
    }
    return true;
}

namespace detail {

// Days since 1970-01-01 of a civil date (proleptic Gregorian).
inline int64_t DaysFromCivil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline void CivilFromDays(int64_t days, int* year, unsigned* month, unsigned* day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (*month <= 2 ? 1 : 0));
}

inline bool ReadDigits(std::string_view text, size_t at, size_t count, int* out) {
    if (at + count > text.size()) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!IsDigit(text[at + i])) {
            return false;
        }
        value = value * 10 + (text[at + i] - '0');
    }
    *out = value;
    return true;
}

}  // namespace detail

// "YYYY-MM-DDTHH:MM:SS[.fraction]Z" (UTC only) to unix milliseconds.
inline bool ParseIsoTime(std::string_view text, int64_t* out) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (text.size() < 20 || text.size() > 30 || text[4] != '-' || text[7] != '-' || text[10] != 'T'
            || text[13] != ':' || text[16] != ':' || text.back() != 'Z'
            || !detail::ReadDigits(text, 0, 4, &year) || !detail::ReadDigits(text, 5, 2, &month)
            || !detail::ReadDigits(text, 8, 2, &day) || !detail::ReadDigits(text, 11, 2, &hour)
            || !detail::ReadDigits(text, 14, 2, &minute) || !detail::ReadDigits(text, 17, 2, &second)) {
        return false;
    }
    int millis = 0;
    if (text.size() > 20) {
        if (text[19] != '.' || text.size() < 22) {
            return false;
        }
        const size_t digits = text.size() - 21;
        int scale = 100;
        for (size_t i = 0; i < digits; ++i) {
            const char c = text[20 + i];
            if (!detail::IsDigit(c)) {
                return false;
            }
            if (scale > 0) {
                millis += (c - '0') * scale;
                scale /= 10;
            }
        }
    }
    static const int kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > kDays[month - 1] || hour > 23 || minute > 59
            || second > 60) {
        return false;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && day == 29 && !leap) {
        return false;
    }
    const int64_t days = detail::DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    *out = ((days * 24 + hour) * 60 + minute) * 60000LL + (second > 59 ? 59 : second) * 1000LL + millis;
    return true;
}

// Unix milliseconds to "YYYY-MM-DDTHH:MM:SS.mmmZ".
inline std::string FormatIsoTime(int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    const int64_t days = ms / 86400000LL;
    const int64_t rest = ms % 86400000LL;
    int year = 0;
    unsigned month = 0, day = 0;
    detail::CivilFromDays(days, &year, &month, &day);
    char buffer[32];
    const int hour = static_cast<int>(rest / 3600000);
    const int minute = static_cast<int>(rest / 60000 % 60);
    const int second = static_cast<int>(rest / 1000 % 60);
    const int millis = static_cast<int>(rest % 1000);
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", year, month, day, hour, minute, second,
            millis);
    return buffer;
}

// ---------------------------------------------------------------------------------------------------------------
// Parsing (from json::Value); each returns false with the failing field in `error`

namespace detail {

struct Reader {
    const json::Value& object;
    std::string* error;
    std::string prefix;

    bool Fail(std::string_view field, const char* why) const {
        if (error != nullptr && error->empty()) {
            *error = prefix + std::string(field) + ": " + why;
        }
        return false;
    }

    // A string member. Missing (or null when nullable) gives an empty string and fails only when required.
    bool String(std::string_view key, std::string* out, bool required, size_t maxChars, TextRule rule,
            bool nullable = false) const {
        const json::Value* value = object.Find(key);
        if (value == nullptr || (nullable && value->IsNull())) {
            out->clear();
            return !required || Fail(key, "missing");
        }
        if (!value->IsString()) {
            return Fail(key, "not a string");
        }
        if (!ValidText(value->AsString(), maxChars, rule)) {
            return Fail(key, "too long or has control characters");
        }
        *out = value->AsString();
        return true;
    }

    // A string member matching `check`. Missing (or null when nullable) gives an empty string.
    template <typename Check>
    bool Pattern(std::string_view key, std::string* out, bool required, Check check, bool nullable = false) const {
        const json::Value* value = object.Find(key);
        if (value == nullptr || (nullable && value->IsNull())) {
            out->clear();
            return !required || Fail(key, "missing");
        }
        if (!value->IsString() || !check(value->AsString())) {
            return Fail(key, "invalid");
        }
        *out = value->AsString();
        return true;
    }

    bool Int(std::string_view key, int64_t* out, bool required, int64_t min, int64_t max) const {
        const json::Value* value = object.Find(key);
        if (value == nullptr) {
            return !required || Fail(key, "missing");
        }
        int64_t number = 0;
        if (!value->GetInt64(&number) || number < min || number > max) {
            return Fail(key, "not an integer in range");
        }
        *out = number;
        return true;
    }

    bool Bool(std::string_view key, bool* out, bool required) const {
        const json::Value* value = object.Find(key);
        if (value == nullptr) {
            return !required || Fail(key, "missing");
        }
        if (!value->IsBool()) {
            return Fail(key, "not a boolean");
        }
        *out = value->AsBool();
        return true;
    }

    // An ISO time; nullable members accept null (0).
    bool Time(std::string_view key, int64_t* out, bool required, bool nullable = false) const {
        const json::Value* value = object.Find(key);
        if (value == nullptr || (nullable && value->IsNull())) {
            *out = 0;
            return !required || Fail(key, "missing");
        }
        if (!value->IsString() || !ParseIsoTime(value->AsString(), out)) {
            return Fail(key, "not an ISO 8601 UTC time");
        }
        return true;
    }
};

constexpr int64_t kMaxCount = 9007199254740991LL;  // 2^53 - 1: counts are exact in a double

}  // namespace detail

inline bool ParseAuthor(const json::Value& value, Author* out, std::string* error = nullptr) {
    if (!value.IsObject()) {
        if (error != nullptr && error->empty()) {
            *error = "author: not an object";
        }
        return false;
    }
    const detail::Reader read{value, error, "author."};
    Author author;
    std::string kind;
    // No accounts: the author is the name given at publish time ("name" in the v2 API, "display_name" in older answers);
    // handle, kind and verified are optional.
    const char* nameKey = value.Find("name") != nullptr ? "name" : "display_name";
    if (!read.Pattern("handle", &author.handle, false, ValidHandle, true)
        || !read.String(nameKey, &author.displayName, true, kMaxDisplayNameChars, detail::TextRule::OneLine)
        || !read.String("kind", &kind, false, 16, detail::TextRule::OneLine, true)
        || !read.Bool("verified", &author.verified, false)) {
        return false;
    }
    if (author.displayName.empty()) {
        return read.Fail(nameKey, "empty");
    }
    if (kind == "user" || kind == "player") {
        author.kind = AuthorKind::User;
    } else if (kind == "legacy") {
        author.kind = AuthorKind::Legacy;
    } else if (kind == "official") {
        author.kind = AuthorKind::Official;
    } else {
        author.kind = AuthorKind::User;  // an unknown kind is drawn as a regular author
    }
    author.verified = author.verified && author.kind == AuthorKind::Official;
    *out = std::move(author);
    return true;
}

inline bool ParseVersionRef(const json::Value& value, VersionRef* out, std::string* error = nullptr,
        const char* prefix = "latest_version.") {
    if (!value.IsObject()) {
        if (error != nullptr && error->empty()) {
            *error = std::string(prefix) + ": not an object";
        }
        return false;
    }
    const detail::Reader read{value, error, prefix};
    VersionRef version;
    int64_t number = 0;
    int64_t size = 0;
    if (!read.Int("number", &number, true, 1, 1000000)
            || !read.Pattern("label", &version.label, false, ValidLabel)
            || !read.String("nickel_min", &version.nickelMin, false, 32, detail::TextRule::OneLine, true)
            || !read.Time("created_at", &version.createdAt, true)
            || !read.Int("size", &size, true, 0, static_cast<int64_t>(kMaxFileBytes))
            || !read.Pattern("sha256", &version.sha256, true, ValidSha256)
            || !read.Bool("reviewed", &version.reviewed, false)
            || !read.Time("reviewed_at", &version.reviewedAt, false, true)
            || !read.String("changelog", &version.changelog, false, kMaxChangelogChars, detail::TextRule::MultiLine)) {
        return false;
    }
    if (!version.nickelMin.empty()) {
        int parts[3] = {};
        if (!ParseNickelVersion(version.nickelMin, parts)) {
            return read.Fail("nickel_min", "not a version");
        }
    }
    version.number = static_cast<int>(number);
    version.size = static_cast<uint32_t>(size);
    *out = std::move(version);
    return true;
}

// An ItemSummary (lists, lookups) into a Card.
inline bool ParseCard(const json::Value& value, Card* out, std::string* error = nullptr) {
    if (!value.IsObject()) {
        if (error != nullptr && error->empty()) {
            *error = "item: not an object";
        }
        return false;
    }
    const detail::Reader read{value, error, ""};
    Card card;
    std::string kind;
    if (!read.Pattern("id", &card.id, true, ValidId)
            || !read.String("kind", &kind, true, 16, detail::TextRule::OneLine)) {
        return false;
    }
    if (!ParseKind(kind, &card.kind)) {
        return read.Fail("kind", "unknown kind");
    }
    if (!read.Pattern("slug", &card.slug, true, ValidSlug)
        || !read.String("name", &card.name, true, kMaxNameChars, detail::TextRule::OneLine)
        || !read.String("summary", &card.summary, true, kMaxSummaryChars, detail::TextRule::OneLine)
        || !read.Pattern("license", &card.license, false, ValidLicense, true)
        || !read.Bool("featured", &card.featured, false) || !read.Time("published_at", &card.publishedAt, true)
        || !read.Time("updated_at", &card.updatedAt, true)) {
        return false;
    }
    if (card.name.empty()) {
        return read.Fail("name", "empty");
    }
    const json::Value* author = value.Find("author");
    if (author == nullptr || author->IsNull()) {
        // the plain display name of an item published without an account
        if (!read.String("author_name", &card.author.displayName, true, kMaxDisplayNameChars, detail::TextRule::OneLine)
            || card.author.displayName.empty()) {
            return read.Fail("author", "missing");
        }
    } else if (!ParseAuthor(*author, &card.author, error)) {
        return false;
    }
    const json::Value* latest = value.Find("latest_version");
    if (latest == nullptr) {
        return read.Fail("latest_version", "missing");
    }
    if (!ParseVersionRef(*latest, &card.latest, error)) {
        return false;
    }
    const json::Value* tags = value.Find("tags");
    if (tags == nullptr || !tags->IsArray() || tags->size() > kMaxTags) {
        return read.Fail("tags", "missing, not an array or more than 5");
    }
    for (size_t i = 0; i < tags->size(); ++i) {
        const json::Value& tag = tags->At(i);
        if (!tag.IsString() || !ValidTag(tag.AsString())) {
            return read.Fail("tags", "invalid tag");
        }
        card.tags.push_back(tag.AsString());
    }
    if (const json::Value* capabilities = value.Find("capabilities")) {
        if (!capabilities->IsArray() || capabilities->size() > 64) {
            return read.Fail("capabilities", "not an array");
        }
        for (size_t i = 0; i < capabilities->size(); ++i) {
            Capability capability{};
            const json::Value& entry = capabilities->At(i);
            if (!entry.IsString()) {
                return read.Fail("capabilities", "not a string");
            }
            if (ParseCapability(entry.AsString(), &capability)) {
                bool repeated = false;
                for (const Capability known : card.capabilities) {
                    repeated = repeated || known == capability;
                }
                if (!repeated) {
                    card.capabilities.push_back(capability);
                }
            }
        }
    }
    if (const json::Value* categories = value.Find("categories")) {
        if (!categories->IsArray() || categories->size() > 16) {
            return read.Fail("categories", "not an array");
        }
        for (size_t i = 0; i < categories->size(); ++i) {
            const json::Value& entry = categories->At(i);
            if (!entry.IsString()) {
                return read.Fail("categories", "not a string");
            }
            if (KnownCategory(entry.AsString())) {
                card.categories.push_back(entry.AsString());
            }
        }
    }
    if (const json::Value* changes = value.Find("changes"); changes != nullptr && !changes->IsNull()) {
        if (!changes->IsObject()) {
            return read.Fail("changes", "not an object");
        }
        const detail::Reader inner{*changes, error, "changes."};
        int64_t moduleCount = -1;
        int64_t keybindCount = -1;
        const json::Value* keys = changes->Find("keybinds");
        if (!inner.Int("modules", &moduleCount, false, 0, 100000)
                || (keys != nullptr && !keys->IsNull() && !inner.Int("keybinds", &keybindCount, false, 0, 100000))) {
            return false;
        }
        card.changedModules = static_cast<int>(moduleCount);
        card.changedKeybinds = static_cast<int>(keybindCount);
    }
    const json::Value* stats = value.Find("stats");
    if (stats == nullptr || !stats->IsObject()) {
        return read.Fail("stats", "missing or not an object");
    }
    const detail::Reader stat{*stats, error, "stats."};
    int64_t downloads = 0;
    if (!stat.Int("downloads", &downloads, true, 0, detail::kMaxCount)) {
        return false;
    }
    if (const json::Value* score = stats->Find("trending_score")) {
        if (!score->IsNumber()) {
            return stat.Fail("trending_score", "not a number");
        }
        card.stats.trendingScore = score->AsDouble();
    }
    card.stats.downloads = static_cast<uint64_t>(downloads);
    *out = std::move(card);
    return true;
}

// True for a page of the website itself: https://nickelclient.com/ followed by any path, with no space, quote, control
// character or other character that has no place in a URL.
inline bool SitePageUrl(std::string_view url) {
    constexpr std::string_view site = "https://nickelclient.com/";
    if (url.size() > 200 || url.compare(0, site.size(), site) != 0 || url.find_first_of(" \"<>\\^`{|}") != url.npos) {
        return false;
    }
    return std::none_of(url.begin(), url.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f;
    });
}

// The page "Open on nickelclient.com" opens: the item's page_url from the server when it is a page of the site
// (SitePageUrl), else the community page of the website for that kind.
inline std::string SitePageFor(Kind kind, const std::string& pageUrl) {
    if (SitePageUrl(pageUrl)) {
        return pageUrl;
    }
    return kind == Kind::Script ? "https://nickelclient.com/community.html#scripts"
                                : "https://nickelclient.com/community.html#configs";
}

inline bool ParseDetail(const json::Value& value, Detail* out, std::string* error = nullptr) {
    Detail detail;
    if (!ParseCard(value, &detail.card, error)) {
        return false;
    }
    const detail::Reader read{value, error, ""};
    std::string pageUrl;
    if (!read.String("description", &detail.description, true, kMaxDescriptionChars, detail::TextRule::MultiLine)
            || !read.String("state", &detail.state, true, 16, detail::TextRule::OneLine)
            || !read.String("state_reason", &detail.stateReason, false, kMaxReasonChars, detail::TextRule::OneLine,
                    true)
            || !read.Bool("listed", &detail.listed, false)
            || !read.String("page_url", &pageUrl, false, 200, detail::TextRule::OneLine)) {
        return false;
    }
    // Only a page of the site itself is ever opened (SitePageUrl); anything else is dropped.
    if (SitePageUrl(pageUrl)) {
        detail.pageUrl = pageUrl;
    }
    const json::Value* versions = value.Find("versions");
    if (versions == nullptr || !versions->IsArray() || versions->size() > kMaxDetailVersions) {
        return read.Fail("versions", "missing, not an array or more than 10");
    }
    for (size_t i = 0; i < versions->size(); ++i) {
        VersionRef version;
        if (!ParseVersionRef(versions->At(i), &version, error, "versions[].")) {
            return false;
        }
        detail.versions.push_back(std::move(version));
    }
    *out = std::move(detail);
    return true;
}

// A page of GET /items: { items, next_cursor, total_estimate }. Invalid items are dropped and counted.
inline bool ParsePage(const json::Value& value, Page* out, std::string* error = nullptr) {
    if (!value.IsObject()) {
        if (error != nullptr && error->empty()) {
            *error = "page: not an object";
        }
        return false;
    }
    const detail::Reader read{value, error, ""};
    const json::Value* items = value.Find("items");
    if (items == nullptr || !items->IsArray() || items->size() > kMaxPageItems) {
        return read.Fail("items", "missing, not an array or more than 50");
    }
    Page page;
    for (size_t i = 0; i < items->size(); ++i) {
        Card card;
        std::string ignored;
        if (!ParseCard(items->At(i), &card, &ignored)) {
            ++page.dropped;
            continue;
        }
        bool repeated = false;
        for (const Card& seen : page.items) {
            repeated = repeated || seen.id == card.id;
        }
        if (!repeated) {
            page.items.push_back(std::move(card));
        }
    }
    const json::Value* cursor = value.Find("next_cursor");
    if (cursor != nullptr && !cursor->IsNull()) {
        if (!cursor->IsString() || !ValidCursor(cursor->AsString())) {
            return read.Fail("next_cursor", "invalid");
        }
        page.nextCursor = cursor->AsString();
    }
    if (!read.Int("total_estimate", &page.totalEstimate, false, 0, detail::kMaxCount)) {
        return false;
    }
    *out = std::move(page);
    return true;
}

// The answer of POST /items/lookup for `ids`: exactly one entry per id, in the same order.
inline bool ParseLookup(const json::Value& value, const std::vector<std::string>& ids, std::vector<LookupEntry>* out,
        std::string* error = nullptr) {
    const json::Value* items = value.IsObject() ? value.Find("items") : nullptr;
    if (items == nullptr || !items->IsArray() || items->size() != ids.size()) {
        if (error != nullptr && error->empty()) {
            *error = "items: missing, not an array or not one entry per id";
        }
        return false;
    }
    std::vector<LookupEntry> entries;
    for (size_t i = 0; i < items->size(); ++i) {
        const json::Value& item = items->At(i);
        LookupEntry entry;
        entry.id = ids[i];
        if (item.IsObject() && item.Find("latest_version") != nullptr) {
            if (!ParseCard(item, &entry.card, error)) {
                return false;
            }
            entry.found = true;
        } else {
            const detail::Reader read{item, error, "items[]."};
            std::string state;
            if (!item.IsObject() || !read.Pattern("id", &entry.missing.id, true, ValidId)
                    || !read.String("state", &state, true, 16, detail::TextRule::OneLine)
                    || !ParseMissingState(state, &entry.missing.state)
                    || !read.Pattern("reason_category", &entry.missing.reasonCategory, false, KnownReasonCategory)) {
                return read.Fail("state", "not an item summary nor a missing item");
            }
        }
        const std::string& answered = entry.found ? entry.card.id : entry.missing.id;
        if (answered != ids[i]) {
            if (error != nullptr && error->empty()) {
                *error = "items[]: answers another id";
            }
            return false;
        }
        entries.push_back(std::move(entry));
    }
    *out = std::move(entries);
    return true;
}

// GET /revocations: { files: [{ sha256, reason, revoked_at }] }. Invalid entries are dropped (counted in `dropped`).
inline bool ParseRevocations(const json::Value& value, std::vector<Revocation>* out, size_t* dropped = nullptr,
        std::string* error = nullptr) {
    const json::Value* files = value.IsObject() ? value.Find("files") : nullptr;
    if (files == nullptr || !files->IsArray() || files->size() > kMaxRevocations) {
        if (error != nullptr && error->empty()) {
            *error = "files: missing or not an array";
        }
        return false;
    }
    std::vector<Revocation> list;
    size_t refused = 0;
    for (size_t i = 0; i < files->size(); ++i) {
        const json::Value& file = files->At(i);
        Revocation revocation;
        std::string ignored;
        const detail::Reader read{file, &ignored, ""};
        if (!file.IsObject() || !read.Pattern("sha256", &revocation.sha256, true, ValidSha256)
                || !read.String("reason", &revocation.reason, false, kMaxReasonChars, detail::TextRule::OneLine, true)
                || !read.Time("revoked_at", &revocation.revokedAt, false, true)) {
            ++refused;
            continue;
        }
        list.push_back(std::move(revocation));
    }
    if (dropped != nullptr) {
        *dropped = refused;
    }
    *out = std::move(list);
    return true;
}

// A list of ScanNote objects ({ field, code, message, line }); invalid entries are dropped, at most kMaxScanNotes kept.
inline void ParseScanNotes(const json::Value* list, std::vector<ScanNote>* out) {
    out->clear();
    for (size_t i = 0; list != nullptr && list->IsArray() && i < list->size() && out->size() < kMaxScanNotes; ++i) {
        const json::Value& entry = list->At(i);
        if (!entry.IsObject()) {
            continue;
        }
        std::string ignored;
        const detail::Reader read{entry, &ignored, ""};
        ScanNote note;
        int64_t line = 0;
        const json::Value* lineValue = entry.Find("line");
        if (!read.String("field", &note.field, false, 64, detail::TextRule::OneLine, true)
            || !read.String("code", &note.code, false, 64, detail::TextRule::OneLine, true)
            || !read.String("message", &note.message, false, kMaxProblemTextChars, detail::TextRule::OneLine, true)
            || (lineValue != nullptr && !lineValue->IsNull() && !read.Int("line", &line, false, 0, 10000000))) {
            continue;
        }
        if (note.message.empty() && note.code.empty()) {
            continue;
        }
        note.line = static_cast<int>(line);
        out->push_back(std::move(note));
    }
}

// The answer of POST /items/check.
inline bool ParseCheckSummary(const json::Value& value, CheckSummary* out, std::string* error = nullptr) {
    if (!value.IsObject()) {
        if (error != nullptr && error->empty()) {
            *error = "check: not an object";
        }
        return false;
    }
    const detail::Reader read{value, error, ""};
    CheckSummary check;
    int64_t size = 0;
    if (!read.Bool("ok", &check.ok, true) || !read.Pattern("sha256", &check.sha256, false, ValidSha256)
        || !read.Int("size", &size, false, 0, static_cast<int64_t>(kMaxFileBytes))) {
        return false;
    }
    check.size = static_cast<uint32_t>(size);
    if (const json::Value* capabilities = value.Find("capabilities");
        capabilities != nullptr && capabilities->IsArray()) {
        for (size_t i = 0; i < capabilities->size() && i < 64; ++i) {
            Capability capability{};
            const json::Value& entry = capabilities->At(i);
            if (entry.IsString() && ParseCapability(entry.AsString(), &capability)
                && std::find(check.capabilities.begin(), check.capabilities.end(), capability)
                           == check.capabilities.end()) {
                check.capabilities.push_back(capability);
            }
        }
    }
    ParseScanNotes(value.Find("warnings"), &check.warnings);
    ParseScanNotes(value.Find("errors"), &check.errors);
    check.ok = check.ok && check.errors.empty();
    *out = std::move(check);
    return true;
}

// A problem body; fields that are missing or invalid stay empty (the HTTP status still says what happened).
inline Problem ParseProblem(const json::Value& value, int httpStatus) {
    Problem problem;
    problem.status = httpStatus;
    if (!value.IsObject()) {
        return problem;
    }
    std::string ignored;
    const detail::Reader read{value, &ignored, ""};
    const auto code = [](std::string_view text) {
        if (text.empty() || text.size() > 64) {
            return false;
        }
        for (const char c : text) {
            if (!detail::IsLowerAlnum(c) && c != '_') {
                return false;
            }
        }
        return true;
    };
    std::string text;
    if (read.Pattern("code", &text, false, code)) {
        problem.code = text;
    }
    if (read.String("title", &text, false, kMaxProblemTextChars, detail::TextRule::OneLine)) {
        problem.title = text;
    }
    if (read.String("detail", &text, false, kMaxProblemTextChars, detail::TextRule::OneLine)) {
        problem.detail = text;
    }
    if (read.String("request_id", &text, false, 64, detail::TextRule::OneLine)) {
        problem.requestId = text;
    }
    int64_t retry = 0;
    if (read.Int("retry_after", &retry, false, 0, 86400)) {
        problem.retryAfter = static_cast<int>(retry);
    }
    if (read.String("state", &text, false, 16, detail::TextRule::OneLine) && (text == "removed" || text == "deleted")) {
        problem.goneState = text;
    }
    if (read.Pattern("reason_category", &text, false, KnownReasonCategory)) {
        problem.reasonCategory = text;
    }
    ParseScanNotes(value.Find("errors"), &problem.errors);
    return problem;
}

}  // namespace nickel::pub
