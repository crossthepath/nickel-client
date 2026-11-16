#pragma once
// What the PUBLIC pages of the menu do, without drawing: the view of each kind (sort, search, selection, the Starred
// tab), the requests they make, the install / update / uninstall flow, revocations, updates, favourites, reports, and
// publishing from OTHER -> Scripting and OTHER -> Configs. public_ui.hpp draws it with the menu's widgets;
// tests/public_page_checks.cpp drives it with recorded requests and hand-made snapshots.
//
// - No sign-in anywhere: publishing and reports carry this install's key (public_identity.hpp); browsing, installing
//   and favourites never create it. Stars are local favourites only, kept in root\public\starred.json
//   (public_local.hpp): starring sends no request and never downloads, installs or runs anything. The Starred tab
//   shows them through the read-only lookup.
// - Publishing is final and immediate once the server's automatic check passes: there is no way to delete, unlist or
//   hide an item from here. The local file stays the author's; edits never change the public copy. Only the install
//   that published an item (root\public\published.json, by fingerprint) is offered "Publish update".
//
// - Nothing is requested while the "Online community (PUBLIC)" setting is off: Tick and the page calls return early,
//   and the menu does not even create the PublicClient.
// - The render thread calls Tick once per frame with the client's latest snapshot. Answers are handled once, when
//   their object first appears (results are immutable objects: a new pointer is a new answer).
// - Scripts installed or updated here are held by the installer (ScriptLibrary::WriteSource): only Start runs them.
// - The search box is sent 700 ms after the last key, from 2 characters (an empty box at once); Enter sends it now.
// No ImGui, no menu globals (tools/boundaries.json).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "public_client.hpp"
#include "public_identity.hpp"
#include "public_installer.hpp"
#include "public_local.hpp"
#include "script_library.hpp"

namespace nickel::pub {

// The Nickel version items must support (ListQuery::compat and the Install check). Kept equal to the launcher's
// <Version> in desktop/Nickel.Desktop.csproj by tests/public_page_checks.cpp.
constexpr const char* kRunningNickel = "1.1.0-beta";

// The requests the page makes: PublicClient in the game (ClientRequests), a recorder in tests.
class PageRequests {
public:
    virtual ~PageRequests() = default;
    virtual bool List(const ListQuery& query) = 0;
    virtual bool More(Kind kind) = 0;
    virtual bool Detail(const std::string& id) = 0;
    virtual bool Source(const std::string& id, int number, const std::string& sha256) = 0;
    virtual bool File(const std::string& id, int number, const std::string& sha256) = 0;
    virtual bool Lookup(std::vector<std::string> ids) = 0;
    virtual bool Revocations() = 0;
    virtual bool LookupStarred(std::vector<std::string> ids) = 0;
    virtual bool Check(Kind kind, const std::string& fileName, const std::string& bytes, const std::string& ref) = 0;
    virtual bool Publish(const PublishRequest& request) = 0;
    virtual bool PublishVersion(const std::string& id, const PublishRequest& request) = 0;
    virtual bool Report(const std::string& id, const std::string& category, const std::string& details) = 0;
};

class ClientRequests : public PageRequests {
public:
    explicit ClientRequests(PublicClient* client) : client_(client) {}

    bool List(const ListQuery& query) override { return client_->List(query); }

    bool More(Kind kind) override { return client_->More(kind); }

    bool Detail(const std::string& id) override { return client_->Detail(id); }

    bool Source(const std::string& id, int number, const std::string& sha256) override {
        return client_->Source(id, number, sha256);
    }

    bool File(const std::string& id, int number, const std::string& sha256) override {
        return client_->File(id, number, sha256);
    }

    bool Lookup(std::vector<std::string> ids) override { return client_->Lookup(std::move(ids)); }

    bool Revocations() override { return client_->Revocations(); }

    bool LookupStarred(std::vector<std::string> ids) override { return client_->LookupStarred(std::move(ids)); }

    bool Check(Kind kind, const std::string& fileName, const std::string& bytes, const std::string& ref) override {
        return client_->Check(kind, fileName, bytes, ref);
    }

    bool Publish(const PublishRequest& request) override { return client_->Publish(request); }

    bool PublishVersion(const std::string& id, const PublishRequest& request) override {
        return client_->PublishVersion(id, request);
    }

    bool Report(const std::string& id, const std::string& category, const std::string& details) override {
        return client_->Report(id, category, details);
    }

private:
    PublicClient* client_;
};

// ---------------------------------------------------------------------------------------------------------------
// Words and numbers shown on the pages

// "950", "12.4k", "124k", "3.1M".
inline std::string FormatCount(uint64_t n) {
    char text[32];
    if (n < 1000) {
        std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(n));
    } else if (n < 100000) {
        std::snprintf(text, sizeof(text), "%.1fk", static_cast<double>(n / 100) / 10.0);
    } else if (n < 1000000) {
        std::snprintf(text, sizeof(text), "%lluk", static_cast<unsigned long long>(n / 1000));
    } else if (n < 100000000) {
        std::snprintf(text, sizeof(text), "%.1fM", static_cast<double>(n / 100000) / 10.0);
    } else {
        std::snprintf(text, sizeof(text), "%lluM", static_cast<unsigned long long>(n / 1000000));
    }
    std::string out = text;
    const size_t zero = out.find(".0");
    if (zero != std::string::npos && zero + 2 < out.size() && (out[zero + 2] == 'k' || out[zero + 2] == 'M')) {
        out.erase(zero, 2);
    }
    return out;
}

// "980 B", "12.3 KB".
inline std::string FormatSize(uint64_t bytes) {
    char text[32];
    if (bytes < 1024) {
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
    }
    return text;
}

// "just now", "5 minutes ago", "3 hours ago", "yesterday", "4 days ago", "2 weeks ago", "3 months ago".
inline std::string FormatAgo(int64_t thenMs, int64_t nowMs) {
    if (thenMs <= 0) {
        return "unknown";
    }
    const int64_t minutes = (nowMs - thenMs) / 60000;
    if (minutes < 1) {
        return "just now";
    }
    if (minutes < 60) {
        return minutes == 1 ? "1 minute ago" : std::to_string(minutes) + " minutes ago";
    }
    if (minutes < 1440) {
        return minutes < 120 ? "1 hour ago" : std::to_string(minutes / 60) + " hours ago";
    }
    if (minutes < 2880) {
        return "yesterday";
    }
    if (minutes < 10080) {
        return std::to_string(minutes / 1440) + " days ago";
    }
    if (minutes < 20160) {
        return "last week";
    }
    if (minutes < 43200) {
        return std::to_string(minutes / 10080) + " weeks ago";
    }
    return minutes < 2 * 43200 ? "1 month ago" : std::to_string(minutes / 43200) + " months ago";
}

// "12 Nov 2026".
inline std::string FormatDay(int64_t ms) {
    const std::string iso = FormatIsoTime(ms); // 2026-11-12T...
    if (iso.size() < 10) {
        return std::string();
    }
    static const char* const kMonths[] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int month = std::atoi(iso.substr(5, 2).c_str());
    const int day = std::atoi(iso.substr(8, 2).c_str());
    if (month < 1 || month > 12) {
        return std::string();
    }
    return std::to_string(day) + " " + kMonths[month - 1] + " " + iso.substr(0, 4);
}

// "v1.3.0", or "version 7" for an empty label.
inline std::string VersionText(const VersionRef& version) {
    return version.label.empty() ? "version " + std::to_string(version.number) : "v" + version.label;
}

