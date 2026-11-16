#pragma once
// Installs, updates and uninstalls PUBLIC scripts and configs, and keeps their provenance. No network, no menu, no
// ImGui: tests/public_client_checks.cpp runs it on a temporary folder with a ScriptLibrary over that folder.
//
// - Scripts are written only through overlay::ScriptLibrary (WriteSource / Remove / Hold), so every installed or
//   updated script is HELD: it never runs before the player presses Start, and the engine keeps the replaced file as
//   its one-level backup. Nothing here starts a script.
// - Configs are written byte for byte as served (configs\.public\tmp, then MoveFileEx into place); a replaced config
//   is first moved to configs\.public\backup\<name>-<unix ms>.ini. A config never loads by itself.
// - Provenance sidecars, one per folder: scripts\.public\installed.json and configs\.public\installed.json, entries
//   { id, version_number, label, sha256, file, installed_at, skipped_version }. sha256 is the hash of the file as
//   written (a script's leading UTF-8 byte order mark is dropped by the library; download_sha256 then keeps the
//   served hash). No running state is stored: that is ScriptInfo::held and the script's state.
// - Every change of an installed file names the hash it expects (the sidecar's, or the file's own when the player
//   chose to replace local edits), so a file the player edited is never overwritten or removed silently.
// - Revocations: a script whose hash (sidecar or on disk) is revoked is held through ScriptLibrary::Hold.
// Every call may be made from any thread; one mutex orders them. Paths: InstallerOptions::root, %APPDATA%\Nickel in
// the game.
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json_value.hpp"
#include "public_model.hpp"
#include "script_library.hpp"

namespace nickel::pub {

// One installed item (provenance only).
struct InstallRecord {
    std::string id;
    int versionNumber = 0;          // the installed version's identity
    std::string label;              // for display
    std::string sha256;             // of the file as written
    std::string downloadSha256;     // of the file as served, when it differs from sha256 (a dropped BOM)
    std::string file;               // file name in its folder: "<name>.luau" or "<name>.ini"
    std::string installedAt;        // ISO 8601 UTC
    int skippedVersion = 0;         // "Skip this version": no update pill for that number

