// Windows open / save dialogs for the menu (Import / Export in Configs, Scripting and Schematics).
// The dialog runs on a worker thread so the game keeps rendering; the chosen path is handed back to the render thread
// by PumpFileDialogs(), called once per frame, which runs the callback. One dialog at a time.
#pragma once
#include <windows.h>
#include <commdlg.h>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#pragma comment(lib, "comdlg32.lib")

namespace filedialog {
inline std::mutex guard;
inline bool busy = false;
inline std::function<void(const std::wstring&)> pendingDone;
inline std::wstring pendingPath;
inline bool pendingReady = false;

// filter: pairs separated by '\0', e.g. L"Nickel config (*.ini)\0*.ini\0". defaultExt without the dot.
inline bool Ask(HWND owner, bool save, std::wstring filter, std::wstring title, std::wstring defaultName, std::wstring defaultExt,
                std::function<void(const std::wstring&)> done) {
  {std::lock_guard<std::mutex> lock(guard); if (busy) return false; busy = true;}
  std::thread([=]() {
    wchar_t file[MAX_PATH]{};
    if (!defaultName.empty()) wcsncpy_s(file, defaultName.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = owner; ofn.lpstrFilter = filter.c_str(); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title.c_str(); ofn.lpstrDefExt = defaultExt.empty() ? nullptr : defaultExt.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    std::lock_guard<std::mutex> lock(guard);
    busy = false;
    if (ok && file[0]) {pendingPath = file; pendingDone = done; pendingReady = true;}
  }).detach();
  return true;
}
// Filter list for the dialog: label, pattern, label, pattern... (double-null terminated).
inline std::wstring Filter(std::initializer_list<const wchar_t*> parts) {std::wstring f; for (auto* p : parts) {f += p; f.push_back(L'\0');} f.push_back(L'\0'); return f;}
inline void Pump() {
  std::function<void(const std::wstring&)> done; std::wstring path;
  {std::lock_guard<std::mutex> lock(guard); if (!pendingReady) return; pendingReady = false; done = std::move(pendingDone); path = std::move(pendingPath);}
  if (done) done(path);
}
inline std::wstring Stem(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/"); std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.'); return dot == std::wstring::npos ? name : name.substr(0, dot);
}
inline std::wstring Extension(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L"\\/");
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
  std::wstring e = path.substr(dot); for (auto& c : e) c = (wchar_t)towlower(c); return e;
}
// First free "<folder>\<stem><ext>", then "<stem> (2)<ext>", "<stem> (3)<ext>"...
inline std::wstring FreeName(const std::wstring& folder, const std::wstring& stem, const std::wstring& ext) {
  for (int n = 1; n < 1000; ++n) {
    const std::wstring candidate = folder + L"\\" + stem + (n == 1 ? L"" : L" (" + std::to_wstring(n) + L")") + ext;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
  }
  return folder + L"\\" + stem + L" (copy)" + ext;
}
}  // namespace filedialog