// A capability in the plain words of auth-and-security.md, section 6.3.
inline const char* CapabilityText(Capability capability) {
    switch (capability) {
        case Capability::Render:
            return "Draws on the screen";
        case Capability::Input:
            return "Reads your keys and mouse buttons";
        case Capability::Notify:
            return "Shows notifications";
        case Capability::ChatPrint:
            return "Prints messages in your chat";
        case Capability::ChatSend:
            return "Sends chat messages as you";
        case Capability::ChatCommand:
            return "Runs commands as you";
        case Capability::ChatRead:
            return "Reads the chat";
        case Capability::Storage:
            return "Saves its own data";
        case Capability::ModulesRead:
            return "Reads which modules are on";
        case Capability::ModulesControl:
            return "Turns modules on and off and changes their settings";
        case Capability::Movement:
            return "Moves your player";
        case Capability::Combat:
            return "Attacks and aims for you";
        case Capability::WorldEdit:
            return "Places and breaks blocks";
        case Capability::Inventory:
            return "Uses your inventory";
        case Capability::Container:
            return "Moves items in containers";
        case Capability::Pathing:
            return "Walks to places by itself";
        case Capability::Packets:
            return "Reads and changes network packets";
        case Capability::Commands:
            return "Adds chat commands";
        case Capability::Dynamic:
            return "Code that cannot be checked: treated as having every capability";
    }
    return "Unknown capability";
}

// chat.send or chat.command together with input: the script could post what you type.
inline bool ChatLeakWarning(const std::vector<Capability>& capabilities) {
    bool chat = false;
    bool input = false;
    for (const Capability c : capabilities) {
        chat = chat || c == Capability::ChatSend || c == Capability::ChatCommand;
        input = input || c == Capability::Input;
    }
    return chat && input;
}

// One pill per card, by priority.
enum class Badge : uint8_t {
    None,
    Installed,
    Held,
    Running,
    Update,
    Modified,
    Withdrawn,
    Removed,
    Unavailable,
    NotListed,
    Revoked,
    Incompatible
};

inline const char* BadgeText(Badge badge) {
    static const char* const kNames[] = {"",
                                         "INSTALLED",
                                         "HELD",
                                         "RUNNING",
                                         "UPDATE",
                                         "EDITED",
                                         "WITHDRAWN",
                                         "REMOVED",
                                         "UNAVAILABLE",
                                         "NOT LISTED",
                                         "REVOKED",
                                         "NEEDS NEWER NICKEL"};
    const size_t index = static_cast<size_t>(badge);
    return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "";
}

// script: the installed script's entry in ScriptLibrary::List() (nullptr for configs or when it is not listed).
inline Badge BadgeOf(Local local, const overlay::ScriptInfo* script, bool compatible) {
    switch (local) {
        case Local::Revoked:
            return Badge::Revoked;
        case Local::Removed:
            return Badge::Removed;
        case Local::Unavailable:
            return Badge::Unavailable;
        case Local::NotListed:
            return Badge::NotListed;
        case Local::Withdrawn:
            return Badge::Withdrawn;
        case Local::UpdateAvailable:
            return Badge::Update;
        case Local::Modified:
            return Badge::Modified;
        case Local::Installed:
            if (script != nullptr && script->held) {
                return Badge::Held;
            }
            if (script != nullptr && script->state == overlay::ScriptState::Running) {
                return Badge::Running;
            }
            return Badge::Installed;
        case Local::None:
            break;
    }
    return compatible ? Badge::None : Badge::Incompatible;
}

// The sentence for a local state that is not simply installed ("" otherwise).
inline const char* LocalReason(Local local) {
    switch (local) {
        case Local::Revoked:
            return "Revoked by the Nickel team: it is held and never starts by itself.";
        case Local::Removed:
            return "Removed from PUBLIC. Your file is kept.";
        case Local::Unavailable:
            return "Temporarily unavailable. Your file is kept.";
        case Local::NotListed:
            return "No longer listed. Your file is kept.";
        case Local::Withdrawn:
            return "The version you have was withdrawn. You can install the current one.";
        case Local::Modified:
            return "You edited this file since it was installed.";
        default:
            return "";
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The list area

enum class ListView : uint8_t {
    Off,
    Loading,
    Ready,
    Empty,
    NoMatch,
    Offline,
    Unavailable,
    Maintenance,
    RateLimited,
    BadQuery
};

struct ListState {
    ListView view = ListView::Loading;
    std::string message; // the centred sentence (Ready: the note above the grid, "" for none)
    std::string detail;  // the server's problem text, when there is one
    int retryIn = 0;     // seconds before Retry may be pressed (rate limit, maintenance)
    bool fromCache = false;
};

inline std::string KindWord(Kind kind, bool plural) {
    return kind == Kind::Script ? (plural ? "scripts" : "script") : (plural ? "configs" : "config");
}

// Why a starred item is no longer listed, for its row in the Starred tab.
inline std::string MissingText(const Missing& missing) {
    switch (missing.state) {
        case MissingState::Hidden:
            return "Hidden by the Nickel team.";
        case MissingState::Removed:
            return missing.reasonCategory.empty() ? "Removed by the Nickel team."
                                                  : "Removed by the Nickel team (" + missing.reasonCategory + ").";
        case MissingState::Deleted:
            return "Deleted from PUBLIC.";
        case MissingState::NotFound:
        default:
            return "No longer on PUBLIC.";
    }
}

// retryUntil: unix ms until which a 429 or 503 forbids a retry (0: none).
inline ListState ListStateOf(Kind kind, const ListResult* list, bool enabled, int64_t retryUntil, int64_t nowMs) {
    ListState state;
    if (!enabled) {
        state.view = ListView::Off;
        state.message = "PUBLIC is off. It lists the scripts and configs players share on nickelclient.com.";
        return state;
    }
    if (list == nullptr || (list->loading && list->items.empty())) {
        state.view = ListView::Loading;
        return state;
    }
    const Outcome& outcome = list->outcome;
    state.retryIn = retryUntil > nowMs ? static_cast<int>((retryUntil - nowMs + 999) / 1000) : 0;
    state.detail = outcome.problem.Text();
    if (!list->items.empty()) {
        state.view = ListView::Ready;
        state.fromCache = list->fromCache;
        if (list->fromCache) {
            state.message = "Offline \xc2\xb7 showing the list from " + FormatAgo(list->fetchedAt, nowMs);
        } else if (!outcome.Ok() && outcome.status != Status::Cancelled && outcome.status != Status::Loading) {
            state.message = outcome.status == Status::RateLimited
                                    ? "Too many requests. Try again in " + std::to_string(state.retryIn) + " s."
                                    : std::string("More could not be loaded (") + StatusName(outcome.status) + ").";
        }
        return state;
    }
    switch (outcome.status) {
        case Status::Ok:
            if (list->query.text.empty() && list->query.tags.empty() && list->query.categories.empty()
                && !list->query.reviewedOnly) {
                state.view = ListView::Empty;
                state.message = "No " + KindWord(kind, true) + " shared yet. Publish yours from OTHER > "
                                + (kind == Kind::Script ? "Scripting." : "Configs.");
            } else {
                state.view = ListView::NoMatch;
                state.message = "No " + KindWord(kind, true) + " match \"" + list->query.text + "\".";
            }
            break;
        case Status::Offline:
        case Status::Timeout:
            state.view = ListView::Offline;
            state.message = "Can't reach nickelclient.com.";
            break;
        case Status::RateLimited:
            state.view = ListView::RateLimited;
            state.message = "Too many requests. Try again in " + std::to_string(state.retryIn) + " s.";
            break;
        case Status::Maintenance:
            state.view = ListView::Maintenance;
            state.message = state.retryIn > 0
                                    ? "PUBLIC is under maintenance. Back in " + std::to_string(state.retryIn) + " s."
                                    : "PUBLIC is under maintenance.";
            break;
        case Status::BadRequest:
            state.view = ListView::BadQuery;
            state.message = "This search could not be run.";
            break;
        case Status::Cancelled:
        case Status::Loading:
            state.view = ListView::Loading;
            break;
        default:
            state.view = ListView::Unavailable;
            state.message = "PUBLIC is unavailable right now.";
            break;
    }
    return state;
}

// ---------------------------------------------------------------------------------------------------------------
// Config previews (the settings summary): the sections of a config and how many settings each one sets

struct ConfigSection {
    std::string name; // the section, a module id for module sections
    int enabled = -1; // enabled=1 / 0, -1 when the section has no enabled key
    int settings = 0; // keys other than enabled
};

inline std::vector<ConfigSection> ConfigSummary(std::string_view ini) {
    std::vector<ConfigSection> sections;
    size_t at = 0;
    if (ini.size() >= 3 && static_cast<unsigned char>(ini[0]) == 0xEF && static_cast<unsigned char>(ini[1]) == 0xBB
        && static_cast<unsigned char>(ini[2]) == 0xBF) {
        at = 3;
    }
    while (at < ini.size() && sections.size() < 1000) {
        size_t end = ini.find('\n', at);
        if (end == std::string_view::npos) {
            end = ini.size();
        }
        std::string_view line = ini.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']' && line.size() > 2) {
            ConfigSection section;
            section.name = std::string(line.substr(1, line.size() - 2));
            sections.push_back(std::move(section));
            continue;
        }
        const size_t equals = line.find('=');
        if (sections.empty() || equals == std::string_view::npos || equals == 0) {
            continue;
        }
        std::string_view key = line.substr(0, equals);
        while (!key.empty() && key.back() == ' ') {
            key.remove_suffix(1);
        }
        if (key == "enabled") {
            std::string_view value = line.substr(equals + 1);
            while (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            sections.back().enabled = value == "1" || value == "true" ? 1 : 0;
        } else {
            ++sections.back().settings;
        }
    }
    return sections;
}

// The lines of a source (\n, \r\n or \r), at most `maxLines`.
inline std::vector<std::string> SourceLines(std::string_view text, size_t maxLines = 20000) {
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < text.size() && lines.size() < maxLines; ++i) {
        const char c = text[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            lines.push_back(std::move(line));
            line.clear();
        } else if (c == '\t') {
            line += "    ";
        } else {
            line += c;
        }
    }
    if (!line.empty() && lines.size() < maxLines) {
        lines.push_back(std::move(line));
    }
    return lines;
}

// ---------------------------------------------------------------------------------------------------------------
// Files the configs do not carry

// The "Online community (PUBLIC)" setting lives in its own file, root\public\settings.ini, not in the preferences
// ini: configs are copies of that ini (Save, Import, Load), so a config loaded or shared could otherwise turn PUBLIC
// on or off. Only the Settings toggle and the page's Turn on button change it.
inline std::wstring CommunitySettingPath(const std::wstring& root) {
    return root + L"\\public\\settings.ini";
}

inline bool ReadCommunitySetting(const std::wstring& root) {
    return GetPrivateProfileIntW(L"public", L"community", 0, CommunitySettingPath(root).c_str()) == 1;
}

// WritePrivateProfileStringW keeps a file in the ANSI code page unless it starts with a UTF-16LE BOM, which would
// turn an author name such as "Masha" in Cyrillic into "????". The file is made UTF-16LE before the first write; an
// ANSI file of an earlier build is converted once (its text read in the ANSI code page, as Windows wrote it).
inline bool MakeUnicodeIni(const std::wstring& path) {
    std::string bytes;
    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists && !detail::ReadAll(path, &bytes, 64 * 1024)) {
        return false;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return true;
    }
    std::wstring text;
    if (!bytes.empty()) {
        const int count = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (count <= 0) {
            return false;
        }
        text.resize(static_cast<size_t>(count));
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), count);
    }
    std::string utf16 = "\xFF\xFE";
    utf16.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    return detail::ReplaceFile(path, utf16);
}

