#pragma once
// The files of the scripts folder that the engine's ScriptLibrary (EngineScriptLibrary in script_engine.hpp) reads and
// writes for other components: script files with their revision, the held list and the backups. Windows, no Lua, no
// menu; the scripts folder is a parameter, so tests/script_files_checks.cpp runs it on a temporary folder.
// Layout inside the scripts folder (the folder scan lists only *.luau and lib\*.luau, so none of these runs):
//   <name>.luau                       a script
//   .<name>.<thread>.<n>.write        a script being written, moved over <name>.luau when complete
//   .nickel\held.txt                  the held list (ParseHeldList / FormatHeldList in script_library.hpp)
//   .nickel\backup\<name>.luau.bak    the previous version of a script, one level
// Locks: `files` (scriptFiles in the engine) orders the library's writers for the whole of a call; `guard`
// (scriptGuard) is taken only to change the held list (and, for a write, to move the finished temporary file over the
// script in the same section) and is never held while waiting for `files`, so the two cannot deadlock and the game
// thread waits at most for the held list and one file move, never for a script's text to be written.
// Design: docs/architecture/script-management-next.md.
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "json_value.hpp"
#include "script_library.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace overlay {

// UTF-8 to UTF-16, for file names.
inline std::wstring ScriptWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring value(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), count);
    return value;
}

inline std::wstring ScriptFilePath(const std::wstring& folder, const std::string& name) {
    return folder + L"\\" + ScriptWide(name) + L".luau";
}

inline std::wstring HeldListPath(const std::wstring& folder) {
    return folder + L"\\.nickel\\held.txt";
}

inline std::wstring ScriptBackupPath(const std::wstring& folder, const std::string& name) {
    return folder + L"\\.nickel\\backup\\" + ScriptWide(name) + L".luau.bak";
}

// True when `path` is an existing file (not a folder).
inline bool ScriptFileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The bytes of the file at `path`; false when it cannot be opened or read.
inline bool ReadScriptBytes(const std::wstring& path, std::string& bytes) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file) {
        return false;
    }
    bytes.clear();
    char buffer[8192];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.append(buffer, count);
    }
    const bool ok = !ferror(file);
    fclose(file);
    return ok;
}

// The SHA-256 of `bytes` as 64 lowercase hex characters (Windows CNG, as game::Sha256), or empty when CNG fails.
inline std::string ScriptSha256(const std::string& bytes) {
    uint8_t digest[32]{};
    if (bytes.size() > 0xffffffffu
        || !BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                      reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                      static_cast<ULONG>(bytes.size()), digest, 32))) {
        return {};
    }
    return Sha256Hex(digest);
}

// The text of the script file at `path` and its revision, from one read.
inline bool ReadScriptSource(const std::wstring& path, ScriptSource& source) {
    if (!ReadScriptBytes(path, source.text)) {
        return false;
    }
    source.sha256 = ScriptSha256(source.text);
    return !source.sha256.empty();
}

// Creates the scripts folder (and its parent), and .nickel\backup inside it.
inline void MakeScriptFolders(const std::wstring& folder) {
    CreateDirectoryW(folder.substr(0, folder.find_last_of(L'\\')).c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    CreateDirectoryW((folder + L"\\.nickel").c_str(), nullptr);
    CreateDirectoryW((folder + L"\\.nickel\\backup").c_str(), nullptr);
}

// Writes `bytes` to a new temporary file next to `target`, ".<stem>.<thread>.<n>.write" (a name the folder scan
// ignores and no other call uses), and sets `temporary` to its path. False when it cannot be written completely; the
// temporary file is deleted then.
inline bool WriteScriptTemporary(const std::wstring& target, const std::wstring& stem, const std::string& bytes,
                                 std::wstring& temporary) {
    static std::atomic<unsigned> writes{0};
    temporary = target.substr(0, target.find_last_of(L'\\')) + L"\\." + stem + L"."
            + std::to_wstring(GetCurrentThreadId()) + L"." + std::to_wstring(++writes) + L".write";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") || !file) {
        return false;
    }
    const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = fclose(file) == 0;
    if (!written || !closed) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

// Moves a temporary file of WriteScriptTemporary over `target`; the temporary file is deleted on failure.
inline bool MoveScriptTemporary(const std::wstring& temporary, const std::wstring& target) {
    if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(temporary.c_str());
    return false;
}

// Writes `bytes` to `target` in one step: to a temporary file (WriteScriptTemporary), then moved over the target.
inline bool WriteScriptFileInOneStep(const std::wstring& target, const std::wstring& stem, const std::string& bytes) {
    std::wstring temporary;
    return WriteScriptTemporary(target, stem, bytes, temporary) && MoveScriptTemporary(temporary, target);
}

// Writes the held list of `folder` (.nickel\held.txt).
inline bool SaveHeldList(const std::wstring& folder, const std::vector<std::string>& names) {
    if (folder.empty()) {
        return false;
    }
    MakeScriptFolders(folder);
    return WriteScriptFileInOneStep(HeldListPath(folder), L"held", FormatHeldList(names));
}

// What reading a file of the scripts folder gave.
enum class ScriptListRead : uint8_t { Missing, Read, Unreadable };

// The held list of `folder` into `names`: Missing (no held.txt: nothing is held), Read, or Unreadable (it exists but
// cannot be opened or read, or is not text: ReadableHeldList), with `names` left empty.
inline ScriptListRead ReadHeldListFile(const std::wstring& folder, std::vector<std::string>& names) {
    names.clear();
    const std::wstring path = HeldListPath(folder);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ScriptListRead::Missing
                                                                              : ScriptListRead::Unreadable;
    }
    std::string text;
    if (!ReadScriptBytes(path, text) || !ReadableHeldList(text)) {
        return ScriptListRead::Unreadable;
    }
    names = ParseHeldList(text);
    return ScriptListRead::Read;
}

