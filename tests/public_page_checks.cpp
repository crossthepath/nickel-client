// What the PUBLIC pages of the menu do, without ImGui and without the network (native/public_page.hpp): requests are
// recorded, answers are hand-made snapshots, and installs, favourites (local stars), the install key and what was
// published go to a temporary folder through a ScriptLibrary over that folder. Nothing here reaches the internet or
// %APPDATA%.
#include <windows.h>

#include "public_page.hpp"
#include "script_files.hpp"

#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
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

std::string Sha(const std::string& bytes) {
    return net::Sha256Hex(bytes);
}

// ---------------------------------------------------------------------------------------------------------------
// Fakes

struct Recorded {
    std::string call;
    ListQuery query;
    std::string id;
    int number = 0;
    std::vector<std::string> ids;
    Kind kind = Kind::Script;
    std::string fileName;
    std::string bytes;
    std::string ref;
    std::string category;
    std::string details;
    PublishRequest request;
};

class RecordingRequests : public PageRequests {
public:
    std::vector<Recorded> calls;

    bool List(const ListQuery& query) override {
        return Add("list", [&](Recorded& r) { r.query = query; });
    }

    bool More(Kind) override {
        return Add("more", [](Recorded&) {});
    }

    bool Detail(const std::string& id) override {
        return Add("detail", [&](Recorded& r) { r.id = id; });
    }

    bool Source(const std::string& id, int number, const std::string&) override {
        return Add("source", [&](Recorded& r) {
            r.id = id;
            r.number = number;
        });
    }

    bool File(const std::string& id, int number, const std::string&) override {
        return Add("file", [&](Recorded& r) {
            r.id = id;
            r.number = number;
        });
    }

    bool Lookup(std::vector<std::string> ids) override {
        return Add("lookup", [&](Recorded& r) { r.ids = ids; });
    }

    bool Revocations() override {
        return Add("revocations", [](Recorded&) {});
    }

    bool LookupStarred(std::vector<std::string> ids) override {
        return Add("starred", [&](Recorded& r) { r.ids = ids; });
    }

    bool Check(Kind kind, const std::string& fileName, const std::string& bytes, const std::string& ref) override {
        return Add("check", [&](Recorded& r) {
            r.kind = kind;
            r.fileName = fileName;
            r.bytes = bytes;
            r.ref = ref;
        });
    }

    bool Publish(const PublishRequest& request) override {
        return Add("publish", [&](Recorded& r) { r.request = request; });
    }

    bool PublishVersion(const std::string& id, const PublishRequest& request) override {
        return Add("version", [&](Recorded& r) {
            r.id = id;
            r.request = request;
        });
    }

    bool Report(const std::string& id, const std::string& category, const std::string& details) override {
        return Add("report", [&](Recorded& r) {
            r.id = id;
            r.category = category;
            r.details = details;
        });
    }

    int Count(const std::string& call) const {
        int n = 0;
        for (const Recorded& r : calls) {
            n += r.call == call ? 1 : 0;
        }
        return n;
    }

    const Recorded* Last(const std::string& call) const {
        for (size_t i = calls.size(); i > 0; --i) {
            if (calls[i - 1].call == call) {
                return &calls[i - 1];
            }
        }
        return nullptr;
    }

private:
    template <typename Fill>
    bool Add(const char* call, Fill fill) {
        Recorded r;
        r.call = call;
        fill(r);
        calls.push_back(std::move(r));
        return true;
    }
};