inline bool WriteCommunitySetting(const std::wstring& root, bool on) {
    detail::MakeFolder(root);
    detail::MakeFolder(root + L"\\public");
    MakeUnicodeIni(CommunitySettingPath(root));
    return WritePrivateProfileStringW(L"public", L"community", on ? L"1" : L"0", CommunitySettingPath(root).c_str())
           != FALSE;
}

// The author name the player publishes under (a display name, no account behind it), in the same file.
inline std::string ReadAuthorName(const std::wstring& root) {
    wchar_t value[128] = {};
    GetPrivateProfileStringW(L"public", L"author_name", L"", value, 128, CommunitySettingPath(root).c_str());
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string text(count > 1 ? static_cast<size_t>(count - 1) : 0, '\0');
    if (count > 1) {
        WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), count, nullptr, nullptr);
    }
    return text;
}

inline bool WriteAuthorName(const std::wstring& root, const std::string& name) {
    detail::MakeFolder(root);
    detail::MakeFolder(root + L"\\public");
    if (!MakeUnicodeIni(CommunitySettingPath(root))) {
        return false; // never store a name the ANSI code page would garble
    }
    return WritePrivateProfileStringW(
                   L"public", L"author_name", detail::ToWide(name).c_str(), CommunitySettingPath(root).c_str())
           != FALSE;
}

// Tags as the player types them ("pvp, hud" or "pvp hud"): lower case, split on commas and spaces, repeated ones
// dropped. False when one is not a valid tag or there are more than 5.
inline bool ParseTags(std::string_view text, std::vector<std::string>* out) {
    std::vector<std::string> tags;
    std::string tag;
    auto flush = [&] {
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) {
            tags.push_back(tag);
        }
        tag.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ' ' || c == '\t') {
            flush();
        } else {
            tag += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    }
    flush();
    if (tags.size() > kMaxTags) {
        return false;
    }
    for (const std::string& each : tags) {
        if (!ValidTag(each)) {
            return false;
        }
    }
    *out = std::move(tags);
    return true;
}

// One section of an ini as GetPrivateProfileSectionW gives it ("key=value\0...\0\0"); empty when it has none.
inline std::wstring ReadIniSection(const std::wstring& path, const wchar_t* section) {
    std::wstring buffer(4096, L'\0');
    while (true) {
        const DWORD got =
                GetPrivateProfileSectionW(section, &buffer[0], static_cast<DWORD>(buffer.size()), path.c_str());
        if (got + 2 < buffer.size() || buffer.size() >= (1u << 22)) {
            buffer.resize(got == 0 ? 0 : got + 1);
            return buffer;
        }
        buffer.assign(buffer.size() * 2, L'\0');
    }
}

// Loads the config `from` over the preferences ini `to` and keeps the player's own sections `keep` (PUBLIC configs:
// [menu] and [fonts], which the site strips, so their keys must not fall back to the defaults). A section the player
// does not have is removed from the result. False when the copy failed (then `to` is unchanged).
inline bool
CopyConfigKeepingSections(const std::wstring& from, const std::wstring& to, const std::vector<const wchar_t*>& keep) {
    std::vector<std::wstring> kept;
    for (const wchar_t* section : keep) {
        kept.push_back(ReadIniSection(to, section));
    }
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        return false;
    }
    for (size_t i = 0; i < keep.size(); ++i) {
        WritePrivateProfileStringW(keep[i], nullptr, nullptr, to.c_str());
        if (!kept[i].empty()) {
            WritePrivateProfileSectionW(keep[i], kept[i].c_str(), to.c_str());
        }
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, to.c_str()); // flush the cached ini
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The page

struct PageOptions {
    std::wstring root;                         // %APPDATA%\Nickel in the game
    overlay::ScriptLibrary* library = nullptr; // ScriptsLibrary()
    InstallIdentity* identity = nullptr;       // the install key the client sends: who owns what was published
    std::string running = kRunningNickel;
    std::function<int64_t()> now; // for the installer's records (tests); the clock when empty
};