// The scripts that PUBLIC installed in `folder`, from scripts\.public\installed.json (public_installer.hpp: items[]
// with "file" "<name>.luau"): Missing (no PUBLIC script), Read, or Unreadable (it exists but cannot be read or parsed).
// Every entry with a .luau file counts, without the installer's other checks, so a damaged entry still names its
// script.
inline ScriptListRead ReadPublicScriptNames(const std::wstring& folder, std::vector<std::string>& names) {
    names.clear();
    const std::wstring path = folder + L"\\.public\\installed.json";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ScriptListRead::Missing
                                                                              : ScriptListRead::Unreadable;
    }
    std::string text;
    nickel::json::Value root;
    if (!ReadScriptBytes(path, text) || !nickel::json::Parse(text, &root) || !root.IsObject()
        || !root["items"].IsArray()) {
        return ScriptListRead::Unreadable;
    }
    const nickel::json::Value& items = root["items"];
    for (size_t i = 0; i < items.size(); ++i) {
        const nickel::json::Value& file = items.At(i)["file"];
        if (!file.IsString()) {
            continue;
        }
        const std::string& value = file.AsString();
        if (value.size() > 5 && _stricmp(value.c_str() + value.size() - 5, ".luau") == 0) {
            names.push_back(value.substr(0, value.size() - 5));
        }
    }
    return ScriptListRead::Read;
}

// The held names of one scripts folder: read from .nickel\held.txt when made, written back at each change. Not locked:
// the engine guards it with scriptGuard (a test with its own mutex).
// Fails closed: when held.txt exists but cannot be read (ReadHeldListFile: Unreadable), the list is DEGRADED. Every
// script that PUBLIC installed (ReadPublicScriptNames; every script when installed.json cannot be read either) is then
// held, so a community script waiting for Start never loads; held.txt is never written from that state (Add and Remove
// change this session only); Refresh, at each folder scan, reads it again and, once it can, merges this session's
// changes into it and writes it.
class ScriptHeldNames {
public:
    explicit ScriptHeldNames(std::wstring folder) : folder_(std::move(folder)) { Load(); }

    bool Contains(const std::string& name) const {
        if (Listed(names_, name)) {
            return true;
        }
        if (!degraded_ || Listed(released_, name)) {
            return false;
        }
        return allPublic_ || Listed(public_, name);
    }

    // Adds `name` and writes the list. False when the list cannot be written, or when `name` could not be read back
    // from it (not a HeldListName); nothing changes then. `added` (optional) tells whether the name was missing.
    // Degraded: held for this session only (nothing is written), true.
    bool Add(const std::string& name, bool* added = nullptr) {
        if (added) {
            *added = false;
        }
        if (!HeldListName(name)) {
            return false;
        }
        if (degraded_) {
            const bool wasHeld = Contains(name);
            Erase(released_, name);
            if (!Listed(names_, name)) {
                names_.push_back(name);
            }
            if (added) {
                *added = !wasHeld;
            }
            return true;
        }
        if (Contains(name)) {
            return true;
        }
        names_.push_back(name);
        if (!SaveHeldList(folder_, names_)) {
            names_.pop_back();
            return false;
        }
        if (added) {
            *added = true;
        }
        return true;
    }

