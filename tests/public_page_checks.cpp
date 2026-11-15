// What the PUBLIC pages of the menu do, without ImGui and without the network (native/public_page.hpp,
// native/public_session.hpp): requests are recorded, answers are hand-made snapshots, and installs go to a temporary
// folder through a ScriptLibrary over that folder. Nothing here reaches the internet, the Credential Manager or
// %APPDATA%.
#include <windows.h>

#include "public_page.hpp"
#include "public_session.hpp"
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
    bool starred = false;
    std::vector<std::string> ids;
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

    bool Library() override {
        return Add("library", [](Recorded&) {});
    }

    bool Star(const std::string& id, bool starred) override {
        return Add("star", [&](Recorded& r) {
            r.id = id;
            r.starred = starred;
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

std::wstring TemporaryRoot() {
    wchar_t buffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buffer);
    std::wstring root = std::wstring(buffer) + L"nickel-public-page-checks-" + std::to_wstring(GetCurrentProcessId())
                        + L"-" + std::to_wstring(GetTickCount64());
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

    Session session;
    const std::string good =
            "{\"version\":1,\"client_id\":\"nickel-launcher\",\"access_token\":\"nk_at_abc\","
            "\"access_expires_at\":1800003600,\"refresh_token\":\"nk_rt_def\",\"scope\":\"community ai\","
            "\"handle\":\"steve\"}";
    Check(ParseSession(good, &session) && session.accessToken == "nk_at_abc" && session.accessExpiresAt == 1800003600
                  && session.handle == "steve",
          "the launcher's session blob");
    Check(SessionUsable(session, 1800003600 - 61) && !SessionUsable(session, 1800003600 - 60),
          "60 seconds of margin before expiry");
    Session other;
    Check(!ParseSession(
                  "{\"version\":2,\"access_token\":\"nk_at_abc\",\"access_expires_at\":1,\"refresh_token\":\"r\"}",
                  &other),
          "another version reads as signed out");
    Check(!ParseSession("{\"version\":1,\"access_token\":\"nk_at_abc\",\"access_expires_at\":1}", &other),
          "no refresh token reads as signed out");
    Check(!ParseSession(
                  "{\"version\":1,\"access_token\":\"bad token\",\"access_expires_at\":1,\"refresh_token\":\"r\"}",
                  &other),
          "a token with a space is refused");
    Check(!ParseSession("not json", &other) && !ParseSession(std::string(3000, ' '), &other), "garbage and oversize");
    Check(ParseSession("{\"version\":1,\"access_token\":\"t\",\"access_expires_at\":5,\"refresh_token\":\"r\","
                       "\"handle\":\"Bad Handle\"}",
                       &other)
                  && other.handle.empty(),
          "an invalid handle is dropped");
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
        page.Tick(snap, false, true, now);
        page.Shown(Kind::Script);
        page.Select(Kind::Script, "7k2m9x4qa1dz");
        Check(requests.calls.empty(), "off: no request");
        Check(page.StateOf(Kind::Script, snap, false).view == ListView::Off, "off: the enable prompt");
        page.Select(Kind::Script, "");

        // On: revocations once, the first list on Shown, the library only when signed in.
        page.SetRequests(&requests);
        page.Tick(snap, true, false, now);
        Check(requests.Count("revocations") == 1 && requests.Count("library") == 0, "on: revocations, no library");
        page.Tick(snap, true, true, now += 16);
        Check(requests.Count("revocations") == 1 && requests.Count("library") == 1, "signed in: the library once");
        page.Tick(snap, true, true, now += 16);
        Check(requests.Count("library") == 1, "the library is not asked again");
        page.Shown(Kind::Script);
        page.Shown(Kind::Script);
        Check(requests.Count("list") == 1, "the first list is asked once");
        const Recorded* list = requests.Last("list");
        Check(list != nullptr && list->query.kind == Kind::Script && list->query.sort == Sort::Top
                      && list->query.compat == kRunningNickel && list->query.limit == 24 && list->query.text.empty(),
              "first list: Top Rated, compatible items, 24");
        Check(requests.Count("lookup") == 0, "no lookup without installed items");

        // Sorts and search (700 ms, from 2 characters; Enter at once).
        page.SetSort(Kind::Script, Sort::Trending);
        Check(requests.Count("list") == 2 && requests.Last("list")->query.sort == Sort::Trending, "Trending");
        page.SetSort(Kind::Script, Sort::Trending);
        Check(requests.Count("list") == 2, "the same sort asks nothing");
        page.Type(Kind::Script, "a");
        page.Tick(snap, true, true, now += 800);
        Check(requests.Count("list") == 2, "one character is not sent");
        page.Type(Kind::Script, "au");
        page.Tick(snap, true, true, now += 500);
        Check(requests.Count("list") == 2, "not before 700 ms");
        page.Tick(snap, true, true, now += 250);
        Check(requests.Count("list") == 3 && requests.Last("list")->query.text == "au"
                      && requests.Last("list")->query.sort == Sort::Trending,
              "sent 700 ms after the last key, with the sort");
        page.Type(Kind::Script, "auto gg");
        page.Submit(Kind::Script);
        Check(requests.Count("list") == 4 && requests.Last("list")->query.text == "auto gg", "Enter sends at once");
        page.Tick(snap, true, true, now += 800);
        Check(requests.Count("list") == 4, "nothing more once sent");
        page.ClearSearch(Kind::Script);
        Check(requests.Count("list") == 5 && requests.Last("list")->query.text.empty(), "Clear search");

        // Rate limit: Retry waits for the countdown.
        auto limited = std::make_shared<ListResult>();
        limited->query.kind = Kind::Script;
        limited->outcome.status = Status::RateLimited;
        limited->outcome.retryAfter = 20;
        snap.lists[0] = limited;
        page.Tick(snap, true, true, now += 16);
        Check(page.StateOf(Kind::Script, snap, true).retryIn == 20, "the countdown starts at the answer");
        Check(!page.Retry(Kind::Script) && requests.Count("list") == 5, "Retry refused while it runs");
        page.Tick(snap, true, true, now += 20000);
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
        page.Tick(snap, true, true, now += 16);
        Check(page.Install(card), "Install starts");
        Check(requests.Count("file") == 1 && requests.Last("file")->id == card.id && requests.Last("file")->number == 3,
              "the install download is asked by id and version");
        Check(page.Pending().step == Step::Downloading, "downloading");
        page.Tick(snap, true, true, now += 16);
        Check(page.Pending().step == Step::Downloading && !Exists(root + L"\\scripts\\Auto GG.luau"),
              "the older answer in the slot is not installed");
        snap.file = FileAnswer(card, bytes, Status::Ok, true);
        page.Tick(snap, true, true, now += 16);
        Check(page.Pending().step == Step::Downloading, "still downloading while the slot loads");
        snap.file = FileAnswer(card, bytes);
        page.Tick(snap, true, true, now += 16);
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
        page.Tick(snap, true, true, now += 16);
        Check(page.Pending().step == Step::NameConflict
                      && page.Pending().message.find("Chat Filter") != std::string::npos,
              "name conflict asked");
        page.Resolve(OnConflict::KeepBoth);
        Check(page.Pending().step == Step::None && page.InstalledCount(Kind::Script) == 2,
              "Keep both installs under another name");
        page.TakeNotices();

        // The update check: lookup of installed ids when the page opens again (30 minutes later).
        page.Tick(snap, true, true, now += 31 * 60 * 1000LL);
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
        page.Tick(snap, true, true, now += 16);
        local = page.LocalOf(Kind::Script, card.id);
        Check(local != nullptr && local->state == Local::UpdateAvailable && page.AnyUpdate(Kind::Script),
              "an update is found");
        Check(!page.AnyUpdate(Kind::Config), "no config update");

        // Update: the new version, held again.
        const std::string bytes4 = "print('gg 2')\n";
        const Card card4 = MakeCard(card.id, Kind::Script, "Auto GG", 4, bytes4);
        page.Install(card4, true);
        snap.file = FileAnswer(card4, bytes4);
        page.Tick(snap, true, true, now += 16);
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
        page.Tick(snap, true, true, now += 16);
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
        page.Tick(snap, true, true, now += 16);
        notices = page.TakeNotices();
        Check(HasNotice(notices,
                        "Auto GG was revoked by the Nickel team: Steals session tokens. It was stopped and held."),
              "revocation notice");
        Check(library.Held("Auto GG") && page.LocalOf(Kind::Script, card.id)->state == Local::Revoked,
              "the revoked script is held");
        page.Tick(snap, true, true, now += 16);
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
        page.Tick(snap, true, true, now += 16);
        Check(page.Pending().step == Step::Failed && page.Pending().message.find("damaged") != std::string::npos
                      && !Exists(root + L"\\scripts\\Broken.luau"),
              "damaged download: nothing installed");
        page.CancelPending();

        // A config: written, never loaded by itself.
        const std::string ini = "[kill_aura]\r\nenabled=1\r\n";
        const Card config = MakeCard("9x8w7v6t5s4r", Kind::Config, "Hypixel PvP", 1, ini);
        page.Install(config);
        snap.file = FileAnswer(config, ini);
        page.Tick(snap, true, true, now += 16);
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
        page.Tick(snap, true, true, now += 16);
        page.OpenSource(config);
        Check(requests.Count("source") == 1, "a shown source is not asked again");
        page.OpenSource(config, true);
        Check(requests.Count("source") == 2, "Retry asks again");

        // Stars: as asked while the request runs or succeeded, else the library.
        auto libraryAnswer = std::make_shared<LibraryResult>();
        libraryAnswer->stars = {"9x8w7v6t5s4r"};
        snap.library = libraryAnswer;
        Check(PublicPage::Starred(snap, "9x8w7v6t5s4r") && !PublicPage::Starred(snap, card.id),
              "stars from the library");
        page.Star(card.id, true);
        Check(requests.Last("star") != nullptr && requests.Last("star")->starred, "star request");
        auto star = std::make_shared<StarResult>();
        star->id = card.id;
        star->starred = true;
        star->loading = true;
        snap.star = star;
        Check(PublicPage::Starred(snap, card.id), "a star shows at once");
        auto refused = std::make_shared<StarResult>(*star);
        refused->loading = false;
        refused->outcome.status = Status::SignedOut;
        snap.star = refused;
        page.Tick(snap, true, true, now += 16);
        Check(!PublicPage::Starred(snap, card.id), "a refused star is not shown");
        Check(HasNotice(page.TakeNotices(), "Sign in again through the Nickel launcher"), "signed out: sign-in hint");

        // Off again: nothing more is asked, and the first list is asked again when PUBLIC comes back.
        const size_t before = requests.calls.size();
        page.SetRequests(nullptr);
        page.Tick(snap, false, true, now += 16);
        page.Shown(Kind::Script);
        page.Install(card);
        Check(requests.calls.size() == before, "turned off: no request");
        page.SetRequests(&requests);
        page.Tick(snap, true, true, now += 16);
        page.Shown(Kind::Script);
        Check(requests.Count("list") == 7 && requests.Count("revocations") == 2,
              "turned on again: list and revocations");
    }
    DeleteTree(root);
}

} // namespace

int main() {
    WordChecks();
    ListStateChecks();
    ParserChecks();
    VersionChecks();
    PageChecks();
    if (failed == 0) {
        std::printf("public_page_checks: all checks passed\n");
        return 0;
    }
    std::printf("public_page_checks: %d failed\n", failed);
    return 1;
}