// What one kind's page shows and asks for.
struct KindView {
    Sort sort = Sort::Popular;
    std::string applied; // the search text of the list shown
    std::string typed;   // the search box
    int64_t typedAt = 0;
    bool listed = false;                        // its first list was asked for
    bool starredTab = false;                    // the Starred tab is shown instead of a ranking
    std::string selected;                       // the card shown in the side panel ("" for none)
    bool sourceOpen = false;                    // the main panel shows the selected item's source / settings
    std::shared_ptr<const ListResult> seenList; // kept alive: a new answer is a new object
    int64_t retryUntil = 0;
    bool relistDue = false; // a sort or search change waits for the 429 / 503 countdown, then is sent
};

// A message shown on the page itself (the HUD notifications may be off): problems stay until dismissed, the others
// for 8 seconds.
struct PageMessage {
    std::string text;
    int64_t at = 0;
    bool problem = false;
};

enum class Step : uint8_t {
    None,
    Downloading,
    NameConflict,
    Modified,
    Failed
};

struct PendingInstall {
    Step step = Step::None;
    Card card;
    bool update = false;
    std::string bytes; // the verified download, kept while the player answers a question
    std::string message;
    std::shared_ptr<const FileResult> stale; // the file slot when the download was asked (an older answer)
};

// Publishing a local script or config: the check first, then Publish (or Publish update for an item this install
// owns), then the mapping is remembered.
enum class PublishStep : uint8_t {
    None,
    Checking,   // POST /items/check on its way
    Ready,      // the check passed: the form can be sent
    Publishing, // POST /items or /versions on its way (the form cannot be closed)
    Done,
    Failed
};

struct PublishFields {
    std::string name;
    std::string summary;
    std::string description;
    std::string tags; // as typed
    std::string authorName;
    std::string changelog; // Publish update
};

struct PublishDraft {
    PublishStep step = PublishStep::None;
    Kind kind = Kind::Script;
    std::string localName; // the script or config name in this install
    bool update = false;   // this install owns `itemId`: Publish update
    std::string itemId;
    std::string fileName;
    std::string bytes; // what was read and checked: exactly what is published
    std::string sha256;
    CheckSummary check; // when the check answered
    bool checked = false;
    std::string message; // Failed or Done
    std::string warning; // an earlier publish of this file this session got no clear answer: it may be public already
    std::vector<ScanNote> errors;
    std::shared_ptr<const CheckResult> staleCheck;
    std::shared_ptr<const PublishResult> stalePublish;
};

class PublicPage {
public:
    explicit PublicPage(PageOptions options)
        : options_(std::move(options)), installer_(InstallerOptions{options_.root, options_.library, options_.now}),
          starred_(StarredPath(options_.root)), published_(PublishedPath(options_.root)) {}

    // The requests go there (nullptr while PUBLIC is off).
    void SetRequests(PageRequests* requests) { requests_ = requests; }

    bool Online() const { return requests_ != nullptr; }

    Installer& Installs() { return installer_; }

    KindView& View(Kind kind) { return views_[Slot(kind)]; }

    const PendingInstall& Pending() const { return pending_; }

    // A download for an install or update is on its way: Install refuses another one until it is answered.
    bool Busy() const { return pending_.step == Step::Downloading; }

    const std::string& Running() const { return options_.running; }

    // Once per frame on the render thread, menu open or not.
    void Tick(const Snapshot& snap, bool enabled, int64_t nowMs) {
        now_ = nowMs;
        if (!enabled || requests_ == nullptr) {
            if (wasEnabled_) {
                for (KindView& view : views_) {
                    view.listed = false;
                    view.relistDue = false;
                }
                revocationsAt_ = 0;
                lookupAt_ = 0;
                starredAsked_.clear();
            }
            wasEnabled_ = false;
            return;
        }
        wasEnabled_ = true;
        messages_.erase(std::remove_if(messages_.begin(),
                                       messages_.end(),
                                       [&](const PageMessage& m) { return !m.problem && nowMs - m.at > 8000; }),
                        messages_.end());
        if (revocationsAt_ == 0 || nowMs - revocationsAt_ > 3 * 3600 * 1000LL) {
            revocationsAt_ = nowMs;
            requests_->Revocations();
        }
        for (int k = 0; k < 2; ++k) {
            KindView& view = views_[k];
            const ListResult* list = snap.lists[k].get();
            if (snap.lists[k] != view.seenList) {
                view.seenList = snap.lists[k];
                if (list != nullptr && !list->loading && !list->loadingMore
                    && (list->outcome.status == Status::RateLimited || list->outcome.status == Status::Maintenance)) {
                    const int seconds = list->outcome.retryAfter > 0 ? list->outcome.retryAfter : 30;
                    view.retryUntil = nowMs + seconds * 1000LL;
                }
            }
            if (view.relistDue && view.retryUntil <= nowMs) {
                Relist(static_cast<Kind>(k));
            }
            if (view.listed && !view.starredTab && view.typed != view.applied && nowMs - view.typedAt >= 700) {
                const size_t chars = detail::CodePoints(view.typed);
                if (chars == 0 || chars >= 2) {
                    Submit(static_cast<Kind>(k));
                }
            }
        }
        if (snap.lookup != seenLookup_) {
            seenLookup_ = snap.lookup;
            if (seenLookup_ != nullptr && !seenLookup_->loading && seenLookup_->outcome.Ok()) {
                lookup_ = seenLookup_->entries;
                haveLookup_ = true;
                RefreshStates();
            }
        }
        if (snap.revocations != seenRevocations_) {
            seenRevocations_ = snap.revocations;
            const RevocationsResult* r = seenRevocations_.get();
            if (r != nullptr && !r->loading && (r->outcome.Ok() || r->fromCache)) {
                const std::vector<RevokedInstall> before = installer_.Revoked();
                for (const RevokedInstall& revoked : installer_.ApplyRevocations(r->files)) {
                    bool known = false;
                    for (const RevokedInstall& old : before) {
                        known = known || (old.kind == revoked.kind && old.id == revoked.id);
                    }
                    if (!known) {
                        std::string reason = revoked.reason;
                        while (!reason.empty() && (reason.back() == '.' || reason.back() == ' ')) {
                            reason.pop_back();
                        }
                        Notice(revoked.name + " was revoked by the Nickel team"
                                       + (reason.empty() ? std::string() : ": " + reason)
                                       + (revoked.kind == Kind::Script ? ". It was stopped and held." : "."),
                               true);
                    }
                }
                RefreshStates();
            }
        }
        if (snap.report != seenReport_) {
            seenReport_ = snap.report;
            const ReportResult* report = seenReport_.get();
            if (report != nullptr && !report->loading && report->id == reportPending_) {
                reportPending_.clear();
                if (report->outcome.Ok()) {
                    Notice("Thanks. The report was sent to the Nickel team.");
                } else if (report->outcome.status == Status::Conflict
                           && report->outcome.problem.code == "already_reported") {
                    Notice("You already reported this.", true);
                } else if (report->outcome.status == Status::RateLimited) {
                    Notice("Too many reports. Try again later.", true);
                } else {
                    Notice(WriteRefusal(report->outcome, "report"), true);
                }
            }
        }
        HandleCheck(snap);
        HandlePublish(snap);
        lastCheck_ = snap.check;
        lastPublish_ = snap.publish;
        const FileResult* file = snap.file.get();
        if (pending_.step == Step::Downloading && file != nullptr && snap.file != pending_.stale && !file->loading
            && file->id == pending_.card.id && file->number == pending_.card.latest.number) {
            if (file->outcome.Ok()) {
                pending_.bytes = file->bytes;
                Finish(pending_.update ? installer_.Update(PackageNow(), false)
                                       : installer_.Install(PackageNow(), OnConflict::Ask));
            } else {
                pending_.step = Step::Failed;
                pending_.message = DownloadFailure(file->outcome);
            }
        }
        lastFile_ = snap.file;
        lastSource_ = snap.source;
    }