    // The file name without its extension: the script's name for ScriptLibrary, the config's name for the menu.
    std::string Name() const {
        const size_t dot = file.find_last_of('.');
        return dot == std::string::npos ? file : file.substr(0, dot);
    }
};

// A verified download to install (the FileResult of public_client.hpp with its card).
struct Package {
    Kind kind = Kind::Script;
    std::string id;
    int versionNumber = 0;
    std::string label;
    std::string name;     // the item's name; the file is named after it
    std::string sha256;   // as served, checked again against bytes
    std::string bytes;
};

inline Package PackageOf(const Card& card, const VersionRef& version, std::string bytes) {
    Package package;
    package.kind = card.kind;
    package.id = card.id;
    package.versionNumber = version.number;
    package.label = version.label;
    package.name = card.name;
    package.sha256 = version.sha256;
    package.bytes = std::move(bytes);
    return package;
}

enum class InstallStatus : uint8_t {
    Done,
    NameConflict,   // a file with that name exists and did not come from PUBLIC: ask Keep both / Replace / Cancel
    Modified,       // the file is not the one installed (local edits): ask before replacing or removing it
    NotInstalled,   // Update / Uninstall of an id without a record
    BadPackage,     // the bytes do not match their hash, are too large, hold a NUL byte, or a script is not UTF-8
    InvalidName,
    Failed,         // the disk refused; nothing changed
};

inline const char* InstallStatusName(InstallStatus status) {
    static const char* const kNames[] = {"done", "name conflict", "modified", "not installed", "bad package",
            "invalid name", "failed"};
    const size_t index = static_cast<size_t>(status);
    return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "unknown";
}

// What Install does when the file name is taken by a file that did not come from PUBLIC.
enum class OnConflict : uint8_t {
    Ask,       // NameConflict, nothing written
    KeepBoth,  // the first free "<name> (2)", "<name> (3)"...
    Replace,   // the other file is replaced (scripts: the engine's backup; configs: configs\.public\backup)
};

struct InstallOutcome {
    InstallStatus status = InstallStatus::Failed;
    std::string name;                                  // the file's name (without extension) used or in the way
    InstallRecord record;                              // when Done (Install, Update)
    std::vector<overlay::CompileError> compileErrors;  // scripts: shown with the "Installed (not started)" notice
};

// A record whose file was found on the revocation list.
struct RevokedInstall {
    Kind kind = Kind::Script;
    std::string id;
    std::string name;
    std::string reason;
    bool held = false;   // scripts: ScriptLibrary::Hold succeeded (false: the script is not listed yet, try again)
};

// An installed item's state for the cards (DecideLocal).
struct LocalState {
    std::string id;
    Local state = Local::None;
    InstallRecord record;
};

// The local state of an installed record. entry: its lookup answer, or nullptr when there is none yet.
inline Local DecideLocal(const InstallRecord& record, const LookupEntry* entry, bool revoked, bool modified) {
    if (revoked) {
        return Local::Revoked;
    }
    if (entry != nullptr && !entry->found) {
        switch (entry->missing.state) {
            case MissingState::Deleted:
            case MissingState::Removed:
                return Local::Removed;
            case MissingState::Hidden:
                return Local::Unavailable;
            case MissingState::NotFound:
            default:
                return Local::NotListed;
        }
    }
    if (modified) {
        return Local::Modified;
    }
    if (entry != nullptr) {
        const int latest = entry->card.latest.number;
        if (latest > record.versionNumber && latest != record.skippedVersion) {
            return Local::UpdateAvailable;
        }
        if (latest < record.versionNumber) {
            return Local::Withdrawn;  // never presented as an update
        }
    }
    return Local::Installed;
}

// ---------------------------------------------------------------------------------------------------------------
// Files

namespace detail {

inline std::wstring ToWide(std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(count > 0 ? count : 0), L'\0');
    if (count > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
    }
    return wide;
}

inline std::string HashHex(std::string_view data) {
    uint8_t digest[32] = {};
    if (data.size() > 0xFFFFFFFFu
            || !BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                    reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), digest,
                    sizeof(digest)))) {
        return std::string();
    }
    return overlay::Sha256Hex(digest);
}

inline bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The bytes of a file of at most maxBytes; false when it is missing, larger, or cannot be read.
inline bool ReadAll(const std::wstring& path, std::string* bytes, size_t maxBytes) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        return false;
    }
    std::string data;
    char buffer[16 * 1024];
    bool ok = true;
    for (;;) {
        const size_t got = fread(buffer, 1, sizeof(buffer), file);
        data.append(buffer, got);
        if (data.size() > maxBytes) {
            ok = false;
            break;
        }
        if (got < sizeof(buffer)) {
            ok = ferror(file) == 0;
            break;
        }
    }
    fclose(file);
    if (ok) {
        *bytes = std::move(data);
    }
    return ok;
}

inline bool WriteAll(const std::wstring& path, std::string_view bytes) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }
    const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (!written || !flushed || !closed) {
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

inline void MakeFolder(const std::wstring& folder) {
    CreateDirectoryW(folder.c_str(), nullptr);
}

// A file name stem from an item's name: characters Windows refuses dropped, spaces collapsed, at most 64 bytes, and a
// name the script library accepts (overlay::ValidScriptName, also used for configs); "public-<id>" otherwise.
inline std::string CleanStem(std::string_view name, std::string_view id) {
    std::string stem;
    bool space = false;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool refused = u < 0x20 || u == 0x7F || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
        if (refused) {
            continue;
        }
        if (c == ' ') {
            space = !stem.empty();
            continue;
        }
        if (space) {
            stem += ' ';
            space = false;
        }
        stem += c;
    }
    if (stem.size() > 64) {
        size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        stem.resize(cut);
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
        stem.pop_back();
    }
    while (!stem.empty() && (stem.front() == ' ' || stem.front() == '.')) {
        stem.erase(stem.begin());
    }
    if (!overlay::ValidScriptName(stem)) {
        stem = "public-" + std::string(id);
    }
    return stem;
}