    // Removes `name`, then writes the list; false when it could not be written (the name is removed in memory anyway,
    // so a script that is started runs; it may be held again after a restart). Degraded: released for this session
    // only (nothing is written), false.
    bool Remove(const std::string& name) {
        if (degraded_) {
            Erase(names_, name);
            if (HeldListName(name) && !Listed(released_, name)) {
                released_.push_back(name);
            }
            return false;
        }
        const bool removed = Erase(names_, name);
        return !removed || SaveHeldList(folder_, names_);
    }

    // At each folder scan: reads a degraded list again (and installed.json while it stays degraded). Nothing to do
    // otherwise.
    void Refresh() {
        if (degraded_) {
            Load();
        }
    }

    // True while held.txt cannot be read (see the class comment).
    bool Degraded() const { return degraded_; }

    // A line for the log when the list became degraded or readable again since the last call, else empty: logged once
    // per change, not at each scan.
    std::string TakeNotice() {
        std::string notice;
        notice.swap(notice_);
        return notice;
    }

    const std::vector<std::string>& Names() const { return names_; }

private:
    static bool Listed(const std::vector<std::string>& list, const std::string& name) {
        for (const std::string& held : list) {
            if (SameScriptName(held, name)) {
                return true;
            }
        }
        return false;
    }

    static bool Erase(std::vector<std::string>& list, const std::string& name) {
        bool erased = false;
        for (size_t i = 0; i < list.size();) {
            if (SameScriptName(list[i], name)) {
                list.erase(list.begin() + i);
                erased = true;
            } else {
                ++i;
            }
        }
        return erased;
    }

    void Load() {
        if (folder_.empty()) {
            return;
        }
        std::vector<std::string> names;
        if (ReadHeldListFile(folder_, names) == ScriptListRead::Unreadable) {
            if (!degraded_) {
                notice_ = "Scripts: .nickel\\held.txt cannot be read; every PUBLIC script stays held until it can.";
            }
            degraded_ = true;
            allPublic_ = ReadPublicScriptNames(folder_, public_) == ScriptListRead::Unreadable;
            return;
        }
        if (!degraded_) {
            names_ = std::move(names);
            return;
        }
        // Readable again: the file's list with this session's holds and starts.
        const std::vector<std::string> fromFile = names;
        for (const std::string& name : names_) {
            if (!Listed(names, name)) {
                names.push_back(name);
            }
        }
        for (const std::string& name : released_) {
            Erase(names, name);
        }
        degraded_ = false;
        allPublic_ = false;
        public_.clear();
        released_.clear();
        names_ = std::move(names);
        if (names_ != fromFile) {
            SaveHeldList(folder_, names_);
        }
        notice_ = "Scripts: .nickel\\held.txt is readable again.";
    }

    std::wstring folder_;
    std::vector<std::string> names_;     // the list; degraded, the names held in this session
    bool degraded_ = false;
    bool allPublic_ = false;             // degraded and installed.json unreadable too: every script is held
    std::vector<std::string> public_;    // degraded: the scripts PUBLIC installed
    std::vector<std::string> released_;  // degraded: the names started or removed in this session
    std::string notice_;
};

// The library's writes to one scripts folder (see the locks at the top).
struct ScriptFolderFiles {
    std::wstring folder;
    std::mutex& files;
    std::recursive_mutex& guard;
    ScriptHeldNames& held;
};

inline bool ScriptHasBackup(const std::wstring& folder, const std::string& name) {
    return !folder.empty() && ScriptFileExists(ScriptBackupPath(folder, name));
}