    // While a kind's page is shown (every frame): its first list, and the update check of installed items (when the
    // page opens, then at most every 30 minutes).
    void Shown(Kind kind) {
        if (requests_ == nullptr) {
            return;
        }
        KindView& view = View(kind);
        if (view.starredTab) {
            AskStarred(false);
        }
        if (!view.listed && !view.starredTab) {
            view.listed = true;
            requests_->List(QueryOf(kind));
        }
        if (lookupAt_ == 0 || now_ - lookupAt_ > 30 * 60 * 1000LL) {
            lookupAt_ = now_ == 0 ? 1 : now_;
            RefreshStates();
            std::vector<std::string> ids;
            for (Kind k : {Kind::Script, Kind::Config}) {
                for (const InstallRecord& record : installer_.Records(k)) {
                    ids.push_back(record.id);
                }
            }
            if (!ids.empty()) {
                requests_->Lookup(std::move(ids));
            }
        }
    }

    ListQuery QueryOf(Kind kind) const {
        const KindView& view = views_[Slot(kind)];
        ListQuery query;
        query.kind = kind;
        query.sort = view.sort;
        query.text = view.applied;
        query.compat = options_.running;
        query.limit = 24;
        return query;
    }

    void SetSort(Kind kind, Sort sort) {
        KindView& view = View(kind);
        if (view.starredTab) {
            view.starredTab = false;
            if (view.sort != sort || !view.listed) {
                view.sort = sort;
                Relist(kind);
            }
            return;
        }
        if (view.sort == sort) {
            return;
        }
        view.sort = sort;
        Relist(kind);
    }

    // The search box changed (every key).
    void Type(Kind kind, const std::string& text) {
        KindView& view = View(kind);
        if (text != view.typed) {
            view.typed = text;
            view.typedAt = now_;
        }
    }

    // Enter, or the debounce: the box is sent now.
    void Submit(Kind kind) {
        KindView& view = View(kind);
        if (view.applied == view.typed && view.listed) {
            return;
        }
        view.applied = view.typed;
        Relist(kind);
    }

    void ClearSearch(Kind kind) {
        KindView& view = View(kind);
        view.typed.clear();
        view.applied.clear();
        Relist(kind);
    }

    // Retry / Refresh: the first page again (refused while a 429 or 503 countdown runs).
    bool Retry(Kind kind) {
        KindView& view = View(kind);
        if (view.starredTab) {
            AskStarred(true);
            return true;
        }
        if (view.retryUntil > now_) {
            return false;
        }
        Relist(kind);
        return true;
    }

    bool LoadMore(Kind kind) { return requests_ != nullptr && requests_->More(kind); }

    void Select(Kind kind, const std::string& id) {
        KindView& view = View(kind);
        if (view.selected == id) {
            return;
        }
        view.selected = id;
        view.sourceOpen = false;
        if (!id.empty() && requests_ != nullptr) {
            requests_->Detail(id);
        }
        if (pending_.step != Step::Downloading && pending_.card.id != id) {
            pending_ = PendingInstall();
        }
    }

    // The source (scripts) or settings (configs) of the latest version, read without counting a download.
    // force: ask again (Retry after a failure).
    void OpenSource(const Card& card, bool force = false) {
        KindView& view = View(card.kind);
        view.selected = card.id;
        view.sourceOpen = true;
        if (requests_ != nullptr && (force || !SourceShown(card))) {
            requests_->Source(card.id, card.latest.number, card.latest.sha256);
        }
    }

    void CloseSource(Kind kind) { View(kind).sourceOpen = false; }

    // The source slot holds the latest version of this card (asked earlier).
    bool SourceShown(const Card& card) const {
        return lastSource_ != nullptr && lastSource_->id == card.id && lastSource_->number == card.latest.number;
    }

    // Install (or update) the card's latest version: the counted download, then the installer.
    bool Install(const Card& card, bool update = false) {
        if (requests_ == nullptr || pending_.step == Step::Downloading) {
            return false;
        }
        pending_ = PendingInstall();
        pending_.card = card;
        pending_.update = update;
        if (!CompatibleWith(card.latest.nickelMin, options_.running)) {
            pending_.step = Step::Failed;
            pending_.message = "This needs Nickel " + card.latest.nickelMin + " or newer.";
            return false;
        }
        pending_.step = Step::Downloading;
        pending_.stale = lastFile_;
        requests_->File(card.id, card.latest.number, card.latest.sha256);
        return true;
    }

    // The answer to NameConflict: KeepBoth or Replace (Ask cancels).
    void Resolve(OnConflict choice) {
        if (pending_.step != Step::NameConflict) {
            return;
        }
        if (choice == OnConflict::Ask) {
            pending_ = PendingInstall();
            return;
        }
        Finish(installer_.Install(PackageNow(), choice));
    }

    // The answer to Modified: replace the player's edits (their file is kept as the backup).
    void ReplaceEdits() {
        if (pending_.step != Step::Modified) {
            return;
        }
        Finish(installer_.Update(PackageNow(), true));
    }

    void CancelPending() {
        if (pending_.step != Step::Downloading) {
            pending_ = PendingInstall();
        }
    }

    // Done, Modified (ask, then evenIfModified), NotInstalled, Failed.
    InstallStatus Uninstall(Kind kind, const std::string& id, bool evenIfModified) {
        InstallRecord record;
        installer_.Find(kind, id, &record);
        const InstallOutcome outcome = installer_.Uninstall(kind, id, evenIfModified);
        if (outcome.status == InstallStatus::Done) {
            Notice("Uninstalled " + record.Name() + ".");
            RefreshStates();
        } else if (outcome.status != InstallStatus::Modified) {
            Notice(std::string("Could not uninstall ") + record.Name() + " (" + InstallStatusName(outcome.status)
                           + ").",
                   true);
        }
        return outcome.status;
    }

    // The player's Start button for an installed script (render thread).
    bool Start(const std::string& name) {
        const bool started = options_.library != nullptr && options_.library->Start(name);
        Notice(started ? "Started " + name + "." : "Could not start " + name + ".", !started);
        return started;
    }

    bool Stop(const std::string& name) { return options_.library != nullptr && options_.library->Stop(name); }

    // Stars (on) or unstars an item: a local favourite, saved in root\public\starred.json at once. No request is
    // made, the install key is neither read nor created, and nothing is downloaded, installed or run. One's own
    // creations can be starred too. False when the id is invalid, the list is full or the file could not be saved.
    bool Star(Kind kind, const std::string& id, bool on) {
        if (!ValidId(id)) {
            return false;
        }
        if (!starred_.Set(kind, id, on)) {
            Notice(on ? "Could not add it to your starred " + KindWord(kind, true) + "."
                      : "Could not remove it from your starred " + KindWord(kind, true) + ".",
                   true);
            return false;
        }
        return true;
    }

    bool IsStarred(Kind kind, const std::string& id) const { return starred_.Has(kind, id); }

    const StarredList& Starred() const { return starred_; }

    // ---- the Starred tab

    void ShowStarred(Kind kind) {
        View(kind).starredTab = true;
        AskStarred(false);
    }

    // The cards of the Starred tab of `kind`: the lookup answer for the ids this install starred, newest star first,
    // filtered by the search box (name, summary or author). `missing`: starred items no longer available.
    std::vector<Card> StarredCards(Kind kind, const Snapshot& snap, size_t* missing = nullptr) const {
        std::vector<Card> cards;
        size_t gone = 0;
        const LookupResult* lookup = snap.starred.get();
        if (lookup != nullptr && !lookup->loading && lookup->outcome.Ok()) {
            const std::string filter = Lower(views_[Slot(kind)].typed);
            for (const std::string& id : starred_.Ids(kind)) {
                for (const LookupEntry& entry : lookup->entries) {
                    if (entry.id != id) {
                        continue;
                    }
                    if (!entry.found) {
                        ++gone;
                    } else if (entry.card.kind == kind
                               && (filter.empty() || Lower(entry.card.name).find(filter) != std::string::npos
                                   || Lower(entry.card.summary).find(filter) != std::string::npos
                                   || Lower(entry.card.author.displayName).find(filter) != std::string::npos)) {
                        cards.push_back(entry.card);
                    }
                }
            }
        }
        if (missing != nullptr) {
            *missing = gone;
        }
        return cards;
    }