// The first stem among "<stem>", "<stem> (2)", ... "<stem> (99)" with no file "<folder>\<stem><extension>".
// The first of "stem", "stem (2)", ... that is a valid name, has no file in `folder` and is not `taken` (a name another
// PUBLIC record owns, even when its file is gone).
inline std::string FreeStem(const std::wstring& folder, const std::string& stem, const wchar_t* extension,
        const std::function<bool(const std::string&)>& taken) {
    for (int n = 1; n < 100; ++n) {
        const std::string suffix = n == 1 ? std::string() : " (" + std::to_string(n) + ")";
        std::string base = stem;
        while (base.size() + suffix.size() > 64) {
            base.pop_back();
            while (!base.empty() && (static_cast<unsigned char>(base.back()) & 0xC0) == 0x80) {
                base.pop_back();
            }
            if (!base.empty() && (static_cast<unsigned char>(base.back()) & 0xC0) == 0xC0) {
                base.pop_back();
            }
        }
        const std::string candidate = base + suffix;
        if (overlay::ValidScriptName(candidate) && !FileExists(folder + L"\\" + ToWide(candidate) + extension)
                && !(taken && taken(candidate))) {
            return candidate;
        }
    }
    return std::string();
}

inline bool NoNulByte(std::string_view bytes) {
    return bytes.find('\0') == std::string_view::npos;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Sidecar (installed.json)

inline std::string FormatSidecar(const std::vector<InstallRecord>& records) {
    json::Value items = json::Value::MakeArray();
    for (const InstallRecord& record : records) {
        json::Value item = json::Value::MakeObject();
        item.Set("id", json::Value::MakeString(record.id));
        item.Set("version_number", json::Value::MakeInt(record.versionNumber));
        item.Set("label", json::Value::MakeString(record.label));
        item.Set("sha256", json::Value::MakeString(record.sha256));
        if (!record.downloadSha256.empty()) {
            item.Set("download_sha256", json::Value::MakeString(record.downloadSha256));
        }
        item.Set("file", json::Value::MakeString(record.file));
        item.Set("installed_at", json::Value::MakeString(record.installedAt));
        item.Set("skipped_version", record.skippedVersion > 0 ? json::Value::MakeInt(record.skippedVersion)
                                                              : json::Value());
        items.Push(std::move(item));
    }
    json::Value root = json::Value::MakeObject();
    root.Set("format", json::Value::MakeInt(1));
    root.Set("items", std::move(items));
    return json::Serialize(root) + "\n";
}

// The records of a sidecar for files with `extension` (".luau" or ".ini"); invalid entries and repeated ids or files
// are dropped. An unreadable document gives no record.
inline std::vector<InstallRecord> ParseSidecar(std::string_view text, std::string_view extension) {
    std::vector<InstallRecord> records;
    json::Value root;
    if (!json::Parse(text, &root) || !root.IsObject()) {
        return records;
    }
    const json::Value& items = root["items"];
    for (size_t i = 0; i < items.size() && items.IsArray(); ++i) {
        const json::Value& item = items.At(i);
        InstallRecord record;
        std::string ignored;
        const detail::Reader read{item, &ignored, ""};
        int64_t number = 0;
        int64_t skipped = 0;
        const json::Value* skip = item.Find("skipped_version");
        int64_t at = 0;
        if (!item.IsObject() || !read.Pattern("id", &record.id, true, ValidId)
                || !read.Int("version_number", &number, true, 1, 1000000)
                || !read.Pattern("label", &record.label, false, ValidLabel)
                || !read.Pattern("sha256", &record.sha256, true, ValidSha256)
                || !read.Pattern("download_sha256", &record.downloadSha256, false, ValidSha256)
                || !read.String("file", &record.file, true, 80, detail::TextRule::OneLine)
                || !read.String("installed_at", &record.installedAt, true, 40, detail::TextRule::OneLine)
                || !ParseIsoTime(record.installedAt, &at)
                || (skip != nullptr && !skip->IsNull() && !read.Int("skipped_version", &skipped, false, 1, 1000000))) {
            continue;
        }
        const std::string name = record.Name();
        if (record.file.size() <= extension.size()
                || record.file.compare(record.file.size() - extension.size(), extension.size(), extension) != 0
                || !overlay::ValidScriptName(name)) {
            continue;
        }
        bool repeated = false;
        for (const InstallRecord& seen : records) {
            repeated = repeated || seen.id == record.id || overlay::SameScriptName(seen.Name(), name);
        }
        if (repeated) {
            continue;
        }
        record.versionNumber = static_cast<int>(number);
        record.skippedVersion = static_cast<int>(skipped);
        records.push_back(std::move(record));
    }
    return records;
}

// ---------------------------------------------------------------------------------------------------------------
// Installer

struct InstallerOptions {
    std::wstring root;                          // %APPDATA%\Nickel in the game; a temporary folder in tests
    overlay::ScriptLibrary* scripts = nullptr;  // writes, removes and holds scripts; its Folder() is root\scripts
    std::function<int64_t()> now;               // unix ms; the system clock when empty
};

class Installer {
public:
    explicit Installer(InstallerOptions options) : options_(std::move(options)) { Reload(); }

    Installer(const Installer&) = delete;
    Installer& operator=(const Installer&) = delete;

    // The scripts folder: the library's, else root\scripts.
    std::wstring ScriptsFolder() const {
        if (options_.scripts != nullptr) {
            const std::wstring folder = options_.scripts->Folder();
            if (!folder.empty()) {
                return folder;
            }
        }
        return options_.root + L"\\scripts";
    }

    std::wstring ConfigsFolder() const { return options_.root + L"\\configs"; }

    // Reads both sidecars again (after another process changed them).
    void Reload() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Kind kind : {Kind::Script, Kind::Config}) {
            std::string text;
            records_[Slot(kind)] = detail::ReadAll(SidecarPath(kind), &text, 4 * 1024 * 1024)
                    ? ParseSidecar(text, Extension(kind))
                    : std::vector<InstallRecord>();
        }
    }

    std::vector<InstallRecord> Records(Kind kind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_[Slot(kind)];
    }

    bool Find(Kind kind, const std::string& id, InstallRecord* out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const InstallRecord* record = FindLocked(kind, id);
        if (record != nullptr && out != nullptr) {
            *out = *record;
        }
        return record != nullptr;
    }

    // Installs a download. An id that is already installed is updated instead (Update, keeping local edits safe).
    InstallOutcome Install(const Package& package, OnConflict onConflict = OnConflict::Ask) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FindLocked(package.kind, package.id) != nullptr) {
            return UpdateLocked(package, false);
        }
        InstallOutcome outcome;
        if (!ValidPackage(package)) {
            outcome.status = InstallStatus::BadPackage;
            return outcome;
        }
        const std::wstring folder = Folder(package.kind);
        const wchar_t* extension = package.kind == Kind::Script ? L".luau" : L".ini";
        std::string name = detail::CleanStem(package.name, package.id);
        if (onConflict == OnConflict::KeepBoth) {
            const std::vector<InstallRecord>& owned = records_[Slot(package.kind)];
            name = detail::FreeStem(folder, name, extension, [&owned](const std::string& candidate) {
                for (const InstallRecord& other : owned) {
                    if (overlay::SameScriptName(other.Name(), candidate)) {
                        return true;
                    }
                }
                return false;
            });
            if (name.empty()) {
                outcome.status = InstallStatus::InvalidName;
                return outcome;
            }
        }
        outcome.name = name;
        for (const InstallRecord& other : records_[Slot(package.kind)]) {
            if (overlay::SameScriptName(other.Name(), name)) {
                // Another PUBLIC item has that file: Replace would leave its record pointing at this one.
                outcome.status = InstallStatus::NameConflict;
                return outcome;
            }
        }
        // What the write replaced, so that a failed save puts it back: a script the engine kept as its backup, or
        // the path a replaced config was moved to.
        bool replacedScript = false;
        std::wstring configBackup;
        outcome.status = package.kind == Kind::Script
                ? WriteScript(name, package.bytes, onConflict == OnConflict::Replace, &replacedScript)
                : WriteConfig(name, package, onConflict == OnConflict::Replace, std::string(), &configBackup);
        if (outcome.status != InstallStatus::Done) {
            return outcome;
        }
        InstallRecord record = RecordOf(package, name);
        std::vector<InstallRecord> records = records_[Slot(package.kind)];
        records.push_back(record);
        if (!SaveLocked(package.kind, records)) {
            UndoWrite(package.kind, name, record.sha256, replacedScript, configBackup);
            outcome.status = InstallStatus::Failed;
            return outcome;
        }
        outcome.record = record;
        AddCompileErrors(package, &outcome);
        return outcome;
    }

    // Replaces an installed item's file with a newer (or the current) version. The file must still be the one
    // installed; replaceLocalEdits replaces the player's edited file too (scripts: the engine keeps it as the backup;
    // configs: moved to configs\.public\backup). The script is held again until Start.
    InstallOutcome Update(const Package& package, bool replaceLocalEdits = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        return UpdateLocked(package, replaceLocalEdits);
    }

    // Removes an installed item. The file must be the one installed, unless evenIfModified. A file already gone only
    // drops the record. Scripts: ScriptLibrary::Remove (backup, unload, no longer held); configs: moved to
    // configs\.public\backup.
    InstallOutcome Uninstall(Kind kind, const std::string& id, bool evenIfModified = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        InstallOutcome outcome;
        const InstallRecord* found = FindLocked(kind, id);
        if (found == nullptr) {
            outcome.status = InstallStatus::NotInstalled;
            return outcome;
        }
        const InstallRecord record = *found;
        outcome.name = record.Name();
        const std::wstring path = FilePath(kind, record.Name());
        std::string current;
        if (detail::FileExists(path)) {
            if (!detail::ReadAll(path, &current, 16 * 1024 * 1024)) {
                outcome.status = InstallStatus::Failed;
                return outcome;
            }
            const std::string currentSha = detail::HashHex(current);
            if (currentSha != record.sha256 && !evenIfModified) {
                outcome.status = InstallStatus::Modified;
                return outcome;
            }
            if (kind == Kind::Script) {
                if (options_.scripts == nullptr) {
                    outcome.status = InstallStatus::Failed;
                    return outcome;
                }
                const overlay::ScriptFileResult removed = options_.scripts->Remove(record.Name(), currentSha);
                if (removed != overlay::ScriptFileResult::Done) {
                    outcome.status = removed == overlay::ScriptFileResult::Conflict ? InstallStatus::Modified
                                                                                    : InstallStatus::Failed;
                    return outcome;
                }
            } else if (!BackUpConfig(record.Name())) {
                outcome.status = InstallStatus::Failed;
                return outcome;
            }
        }
        std::vector<InstallRecord> records = records_[Slot(kind)];
        Erase(&records, id);
        revoked_.erase(std::remove_if(revoked_.begin(), revoked_.end(),
                               [&](const RevokedInstall& r) { return r.kind == kind && r.id == id; }),
                revoked_.end());
        outcome.status = SaveLocked(kind, records) ? InstallStatus::Done : InstallStatus::Failed;
        return outcome;
    }

    // "Skip this version": no update for that number (a later one shows again).
    bool SkipVersion(Kind kind, const std::string& id, int number) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<InstallRecord> records = records_[Slot(kind)];
        for (InstallRecord& record : records) {
            if (record.id == id) {
                record.skippedVersion = number;
                return SaveLocked(kind, records);
            }
        }
        return false;
    }

    // Compares the revocation list with every record (its hashes and the hash of its file on disk). Matching
    // scripts are held (ScriptLibrary::Hold): they stop within a second and stay stopped until Start. Replaces the
    // previous result; returns the matches.
    std::vector<RevokedInstall> ApplyRevocations(const std::vector<Revocation>& revocations) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<RevokedInstall> matches;
        for (Kind kind : {Kind::Script, Kind::Config}) {
            for (const InstallRecord& record : records_[Slot(kind)]) {
                std::string onDisk;
                const std::wstring path = FilePath(kind, record.Name());
                const std::string diskSha = detail::ReadAll(path, &onDisk, 16 * 1024 * 1024) ? detail::HashHex(onDisk)
                                                                                             : std::string();
                for (const Revocation& revocation : revocations) {
                    if (revocation.sha256 != record.sha256 && revocation.sha256 != record.downloadSha256
                            && revocation.sha256 != diskSha) {
                        continue;
                    }
                    RevokedInstall match;
                    match.kind = kind;
                    match.id = record.id;
                    match.name = record.Name();
                    match.reason = revocation.reason;
                    match.held = kind == Kind::Script && options_.scripts != nullptr
                            && options_.scripts->Hold(record.Name());
                    matches.push_back(std::move(match));
                    break;
                }
            }
        }
        revoked_ = matches;
        return matches;
    }

    // The last ApplyRevocations matches.
    std::vector<RevokedInstall> Revoked() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return revoked_;
    }

    // The local state of every record of a kind, joined with a lookup answer (nullptr: none yet). Reads each file to
    // tell local edits: call it when an answer arrives, not every frame.
    std::vector<LocalState> States(Kind kind, const std::vector<LookupEntry>* lookup = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<LocalState> states;
        for (const InstallRecord& record : records_[Slot(kind)]) {
            const LookupEntry* entry = nullptr;
            for (size_t i = 0; lookup != nullptr && i < lookup->size(); ++i) {
                if ((*lookup)[i].id == record.id) {
                    entry = &(*lookup)[i];
                }
            }
            bool revoked = false;
            for (const RevokedInstall& r : revoked_) {
                revoked = revoked || (r.kind == kind && r.id == record.id);
            }
            std::string bytes;
            const bool modified = !detail::ReadAll(FilePath(kind, record.Name()), &bytes, 16 * 1024 * 1024)
                    || detail::HashHex(bytes) != record.sha256;
            LocalState state;
            state.id = record.id;
            state.record = record;
            state.state = DecideLocal(record, entry, revoked, modified);
            states.push_back(std::move(state));
        }
        return states;
    }

    std::wstring SidecarPath(Kind kind) const { return Folder(kind) + L"\\.public\\installed.json"; }

    std::wstring FilePath(Kind kind, const std::string& name) const {
        return Folder(kind) + L"\\" + detail::ToWide(name) + (kind == Kind::Script ? L".luau" : L".ini");
    }