// WriteScriptFile with `files` already taken and `text` already without its byte order mark and within the limit.
// Order: the new text goes to a temporary file first, then the replaced file to the backup, then, under `guard`, the
// name is held and the temporary file moved over the script. A failure before the backup changes nothing; a failure
// after it leaves the script file and the held list as they were, with the backup now a copy of the unchanged file.
// The held entry and the move share one `guard` section, so the folder scan never sees a script held whose file did
// not change: a running script that is not held keeps running when the write fails.
inline ScriptFileResult WriteScriptFileLocked(const ScriptFolderFiles& store, const std::string& name,
                                              const std::string& text, const std::string& expectedSha256) {
    MakeScriptFolders(store.folder);
    const std::wstring target = ScriptFilePath(store.folder, name);
    const bool exists = ScriptFileExists(target);
    std::string current;
    if (!exists) {
        if (!expectedSha256.empty()) {
            return ScriptFileResult::Conflict;
        }
    } else {
        if (!ReadScriptBytes(target, current)) {
            return ScriptFileResult::Failed;
        }
        if (expectedSha256.empty() || ScriptSha256(current) != expectedSha256) {
            return ScriptFileResult::Conflict;
        }
    }
    std::wstring temporary;
    if (!WriteScriptTemporary(target, ScriptWide(name), text, temporary)) {
        return ScriptFileResult::Failed;
    }
    if (exists && !WriteScriptFileInOneStep(ScriptBackupPath(store.folder, name), ScriptWide(name), current)) {
        DeleteFileW(temporary.c_str());
        return ScriptFileResult::Failed;
    }
    std::lock_guard<std::recursive_mutex> lock(store.guard);
    bool added = false;
    if (!store.held.Add(name, &added)) {
        DeleteFileW(temporary.c_str());
        return ScriptFileResult::Failed;
    }
    if (!MoveScriptTemporary(temporary, target)) {
        if (added) {
            store.held.Remove(name);
        }
        return ScriptFileResult::Failed;
    }
    return ScriptFileResult::Done;
}

// ScriptLibrary::WriteSource on `store`. `known`: `name` is a script of List() (its name as the engine has it), which
// may be a name ValidScriptName refuses for new files.
inline ScriptFileResult WriteScriptFile(const ScriptFolderFiles& store, const std::string& name, bool known,
                                        const std::string& source, const std::string& expectedSha256) {
    if (!known && !ValidScriptName(name)) {
        return ScriptFileResult::InvalidName;
    }
    const std::string text = WithoutUtf8Bom(source);
    if (text.size() > kMaxScriptBytes) {
        return ScriptFileResult::TooLarge;
    }
    if (store.folder.empty()) {
        return ScriptFileResult::Failed;
    }
    std::lock_guard<std::mutex> lock(store.files);
    return WriteScriptFileLocked(store, name, text, expectedSha256);
}

// ScriptLibrary::RestoreBackup on `store`: WriteScriptFile with the backup's bytes, under the same lock.
inline ScriptFileResult RestoreScriptBackup(const ScriptFolderFiles& store, const std::string& name, bool known,
                                            const std::string& expectedSha256) {
    if (!known && !ValidScriptName(name)) {
        return ScriptFileResult::InvalidName;
    }
    if (store.folder.empty()) {
        return ScriptFileResult::NotFound;
    }
    std::lock_guard<std::mutex> lock(store.files);
    std::string backup;
    const std::wstring path = ScriptBackupPath(store.folder, name);
    if (!ScriptFileExists(path)) {
        return ScriptFileResult::NotFound;
    }
    if (!ReadScriptBytes(path, backup)) {
        return ScriptFileResult::Failed;
    }
    const std::string text = WithoutUtf8Bom(backup);
    if (text.size() > kMaxScriptBytes) {
        return ScriptFileResult::TooLarge;
    }
    return WriteScriptFileLocked(store, name, text, expectedSha256);
}

// ScriptLibrary::Remove on `store`: the file at revision expectedSha256 is copied to the backup, then deleted, and the
// name leaves the held list.
inline ScriptFileResult RemoveScriptFile(const ScriptFolderFiles& store, const std::string& name, bool known,
                                         const std::string& expectedSha256) {
    if (!known && !ValidScriptName(name)) {
        return ScriptFileResult::InvalidName;
    }
    if (expectedSha256.empty() || store.folder.empty()) {
        return ScriptFileResult::Conflict;
    }
    std::lock_guard<std::mutex> lock(store.files);
    const std::wstring target = ScriptFilePath(store.folder, name);
    std::string current;
    if (!ScriptFileExists(target)) {
        return ScriptFileResult::Conflict;
    }
    if (!ReadScriptBytes(target, current)) {
        return ScriptFileResult::Failed;
    }
    if (ScriptSha256(current) != expectedSha256) {
        return ScriptFileResult::Conflict;
    }
    MakeScriptFolders(store.folder);
    if (!WriteScriptFileInOneStep(ScriptBackupPath(store.folder, name), ScriptWide(name), current)
        || !DeleteFileW(target.c_str())) {
        return ScriptFileResult::Failed;
    }
    std::lock_guard<std::recursive_mutex> guard(store.guard);
    store.held.Remove(name);
    return ScriptFileResult::Done;
}

} // namespace overlay
