#pragma once
// The anonymous identity of this Nickel install for PUBLIC writes (publish, publish update, stars, reports). There is
// no account and no sign-in anywhere: the client generates a 32-byte random secret once (BCryptGenRandom), keeps it
// in <Nickel data>\public\install.key as base64url text, and sends "Authorization: Install <base64url secret>" on
// write requests only. The server stores only SHA-256(secret).
//
// - The key is created on the first write that needs it, never by browsing: reading PUBLIC sends no identity.
// - A key file that exists but cannot be read as a key is never overwritten (it may be the only proof of ownership of
//   published items): State() says Damaged and writes are refused until the player deletes the file.
// - Fingerprint() is SHA-256 of the secret (hex): it marks the items this install published (public_local.hpp), so
//   "Publish update" is offered only to the install that owns an item. The secret itself is never logged, shown or
//   written anywhere else.
// Any thread: one mutex. No ImGui, no menu (tools/boundaries.json); tests/public_client_checks.cpp.
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "https_client.hpp"

namespace nickel::pub {

constexpr size_t kInstallSecretBytes = 32;
constexpr size_t kInstallKeyChars = 43; // base64url of 32 bytes, without padding

// <root>\public\install.key (root: %APPDATA%\Nickel in the game).
inline std::wstring InstallKeyPath(const std::wstring& root) {
    return root + L"\\public\\install.key";
}

namespace detail {

// base64url without padding (RFC 4648, section 5).
inline std::string Base64Url(const uint8_t* bytes, size_t size) {
    std::string text = net::Base64Encode(bytes, size);
    while (!text.empty() && text.back() == '=') {
        text.pop_back();
    }
    for (char& c : text) {
        c = c == '+' ? '-' : c == '/' ? '_' : c;
    }
    return text;
}

inline bool FromBase64Url(std::string_view text, std::string* out) {
    std::string standard(text);
    for (char& c : standard) {
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        } else if (c == '+' || c == '/' || c == '=') {
            return false; // only the url-safe alphabet, without padding
        }
    }
    while (standard.size() % 4 != 0) {
        standard += '=';
    }
    return net::Base64Decode(standard, out);
}

} // namespace detail

enum class IdentityState : uint8_t {
    Missing, // no key yet: the first write creates it
    Ready,   // a valid key is loaded
    Damaged, // the file exists but is not a key: never overwritten, writes are refused
    Failed,  // the key could not be created or read (disk, permissions, random generator)
};

class InstallIdentity {
public:
    explicit InstallIdentity(std::wstring path) : path_(std::move(path)) {}

    ~InstallIdentity() { SecureZeroMemory(secret_, sizeof(secret_)); }

    InstallIdentity(const InstallIdentity&) = delete;
    InstallIdentity& operator=(const InstallIdentity&) = delete;

    const std::wstring& Path() const { return path_; }

    // The current state, reading the file when no key is loaded yet (never creates it).
    IdentityState State() {
        std::lock_guard<std::mutex> lock(mutex_);
        return LoadLocked();
    }

    // The header value "Install <base64url secret>" for a write request, creating the key on first use.
    // False when the key is damaged or cannot be created.
    bool Authorization(std::string* header) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureLocked()) {
            return false;
        }
        *header = "Install " + detail::Base64Url(secret_, sizeof(secret_));
        return true;
    }

    // SHA-256 of the secret as 64 lower-case hex characters; false while there is no valid key (never creates it).
    bool Fingerprint(std::string* hex) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (LoadLocked() != IdentityState::Ready) {
            return false;
        }
        *hex = net::Sha256Hex(std::string_view(reinterpret_cast<const char*>(secret_), sizeof(secret_)));
        return true;
    }

private:
    // Loads the key file once it exists; Missing while it does not.
    IdentityState LoadLocked() {
        if (state_ == IdentityState::Ready || state_ == IdentityState::Damaged) {
            return state_;
        }
        HANDLE file = CreateFileW(
                path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            state_ = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? IdentityState::Missing
                                                                                    : IdentityState::Failed;
            return state_;
        }
        char buffer[128] = {};
        DWORD got = 0;
        const BOOL read = ReadFile(file, buffer, sizeof(buffer), &got, nullptr);
        CloseHandle(file);
        std::string_view text(buffer, read ? got : 0);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.remove_suffix(1);
        }
        std::string decoded;
        if (!read || text.size() != kInstallKeyChars || !detail::FromBase64Url(text, &decoded)
            || decoded.size() != kInstallSecretBytes) {
            SecureZeroMemory(buffer, sizeof(buffer));
            SecureZeroMemory(decoded.data(), decoded.size());
            state_ = IdentityState::Damaged;
            return state_;
        }
        memcpy(secret_, decoded.data(), sizeof(secret_));
        SecureZeroMemory(buffer, sizeof(buffer));
        SecureZeroMemory(decoded.data(), decoded.size());
        state_ = IdentityState::Ready;
        return state_;
    }

    // A loaded key, or a new one written with CREATE_NEW (an existing file is never replaced).
    bool EnsureLocked() {
        const IdentityState state = LoadLocked();
        if (state == IdentityState::Ready) {
            return true;
        }
        if (state != IdentityState::Missing) {
            return false;
        }
        uint8_t fresh[kInstallSecretBytes] = {};
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, fresh, sizeof(fresh), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            state_ = IdentityState::Failed;
            return false;
        }
        const size_t slash = path_.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring folder = path_.substr(0, slash);
            const size_t parent = folder.find_last_of(L"\\/");
            if (parent != std::wstring::npos) {
                CreateDirectoryW(folder.substr(0, parent).c_str(), nullptr);
            }
            CreateDirectoryW(folder.c_str(), nullptr);
        }
        std::string text = detail::Base64Url(fresh, sizeof(fresh)) + "\n";
        HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            SecureZeroMemory(fresh, sizeof(fresh));
            SecureZeroMemory(text.data(), text.size());
            // another writer created it first: use that one
            state_ = GetLastError() == ERROR_FILE_EXISTS ? IdentityState::Missing : IdentityState::Failed;
            return state_ == IdentityState::Missing && LoadLocked() == IdentityState::Ready;
        }
        DWORD written = 0;
        const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
                        && written == text.size() && FlushFileBuffers(file);
        CloseHandle(file);
        SecureZeroMemory(text.data(), text.size());
        if (!ok) {
            DeleteFileW(path_.c_str());
            SecureZeroMemory(fresh, sizeof(fresh));
            state_ = IdentityState::Failed;
            return false;
        }
        memcpy(secret_, fresh, sizeof(secret_));
        SecureZeroMemory(fresh, sizeof(fresh));
        state_ = IdentityState::Ready;
        return true;
    }

    std::wstring path_;
    std::mutex mutex_;
    IdentityState state_ = IdentityState::Missing;
    uint8_t secret_[kInstallSecretBytes] = {};
};

} // namespace nickel::pub