    // The starred items of `kind` the lookup says are gone (hidden, removed, deleted or not found), newest star
    // first: the Starred tab lists them with their state so they can still be unstarred. None while the search box
    // has text, like the cards it filters (a gone item has no name to match).
    std::vector<Missing> StarredMissing(Kind kind, const Snapshot& snap) const {
        std::vector<Missing> gone;
        const LookupResult* lookup = snap.starred.get();
        if (lookup == nullptr || lookup->loading || !lookup->outcome.Ok() || !views_[Slot(kind)].typed.empty()) {
            return gone;
        }
        for (const std::string& id : starred_.Ids(kind)) {
            for (const LookupEntry& entry : lookup->entries) {
                if (entry.id == id && !entry.found) {
                    gone.push_back(entry.missing);
                    gone.back().id = id;
                }
            }
        }
        return gone;
    }

    ListState StarredState(Kind kind, const Snapshot& snap, bool enabled) const {
        ListState state;
        if (!enabled || requests_ == nullptr) {
            return ListStateOf(kind, nullptr, false, 0, now_);
        }
        if (starred_.Ids(kind).empty()) {
            state.view = ListView::Empty;
            state.message = "You have not starred any " + KindWord(kind, true)
                            + " yet. Open one and press Star to keep it here.";
            return state;
        }
        const LookupResult* lookup = snap.starred.get();
        if (lookup == nullptr || lookup->loading) {
            state.view = ListView::Loading;
            return state;
        }
        state.detail = lookup->outcome.problem.Text();
        switch (lookup->outcome.status) {
            case Status::Ok:
                break;
            case Status::Offline:
            case Status::Timeout:
                state.view = ListView::Offline;
                state.message = "Can't reach nickelclient.com.";
                return state;
            case Status::RateLimited:
                state.view = ListView::RateLimited;
                state.message = "Too many requests. Try again in a moment.";
                return state;
            case Status::Maintenance:
                state.view = ListView::Maintenance;
                state.message = "PUBLIC is under maintenance.";
                return state;
            default:
                state.view = ListView::Unavailable;
                state.message = "PUBLIC is unavailable right now.";
                return state;
        }
        size_t missing = 0;
        const std::vector<Card> cards = StarredCards(kind, snap, &missing);
        if (cards.empty() && !views_[Slot(kind)].typed.empty()) {
            state.view = ListView::NoMatch;
            state.message = "No starred " + KindWord(kind, true) + " match \"" + views_[Slot(kind)].typed + "\".";
            return state;
        }
        state.view = ListView::Ready; // the gone ones are listed below the cards (StarredMissing), to unstar them
        if (missing > 0) {
            state.message = std::to_string(missing) + " starred " + KindWord(kind, missing != 1)
                            + (missing == 1 ? " is" : " are") + " no longer available.";
        }
        return state;
    }

    // ---- reports (anonymous: the moderators read them on the website)

    bool Report(const Card& card, const std::string& category, const std::string& details) {
        if (requests_ == nullptr || !ValidId(card.id) || !KnownReportReason(category) || !reportPending_.empty()) {
            return false;
        }
        reportPending_ = card.id;
        return requests_->Report(card.id, category, details);
    }

    bool ReportSending() const { return !reportPending_.empty(); }

    // ---- publishing from OTHER -> Scripting and OTHER -> Configs

    // The item this install published from the local `name`, or nullptr (never one of another install).
    const PublishedRecord* OwnedItem(Kind kind, const std::string& name) {
        return published_.Find(kind, name, Fingerprint());
    }

    const PublishDraft& Draft() const { return draft_; }

    // Opens the publish form for a local script or config: reads its bytes (scripts through the library, configs from
    // root\configs) and asks the check. "Publish update" when this install owns the item published from that name.
    bool BeginPublish(Kind kind, const std::string& localName) {
        if (draft_.step == PublishStep::Publishing) {
            return false;
        }
        draft_ = PublishDraft();
        draft_.kind = kind;
        draft_.localName = localName;
        const auto fail = [&](std::string text) {
            draft_.step = PublishStep::Failed;
            draft_.message = std::move(text);
            return false;
        };
        if (requests_ == nullptr) {
            return fail("Turn PUBLIC on to publish.");
        }
        // published.json must be able to remember the item, or it could never be updated (and publishing is final)
        if (!overlay::ValidScriptName(localName)) {
            return fail("This name cannot be used on PUBLIC. Rename it first.");
        }
        std::string bytes;
        if (kind == Kind::Script) {
            overlay::ScriptSource source;
            if (options_.library == nullptr || !options_.library->ReadSource(localName, source)) {
                return fail("The script could not be read.");
            }
            bytes = std::move(source.text);
        } else if (!detail::ReadAll(installer_.ConfigsFolder() + L"\\" + detail::ToWide(localName) + L".ini",
                                    &bytes,
                                    kMaxFileBytes + 1)) {
            return fail("The config could not be read, or it is over 200 KB.");
        }
        if (bytes.empty()) {
            return fail("An empty file cannot be published.");
        }
        if (bytes.size() > kMaxFileBytes) {
            return fail("Files over 200 KB cannot be published.");
        }
        const char* extension = kind == Kind::Script ? ".luau" : ".ini";
        draft_.fileName = localName + extension;
        if (!detail::ValidUploadName(draft_.fileName, extension)) {
            return fail("This name cannot be used on PUBLIC. Rename it first.");
        }
        draft_.bytes = std::move(bytes);
        draft_.sha256 = net::Sha256Hex(draft_.bytes);
        if (const PublishedRecord* owned = OwnedItem(kind, localName)) {
            draft_.update = true;
            draft_.itemId = owned->id;
        } else if (published_.Records().size() >= kMaxPublished) {
            return fail("This install cannot remember more published items, so it could not update this one.");
        }
        draft_.warning = UncertainWarning(kind, localName, draft_.update);
        draft_.step = PublishStep::Checking;
        draft_.staleCheck = lastCheck_;
        requests_->Check(kind, draft_.fileName, draft_.bytes, localName);
        return true;
    }

    // Sends the form of a new item; "" when it was sent, else what to fix.
    std::string SubmitPublish(const PublishFields& fields) {
        if (draft_.step != PublishStep::Ready || draft_.update || requests_ == nullptr) {
            return "Wait for the check to finish.";
        }
        PublishRequest request = RequestOfDraft();
        request.name = Trim(fields.name);
        request.summary = Trim(fields.summary);
        request.description = Trim(fields.description);
        request.authorName = Trim(fields.authorName);
        if (detail::CodePoints(request.name) == 0 || detail::CodePoints(request.name) > kMaxNameChars) {
            return "Give it a name of 1 to 60 characters.";
        }
        if (detail::CodePoints(request.summary) == 0 || detail::CodePoints(request.summary) > kMaxSummaryChars) {
            return "Write a summary of 1 to 140 characters.";
        }
        if (detail::CodePoints(request.description) > kMaxDescriptionChars) {
            return "The description is over 4000 characters.";
        }
        if (!ParseTags(fields.tags, &request.tags)) {
            return "Up to 5 tags of 2 to 24 lower-case letters, digits or dashes.";
        }
        if (detail::CodePoints(request.authorName) == 0
            || detail::CodePoints(request.authorName) > kMaxDisplayNameChars) {
            return "Set the author name you publish under (1 to 32 characters).";
        }
        const std::string problem = detail::PublishProblem(request, false);
        if (!problem.empty()) {
            return "Check the " + problem + ": it has a character that cannot be used.";
        }
        WriteAuthorName(options_.root, request.authorName);
        draft_.step = PublishStep::Publishing;
        draft_.stalePublish = lastPublish_;
        requests_->Publish(request);
        return "";
    }

