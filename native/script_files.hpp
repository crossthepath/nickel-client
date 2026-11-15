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

// The held names of one scripts folder: read from .nickel\held.txt when made, written back at each change. Not locked:
// the engine guards it with scriptGuard (a test with its own mutex).
class ScriptHeldNames {
public:
    explicit ScriptHeldNames(std::wstring folder) : folder_(std::move(folder)) {
        std::string text;
        if (!folder_.empty() && ReadScriptBytes(HeldListPath(folder_), text)) {
            names_ = ParseHeldList(text);
        }
    }

    bool Contains(const std::string& name) const {
        for (const std::string& held : names_) {
            if (SameScriptName(held, name)) {
                return true;
            }
        }
        return false;
    }

    // Adds `name` and writes the list. False when the list cannot be written, or when `name` could not be read back
    // from it (not a HeldListName); nothing changes then. `added` (optional) tells whether the name was missing.
    bool Add(const std::string& name, bool* added = nullptr) {
        if (added) {
            *added = false;
        }
        if (!HeldListName(name)) {
            return false;
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
    // so a script that is started runs; it may be held again after a restart).
    bool Remove(const std::string& name) {
        bool removed = false;
        for (size_t i = 0; i < names_.size();) {
            if (SameScriptName(names_[i], name)) {
                names_.erase(names_.begin() + i);
                removed = true;
            } else {
                ++i;
            }
        }
        return !removed || SaveHeldList(folder_, names_);
    }

    const std::vector<std::string>& Names() const { return names_; }

private:
    std::wstring folder_;
    std::vector<std::string> names_;
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