private:
    static size_t Slot(Kind kind) { return static_cast<size_t>(kind); }

    static const char* Extension(Kind kind) { return kind == Kind::Script ? ".luau" : ".ini"; }

    std::wstring Folder(Kind kind) const { return kind == Kind::Script ? ScriptsFolder() : ConfigsFolder(); }

    int64_t Now() const {
        if (options_.now) {
            return options_.now();
        }
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        const uint64_t ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        return static_cast<int64_t>(ticks / 10000 - 11644473600000ULL);
    }

    const InstallRecord* FindLocked(Kind kind, const std::string& id) const {
        for (const InstallRecord& record : records_[Slot(kind)]) {
            if (record.id == id) {
                return &record;
            }
        }
        return nullptr;
    }

    static void Erase(std::vector<InstallRecord>* records, const std::string& id) {
        for (size_t i = 0; i < records->size();) {
            if ((*records)[i].id == id) {
                records->erase(records->begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    static bool ValidPackage(const Package& package) {
        if (!ValidId(package.id) || package.versionNumber < 1 || !ValidSha256(package.sha256)
                || !ValidLabel(package.label) || package.bytes.size() > kMaxFileBytes
                || !detail::NoNulByte(package.bytes) || detail::HashHex(package.bytes) != package.sha256) {
            return false;
        }
        return package.kind != Kind::Script || overlay::WellFormedUtf8(package.bytes);
    }

    // The hash of the file as the library writes it (without a leading byte order mark), or as served for configs.
    static std::string WrittenSha(const Package& package) {
        return package.kind == Kind::Script ? detail::HashHex(overlay::WithoutUtf8Bom(package.bytes)) : package.sha256;
    }

    InstallRecord RecordOf(const Package& package, const std::string& name) const {
        InstallRecord record;
        record.id = package.id;
        record.versionNumber = package.versionNumber;
        record.label = package.label;
        record.sha256 = WrittenSha(package);
        record.downloadSha256 = record.sha256 == package.sha256 ? std::string() : package.sha256;
        record.file = name + Extension(package.kind);
        record.installedAt = FormatIsoTime(Now());
        return record;
    }

    void AddCompileErrors(const Package& package, InstallOutcome* outcome) const {
        if (package.kind == Kind::Script && options_.scripts != nullptr) {
            outcome->compileErrors = options_.scripts->Compile(package.bytes);
        }
    }

    static InstallStatus FromScriptResult(overlay::ScriptFileResult result) {
        switch (result) {
            case overlay::ScriptFileResult::Done:
                return InstallStatus::Done;
            case overlay::ScriptFileResult::InvalidName:
                return InstallStatus::InvalidName;
            case overlay::ScriptFileResult::TooLarge:
                return InstallStatus::BadPackage;
            case overlay::ScriptFileResult::Conflict:
                return InstallStatus::NameConflict;
            default:
                return InstallStatus::Failed;
        }
    }

    // A new script file; with `replace`, an existing file of that name is replaced (the engine backs it up, and
    // *replaced says so).
    InstallStatus WriteScript(const std::string& name, const std::string& bytes, bool replace, bool* replaced) {
        *replaced = false;
        if (options_.scripts == nullptr) {
            return InstallStatus::Failed;
        }
        std::string expected;
        if (replace) {
            std::string current;
            const std::wstring path = FilePath(Kind::Script, name);
            if (detail::FileExists(path)) {
                if (!detail::ReadAll(path, &current, 16 * 1024 * 1024)) {
                    return InstallStatus::Failed;
                }
                expected = detail::HashHex(current);
            }
        }
        const InstallStatus status = FromScriptResult(options_.scripts->WriteSource(name, bytes, expected));
        *replaced = status == InstallStatus::Done && !expected.empty();
        return status;
    }

    // Writes a config file: to configs\.public\tmp, then moved into place. expectedSha: the hash the existing file
    // must have ("" when it must not exist, unless `replace`). A replaced file goes to configs\.public\backup first;
    // *backupOut (when given) is that path after a successful write, else empty.
    InstallStatus WriteConfig(const std::string& name, const Package& package, bool replace,
            const std::string& expectedSha, std::wstring* backupOut = nullptr) {
        if (backupOut != nullptr) {
            backupOut->clear();
        }
        const std::wstring folder = ConfigsFolder();
        detail::MakeFolder(options_.root);
        detail::MakeFolder(folder);
        detail::MakeFolder(folder + L"\\.public");
        detail::MakeFolder(folder + L"\\.public\\tmp");
        const std::wstring target = FilePath(Kind::Config, name);
        const bool exists = detail::FileExists(target);
        if (exists && !replace) {
            std::string current;
            if (expectedSha.empty()) {
                return InstallStatus::NameConflict;
            }
            if (!detail::ReadAll(target, &current, 16 * 1024 * 1024)) {
                return InstallStatus::Failed;
            }
            if (detail::HashHex(current) != expectedSha) {
                return InstallStatus::Modified;
            }
        }
        const std::wstring temporary = folder + L"\\.public\\tmp\\" + detail::ToWide(package.id) + L"."
                + std::to_wstring(GetCurrentThreadId()) + L"." + std::to_wstring(++writes_) + L".tmp";
        if (!detail::WriteAll(temporary, package.bytes)) {
            return InstallStatus::Failed;
        }
        std::wstring backup;
        if (exists) {
            backup = BackupPath(name);
            if (backup.empty() || !MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temporary.c_str());
                return InstallStatus::Failed;
            }
        }
        // No MOVEFILE_REPLACE_EXISTING: a file created meanwhile under that name is never overwritten.
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(temporary.c_str());
            if (!backup.empty()) {
                MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
            }
            return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? InstallStatus::NameConflict
                                                                               : InstallStatus::Failed;
        }
        if (backupOut != nullptr) {
            *backupOut = backup;
        }
        return InstallStatus::Done;
    }

    std::wstring BackupPath(const std::string& name) const {
        const std::wstring folder = ConfigsFolder() + L"\\.public\\backup";
        detail::MakeFolder(ConfigsFolder() + L"\\.public");
        detail::MakeFolder(folder);
        const std::wstring base = folder + L"\\" + detail::ToWide(name) + L"-" + std::to_wstring(Now());
        for (int n = 0; n < 100; ++n) {
            const std::wstring candidate = base + (n == 0 ? L"" : L"-" + std::to_wstring(n)) + L".ini";
            if (!detail::FileExists(candidate)) {
                return candidate;
            }
        }
        return std::wstring();
    }

    bool BackUpConfig(const std::string& name) {
        const std::wstring backup = BackupPath(name);
        return !backup.empty()
                && MoveFileExW(FilePath(Kind::Config, name).c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
    }

    // Takes back an install whose record could not be saved. A replaced file is put back: the engine's backup of a
    // script (RestoreBackup swaps it with the PUBLIC file, which becomes the backup; the restored script stays held
    // until Start), or the moved config. Only a fresh install is removed.
    void UndoWrite(Kind kind, const std::string& name, const std::string& sha, bool replacedScript,
            const std::wstring& configBackup) {
        if (kind == Kind::Script) {
            if (options_.scripts != nullptr) {
                if (replacedScript) {
                    options_.scripts->RestoreBackup(name, sha);
                } else {
                    options_.scripts->Remove(name, sha);
                }
            }
        } else if (!configBackup.empty()) {
            MoveFileExW(configBackup.c_str(), FilePath(Kind::Config, name).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        } else {
            DeleteFileW(FilePath(Kind::Config, name).c_str());
        }
    }

    InstallOutcome UpdateLocked(const Package& package, bool replaceLocalEdits) {
        InstallOutcome outcome;
        const InstallRecord* found = FindLocked(package.kind, package.id);
        if (found == nullptr) {
            outcome.status = InstallStatus::NotInstalled;
            return outcome;
        }
        const InstallRecord previous = *found;
        const std::string name = previous.Name();
        outcome.name = name;
        if (!ValidPackage(package)) {
            outcome.status = InstallStatus::BadPackage;
            return outcome;
        }
        const std::wstring path = FilePath(package.kind, name);
        std::string current;
        std::string currentSha;
        if (detail::FileExists(path)) {
            if (!detail::ReadAll(path, &current, 16 * 1024 * 1024)) {
                outcome.status = InstallStatus::Failed;
                return outcome;
            }
            currentSha = detail::HashHex(current);
            if (currentSha != previous.sha256 && !replaceLocalEdits) {
                outcome.status = InstallStatus::Modified;
                return outcome;
            }
        }
        if (package.kind == Kind::Script) {
            if (options_.scripts == nullptr) {
                outcome.status = InstallStatus::Failed;
                return outcome;
            }
            const overlay::ScriptFileResult written = options_.scripts->WriteSource(name, package.bytes, currentSha);
            outcome.status = written == overlay::ScriptFileResult::Conflict ? InstallStatus::Modified
                                                                            : FromScriptResult(written);
        } else {
            outcome.status = WriteConfig(name, package, !currentSha.empty(), std::string());
        }
        if (outcome.status != InstallStatus::Done) {
            return outcome;
        }
        InstallRecord record = RecordOf(package, name);
        record.skippedVersion = 0;
        std::vector<InstallRecord> records = records_[Slot(package.kind)];
        for (InstallRecord& entry : records) {
            if (entry.id == package.id) {
                entry = record;
            }
        }
        if (!SaveLocked(package.kind, records)) {
            outcome.status = InstallStatus::Failed;  // the new file is in place; its record keeps the old version
            return outcome;
        }
        revoked_.erase(std::remove_if(revoked_.begin(), revoked_.end(),
                               [&](const RevokedInstall& r) { return r.kind == package.kind && r.id == package.id; }),
                revoked_.end());
        outcome.record = record;
        AddCompileErrors(package, &outcome);
        return outcome;
    }

    // Writes a sidecar in one step (temporary file, then moved over it) and keeps the records on success.
    bool SaveLocked(Kind kind, const std::vector<InstallRecord>& records) {
        const std::wstring folder = Folder(kind);
        detail::MakeFolder(options_.root);
        detail::MakeFolder(folder);
        detail::MakeFolder(folder + L"\\.public");
        const std::wstring target = SidecarPath(kind);
        const std::wstring temporary = target + L"." + std::to_wstring(GetCurrentThreadId()) + L"."
                + std::to_wstring(++writes_) + L".tmp";
        if (!detail::WriteAll(temporary, FormatSidecar(records))) {
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        records_[Slot(kind)] = records;
        return true;
    }

    InstallerOptions options_;
    mutable std::mutex mutex_;
    std::vector<InstallRecord> records_[2];
    std::vector<RevokedInstall> revoked_;
    unsigned writes_ = 0;
};

}  // namespace nickel::pub