// A ScriptLibrary over a folder, with the engine's own file code (as tests/public_client_checks.cpp).
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
            info.state = running_ == info.name ? overlay::ScriptState::Running : overlay::ScriptState::Stopped;
            scripts.push_back(info);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return scripts;
    }

    std::wstring Folder() const override { return folder_; }

    bool ReadSource(const std::string& name, overlay::ScriptSource& source) const override {
        return overlay::ReadScriptSource(overlay::ScriptFilePath(folder_, name), source);
    }

    std::vector<overlay::CompileError> Compile(const std::string&) const override { return {}; }

    bool HasBackup(const std::string& name) const override { return overlay::ScriptHasBackup(folder_, name); }

    overlay::ScriptFileResult
    WriteSource(const std::string& name, const std::string& source, const std::string& expectedSha256) override {
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
        if (running_ == name) {
            running_.clear();
        }
        return held_.Add(name);
    }

    bool Start(const std::string& name) override {
        if (!Known(name)) {
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(guard_);
        running_ = name;
        return held_.Remove(name);
    }

    bool Stop(const std::string& name) override {
        std::lock_guard<std::recursive_mutex> lock(guard_);
        if (running_ == name) {
            running_.clear();
        }
        return Known(name);
    }

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
    mutable std::string running_;
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

// A new folder on every call. The counter matters: GetTickCount64 moves every ~16 ms, so two roots made in the same tick
// were the same folder, and deleting one (the ANSI settings.ini case) deleted the other's scripts.
std::wstring TemporaryRoot() {
    static int made = 0;
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    std::wstring root = std::wstring(buffer) + L"nickel-public-page-checks-" + std::to_wstring(GetCurrentProcessId())
                        + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++made);
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\scripts").c_str(), nullptr);
    CreateDirectoryW((root + L"\\configs").c_str(), nullptr);
    return root;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

Card MakeCard(const std::string& id,
              Kind kind,
              const std::string& name,
              int number,
              const std::string& bytes,
              const std::string& nickelMin = "1.0.0") {
    Card card;
    card.id = id;
    card.kind = kind;
    card.slug = "item-" + id;
    card.name = name;
    card.summary = "A test item.";
    card.author.handle = "mash";
    card.author.displayName = "Mash";
    card.latest.number = number;
    card.latest.label = "1." + std::to_string(number) + ".0";
    card.latest.nickelMin = nickelMin;
    card.latest.size = static_cast<uint32_t>(bytes.size());
    card.latest.sha256 = Sha(bytes);
    return card;
}

std::shared_ptr<FileResult>
FileAnswer(const Card& card, const std::string& bytes, Status status = Status::Ok, bool loading = false) {
    auto file = std::make_shared<FileResult>();
    file->id = card.id;
    file->number = card.latest.number;
    file->sha256 = card.latest.sha256;
    file->install = true;
    file->loading = loading;
    file->outcome.status = loading ? Status::Loading : status;
    if (status == Status::Ok && !loading) {
        file->bytes = bytes;
    }
    return file;
}

bool HasNotice(const std::vector<std::string>& notices, const std::string& part) {
    for (const std::string& text : notices) {
        if (text.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Words, states and parsers

void WordChecks() {
    Check(FormatCount(950) == "950" && FormatCount(1000) == "1k" && FormatCount(12412) == "12.4k"
                  && FormatCount(124000) == "124k" && FormatCount(3100000) == "3.1M" && FormatCount(2000000) == "2M",
          "counts: 950, 1k, 12.4k, 124k, 3.1M, 2M");
    Check(FormatSize(980) == "980 B" && FormatSize(4210) == "4.1 KB", "sizes");
    const int64_t now = 1794765600000LL;
    Check(FormatAgo(now - 30000, now) == "just now" && FormatAgo(now - 5 * 60000, now) == "5 minutes ago"
                  && FormatAgo(now - 3 * 3600000LL, now) == "3 hours ago" && FormatAgo(0, now) == "unknown",
          "relative times");
    Check(FormatDay(1794765600000LL) == "15 Nov 2026", "review day");
    const int64_t day = 24 * 3600000LL;
    Check(FormatAgo(now - 21 * day, now) == "3 weeks ago" && FormatAgo(now - 35 * day, now) == "1 month ago"
                  && FormatAgo(now - 56 * day, now) == "1 month ago" && FormatAgo(now - 75 * day, now) == "2 months ago"
                  && FormatAgo(now - 100 * day, now) == "3 months ago",
          "weeks, then months without going back (75 days: 2 months)");
    VersionRef version;
    version.number = 7;
    Check(VersionText(version) == "version 7", "a version without a label");
    version.label = "1.3.0";
    Check(VersionText(version) == "v1.3.0", "a version label");
    Check(ChatLeakWarning({Capability::ChatSend, Capability::Input}) && !ChatLeakWarning({Capability::ChatSend})
                  && !ChatLeakWarning({Capability::Input, Capability::Render}),
          "chat with input warns");
    for (int i = 0; i <= static_cast<int>(Capability::Dynamic); ++i) {
        Check(std::string(CapabilityText(static_cast<Capability>(i))) != "Unknown capability",
              "every capability has plain words");
    }

    overlay::ScriptInfo held;
    held.held = true;
    overlay::ScriptInfo running;
    running.state = overlay::ScriptState::Running;
    Check(BadgeOf(Local::None, nullptr, true) == Badge::None, "not installed: no pill");
    Check(BadgeOf(Local::None, nullptr, false) == Badge::Incompatible, "not installed and too new: NEEDS");
    Check(BadgeOf(Local::Installed, &held, true) == Badge::Held, "installed and held: HELD");
    Check(BadgeOf(Local::Installed, &running, true) == Badge::Running, "installed and running: RUNNING");
    Check(BadgeOf(Local::Installed, nullptr, true) == Badge::Installed, "installed config: INSTALLED");
    Check(BadgeOf(Local::UpdateAvailable, &held, true) == Badge::Update, "an update beats held");
    Check(BadgeOf(Local::Revoked, &running, true) == Badge::Revoked
                  && BadgeOf(Local::Removed, nullptr, true) == Badge::Removed,
          "revoked and removed first");
    Check(std::string(BadgeText(Badge::Held)) == "HELD" && std::string(BadgeText(Badge::Update)) == "UPDATE",
          "pill words");
}

void ListStateChecks() {
    const int64_t now = 1794765600000LL;
    Check(ListStateOf(Kind::Script, nullptr, false, 0, now).view == ListView::Off, "off when disabled");
    Check(ListStateOf(Kind::Script, nullptr, true, 0, now).view == ListView::Loading, "loading before an answer");
    ListResult list;
    list.loading = true;
    Check(ListStateOf(Kind::Script, &list, true, 0, now).view == ListView::Loading, "first page on its way");
    list.loading = false;
    ListState state = ListStateOf(Kind::Script, &list, true, 0, now);
    Check(state.view == ListView::Empty && state.message.find("No scripts shared yet") == 0, "empty kind");
    list.query.text = "strafe";
    state = ListStateOf(Kind::Config, &list, true, 0, now);
    Check(state.view == ListView::NoMatch && state.message == "No configs match \"strafe\".", "empty search");
    list.outcome.status = Status::Offline;
    Check(ListStateOf(Kind::Script, &list, true, 0, now).view == ListView::Offline, "offline without a cache");
    list.outcome.status = Status::Timeout;
    Check(ListStateOf(Kind::Script, &list, true, 0, now).view == ListView::Offline, "a timeout reads as offline");
    list.outcome.status = Status::RateLimited;
    state = ListStateOf(Kind::Script, &list, true, now + 20000, now);
    Check(state.view == ListView::RateLimited && state.retryIn == 20
                  && state.message == "Too many requests. Try again in 20 s.",
          "429 with its countdown");
    list.outcome.status = Status::Maintenance;
    Check(ListStateOf(Kind::Script, &list, true, 0, now).view == ListView::Maintenance, "503 maintenance");
    list.outcome.status = Status::BadRequest;
    Check(ListStateOf(Kind::Script, &list, true, 0, now).view == ListView::BadQuery, "400 bad query");
    list.outcome.status = Status::ServerError;
    list.outcome.problem.detail = "Database restarting.";
    state = ListStateOf(Kind::Script, &list, true, 0, now);
    Check(state.view == ListView::Unavailable && state.detail == "Database restarting.", "5xx with the problem text");
    list.outcome.status = Status::Malformed;
    Check(ListStateOf(Kind::Script, &list, true, 0, now).view == ListView::Unavailable, "malformed answer");

    list.items.push_back(MakeCard("7k2m9x4qa1dz", Kind::Script, "One", 1, "x"));
    list.outcome = Outcome();
    list.outcome.status = Status::Offline;
    list.fromCache = true;
    list.fetchedAt = now - 2 * 3600000LL;
    state = ListStateOf(Kind::Script, &list, true, 0, now);
    Check(state.view == ListView::Ready && state.fromCache && state.message.find("2 hours ago") != std::string::npos,
          "offline with the kept list");
    list.fromCache = false;
    list.outcome.status = Status::ServerError;
    state = ListStateOf(Kind::Script, &list, true, 0, now);
    Check(state.view == ListView::Ready && state.message.find("More could not be loaded") == 0,
          "a failed next page keeps the cards");
    list.outcome.status = Status::Ok;
    Check(ListStateOf(Kind::Script, &list, true, 0, now).message.empty(), "no note on a good list");
}

void ParserChecks() {
    const std::string ini = "\xEF\xBB\xBF[menu]\r\ncurrent-config=pvp\r\n\r\n[kill_aura]\r\nenabled=1\r\nrange=3.1\r\n"
                            "cps=12\r\n[fly]\r\nenabled=0\r\n; comment\r\n[hud]\r\nfps-x=1\r\n";
    const std::vector<ConfigSection> sections = ConfigSummary(ini);
    Check(sections.size() == 4 && sections[0].name == "menu" && sections[0].enabled == -1 && sections[0].settings == 1,
          "config summary: a section without enabled");
    Check(sections.size() == 4 && sections[1].name == "kill_aura" && sections[1].enabled == 1
                  && sections[1].settings == 2,
          "config summary: an enabled module and its settings");
    Check(sections.size() == 4 && sections[2].name == "fly" && sections[2].enabled == 0 && sections[2].settings == 0,
          "config summary: a disabled module, comments skipped");
    const std::vector<std::string> lines = SourceLines("a\r\nb\n\tc\rd");
    Check(lines.size() == 4 && lines[0] == "a" && lines[2] == "    c" && lines[3] == "d", "source lines");
    Check(SourceLines("").empty() && SourceLines("x\n").size() == 1, "source lines at the ends");
}

// The version items must support follows the launcher's.
void VersionChecks() {
    std::ifstream file(std::string(NICKEL_SOURCE_DIR) + "/desktop/Nickel.Desktop.csproj");
    std::stringstream text;
    text << file.rdbuf();
    const std::string project = text.str();
    const size_t start = project.find("<Version>");
    const size_t end = project.find("</Version>");
    Check(start != std::string::npos && end != std::string::npos
                  && project.substr(start + 9, end - start - 9) == kRunningNickel,
          "kRunningNickel equals the launcher's <Version>");
    int parts[3] = {};
    Check(ParseNickelVersion(kRunningNickel, parts), "kRunningNickel reads as a Nickel version");
}

// ---------------------------------------------------------------------------------------------------------------
// Files the configs do not carry: the PUBLIC setting and the player's [menu] / [fonts]

void WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::wstring IniText(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t value[256] = {};
    GetPrivateProfileStringW(section, key, L"<none>", value, 256, path.c_str());
    return value;
}

std::string SourceFile(const char* relative) {
    std::ifstream file(std::string(NICKEL_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void ConfigFileChecks() {
    const std::wstring root = TemporaryRoot();
    const std::wstring prefs = root + L"\\module-preferences.ini";
    const std::wstring config = root + L"\\configs\\Hypixel PvP.ini";
    const std::string player = "[menu]\r\nmenu-key=Insert\r\nmenu-key-code=45\r\nraknet-fallback=0\r\n"
                               "current-config=mine\r\n[fonts]\r\nsizes=regular:15\r\n"
                               "[kill_aura]\r\nenabled=0\r\nrange=3\r\n";

    // A PUBLIC config has no [menu] or [fonts]: the player's stay, the modules and the HUD come from the config.
    WriteText(prefs, player);
    WriteText(config, "[kill_aura]\r\nenabled=1\r\n[hud]\r\nclock=1\r\n");
    Check(CopyConfigKeepingSections(config, prefs, {L"menu", L"fonts"}), "a PUBLIC config loads");
    Check(IniText(prefs, L"menu", L"menu-key") == L"Insert" && IniText(prefs, L"menu", L"menu-key-code") == L"45"
                  && IniText(prefs, L"menu", L"raknet-fallback") == L"0",
          "loading a config without [menu] keeps the menu keys and Settings toggles");
    Check(IniText(prefs, L"fonts", L"sizes") == L"regular:15", "and the player's [fonts]");
    Check(IniText(prefs, L"kill_aura", L"enabled") == L"1" && IniText(prefs, L"kill_aura", L"range") == L"<none>"
                  && IniText(prefs, L"hud", L"clock") == L"1",
          "the module and HUD sections come from the config");

    // A [menu] in the config does not replace the player's (the panel always keeps it).
    WriteText(prefs, player);
    WriteText(config, "[menu]\r\nmenu-key=F1\r\npublic-community=1\r\n[kill_aura]\r\nenabled=1\r\n");
    CopyConfigKeepingSections(config, prefs, {L"menu", L"fonts"});
    Check(IniText(prefs, L"menu", L"menu-key") == L"Insert"
                  && IniText(prefs, L"menu", L"public-community") == L"<none>",
          "a config's [menu] is dropped for the player's");

    // The player has no [fonts]: none is taken from the config either.
    WriteText(prefs, "[menu]\r\nmenu-key=Insert\r\n");
    WriteText(config, "[fonts]\r\nsizes=bold:99\r\n[fly]\r\nenabled=1\r\n");
    CopyConfigKeepingSections(config, prefs, {L"menu", L"fonts"});
    Check(IniText(prefs, L"fonts", L"sizes") == L"<none>" && IniText(prefs, L"fly", L"enabled") == L"1",
          "a kept section the player lacks is removed");

    // A long [menu] (item profiles) survives whole.
    std::string longMenu = "[menu]\r\n";
    for (int i = 0; i < 40; ++i) {
        longMenu +=
                "item-profile-" + std::to_string(i) + "=" + std::string(300, static_cast<char>('a' + i % 26)) + "\r\n";
    }
    WriteText(prefs, longMenu);
    WriteText(config, "[fly]\r\nenabled=1\r\n");
    CopyConfigKeepingSections(config, prefs, {L"menu"});
    Check(IniText(prefs, L"menu", L"item-profile-39").size() == 255, "a [menu] larger than the first buffer is kept");

    // A failed copy changes nothing.
    WriteText(prefs, player);
    Check(!CopyConfigKeepingSections(root + L"\\configs\\missing.ini", prefs, {L"menu"})
                  && IniText(prefs, L"kill_aura", L"enabled") == L"0",
          "a missing config leaves the preferences alone");

    // The PUBLIC setting is not in the preferences ini: no config load turns it on or off.
    Check(ReadCommunitySetting(root), "PUBLIC is on without its file (on by default)");
    Check(WriteCommunitySetting(root, false), "the setting is written");
    WriteText(prefs, player);
    WriteText(config, "[menu]\r\npublic-community=1\r\n[public]\r\ncommunity=1\r\n");
    CopyFileW(config.c_str(), prefs.c_str(), FALSE); // a local config load or import (a whole copy)
    Check(!ReadCommunitySetting(root), "a config with public-community=1 does not turn PUBLIC on");
    CopyConfigKeepingSections(config, prefs, {L"menu", L"fonts"});
    Check(!ReadCommunitySetting(root), "nor a PUBLIC config load");
    WriteCommunitySetting(root, true);
    WriteText(config, "[menu]\r\npublic-community=0\r\n[public]\r\ncommunity=0\r\n");
    CopyFileW(config.c_str(), prefs.c_str(), FALSE);
    Check(ReadCommunitySetting(root), "a config with public-community=0 does not turn PUBLIC off");
    Check(CommunitySettingPath(root) == root + L"\\public\\settings.ini", "the setting's own file");

    // The menu wires it that way: not a [menu] toggle, and every config load keeps a missing [menu].
    const std::string menu = SourceFile("native/menu.cpp");
    const size_t options = menu.find("MenuOption connectionOptions[]=");
    const size_t optionsEnd = options == std::string::npos ? options : menu.find('\n', options);
    Check(options != std::string::npos
                  && menu.substr(options, optionsEnd - options).find("public-community") == std::string::npos,
          "public-community is not one of the [menu] toggles");
    const size_t load = menu.find("void LoadConfig(const std::string& name,bool keepPlayerMenu){");
    Check(load != std::string::npos
                  && menu.find("CopyConfigKeepingSections", load) < menu.find("ReadPreferences()", load),
          "LoadConfig copies through CopyConfigKeepingSections");
    const std::string ui = SourceFile("native/public_ui.hpp");
    Check(ui.find("LoadConfig(record.Name(), true)") != std::string::npos
                  && ui.find("WritePrivateProfileStringW(L\"menu\"") == std::string::npos,
          "the PUBLIC page loads configs keeping [menu] and writes nothing to [menu]");
    DeleteTree(root);
}

// ---------------------------------------------------------------------------------------------------------------
// The page

void PageChecks() {
    const std::wstring root = TemporaryRoot();
    {
        FolderLibrary library(root + L"\\scripts");
        PageOptions options;
        options.root = root;
        options.library = &library;
        options.now = [] {
            return int64_t(1794765600000LL);
        };
        PublicPage page(options);
        RecordingRequests requests;
        Snapshot snap;
        int64_t now = 1794765600000LL;

        // Off: nothing at all is asked, even when the page is shown.
        page.Tick(snap, false, now);
        page.Shown(Kind::Script);
        page.Select(Kind::Script, "7k2m9x4qa1dz");
        Check(requests.calls.empty(), "off: no request");
        Check(page.StateOf(Kind::Script, snap, false).view == ListView::Off, "off: the enable prompt");
        page.Select(Kind::Script, "");

        // On: revocations once, the first list on Shown; no account, nothing else is asked.
        page.SetRequests(&requests);
        page.Tick(snap, true, now);
        Check(requests.Count("revocations") == 1 && requests.calls.size() == 1, "on: revocations only");
        page.Tick(snap, true, now += 16);
        Check(requests.Count("revocations") == 1 && requests.calls.size() == 1, "revocations are not asked again");
        page.Shown(Kind::Script);
        page.Shown(Kind::Script);
        Check(requests.Count("list") == 1, "the first list is asked once");
        const Recorded* list = requests.Last("list");
        Check(list != nullptr && list->query.kind == Kind::Script && list->query.sort == Sort::Popular
                      && list->query.compat == kRunningNickel && list->query.limit == 24 && list->query.text.empty(),
              "first list: Popular, compatible items, 24");
        Check(requests.Count("lookup") == 0, "no lookup without installed items");

        // Sorts and search (700 ms, from 2 characters; Enter at once).
        page.SetSort(Kind::Script, Sort::Trending);
        Check(requests.Count("list") == 2 && requests.Last("list")->query.sort == Sort::Trending, "Trending");
        page.SetSort(Kind::Script, Sort::Trending);
        Check(requests.Count("list") == 2, "the same sort asks nothing");
        page.Type(Kind::Script, "a");
        page.Tick(snap, true, now += 800);
        Check(requests.Count("list") == 2, "one character is not sent");
        page.Type(Kind::Script, "au");
        page.Tick(snap, true, now += 500);
        Check(requests.Count("list") == 2, "not before 700 ms");
        page.Tick(snap, true, now += 250);
        Check(requests.Count("list") == 3 && requests.Last("list")->query.text == "au"
                      && requests.Last("list")->query.sort == Sort::Trending,
              "sent 700 ms after the last key, with the sort");
        page.Type(Kind::Script, "auto gg");
        page.Submit(Kind::Script);
        Check(requests.Count("list") == 4 && requests.Last("list")->query.text == "auto gg", "Enter sends at once");
        page.Tick(snap, true, now += 800);
        Check(requests.Count("list") == 4, "nothing more once sent");
        page.ClearSearch(Kind::Script);
        Check(requests.Count("list") == 5 && requests.Last("list")->query.text.empty(), "Clear search");

        // Rate limit: Retry waits for the countdown.
        auto limited = std::make_shared<ListResult>();
        limited->query.kind = Kind::Script;
        limited->outcome.status = Status::RateLimited;
        limited->outcome.retryAfter = 20;
        snap.lists[0] = limited;
        page.Tick(snap, true, now += 16);
        Check(page.StateOf(Kind::Script, snap, true).retryIn == 20, "the countdown starts at the answer");
        Check(!page.Retry(Kind::Script) && requests.Count("list") == 5, "Retry refused while it runs");
        page.Tick(snap, true, now += 20000);
        Check(page.Retry(Kind::Script) && requests.Count("list") == 6, "Retry once it is over");
        snap.lists[0].reset();

        // Selecting a card asks its details.
        page.Select(Kind::Script, "7k2m9x4qa1dz");
        Check(requests.Count("detail") == 1 && requests.Last("detail")->id == "7k2m9x4qa1dz", "details on selection");

        // Install a script: the counted download, then held through the library.
        const std::string bytes = "print('gg')\n";
        const Card card = MakeCard("7k2m9x4qa1dz", Kind::Script, "Auto GG", 3, bytes);
        auto older = FileAnswer(card, bytes); // an answer already in the slot must not be used
        snap.file = older;
        page.Tick(snap, true, now += 16);
        Check(page.Install(card), "Install starts");
        Check(requests.Count("file") == 1 && requests.Last("file")->id == card.id && requests.Last("file")->number == 3,
              "the install download is asked by id and version");
        Check(page.Pending().step == Step::Downloading, "downloading");
        page.Tick(snap, true, now += 16);
        Check(page.Pending().step == Step::Downloading && !Exists(root + L"\\scripts\\Auto GG.luau"),
              "the older answer in the slot is not installed");
        snap.file = FileAnswer(card, bytes, Status::Ok, true);
        page.Tick(snap, true, now += 16);
        Check(page.Pending().step == Step::Downloading, "still downloading while the slot loads");
        snap.file = FileAnswer(card, bytes);
        page.Tick(snap, true, now += 16);
        Check(page.Pending().step == Step::None && Exists(root + L"\\scripts\\Auto GG.luau"), "installed");
        Check(library.Held("Auto GG"), "an installed script is held");
        std::vector<std::string> notices = page.TakeNotices();
        Check(HasNotice(notices, "Installed Auto GG (not started)"), "notice: installed, not started");
        const LocalState* local = page.LocalOf(Kind::Script, card.id);
        Check(local != nullptr && local->state == Local::Installed && page.InstalledCount(Kind::Script) == 1,
              "the card is installed");
        std::vector<overlay::ScriptInfo> known = library.List();
        const int at = overlay::FindScript(known, "Auto GG");
        Check(at >= 0 && BadgeOf(local->state, &known[at], true) == Badge::Held, "the card shows HELD");
        Check(page.Start("Auto GG") && !library.Held("Auto GG"), "Start runs it (and only Start)");
        known = library.List();
        Check(BadgeOf(Local::Installed, &known[overlay::FindScript(known, "Auto GG")], true) == Badge::Running,
              "the card shows RUNNING");
        page.TakeNotices();

        // A name taken by a file that did not come from PUBLIC: Keep both.
        {
            std::ofstream mine(root + L"\\scripts\\Chat Filter.luau", std::ios::binary);
            mine << "-- my own\n";
        }
        const std::string filterBytes = "print('filter')\n";
        const Card filter = MakeCard("pqrstvwxyz01", Kind::Script, "Chat Filter", 1, filterBytes);
        page.Install(filter);
        snap.file = FileAnswer(filter, filterBytes);
        page.Tick(snap, true, now += 16);
        Check(page.Pending().step == Step::NameConflict
                      && page.Pending().message.find("Chat Filter") != std::string::npos,
              "name conflict asked");
        page.Resolve(OnConflict::KeepBoth);
        Check(page.Pending().step == Step::None && page.InstalledCount(Kind::Script) == 2,
              "Keep both installs under another name");
        page.TakeNotices();

        // The update check: lookup of installed ids when the page opens again (30 minutes later).
        page.Tick(snap, true, now += 31 * 60 * 1000LL);
        page.Shown(Kind::Script);
        const Recorded* lookup = requests.Last("lookup");
        Check(lookup != nullptr && lookup->ids.size() == 2, "lookup of the installed ids");
        auto answer = std::make_shared<LookupResult>();
        answer->ids = lookup->ids;
        for (const std::string& id : lookup->ids) {
            LookupEntry entry;
            entry.id = id;
            entry.found = true;
            entry.card = id == card.id ? MakeCard(id, Kind::Script, "Auto GG", 4, "print('gg 2')\n") : filter;
            answer->entries.push_back(entry);
        }
        snap.lookup = answer;
        page.Tick(snap, true, now += 16);
        local = page.LocalOf(Kind::Script, card.id);
        Check(local != nullptr && local->state == Local::UpdateAvailable && page.AnyUpdate(Kind::Script),
              "an update is found");
        Check(!page.AnyUpdate(Kind::Config), "no config update");

        // Update: the new version, held again.
        const std::string bytes4 = "print('gg 2')\n";
        const Card card4 = MakeCard(card.id, Kind::Script, "Auto GG", 4, bytes4);
        page.Install(card4, true);
        snap.file = FileAnswer(card4, bytes4);
        page.Tick(snap, true, now += 16);
        Check(page.Pending().step == Step::None && library.Held("Auto GG"), "updated and held again");
        InstallRecord record;
        Check(page.Installs().Find(Kind::Script, card.id, &record) && record.versionNumber == 4, "the record is v4");
        notices = page.TakeNotices();
        Check(HasNotice(notices, "Updated Auto GG to v1.4.0 (not started)"), "notice: updated, not started");

        // Local edits: the update asks before replacing them.
        {
            std::ofstream edit(root + L"\\scripts\\Auto GG.luau", std::ios::binary);
            edit << "print('my edit')\n";
        }
        page.RefreshStates();
        Check(page.LocalOf(Kind::Script, card.id)->state == Local::Modified, "edits seen");
        const std::string bytes5 = "print('gg 3')\n";
        const Card card5 = MakeCard(card.id, Kind::Script, "Auto GG", 5, bytes5);
        page.Install(card5, true);
        snap.file = FileAnswer(card5, bytes5);
        page.Tick(snap, true, now += 16);
        Check(page.Pending().step == Step::Modified, "edits: asked first");
        page.ReplaceEdits();
        Check(page.Pending().step == Step::None && page.Installs().Find(Kind::Script, card.id, &record)
                      && record.versionNumber == 5,
              "edits replaced on request");
        page.TakeNotices();

        // Revocations: the installed file is held, with a notice.
        page.Start("Auto GG");
        auto revocations = std::make_shared<RevocationsResult>();
        Revocation revoked;
        revoked.sha256 = Sha(bytes5);
        revoked.reason = "Steals session tokens.";
        revocations->files.push_back(revoked);
        snap.revocations = revocations;
        page.Tick(snap, true, now += 16);
        notices = page.TakeNotices();
        Check(HasNotice(notices,
                        "Auto GG was revoked by the Nickel team: Steals session tokens. It was stopped and held."),
              "revocation notice");
        Check(library.Held("Auto GG") && page.LocalOf(Kind::Script, card.id)->state == Local::Revoked,
              "the revoked script is held");
        page.Tick(snap, true, now += 16);
        Check(page.TakeNotices().empty(), "the same revocation is told once");

        // Uninstall: local edits ask first.
        {
            std::ofstream edit(root + L"\\scripts\\Auto GG.luau", std::ios::binary);
            edit << "print('edited again')\n";
        }
        Check(page.Uninstall(Kind::Script, card.id, false) == InstallStatus::Modified
                      && Exists(root + L"\\scripts\\Auto GG.luau"),
              "uninstall of an edited file asks");
        Check(page.Uninstall(Kind::Script, card.id, true) == InstallStatus::Done
                      && !Exists(root + L"\\scripts\\Auto GG.luau"),
              "uninstall anyway");
        Check(page.LocalOf(Kind::Script, card.id) == nullptr, "no longer installed");
        page.TakeNotices();

        // Too new for this Nickel: nothing is downloaded.
        const int files = requests.Count("file");
        const Card future = MakeCard("23456789abcd", Kind::Script, "Future", 1, "x", "9.0.0");
        Check(!page.Install(future) && page.Pending().step == Step::Failed && requests.Count("file") == files,
              "an incompatible item is not downloaded");
        page.CancelPending();

        // A damaged download installs nothing.
        const Card broken = MakeCard("abcdefghjkmn", Kind::Script, "Broken", 1, "print(1)\n");
        page.Install(broken);
        snap.file = FileAnswer(broken, "", Status::DigestMismatch);
        page.Tick(snap, true, now += 16);
        Check(page.Pending().step == Step::Failed && page.Pending().message.find("damaged") != std::string::npos
                      && !Exists(root + L"\\scripts\\Broken.luau"),
              "damaged download: nothing installed");
        page.CancelPending();

        // A config: written, never loaded by itself.
        const std::string ini = "[kill_aura]\r\nenabled=1\r\n";
        const Card config = MakeCard("9x8w7v6t5s4r", Kind::Config, "Hypixel PvP", 1, ini);
        page.Install(config);
        snap.file = FileAnswer(config, ini);
        page.Tick(snap, true, now += 16);
        Check(page.Pending().step == Step::None && Exists(root + L"\\configs\\Hypixel PvP.ini")
                      && page.LocalOf(Kind::Config, config.id) != nullptr,
              "config installed");
        notices = page.TakeNotices();
        Check(HasNotice(notices, "Installed Hypixel PvP. Load it when you want it."), "config notice");

        // Source preview: asked once per version, again on Retry.
        page.OpenSource(config);
        Check(requests.Count("source") == 1 && page.View(Kind::Config).sourceOpen, "settings preview asked");
        auto source = std::make_shared<FileResult>();
        source->id = config.id;
        source->number = 1;
        source->bytes = ini;
        snap.source = source;
        page.Tick(snap, true, now += 16);
        page.OpenSource(config);
        Check(requests.Count("source") == 1, "a shown source is not asked again");
        page.OpenSource(config, true);
        Check(requests.Count("source") == 2, "Retry asks again");

        // Stars: local favourites, kept in public\starred.json; no request, no install key, nothing installed.
        Check(!page.IsStarred(Kind::Script, card.id), "not starred at first");
        const size_t calls = requests.calls.size();
        const size_t installed = page.Installs().Records(Kind::Script).size();
        Check(page.Star(Kind::Script, card.id, true), "Star");
        Check(page.IsStarred(Kind::Script, card.id) && Exists(root + L"\\public\\starred.json"),
              "a star shows at once and is saved");
        page.Tick(snap, true, now += 16);
        Check(requests.calls.size() == calls, "a star makes no request");
        Check(!Exists(root + L"\\public\\install.key"), "a star creates no install key");
        Check(page.Installs().Records(Kind::Script).size() == installed && !page.Busy(),
              "a star downloads and installs nothing");
        Check(page.Star(Kind::Script, card.id, false) && !page.IsStarred(Kind::Script, card.id), "Unstar");
        Check(requests.calls.size() == calls && page.TakeNotices().empty(), "an unstar makes no request either");
        Check(page.Star(Kind::Script, card.id, true) && page.IsStarred(Kind::Script, card.id), "starred again");
        Check(!page.Star(Kind::Script, "NOT-AN-ID", true), "an invalid id is refused");

        // Off again: nothing more is asked, and the first list is asked again when PUBLIC comes back.
        const size_t before = requests.calls.size();
        page.SetRequests(nullptr);
        page.Tick(snap, false, now += 16);
        page.Shown(Kind::Script);
        page.Install(card);
        Check(requests.calls.size() == before, "turned off: no request");
        page.SetRequests(&requests);
        page.Tick(snap, true, now += 16);
        page.Shown(Kind::Script);
        Check(requests.Count("list") == 7 && requests.Count("revocations") == 2,
              "turned on again: list and revocations");
    }
    DeleteTree(root);
}

// The page's own messages, the periodic checks, the countdown on sorts and searches, and one install at a time.
void MessageAndTimerChecks() {
    const std::wstring root = TemporaryRoot();
    {
        FolderLibrary library(root + L"\\scripts");
        PageOptions options;
        options.root = root;
        options.library = &library;
        PublicPage page(options);
        RecordingRequests requests;
        Snapshot snap;
        int64_t now = 1794765600000LL;
        page.SetRequests(&requests);
        page.Tick(snap, true, now);
        Check(requests.Count("revocations") == 1, "revocations once PUBLIC is on");

        // Revocations again every 3 hours.
        page.Tick(snap, true, now + 3 * 3600000LL);
        Check(requests.Count("revocations") == 1, "not again at exactly 3 h");
        page.Tick(snap, true, now += 3 * 3600000LL + 1);
        Check(requests.Count("revocations") == 2, "again after 3 h");
        page.Tick(snap, true, now += 16);
        Check(requests.Count("revocations") == 2, "once per period");

        // Install a config, then the update lookup: when the page opens, then at most every 30 minutes.
        const std::string ini = "[kill_aura]\r\nenabled=1\r\n";
        const Card config = MakeCard("9x8w7v6t5s4r", Kind::Config, "Hypixel PvP", 1, ini);
        Check(page.Install(config) && page.Busy(), "a download is on its way");
        const Card other = MakeCard("2b3c4d5e6f7g", Kind::Config, "Other", 1, "[fly]\r\nenabled=1\r\n");
        const int files = requests.Count("file");
        Check(!page.Install(other) && requests.Count("file") == files && page.Pending().card.id == config.id,
              "a second install waits for the first (the buttons say so)");
        snap.file = FileAnswer(config, ini);
        page.Tick(snap, true, now += 16);
        Check(!page.Busy() && page.LocalOf(Kind::Config, config.id) != nullptr, "installed");
        page.TakeNotices();
        Check(!page.Messages().empty() && page.Messages().back().text.find("Installed Hypixel PvP") == 0
                      && !page.Messages().back().problem,
              "the install is told on the page too");
        page.Tick(snap, true, now += 8001);
        Check(page.Messages().empty(), "a good message goes after 8 s");

        page.Shown(Kind::Config);
        Check(requests.Count("lookup") == 1 && requests.Last("lookup")->ids.size() == 1, "lookup when the page opens");
        const int64_t looked = now;
        page.Tick(snap, true, looked + 30 * 60000LL);
        page.Shown(Kind::Config);
        Check(requests.Count("lookup") == 1, "no lookup again at 30 min");
        page.Tick(snap, true, now = looked + 30 * 60000LL + 1);
        page.Shown(Kind::Config);
        Check(requests.Count("lookup") == 2, "lookup again after 30 min");
        page.Shown(Kind::Config);
        Check(requests.Count("lookup") == 2, "once per period");

        // A refused report stays on the page until dismissed, even once the HUD took its notice.
        Check(page.Report(config, "spam", ""), "a report");
        auto refused = std::make_shared<ReportResult>();
        refused->id = config.id;
        refused->outcome.status = Status::Conflict;
        refused->outcome.problem.code = "already_reported";
        snap.report = refused;
        page.Tick(snap, true, now += 16);
        Check(HasNotice(page.TakeNotices(), "You already reported this."), "report refusal: HUD notice");
        page.Tick(snap, true, now += 60000);
        Check(page.Messages().size() == 1 && page.Messages()[0].problem
                      && page.Messages()[0].text == "You already reported this.",
              "report refusal: kept on the page");
        page.DismissMessages();
        Check(page.Messages().empty(), "Dismiss clears the messages");

        // A revocation is a problem message too.
        auto revocations = std::make_shared<RevocationsResult>();
        Revocation revoked;
        revoked.sha256 = Sha(ini);
        revoked.reason = "Breaks servers";
        revocations->files.push_back(revoked);
        snap.revocations = revocations;
        page.Tick(snap, true, now += 16);
        page.TakeNotices();
        Check(page.Messages().size() == 1 && page.Messages()[0].problem
                      && page.Messages()[0].text.find("Hypixel PvP was revoked by the Nickel team: Breaks servers")
                                 == 0,
              "revocation: kept on the page");
        for (int i = 0; i < 5; ++i) {
            page.Message("message " + std::to_string(i));
        }
        Check(page.Messages().size() == 3 && page.Messages().back().text == "message 4", "at most 3 messages");
        page.DismissMessages();

        // A 429: sorts, searches and Clear search wait for the countdown, then the latest is sent once.
        page.Shown(Kind::Script);
        const int lists = requests.Count("list");
        auto limited = std::make_shared<ListResult>();
        limited->query.kind = Kind::Script;
        limited->outcome.status = Status::RateLimited;
        limited->outcome.retryAfter = 20;
        snap.lists[0] = limited;
        page.Tick(snap, true, now += 16);
        page.SetSort(Kind::Script, Sort::Recent);
        page.Type(Kind::Script, "auto");
        page.Submit(Kind::Script);
        page.Tick(snap, true, now += 800);
        page.ClearSearch(Kind::Script);
        Check(requests.Count("list") == lists, "no list while the countdown runs");
        page.Tick(snap, true, now += 10000);
        Check(requests.Count("list") == lists, "still none before it ends");
        page.Tick(snap, true, now += 10000);
        Check(requests.Count("list") == lists + 1 && requests.Last("list")->query.sort == Sort::Recent
                      && requests.Last("list")->query.text.empty(),
              "the latest sort and search once it ends");
        page.Tick(snap, true, now += 16);
        Check(requests.Count("list") == lists + 1, "sent once");

        // Off and on again: revocations are asked again.
        page.SetRequests(nullptr);
        page.Tick(snap, false, now += 16);
        page.SetRequests(&requests);
        page.Tick(snap, true, now += 16);
        Check(requests.Count("revocations") == 3, "revocations again when PUBLIC comes back");
    }
    DeleteTree(root);
}

std::string FileText(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// ---------------------------------------------------------------------------------------------------------------
// The Starred tab

// Stars `id` locally: no request is recorded.
void StarLocally(PublicPage& page, RecordingRequests& requests, Kind kind, const std::string& id, bool on) {
    const size_t calls = requests.calls.size();
    Check(page.Star(kind, id, on) && page.IsStarred(kind, id) == on && requests.calls.size() == calls,
          "a star is saved locally, without a request");
}

// Stars are local favourites: any number in a row, while PUBLIC is on or off, kept across a restart, never a
// request, never the install key.
void StarredTabChecks() {
    const std::wstring root = TemporaryRoot();
    {
        FolderLibrary library(root + L"\\scripts");
        InstallIdentity identity(InstallKeyPath(root));
        PageOptions options;
        options.root = root;
        options.library = &library;
        options.identity = &identity;
        RecordingRequests requests;
        Snapshot snap;
        int64_t now = 1794765600000LL;
        const std::string first = "wnytem123456";
        const std::string second = "thertem12345";
        {
            PublicPage page(options);
            page.SetRequests(&requests);
            page.Tick(snap, true, now);
            const size_t calls = requests.calls.size();
            StarLocally(page, requests, Kind::Script, first, true);
            StarLocally(page, requests, Kind::Script, second, true);
            StarLocally(page, requests, Kind::Config, second, true);
            page.Tick(snap, false, now += 16);
            page.SetRequests(nullptr);
            StarLocally(page, requests, Kind::Script, second, false);
            Check(page.Starred().Ids(Kind::Script).size() == 1 && page.Starred().Ids(Kind::Config).size() == 1,
                  "favourites are kept per kind, and change while PUBLIC is off too");
            Check(requests.calls.size() == calls && page.TakeNotices().empty() && page.Messages().empty(),
                  "no request and no message for stars");
            Check(!Exists(InstallKeyPath(root)) && identity.State() == IdentityState::Missing,
                  "stars never create the install key");
        }
        {
            PublicPage restarted(options);
            Check(restarted.IsStarred(Kind::Script, first) && !restarted.IsStarred(Kind::Script, second)
                          && restarted.IsStarred(Kind::Config, second),
                  "favourites persist across a restart");
        }
    }
    DeleteTree(root);
}

void StarredTabListChecks() {
    const std::wstring root = TemporaryRoot();
    {
        FolderLibrary library(root + L"\\scripts");
        PageOptions options;
        options.root = root;
        options.library = &library;
        PublicPage page(options);
        RecordingRequests requests;
        Snapshot snap;
        int64_t now = 1794765600000LL;
        page.SetRequests(&requests);
        page.Tick(snap, true, now);

        page.ShowStarred(Kind::Script);
        Check(requests.Count("starred") == 0, "nothing starred: no lookup");
        Check(page.StarredState(Kind::Script, snap, true).view == ListView::Empty
                      && page.StarredState(Kind::Script, snap, true).message.find("not starred") != std::string::npos,
              "nothing starred: the empty state says how");

        const Card a = MakeCard("7k2m9x4qa1dz", Kind::Script, "Auto GG", 3, "print(1)\n");
        const Card b = MakeCard("pqrstvwxyz01", Kind::Script, "Chat Filter", 1, "print(2)\n");
        const Card c = MakeCard("9x8w7v6t5s4r", Kind::Config, "Hypixel PvP", 1, "[fly]\r\nenabled=1\r\n");
        StarLocally(page, requests, Kind::Script, a.id, true);
        StarLocally(page, requests, Kind::Script, b.id, true);
        StarLocally(page, requests, Kind::Config, c.id, true);
        StarLocally(page, requests, Kind::Script, "abcdefghjkmn", true);
        page.Shown(Kind::Script);
        const Recorded* asked = requests.Last("starred");
        Check(asked != nullptr && asked->ids.size() == 4, "the Starred tab looks up every starred id");
        Check(requests.Count("list") == 0, "the Starred tab asks no ranking");
        page.Shown(Kind::Script);
        Check(requests.Count("starred") == 1, "asked once while nothing new was starred");
        Check(page.StarredState(Kind::Script, snap, true).view == ListView::Loading, "loading until it answers");

        auto answer = std::make_shared<LookupResult>();
        answer->ids = asked->ids;
        for (const std::string& id : asked->ids) {
            LookupEntry entry;
            entry.id = id;
            entry.found = id != "abcdefghjkmn";
            entry.card = id == a.id ? a : id == b.id ? b : c;
            entry.missing.id = id;
            entry.missing.state = MissingState::Removed;
            answer->entries.push_back(entry);
        }
        snap.starred = answer;
        std::vector<Card> cards = page.StarredCards(Kind::Script, snap);
        Check(cards.size() == 2 && cards[0].id == b.id && cards[1].id == a.id, "script cards, newest star first");
        Check(page.StarredCards(Kind::Config, snap).size() == 1, "the config is on the Configs tab");
        const ListState state = page.StarredState(Kind::Script, snap, true);
        Check(state.view == ListView::Ready && state.message.find("1 starred script is no longer available") == 0,
              "ready, with the removed one counted");
        const std::vector<Missing> gone = page.StarredMissing(Kind::Script, snap);
        Check(gone.size() == 1 && gone[0].id == "abcdefghjkmn" && gone[0].state == MissingState::Removed
                      && MissingText(gone[0]) == "Removed by the Nickel team."
                      && page.StarredMissing(Kind::Config, snap).empty(),
              "the removed one is listed with its state");

        page.Type(Kind::Script, "chat");
        page.Tick(snap, true, now += 800);
        Check(page.StarredCards(Kind::Script, snap).size() == 1 && requests.Count("list") == 0,
              "the search box filters the starred cards without a request");
        Check(page.StarredMissing(Kind::Script, snap).empty(), "a search hides the unavailable rows too");
        page.Type(Kind::Script, "zzz");
        Check(page.StarredState(Kind::Script, snap, true).view == ListView::NoMatch
                      && page.StarredMissing(Kind::Script, snap).empty(),
              "no match: no card and no unavailable row");
        page.Type(Kind::Script, "");
        Check(page.StarredMissing(Kind::Script, snap).size() == 1, "an empty search shows the unavailable rows again");

        StarLocally(page, requests, Kind::Script, a.id, false);
        Check(page.StarredCards(Kind::Script, snap).size() == 1, "an unstarred card leaves the tab");
        StarLocally(page, requests, Kind::Script, "abcdefghjkmn", false);
        Check(page.StarredMissing(Kind::Script, snap).empty()
                      && page.StarredState(Kind::Script, snap, true).message.empty(),
              "a removed item can still be unstarred");
        StarLocally(page, requests, Kind::Script, b.id, false);
        StarLocally(page, requests, Kind::Script, "abcdefghjkmn", true);
        Check(page.StarredCards(Kind::Script, snap).empty() && page.StarredMissing(Kind::Script, snap).size() == 1
                      && page.StarredState(Kind::Script, snap, true).view == ListView::Ready,
              "when every starred item is gone, they are still listed to be unstarred");
        page.Retry(Kind::Script);
        Check(requests.Count("starred") == 2, "Refresh looks them up again");

        page.SetSort(Kind::Script, Sort::Popular);
        Check(!page.View(Kind::Script).starredTab && requests.Count("list") == 1
                      && requests.Last("list")->query.sort == Sort::Popular,
              "a ranking leaves the Starred tab and lists");
    }
    DeleteTree(root);
}

// ---------------------------------------------------------------------------------------------------------------
// Publishing from OTHER -> Scripting and OTHER -> Configs

std::shared_ptr<CheckResult> CheckAnswer(const Recorded& asked, bool ok) {
    auto answer = std::make_shared<CheckResult>();
    answer->kind = asked.kind;
    answer->fileName = asked.fileName;
    answer->sentSha256 = Sha(asked.bytes);
    answer->ref = asked.ref;
    answer->check.ok = ok;
    answer->check.sha256 = Sha(asked.bytes);
    answer->check.capabilities = {Capability::Render};
    if (!ok) {
        ScanNote note;
        note.code = "obfuscated";
        note.message = "Obfuscated code is not allowed.";
        note.line = 4;
        answer->check.errors.push_back(note);
    }
    return answer;
}

std::shared_ptr<PublishResult> PublishAnswer(const Recorded& sent, bool update, const std::string& id, int number) {
    auto answer = std::make_shared<PublishResult>();
    answer->kind = sent.request.kind;
    answer->ref = sent.request.ref;
    answer->update = update;
    answer->itemId = id;
    answer->versionNumber = number;
    answer->state = "published";
    answer->sentSha256 = Sha(sent.request.bytes);
    return answer;
}

void PublishPageChecks() {
    const std::wstring root = TemporaryRoot();
    {
        FolderLibrary library(root + L"\\scripts");
        InstallIdentity identity(InstallKeyPath(root));
        PageOptions options;
        options.root = root;
        options.library = &library;
        options.identity = &identity;
        PublicPage page(options);
        RecordingRequests requests;
        Snapshot snap;
        int64_t now = 1794765600000LL;
        const std::string script = "-- Auto GG\nprint('gg')\n";
        WriteText(root + L"\\scripts\\Auto GG.luau", script);

        // Off: nothing is read or sent.
        Check(!page.BeginPublish(Kind::Script, "Auto GG") && page.Draft().step == PublishStep::Failed
                      && page.Draft().message.find("Turn PUBLIC on") == 0 && requests.calls.empty(),
              "publishing needs PUBLIC on");
        page.SetRequests(&requests);
        page.Tick(snap, true, now);

        // The check first, on the bytes read through the library.
        Check(page.OwnedItem(Kind::Script, "Auto GG") == nullptr, "not published yet");
        Check(page.BeginPublish(Kind::Script, "Auto GG"), "Publish opens the form");
        const Recorded* asked = requests.Last("check");
        Check(asked != nullptr && asked->kind == Kind::Script && asked->fileName == "Auto GG.luau"
                      && asked->bytes == script && asked->ref == "Auto GG",
              "the check of the script's bytes");
        Check(page.Draft().step == PublishStep::Checking && !page.Draft().update, "checking a new item");
        Check(!page.SubmitPublish(PublishFields()).empty() && requests.Count("publish") == 0,
              "nothing is published before the check passes");
        snap.check = CheckAnswer(*asked, true);
        page.Tick(snap, true, now += 16);
        Check(page.Draft().step == PublishStep::Ready && page.Draft().check.capabilities.size() == 1,
              "the check passed");

        PublishFields fields;
        fields.name = "Auto GG";
        fields.summary = "Says gg after a win.";
        fields.description = "Line one.\nLine two.";
        fields.tags = "PvP, chat";
        Check(page.SubmitPublish(fields).find("author name") != std::string::npos && requests.Count("publish") == 0,
              "an author name is required");
        fields.authorName = "  Mash  ";
        fields.tags = "pvp, hud, chat, fun, util, more";
        Check(page.SubmitPublish(fields).find("5 tags") != std::string::npos, "at most 5 tags");
        fields.tags = "PvP, chat";
        Check(page.SubmitPublish(fields).empty(), "Publish is sent");
        const Recorded* sent = requests.Last("publish");
        Check(sent != nullptr && sent->request.name == "Auto GG" && sent->request.summary == "Says gg after a win."
                      && sent->request.tags.size() == 2 && sent->request.tags[0] == "pvp"
                      && sent->request.authorName == "Mash" && sent->request.bytes == script
                      && sent->request.fileName == "Auto GG.luau",
              "the publish request: fields, tags in lower case, the checked bytes");
        Check(ReadAuthorName(root) == "Mash", "the author name is remembered in public\\settings.ini");
        Check(page.Draft().step == PublishStep::Publishing && !page.ClosePublish(),
              "the form cannot be closed while it publishes");

        // The answer: the mapping script -> item, owned by this install.
        std::string header;
        identity.Authorization(&header); // the client's write created the key
        snap.publish = PublishAnswer(*sent, false, "n3wq7p2x9k4a", 1);
        page.Tick(snap, true, now += 16);
        Check(page.Draft().step == PublishStep::Done
                      && page.Draft().message.find("It is public now") != std::string::npos,
              "published: public now");
        Check(HasNotice(page.TakeNotices(), "Published Auto GG"), "a notice");
        const PublishedRecord* owned = page.OwnedItem(Kind::Script, "auto gg");
        Check(owned != nullptr && owned->id == "n3wq7p2x9k4a" && owned->version == 1
                      && Exists(root + L"\\public\\published.json"),
              "the item is remembered as this install's");
        std::string fingerprint;
        identity.Fingerprint(&fingerprint);
        Check(owned != nullptr && owned->owner == fingerprint
                      && FileText(root + L"\\public\\published.json").find(header.substr(8)) == std::string::npos,
              "published.json keeps the fingerprint, never the secret");
        const size_t beforeStar = requests.calls.size();
        Check(page.Star(Kind::Script, "n3wq7p2x9k4a", true) && page.IsStarred(Kind::Script, "n3wq7p2x9k4a")
                      && requests.calls.size() == beforeStar && page.TakeNotices().empty(),
              "one's own publication can be starred, locally");
        Check(page.ClosePublish(), "closed");

        // Publish update: the owner's own item, with a changelog, from the file as it is now.
        const std::string edited = script + "-- v2\n";
        WriteText(root + L"\\scripts\\Auto GG.luau", edited);
        Check(page.BeginPublish(Kind::Script, "Auto GG") && page.Draft().update
                      && page.Draft().itemId == "n3wq7p2x9k4a",
              "Publish update for the owned item");
        snap.check = CheckAnswer(*requests.Last("check"), true);
        page.Tick(snap, true, now += 16);
        Check(!page.SubmitPublish(fields).empty() && requests.Count("publish") == 1,
              "an owned item is never published again as a new item");
        Check(page.SubmitUpdate("  ").find("what changed") != std::string::npos, "a changelog is required");
        Check(page.SubmitUpdate("Waits a random delay.").empty(), "the update is sent");
        const Recorded* version = requests.Last("version");
        Check(version != nullptr && version->id == "n3wq7p2x9k4a"
                      && version->request.changelog == "Waits a random delay." && version->request.bytes == edited,
              "the version request: the owned id, the changelog, the new bytes");
        snap.publish = PublishAnswer(*version, true, "n3wq7p2x9k4a", 2);
        page.Tick(snap, true, now += 16);
        Check(page.Draft().step == PublishStep::Done && page.OwnedItem(Kind::Script, "Auto GG")->version == 2,
              "the update is remembered");
        page.ClosePublish();
        page.TakeNotices();

        // A refused check: nothing can be published.
        WriteText(root + L"\\scripts\\Packed.luau", "local x = 1\n");
        page.BeginPublish(Kind::Script, "Packed");
        snap.check = CheckAnswer(*requests.Last("check"), false);
        page.Tick(snap, true, now += 16);
        Check(page.Draft().step == PublishStep::Failed && page.Draft().errors.size() == 1
                      && page.Draft().errors[0].line == 4 && !page.SubmitPublish(fields).empty(),
              "a refused check shows why and publishes nothing");
        page.ClosePublish();

        // Configs: the player's file, read as it is.
        const std::string ini = std::string("[menu]\r\nname=Caf\xe9\r\n[kill_aura]\r\nenabled=1\r\n");
        CreateDirectoryW((root + L"\\configs").c_str(), nullptr);
        WriteText(root + L"\\configs\\pvp.ini", ini);
        Check(page.BeginPublish(Kind::Config, "pvp"), "Publish a config");
        Check(requests.Last("check")->kind == Kind::Config && requests.Last("check")->fileName == "pvp.ini"
                      && requests.Last("check")->bytes == ini,
              "the config's exact bytes are checked");
        page.ClosePublish();
        Check(!page.BeginPublish(Kind::Config, "missing") && page.Draft().step == PublishStep::Failed,
              "a missing config is not published");
        page.ClosePublish();

        // Names published.json could not remember are refused before any request: the item could never be updated.
        const int checksBefore = requests.Count("check");
        Check(!page.BeginPublish(Kind::Script, std::string(65, 'a'))
                      && page.Draft().message.find("cannot be used on PUBLIC") != std::string::npos,
              "a script name over 64 bytes is refused");
        page.ClosePublish();
        WriteText(root + L"\\configs\\ my cfg.ini", "[menu]\r\n");
        Check(!page.BeginPublish(Kind::Config, " my cfg") && page.Draft().step == PublishStep::Failed,
              "a config name with a leading space is refused");
        page.ClosePublish();
        Check(requests.Count("check") == checksBefore, "no check is asked for a name that cannot be remembered");

        // A lost answer: the item may be public already, so the player is told to look before publishing again.
        WriteText(root + L"\\scripts\\Lost.luau", "print('lost')\n");
        Check(page.BeginPublish(Kind::Script, "Lost") && page.Draft().warning.empty(), "Publish Lost");
        snap.check = CheckAnswer(*requests.Last("check"), true);
        page.Tick(snap, true, now += 16);
        Check(page.SubmitPublish(fields).empty(), "Lost is sent");
        auto lost = PublishAnswer(*requests.Last("publish"), false, "", 0);
        lost->outcome.status = Status::Timeout;
        snap.publish = lost;
        page.Tick(snap, true, now += 16);
        Check(page.Draft().step == PublishStep::Failed
                      && page.Draft().message.find("may already be public") != std::string::npos
                      && page.Draft().message.find("PUBLIC -> Scripts") != std::string::npos,
              "a timeout says the item may already be public");
        page.TakeNotices();
        Check(page.BeginPublish(Kind::Script, "Lost")
                      && page.Draft().warning.find("may already be public") != std::string::npos,
              "checking it again keeps the warning on the form");
        snap.check = CheckAnswer(*requests.Last("check"), true);
        page.Tick(snap, true, now += 16);
        Check(page.SubmitPublish(fields).empty(), "the player can still publish it");
        snap.publish = PublishAnswer(*requests.Last("publish"), false, "w4qk7m2n9x3a", 1);
        page.Tick(snap, true, now += 16);
        Check(page.Draft().step == PublishStep::Done && page.OwnedItem(Kind::Script, "Lost") != nullptr,
              "published and remembered");
        page.ClosePublish();
        page.TakeNotices();
        Check(page.BeginPublish(Kind::Script, "Lost") && page.Draft().update && page.Draft().warning.empty(),
              "a clear answer clears the warning");
        page.ClosePublish();

        // The author name keeps any script (public\settings.ini is UTF-16).
        const std::string unicode = "\xd0\x9c\xd0\xb0\xd1\x88\xd0\xb0 \xe5\x90\x8d\xe5\x89\x8d";
        Check(WriteAuthorName(root, unicode) && ReadAuthorName(root) == unicode, "a non-ANSI author name round trips");
    }
    {
        // An ANSI settings.ini of an earlier build is converted once, keeping its values.
        const std::wstring old = TemporaryRoot();
        CreateDirectoryW((old + L"\\public").c_str(), nullptr);
        WriteText(old + L"\\public\\settings.ini", "[public]\r\ncommunity=1\r\nauthor_name=Mash\r\n");
        const std::string unicode = "\xe5\x90\x8d\xe5\x89\x8d";
        Check(ReadCommunitySetting(old) && ReadAuthorName(old) == "Mash", "the old file is read");
        Check(WriteAuthorName(old, unicode) && ReadAuthorName(old) == unicode && ReadCommunitySetting(old),
              "after the conversion the name round trips and PUBLIC stays on");
        const std::string bytes = FileText(old + L"\\public\\settings.ini");
        Check(bytes.size() > 2 && bytes[0] == '\xFF' && bytes[1] == '\xFE', "the file is UTF-16LE with a BOM");
        Check(WriteCommunitySetting(old, false) && !ReadCommunitySetting(old) && ReadAuthorName(old) == unicode,
              "the Settings toggle writes the same file");
        DeleteTree(old);
    }
    {
        // Another install on the same folder (another key): the item is not its own, so no Publish update.
        FolderLibrary library(root + L"\\scripts");
        InstallIdentity other(root + L"\\other\\install.key");
        std::string header;
        other.Authorization(&header);
        PageOptions options;
        options.root = root;
        options.library = &library;
        options.identity = &other;
        PublicPage page(options);
        RecordingRequests requests;
        Snapshot snap;
        page.SetRequests(&requests);
        page.Tick(snap, true, 1794765600000LL);
        Check(page.OwnedItem(Kind::Script, "Auto GG") == nullptr, "another install does not own the item");
        Check(page.BeginPublish(Kind::Script, "Auto GG") && !page.Draft().update,
              "another install can only publish it as a new item");
    }
    {
        // No key at all (never wrote): nothing is owned.
        FolderLibrary library(root + L"\\scripts");
        InstallIdentity fresh(root + L"\\fresh\\install.key");
        PageOptions options;
        options.root = root;
        options.library = &library;
        options.identity = &fresh;
        PublicPage page(options);
        Check(page.OwnedItem(Kind::Script, "Auto GG") == nullptr && !Exists(root + L"\\fresh\\install.key"),
              "without a key nothing is owned, and none is created");
    }
    DeleteTree(root);
}

// Reports, and no way to delete, unlist or hide an item.
void ReportAndFinalChecks() {
    const std::wstring root = TemporaryRoot();
    {
        FolderLibrary library(root + L"\\scripts");
        PageOptions options;
        options.root = root;
        options.library = &library;
        PublicPage page(options);
        RecordingRequests requests;
        Snapshot snap;
        int64_t now = 1794765600000LL;
        page.SetRequests(&requests);
        page.Tick(snap, true, now);
        const Card card = MakeCard("7k2m9x4qa1dz", Kind::Script, "Auto GG", 3, "print(1)\n");
        Check(!page.Report(card, "boring", ""), "an unknown category is refused");
        Check(!page.Report(card, "rules", ""), "\"rules\" is not a report reason (the menu sends \"other\")");
        Check(page.Report(card, "malicious", "Sends my chat away.") && page.ReportSending(), "a report");
        Check(!page.Report(card, "spam", ""), "one report at a time");
        const Recorded* report = requests.Last("report");
        Check(report != nullptr && report->id == card.id && report->category == "malicious"
                      && report->details == "Sends my chat away.",
              "the report request");
        auto answer = std::make_shared<ReportResult>();
        answer->id = card.id;
        snap.report = answer;
        page.Tick(snap, true, now += 16);
        Check(!page.ReportSending() && HasNotice(page.TakeNotices(), "The report was sent"), "reported");
        // another 409 than already_reported is not shown as a repeated report
        Check(page.Report(card, "other", "Breaks the rules."), "a report with the reason \"other\"");
        auto conflict = std::make_shared<ReportResult>();
        conflict->id = card.id;
        conflict->outcome.status = Status::Conflict;
        conflict->outcome.problem.code = "conflict";
        snap.report = conflict;
        page.Tick(snap, true, now += 16);
        const auto notices = page.TakeNotices();
        Check(!page.ReportSending() && !HasNotice(notices, "You already reported this.")
                      && HasNotice(notices, "could not be sent"),
              "a 409 that is not already_reported: the report could not be sent");
    }
    DeleteTree(root);

    const std::string client = SourceFile("native/public_client.hpp");
    const std::string pageSource = SourceFile("native/public_page.hpp");
    const std::string ui = SourceFile("native/public_ui.hpp");
    Check(!client.empty() && !pageSource.empty() && !ui.empty(), "the PUBLIC sources are read");
    for (const std::string* source : {&client, &pageSource, &ui}) {
        Check(source->find("/unlist") == std::string::npos && source->find("Unpublish") == std::string::npos
                      && source->find("\"PATCH\"") == std::string::npos && source->find("Bearer") == std::string::npos
                      && source->find("/me/") == std::string::npos,
              "no unlist, unpublish, item edit, bearer token or account endpoint");
    }
    Check(SourceFile("native/public_session.hpp").empty()
                  && SourceFile("native/menu.cpp").find("public_session") == std::string::npos,
          "the launcher session reader is gone");
}

} // namespace

int main() {
    WordChecks();
    ListStateChecks();
    ParserChecks();
    VersionChecks();
    ConfigFileChecks();
    PageChecks();
    MessageAndTimerChecks();
    StarredTabChecks();
    StarredTabListChecks();
    PublishPageChecks();
    ReportAndFinalChecks();
    if (failed == 0) {
        std::printf("public_page_checks: all checks passed\n");
        return 0;
    }
    std::printf("public_page_checks: %d failed\n", failed);
    return 1;
}
