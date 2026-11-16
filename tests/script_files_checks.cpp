// The file half of the engine's ScriptLibrary (native/script_files.hpp) on a temporary scripts folder: revisions, the
// write contract (create, replace, conflicts, size, names), backups and their restore, removal, the held list on
// disk, and two threads writing the same script. Windows only, no Lua, no game; nothing outside the temporary folder
// is touched.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "script_files.hpp"

namespace {

using namespace overlay;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

// Deletes `path` and everything under it.
void DeleteTree(const std::wstring& path) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = path + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}

// The names in `folder` matching `pattern` (FindFirstFileW, as the engine's folder scan lists *.luau).
std::set<std::wstring> Listed(const std::wstring& folder, const wchar_t* pattern) {
    std::set<std::wstring> names;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((folder + L"\\" + pattern).c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            names.insert(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return names;
}

std::string Bytes(const std::wstring& path) {
    std::string bytes;
    ReadScriptBytes(path, bytes);
    return bytes;
}

// One temporary scripts folder with the locks and the held list the engine would give it.
struct Folder {
    std::wstring root;
    std::wstring scripts;
    std::mutex files;
    std::recursive_mutex guard;
    ScriptHeldNames held;
    ScriptFolderFiles store;

    explicit Folder(const std::wstring& base)
        : root(base), scripts(base + L"\\scripts"), held(scripts), store{scripts, files, guard, held} {}

    bool Held(const std::string& name) {
        std::lock_guard<std::recursive_mutex> lock(guard);
        return held.Contains(name);
    }

    std::string Revision(const std::string& name) {
        ScriptSource source;
        return ReadScriptSource(ScriptFilePath(scripts, name), source) ? source.sha256 : std::string();
    }
};

std::wstring NewRoot() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"nickel-script-files-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(GetTickCount64());
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

void Revisions() {
    Check(ScriptSha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "CNG SHA-256 of \"abc\" is the FIPS 180-2 digest");
    Check(ScriptSha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    Check(ValidSha256Hex(ScriptSha256("print(1)")), "a revision is 64 lowercase hex characters");
}

void Writes(const std::wstring& base) {
    Folder f(base);
    const std::wstring file = ScriptFilePath(f.scripts, "Hello");

    Check(WriteScriptFile(f.store, "Hello", false, "print(1)\n", "") == ScriptFileResult::Done,
          "a new file with \"\" (must not exist): Done, and the scripts folder is made");
    Check(Bytes(file) == "print(1)\n" && f.Held("Hello") && f.Held("hello"), "the file is written and the name held");
    Check(!ScriptHasBackup(f.scripts, "Hello"), "a new file has no backup");
    Check(Bytes(HeldListPath(f.scripts)) == "Hello\n", "the held list is on disk");

    const std::string first = f.Revision("Hello");
    Check(first == ScriptSha256("print(1)\n"), "the revision is the SHA-256 of the bytes written");
    Check(WriteScriptFile(f.store, "Hello", false, "print(2)\n", "") == ScriptFileResult::Conflict,
          "\"\" on an existing file: Conflict");
    Check(WriteScriptFile(f.store, "Hello", false, "print(2)\n", ScriptSha256("other")) == ScriptFileResult::Conflict,
          "a stale revision: Conflict");
    Check(WriteScriptFile(f.store, "Missing", false, "print(2)\n", first) == ScriptFileResult::Conflict,
          "a revision for a file that does not exist: Conflict");
    Check(Bytes(file) == "print(1)\n" && !ScriptHasBackup(f.scripts, "Hello"), "a conflict changes nothing");

    Check(WriteScriptFile(f.store, "Hello", false, "print(2)\n", first) == ScriptFileResult::Done,
          "the revision last read: Done");
    Check(Bytes(file) == "print(2)\n" && Bytes(ScriptBackupPath(f.scripts, "Hello")) == "print(1)\n",
          "the replaced file is the backup");

    Check(WriteScriptFile(f.store, "Bom", false, "\xef\xbb\xbfprint(3)", "") == ScriptFileResult::Done
                  && Bytes(ScriptFilePath(f.scripts, "Bom")) == "print(3)",
          "a leading byte order mark is dropped");

    Check(WriteScriptFile(f.store, "a/b", false, "x", "") == ScriptFileResult::InvalidName
                  && WriteScriptFile(f.store, ".hidden", false, "x", "") == ScriptFileResult::InvalidName,
          "a name that is not a ValidScriptName nor in the list: InvalidName");
    Check(WriteScriptFile(f.store, " Spaced", true, "x", "") == ScriptFileResult::Done,
          "a name the list already shows is accepted even if new files may not use it");

    const std::string limit(kMaxScriptBytes, '-');
    Check(WriteScriptFile(f.store, "Big", false, limit + "-", "") == ScriptFileResult::TooLarge
                  && !ScriptFileExists(ScriptFilePath(f.scripts, "Big")),
          "more than 1 MiB: TooLarge, nothing written");
    Check(WriteScriptFile(f.store, "Big", false, "\xef\xbb\xbf" + limit, "") == ScriptFileResult::Done
                  && Bytes(ScriptFilePath(f.scripts, "Big")).size() == kMaxScriptBytes,
          "exactly 1 MiB after the byte order mark is dropped: Done");

    Check(Listed(f.scripts, L"*.luau") == std::set<std::wstring>({L"Hello.luau", L"Bom.luau", L" Spaced.luau", L"Big.luau"}),
          "the folder scan sees only the scripts: no temporary file, backup or held list");
    Check(Listed(f.scripts, L"*.write").empty() && Listed(f.scripts + L"\\.nickel", L"*.write").empty()
                  && Listed(f.scripts + L"\\.nickel\\backup", L"*.write").empty(),
          "no temporary file is left");

    ScriptHeldNames reread(f.scripts);
    Check(reread.Names() == std::vector<std::string>({"Hello", "Bom", " Spaced", "Big"}),
          "the held list reads back after a restart, with the known name ValidScriptName refuses");
}

// Existing user files whose names ValidScriptName refuses for new files (longer than 64 bytes, 40 accented letters,
// a leading space or dot): a write accepted for them keeps them held after a restart, so the new code does not run
// before Start.
void KnownNames(const std::wstring& base) {
    Folder f(base);
    std::string accented;
    for (int i = 0; i < 40; ++i) {
        accented += "\xc3\xa9";
    }
    const std::vector<std::string> names = {std::string(65, 'n'), accented, " Lead", ".dot"};
    bool written = true;
    for (const std::string& name : names) {
        MakeScriptFolders(f.scripts);
        WriteScriptFileInOneStep(ScriptFilePath(f.scripts, name), L"user", "print('user')");
        written &= WriteScriptFile(f.store, name, true, "print('new')", f.Revision(name)) == ScriptFileResult::Done
                && Bytes(ScriptFilePath(f.scripts, name)) == "print('new')" && f.Held(name);
    }
    Check(written, "a write to a known name ValidScriptName refuses: Done and held");
    ScriptHeldNames restarted(f.scripts);
    bool stillHeld = true;
    for (const std::string& name : names) {
        stillHeld &= restarted.Contains(name);
    }
    Check(stillHeld && restarted.Names() == names, "every one of them is still held after a restart");
    Check(!f.held.Add("a/b") && !f.held.Add("line\nbreak") && !f.Held("a/b"),
          "Add refuses a name the list could not read back");
}

// A write that fails after its temporary file was written: the script file and the held list stay as they were, no
// temporary file is left, and nothing under `guard` (the folder scan) ever sees the name held while its file did not
// change, so a running script that was not held keeps running.
void FailedWrites(const std::wstring& base) {
    Folder f(base);
    const std::wstring file = ScriptFilePath(f.scripts, "Locked");
    WriteScriptFile(f.store, "Locked", false, "v1", "");
    {
        std::lock_guard<std::recursive_mutex> lock(f.guard);
        f.held.Remove("Locked");
    }
    const std::string heldBefore = Bytes(HeldListPath(f.scripts));
    const std::string revision = f.Revision("Locked");
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY);   // the move over the script fails

    std::atomic<bool> stop{false};
    std::atomic<bool> seenHeld{false};
    std::thread scan([&] {
        while (!stop) {
            std::lock_guard<std::recursive_mutex> lock(f.guard);
            if (f.held.Contains("Locked")) {
                seenHeld = true;
            }
        }
    });
    bool allFailed = true;
    for (int i = 0; i < 50; ++i) {
        allFailed &= WriteScriptFile(f.store, "Locked", false, "v2", revision) == ScriptFileResult::Failed;
    }
    stop = true;
    scan.join();
    Check(allFailed, "a write whose move fails: Failed");
    Check(!seenHeld, "the scan never sees the name held while the file is unchanged");
    Check(Bytes(file) == "v1" && !f.Held("Locked") && Bytes(HeldListPath(f.scripts)) == heldBefore,
          "the script file and the held list are unchanged");
    Check(Bytes(ScriptBackupPath(f.scripts, "Locked")) == "v1", "the backup is a copy of the unchanged file");
    Check(Listed(f.scripts, L"*.write").empty() && Listed(f.scripts + L"\\.nickel\\backup", L"*.write").empty(),
          "no temporary file is left");
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);

    // the held list cannot be written: Failed before the move, the script file is unchanged
    const std::wstring list = HeldListPath(f.scripts);
    SetFileAttributesW(list.c_str(), FILE_ATTRIBUTE_READONLY);
    Check(WriteScriptFile(f.store, "Locked", false, "v3", revision) == ScriptFileResult::Failed && Bytes(file) == "v1"
                  && !f.Held("Locked") && Listed(f.scripts, L"*.write").empty(),
          "a held list that cannot be written: Failed, the script file is unchanged, no temporary file is left");
    SetFileAttributesW(list.c_str(), FILE_ATTRIBUTE_NORMAL);
    Check(WriteScriptFile(f.store, "Locked", false, "v3", revision) == ScriptFileResult::Done && Bytes(file) == "v3"
                  && f.Held("Locked"),
          "once the disk accepts it, the same write succeeds");
}

void Backups(const std::wstring& base) {
    Folder f(base);
    const std::wstring file = ScriptFilePath(f.scripts, "Undo");
    WriteScriptFile(f.store, "Undo", false, "v1", "");
    WriteScriptFile(f.store, "Undo", false, "v2", f.Revision("Undo"));

    Check(RestoreScriptBackup(f.store, "Undo", false, ScriptSha256("stale")) == ScriptFileResult::Conflict
                  && Bytes(file) == "v2",
          "a restore with a stale revision: Conflict, nothing changes");
    Check(RestoreScriptBackup(f.store, "Undo", false, f.Revision("Undo")) == ScriptFileResult::Done
                  && Bytes(file) == "v1" && Bytes(ScriptBackupPath(f.scripts, "Undo")) == "v2",
          "a restore swaps the file and the backup");
    Check(RestoreScriptBackup(f.store, "Undo", false, f.Revision("Undo")) == ScriptFileResult::Done
                  && Bytes(file) == "v2" && Bytes(ScriptBackupPath(f.scripts, "Undo")) == "v1",
          "a second restore undoes the first");
    Check(RestoreScriptBackup(f.store, "Never", false, "") == ScriptFileResult::NotFound, "no backup: NotFound");
    Check(RestoreScriptBackup(f.store, "a/b", false, "") == ScriptFileResult::InvalidName, "an invalid name: InvalidName");

    // a restored script is held, as every library write
    {
        std::lock_guard<std::recursive_mutex> lock(f.guard);
        f.held.Remove("Undo");
    }
    RestoreScriptBackup(f.store, "Undo", false, f.Revision("Undo"));
    Check(f.Held("Undo"), "a restored script is held");
}

void Removal(const std::wstring& base) {
    Folder f(base);
    const std::wstring file = ScriptFilePath(f.scripts, "Gone");
    WriteScriptFile(f.store, "Gone", false, "local x = 1", "");
    const std::string revision = f.Revision("Gone");

    Check(RemoveScriptFile(f.store, "Gone", false, ScriptSha256("edited")) == ScriptFileResult::Conflict
                  && Bytes(file) == "local x = 1",
          "a wrong revision: Conflict, the file stays");
    Check(RemoveScriptFile(f.store, "Gone", false, "") == ScriptFileResult::Conflict && ScriptFileExists(file),
          "an empty revision: Conflict (a removal must name what it removes)");
    Check(RemoveScriptFile(f.store, "Gone", false, revision) == ScriptFileResult::Done && !ScriptFileExists(file),
          "the right revision: Done, the file is deleted");
    Check(Bytes(ScriptBackupPath(f.scripts, "Gone")) == "local x = 1" && ScriptHasBackup(f.scripts, "Gone"),
          "the removed file is kept as the backup");
    Check(!f.Held("Gone") && Bytes(HeldListPath(f.scripts)).empty(), "the name leaves the held list");
    Check(RemoveScriptFile(f.store, "Gone", false, revision) == ScriptFileResult::Conflict,
          "removing a file that is not there: Conflict");

    Check(RestoreScriptBackup(f.store, "Gone", false, "") == ScriptFileResult::Done && Bytes(file) == "local x = 1"
                  && f.Held("Gone"),
          "a restore with \"\" brings a removed script back, held");
}

void HeldNames(const std::wstring& base) {
    Folder f(base);
    bool added = false;
    Check(f.held.Add("Fly", &added) && added && f.held.Add("fly", &added) && !added, "Add tells whether it added");
    Check(f.held.Names() == std::vector<std::string>({"Fly"}), "a name is held once, ignoring case");
    Check(f.held.Remove("FLY") && !f.Held("Fly") && Bytes(HeldListPath(f.scripts)).empty(),
          "Remove ignores case and writes the list");

    // a list saved by hand with CRLF, blank lines and a repeat
    MakeScriptFolders(f.scripts);
    WriteScriptFileInOneStep(HeldListPath(f.scripts), L"held", "One\r\n\r\ntwo\r\nONE\r\n");
    ScriptHeldNames reread(f.scripts);
    Check(reread.Names() == std::vector<std::string>({"One", "two"}), "a list written elsewhere is read as ParseHeldList");

    ScriptHeldNames none(base + L"\\no-such-folder");
    Check(none.Names().empty() && !none.Contains("One"), "with no held.txt, nothing is held");
}

// Two threads writing the same script, each from the revision it last read: the files lock orders them, so no write
// is lost half way, every temporary file is gone and the file holds one of the writes that succeeded.
void Threads(const std::wstring& base) {
    Folder f(base);
    WriteScriptFile(f.store, "Race", false, "start", "");
    std::mutex doneGuard;
    std::set<std::string> done = {"start"};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i) {
                const std::string text = "thread " + std::to_string(t) + " write " + std::to_string(i);
                const std::string revision = f.Revision("Race");
                const ScriptFileResult result = WriteScriptFile(f.store, "Race", false, text, revision);
                if (result == ScriptFileResult::Done) {
                    std::lock_guard<std::mutex> lock(doneGuard);
                    done.insert(text);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const std::string last = Bytes(ScriptFilePath(f.scripts, "Race"));
    Check(done.size() > 2, "both threads write");
    Check(done.count(last) == 1, "the file holds one whole write that succeeded");
    Check(done.count(Bytes(ScriptBackupPath(f.scripts, "Race"))) == 1, "the backup is a whole earlier write");
    Check(Listed(f.scripts, L"*.write").empty() && Listed(f.scripts + L"\\.nickel\\backup", L"*.write").empty(),
          "no temporary file is lost");
}

} // namespace

int main() {
    const std::wstring root = NewRoot();
    Revisions();
    Writes(root + L"\\writes");
    Backups(root + L"\\backups");
    KnownNames(root + L"\\known");
    FailedWrites(root + L"\\failed");
    Removal(root + L"\\removal");
    HeldNames(root + L"\\held");
    Threads(root + L"\\threads");
    DeleteTree(root);
    std::printf(failed ? "%d check(s) FAILED\n" : "all checks passed\n", failed);
    return failed ? 1 : 0;
}