    // Sends a new version of the owned item; "" when it was sent, else what to fix.
    std::string SubmitUpdate(const std::string& changelog) {
        if (draft_.step != PublishStep::Ready || !draft_.update || requests_ == nullptr) {
            return "Wait for the check to finish.";
        }
        PublishRequest request = RequestOfDraft();
        request.changelog = Trim(changelog);
        if (detail::CodePoints(request.changelog) == 0 || detail::CodePoints(request.changelog) > kMaxChangelogChars) {
            return "Say what changed in this version (1 to 2000 characters).";
        }
        if (!detail::PublishProblem(request, true).empty()) {
            return "The changelog has a character that cannot be used.";
        }
        draft_.step = PublishStep::Publishing;
        draft_.stalePublish = lastPublish_;
        requests_->PublishVersion(draft_.itemId, request);
        return "";
    }

    // Closes the form (refused while the publish is on its way: its answer must be remembered).
    bool ClosePublish() {
        if (draft_.step == PublishStep::Publishing) {
            return false;
        }
        draft_ = PublishDraft();
        return true;
    }

    std::string AuthorName() const { return ReadAuthorName(options_.root); }

    void SkipVersion(const Card& card) {
        installer_.SkipVersion(card.kind, card.id, card.latest.number);
        RefreshStates();
    }

    const LocalState* LocalOf(Kind kind, const std::string& id) const {
        for (const LocalState& state : states_[Slot(kind)]) {
            if (state.id == id) {
                return &state;
            }
        }
        return nullptr;
    }

    bool AnyUpdate(Kind kind) const {
        for (const LocalState& state : states_[Slot(kind)]) {
            if (state.state == Local::UpdateAvailable) {
                return true;
            }
        }
        return false;
    }

    size_t InstalledCount(Kind kind) const { return states_[Slot(kind)].size(); }

    // Reads the records and files again (after the player changed files, or an answer).
    void RefreshStates() {
        installer_.Reload();
        for (Kind kind : {Kind::Script, Kind::Config}) {
            states_[Slot(kind)] = installer_.States(kind, haveLookup_ ? &lookup_ : nullptr);
        }
    }

    // Notifications for the menu, oldest first.
    std::vector<std::string> TakeNotices() {
        std::vector<std::string> out;
        out.swap(notices_);
        return out;
    }

    // The messages the page shows itself, oldest first (at most 3; kept after TakeNotices).
    const std::vector<PageMessage>& Messages() const { return messages_; }

    void DismissMessages() { messages_.clear(); }

    // A message for the page only (the menu already told the HUD, as LoadConfig does).
    void Message(std::string text, bool problem = false) {
        PageMessage message;
        message.text = std::move(text);
        message.at = now_;
        message.problem = problem;
        messages_.push_back(std::move(message));
        if (messages_.size() > 3) {
            messages_.erase(messages_.begin());
        }
    }

    ListState StateOf(Kind kind, const Snapshot& snap, bool enabled) const {
        return ListStateOf(kind, snap.List(kind), enabled && requests_ != nullptr, views_[Slot(kind)].retryUntil, now_);
    }

    int64_t Now() const { return now_; }

private:
    static size_t Slot(Kind kind) { return static_cast<size_t>(kind); }

