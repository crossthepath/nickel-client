#pragma once
// The player's nickelclient.com session as the overlay sees it: the access token the launcher keeps in the Windows
// Credential Manager (docs/architecture/account-session.md: generic credential "nickelclient.com/session", UTF-8 JSON
// of version 1). The overlay only reads it; signing in, refreshing and signing out are the launcher's.
//
// - An access token within 60 seconds of its expiry reads as signed out: the launcher refreshes it the next time it
//   runs, and the PUBLIC pages then ask the player to open the launcher.
// - A token the API refused (401) stays refused until the credential holds another one.
// - Nothing is cached on disk, logged or shown: the token only goes to PublicClient through TokenSource.
// No ImGui and no menu here (tools/boundaries.json); tests/public_page_checks.cpp checks ParseSession.
#include <windows.h>
#include <wincred.h>
#pragma comment(lib, "advapi32.lib")

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "json_value.hpp"
#include "public_client.hpp"

namespace nickel::pub {

struct Session {
    std::string accessToken;
    int64_t accessExpiresAt = 0; // unix seconds
    std::string handle;
};

// The credential blob: version 1 with an access token, its expiry and a refresh token (a blob without one reads as
// signed out, like the launcher).
inline bool ParseSession(std::string_view blob, Session* out) {
    json::Value root;
    if (blob.empty() || blob.size() > 2560 || !json::Parse(blob, &root) || !root.IsObject()) {
        return false;
    }
    const json::Value* version = root.Find("version");
    const json::Value* access = root.Find("access_token");
    const json::Value* expires = root.Find("access_expires_at");
    const json::Value* refresh = root.Find("refresh_token");
    int64_t at = 0;
    if (version == nullptr || version->AsInt64(0) != 1 || access == nullptr || !access->IsString()
        || !detail::ValidAccessToken(access->AsString()) || expires == nullptr || !expires->GetInt64(&at)
        || refresh == nullptr || !refresh->IsString() || refresh->AsString().empty()) {
        return false;
    }
    out->accessToken = access->AsString();
    out->accessExpiresAt = at;
    const json::Value* handle = root.Find("handle");
    out->handle = handle != nullptr && handle->IsString() && ValidHandle(handle->AsString()) ? handle->AsString()
                                                                                             : std::string();
    return true;
}

// Whether a session's access token can still be sent at `nowSeconds` (60 seconds of margin, as the launcher).
inline bool SessionUsable(const Session& session, int64_t nowSeconds) {
    return !session.accessToken.empty() && session.accessExpiresAt - 60 > nowSeconds;
}

class CredentialTokenSource : public TokenSource {
public:
    explicit CredentialTokenSource(std::wstring target = L"nickelclient.com/session") : target_(std::move(target)) {}

    bool AccessToken(std::string* token) override {
        Session session;
        if (!Current(&session)) {
            return false;
        }
        *token = session.accessToken;
        return true;
    }

    void TokenRejected(const std::string& token) override {
        std::lock_guard<std::mutex> lock(mutex_);
        rejected_ = token;
    }

    // For the menu: signed in with a token that was not refused, and the handle ("" when the launcher has none).
    // Reads the credential at most every 5 seconds.
    bool SignedIn(std::string* handle) {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(mutex_);
        if (checkedAt_ == 0 || now - checkedAt_ > 5000) {
            checkedAt_ = now;
            Session session;
            signedIn_ = ReadLocked(&session);
            handle_ = signedIn_ ? session.handle : std::string();
        }
        if (handle != nullptr) {
            *handle = handle_;
        }
        return signedIn_;
    }

private:
    bool Current(Session* session) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ReadLocked(session);
    }

    bool ReadLocked(Session* session) {
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(target_.c_str(), CRED_TYPE_GENERIC, 0, &credential) || credential == nullptr) {
            return false;
        }
        std::string blob(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        CredFree(credential);
        const bool parsed = ParseSession(blob, session);
        SecureZeroMemory(blob.data(), blob.size()); // the blob also holds the refresh token, never used here
        if (!parsed || !SessionUsable(*session, detail::NowUnixMs() / 1000)) {
            return false;
        }
        if (!rejected_.empty() && session->accessToken == rejected_) {
            return false;
        }
        rejected_.clear();
        return true;
    }

    std::wstring target_;
    std::mutex mutex_;
    std::string rejected_;
    ULONGLONG checkedAt_ = 0;
    bool signedIn_ = false;
    std::string handle_;
};

} // namespace nickel::pub