    static std::string Lower(std::string text) {
        for (char& c : text) {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
        return text;
    }

    static std::string Trim(const std::string& text) {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::string();
        }
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    // The fingerprint of this install's key, read once it exists ("" before the first write); while there is none,
    // the file is looked for at most every 2 seconds (the side panels ask every frame).
    std::string Fingerprint() {
        if (!fingerprintKnown_ && options_.identity != nullptr
            && (fingerprintAt_ == 0 || now_ < fingerprintAt_ || now_ - fingerprintAt_ >= 2000)) {
            fingerprintAt_ = now_ == 0 ? 1 : now_;
            fingerprintKnown_ = options_.identity->Fingerprint(&fingerprint_);
        }
        return fingerprintKnown_ ? fingerprint_ : std::string();
    }

    // The Starred tab's lookup: when a starred id was not asked yet, or force (Refresh).
    void AskStarred(bool force) {
        if (requests_ == nullptr) {
            return;
        }
        std::vector<std::string> ids;
        bool fresh = force;
        for (Kind kind : {Kind::Script, Kind::Config}) {
            for (const std::string& id : starred_.Ids(kind)) {
                ids.push_back(id);
                fresh = fresh || std::find(starredAsked_.begin(), starredAsked_.end(), id) == starredAsked_.end();
            }
        }
        if (!fresh || ids.empty()) {
            return;
        }
        starredAsked_ = ids;
        requests_->LookupStarred(std::move(ids));
    }

    // Publishes of this session whose answer was lost (timeout, offline, 5xx, unreadable answer).
    struct Uncertain {
        Kind kind = Kind::Script;
        std::string name;
    };

    bool IsUncertain(Kind kind, const std::string& name) const {
        for (const Uncertain& u : uncertain_) {
            if (u.kind == kind && overlay::SameScriptName(u.name, name)) {
                return true;
            }
        }
        return false;
    }

    void RememberUncertain(Kind kind, const std::string& name) {
        if (!IsUncertain(kind, name) && uncertain_.size() < 64) {
            uncertain_.push_back(Uncertain{kind, name});
        }
    }

    void ForgetUncertain(Kind kind, const std::string& name) {
        uncertain_.erase(std::remove_if(uncertain_.begin(),
                                        uncertain_.end(),
                                        [&](const Uncertain& u) {
                                            return u.kind == kind && overlay::SameScriptName(u.name, name);
                                        }),
                         uncertain_.end());
    }

    // "" unless an earlier publish of (kind, name) this session got no clear answer.
    std::string UncertainWarning(Kind kind, const std::string& name, bool update) const {
        if (!IsUncertain(kind, name)) {
            return std::string();
        }
        const std::string where = kind == Kind::Script ? "PUBLIC -> Scripts" : "PUBLIC -> Configs";
        return update ? "No clear answer came back for the update of " + name
                                + ", so it may already be published. Look at the item's versions in " + where
                                + " before you send it again."
                      : "No clear answer came back for " + name
                                + ", so it may already be public. Look for it in " + where
                                + " (Recent) before you publish again: a second publish makes a second item that "
                                  "can never be removed.";
    }

    PublishRequest RequestOfDraft() const {
        PublishRequest request;
        request.kind = draft_.kind;
        request.fileName = draft_.fileName;
        request.bytes = draft_.bytes;
        request.ref = draft_.localName;
        return request;
    }

    // Why a write was refused, for the player. what: "report", "publish".
    static std::string WriteRefusal(const Outcome& outcome, const char* what) {
        switch (outcome.status) {
            case Status::NoIdentity:
                return "This install's PUBLIC key (public\\install.key) cannot be used. If it is damaged, delete it to "
                       "get a new one; what you published with it can then no longer be updated.";
            case Status::Unauthorized:
                return "The server refused this install's PUBLIC key.";
            case Status::Forbidden:
                return "This install is blocked from PUBLIC.";
            case Status::Offline:
            case Status::Timeout:
                return "Can't reach nickelclient.com.";
            case Status::RateLimited:
                return "Too many requests. Try again in "
                       + std::to_string(outcome.retryAfter > 0 ? outcome.retryAfter : 30) + " s.";
            default:
                return std::string("The ") + what + " could not be sent (" + StatusName(outcome.status) + ")."
                       + (outcome.problem.Text().empty() ? std::string() : " " + outcome.problem.Text());
        }
    }

    void HandleCheck(const Snapshot& snap) {
        const CheckResult* check = snap.check.get();
        if (draft_.step != PublishStep::Checking || check == nullptr || snap.check == draft_.staleCheck
            || check->loading || check->sentSha256 != draft_.sha256 || check->ref != draft_.localName) {
            return;
        }
        if (check->outcome.Ok()) {
            draft_.check = check->check;
            draft_.checked = true;
            if (check->check.ok) {
                draft_.step = PublishStep::Ready;
            } else {
                draft_.step = PublishStep::Failed;
                draft_.message = "The automatic check refused this file. It cannot be published as it is.";
                draft_.errors = check->check.errors;
            }
            return;
        }
        draft_.step = PublishStep::Failed;
        draft_.errors = check->outcome.problem.errors;
        draft_.message = check->outcome.status == Status::Rejected || check->outcome.status == Status::BadRequest
                                 ? "The automatic check refused this file. It cannot be published as it is."
                                 : WriteRefusal(check->outcome, "check");
    }

    void HandlePublish(const Snapshot& snap) {
        const PublishResult* result = snap.publish.get();
        if (draft_.step != PublishStep::Publishing || result == nullptr || snap.publish == draft_.stalePublish
            || result->loading || result->sentSha256 != draft_.sha256 || result->ref != draft_.localName) {
            return;
        }
        if (!result->outcome.Ok()) {
            draft_.step = PublishStep::Failed;
            draft_.errors = result->outcome.problem.errors;
            const std::string said = result->outcome.problem.Text();
            switch (result->outcome.status) {
                case Status::Rejected:
                case Status::BadRequest:
                    draft_.message = "The server refused it." + (said.empty() ? std::string() : " " + said);
                    break;
                case Status::Conflict:
                    draft_.message = said.empty() ? "This file is already published." : said;
                    break;
                case Status::NotFound:
                case Status::Gone:
                    draft_.message = "This item is no longer on PUBLIC.";
                    break;
                case Status::Forbidden:
                    draft_.message = draft_.update ? "Only the install that published this item can update it."
                                                   : WriteRefusal(result->outcome, "publish");
                    break;
                case Status::Offline:
                case Status::Timeout:
                case Status::Malformed:
                case Status::ServerError:
                    // the request may have reached the server: it may have published it and the answer was lost
                    RememberUncertain(draft_.kind, draft_.localName);
                    draft_.message = UncertainWarning(draft_.kind, draft_.localName, draft_.update);
                    break;
                default:
                    draft_.message = WriteRefusal(result->outcome, "publish");
                    break;
            }
            Notice(draft_.message, true);
            return;
        }
        ForgetUncertain(draft_.kind, draft_.localName);
        fingerprintKnown_ = false; // the first write created the key
        fingerprintAt_ = 0;
        PublishedRecord record;
        if (const PublishedRecord* owned = OwnedItem(draft_.kind, draft_.localName)) {
            record = *owned;
        }
        record.kind = draft_.kind;
        record.name = draft_.localName;
        record.id = result->itemId;
        record.owner = Fingerprint();
        record.version = result->versionNumber > 0 ? result->versionNumber : record.version;
        const std::string at = FormatIsoTime(now_);
        if (draft_.update) {
            record.updatedAt = at;
        } else {
            record.publishedAt = at;
            record.updatedAt.clear();
        }
        const bool remembered = published_.Remember(record);
        draft_.step = PublishStep::Done;
        const bool pending = result->state == "pending";
        draft_.message = (draft_.update ? "Published the update of " : "Published ") + draft_.localName
                         + (pending ? ". It will be public once it is approved." : ". It is public now.");
        if (!remembered) {
            draft_.message += " This install could not save that it owns it, so it cannot be updated from here.";
        }
        Notice(draft_.message, !remembered);
    }

    // The first page again; while a 429 or 503 countdown runs it is kept for Tick to send when it ends.
    void Relist(Kind kind) {
        KindView& view = View(kind);
        view.listed = true;
        if (view.retryUntil > now_) {
            view.relistDue = true;
            return;
        }
        view.relistDue = false;
        if (requests_ != nullptr) {
            requests_->List(QueryOf(kind));
        }
    }

    Package PackageNow() const { return PackageOf(pending_.card, pending_.card.latest, pending_.bytes); }

    void Finish(const InstallOutcome& outcome) {
        const bool script = pending_.card.kind == Kind::Script;
        switch (outcome.status) {
            case InstallStatus::Done: {
                const std::string name = outcome.record.Name();
                std::string text = pending_.update ? "Updated " + name + " to " + VersionText(pending_.card.latest)
                                                   : "Installed " + name;
                text += script ? " (not started). Press Start to run it." : ". Load it when you want it.";
                if (!outcome.compileErrors.empty()) {
                    text += " It has " + std::to_string(outcome.compileErrors.size()) + " compile error"
                            + (outcome.compileErrors.size() == 1 ? "." : "s.");
                }
                Notice(text, !outcome.compileErrors.empty());
                pending_ = PendingInstall();
                RefreshStates();
                return;
            }
            case InstallStatus::NameConflict:
                pending_.step = Step::NameConflict;
                pending_.message = "A " + KindWord(pending_.card.kind, false) + " named \"" + outcome.name
                                   + "\" already exists and did not come from PUBLIC.";
                return;
            case InstallStatus::Modified:
                pending_.step = Step::Modified;
                pending_.message = "You edited " + outcome.name + ". Replace your edits with this version? "
                                   + (script ? "Your file is kept as the script's backup."
                                             : "Your file is kept in configs\\.public\\backup.");
                return;
            default:
                pending_.step = Step::Failed;
                pending_.bytes.clear();
                pending_.message =
                        outcome.status == InstallStatus::BadPackage
                                ? "The download was damaged. Nothing was installed."
                                : std::string("Nothing was installed (") + InstallStatusName(outcome.status) + ").";
                return;
        }
    }

    std::string DownloadFailure(const Outcome& outcome) const {
        switch (outcome.status) {
            case Status::DigestMismatch:
            case Status::TooLarge:
            case Status::Malformed:
                return "The download was damaged. Nothing was installed.";
            case Status::Offline:
            case Status::Timeout:
                return "Can't reach nickelclient.com. Nothing was installed.";
            case Status::RateLimited:
                return "Too many requests. Try again in "
                       + std::to_string(outcome.retryAfter > 0 ? outcome.retryAfter : 30) + " s.";
            case Status::Gone:
            case Status::NotFound:
                return "This item is no longer available.";
            default:
                return std::string("The download failed (") + StatusName(outcome.status) + ").";
        }
    }

    void Notice(std::string text, bool problem = false) {
        Message(text, problem);
        if (notices_.size() < 16) {
            notices_.push_back(std::move(text));
        }
    }

    PageOptions options_;
    Installer installer_;
    PageRequests* requests_ = nullptr;
    KindView views_[2];
    PendingInstall pending_;
    std::vector<LocalState> states_[2];
    std::vector<LookupEntry> lookup_;
    bool haveLookup_ = false;
    std::shared_ptr<const LookupResult> seenLookup_;
    std::shared_ptr<const RevocationsResult> seenRevocations_;
    std::shared_ptr<const ReportResult> seenReport_;
    std::shared_ptr<const CheckResult> lastCheck_;
    std::shared_ptr<const PublishResult> lastPublish_;
    StarredList starred_;
    PublishedList published_;
    std::vector<std::string> starredAsked_;
    std::string reportPending_;
    std::vector<Uncertain> uncertain_;
    PublishDraft draft_;
    std::string fingerprint_;
    bool fingerprintKnown_ = false;
    int64_t fingerprintAt_ = 0;
    std::shared_ptr<const FileResult> lastFile_;
    std::shared_ptr<const FileResult> lastSource_;
    std::vector<std::string> notices_;
    std::vector<PageMessage> messages_;
    int64_t now_ = 0;
    int64_t revocationsAt_ = 0;
    int64_t lookupAt_ = 0;
    bool wasEnabled_ = false;
};

} // namespace nickel::pub
