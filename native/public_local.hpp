#pragma once
// What this install remembers about PUBLIC on its own disk, next to install.key (public_identity.hpp):
// - <root>\public\starred.json: the creations this install starred, ids per kind, newest first. Stars are favourites:
//   the "Starred" tab of PUBLIC -> Scripts and PUBLIC -> Configs lists them (cards fetched with POST /items/lookup).
// - <root>\public\published.json: what this install published from OTHER -> Scripting and OTHER -> Configs: the local
//   script or config name, the public item id, and the owner (the install's fingerprint, never the secret). Only an
//   entry whose owner is the current install offers "Publish update"; nothing here can delete, unlist or hide an item
//   (publishing is final).
// Files are rewritten whole through a temporary file. No ImGui, no menu (tools/boundaries.json); tests in
// tests/public_client_checks.cpp and tests/public_page_checks.cpp.
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "json_value.hpp"
#include "public_installer.hpp"
#include "public_model.hpp"

namespace nickel::pub {

constexpr size_t kMaxStarred = 2000; // per kind
constexpr size_t kMaxPublished = 2000;

inline std::wstring StarredPath(const std::wstring& root) {
    return root + L"\\public\\starred.json";
}

inline std::wstring PublishedPath(const std::wstring& root) {
    return root + L"\\public\\published.json";
}

namespace detail {

// Writes `text` to `path` through "<path>.tmp" (MoveFileEx), creating the folder.
inline bool ReplaceFile(const std::wstring& path, const std::string& text) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const std::wstring folder = path.substr(0, slash);
        const size_t parent = folder.find_last_of(L"\\/");
        if (parent != std::wstring::npos) {
            MakeFolder(folder.substr(0, parent));
        }
        MakeFolder(folder);
    }
    const std::wstring temporary = path + L".tmp";
    if (!WriteAll(temporary, text)) {
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Starred

class StarredList {
public:
    explicit StarredList(std::wstring path) : path_(std::move(path)) { Load(); }

    void Load() {
        ids_[0].clear();
        ids_[1].clear();
        std::string text;
        json::Value root;
        if (!detail::ReadAll(path_, &text, 1024 * 1024) || !json::Parse(text, &root) || !root.IsObject()) {
            return;
        }
        for (Kind kind : {Kind::Script, Kind::Config}) {
            const json::Value* list = root.Find(KindName(kind));
            if (list == nullptr || !list->IsArray()) {
                continue;
            }
            std::vector<std::string>& ids = ids_[Slot(kind)];
            for (size_t i = 0; i < list->size() && ids.size() < kMaxStarred; ++i) {
                const json::Value& id = list->At(i);
                if (id.IsString() && ValidId(id.AsString())
                    && std::find(ids.begin(), ids.end(), id.AsString()) == ids.end()) {
                    ids.push_back(id.AsString());
                }
            }
        }
    }

    bool Has(Kind kind, const std::string& id) const {
        const std::vector<std::string>& ids = ids_[Slot(kind)];
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    // Newest first.
    const std::vector<std::string>& Ids(Kind kind) const { return ids_[Slot(kind)]; }

    // Stars (on) or unstars an id and saves; false when it was refused (an invalid id, the cap) or not saved.
    bool Set(Kind kind, const std::string& id, bool on) {
        if (!ValidId(id)) {
            return false;
        }
        std::vector<std::string>& ids = ids_[Slot(kind)];
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (on) {
            if (ids.size() >= kMaxStarred) {
                return false;
            }
            ids.insert(ids.begin(), id);
        }
        return Save();
    }

private:
    static size_t Slot(Kind kind) { return static_cast<size_t>(kind); }

    bool Save() const {
        json::Value root = json::Value::MakeObject();
        root.Set("format", json::Value::MakeInt(1));
        for (Kind kind : {Kind::Script, Kind::Config}) {
            json::Value list = json::Value::MakeArray();
            for (const std::string& id : ids_[Slot(kind)]) {
                list.Push(json::Value::MakeString(id));
            }
            root.Set(KindName(kind), std::move(list));
        }
        return detail::ReplaceFile(path_, json::Serialize(root) + "\n");
    }

    std::wstring path_;
    std::vector<std::string> ids_[2];
};

// ---------------------------------------------------------------------------------------------------------------
// Published

struct PublishedRecord {
    Kind kind = Kind::Script;
    std::string name;        // the local script or config name (file name without extension)
    std::string id;          // the public item
    std::string owner;       // InstallIdentity::Fingerprint() of the install that published it
    int version = 0;         // the latest version number this install published (0: unknown)
    std::string publishedAt; // ISO 8601 UTC
    std::string updatedAt;   // ISO 8601 UTC of the latest "Publish update", "" for none
};

class PublishedList {
public:
    explicit PublishedList(std::wstring path) : path_(std::move(path)) { Load(); }

    void Load() {
        records_.clear();
        std::string text;
        json::Value root;
        if (!detail::ReadAll(path_, &text, 4 * 1024 * 1024) || !json::Parse(text, &root) || !root.IsObject()) {
            return;
        }
        const json::Value* items = root.Find("items");
        for (size_t i = 0; items != nullptr && items->IsArray() && i < items->size(); ++i) {
            const json::Value& item = items->At(i);
            PublishedRecord record;
            std::string kind;
            std::string ignored;
            int64_t version = 0;
            const detail::Reader read{item, &ignored, ""};
            if (!item.IsObject() || !read.String("kind", &kind, true, 16, detail::TextRule::OneLine)
                || !ParseKind(kind, &record.kind)
                || !read.String("name", &record.name, true, 80, detail::TextRule::OneLine)
                || !overlay::ValidScriptName(record.name) || !read.Pattern("id", &record.id, true, ValidId)
                || !read.Pattern("owner", &record.owner, true, ValidSha256)
                || !read.Int("version", &version, false, 0, 1000000)
                || !read.String("published_at", &record.publishedAt, false, 40, detail::TextRule::OneLine)
                || !read.String("updated_at", &record.updatedAt, false, 40, detail::TextRule::OneLine)) {
                continue;
            }
            record.version = static_cast<int>(version);
            if (records_.size() < kMaxPublished) {
                records_.push_back(std::move(record));
            }
        }
    }

    const std::vector<PublishedRecord>& Records() const { return records_; }

    // The item `owner` published from the local `name` of `kind` (names compared as the script library does), or
    // nullptr. An entry of another install (another fingerprint) is never returned.
    const PublishedRecord* Find(Kind kind, const std::string& name, const std::string& owner) const {
        if (owner.empty()) {
            return nullptr;
        }
        for (const PublishedRecord& record : records_) {
            if (record.kind == kind && record.owner == owner && overlay::SameScriptName(record.name, name)) {
                return &record;
            }
        }
        return nullptr;
    }

    // Adds or replaces the entry for (kind, name, owner) and saves.
    bool Remember(const PublishedRecord& record) {
        if (!ValidId(record.id) || !ValidSha256(record.owner) || !overlay::ValidScriptName(record.name)) {
            return false;
        }
        bool replaced = false;
        for (PublishedRecord& existing : records_) {
            if (existing.kind == record.kind && existing.owner == record.owner
                && overlay::SameScriptName(existing.name, record.name)) {
                existing = record;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (records_.size() >= kMaxPublished) {
                return false;
            }
            records_.push_back(record);
        }
        return Save();
    }

private:
    bool Save() const {
        json::Value items = json::Value::MakeArray();
        for (const PublishedRecord& record : records_) {
            json::Value item = json::Value::MakeObject();
            item.Set("kind", json::Value::MakeString(KindName(record.kind)));
            item.Set("name", json::Value::MakeString(record.name));
            item.Set("id", json::Value::MakeString(record.id));
            item.Set("owner", json::Value::MakeString(record.owner));
            item.Set("version", json::Value::MakeInt(record.version));
            item.Set("published_at", json::Value::MakeString(record.publishedAt));
            item.Set("updated_at", json::Value::MakeString(record.updatedAt));
            items.Push(std::move(item));
        }
        json::Value root = json::Value::MakeObject();
        root.Set("format", json::Value::MakeInt(1));
        root.Set("items", std::move(items));
        return detail::ReplaceFile(path_, json::Serialize(root) + "\n");
    }

    std::wstring path_;
    std::vector<PublishedRecord> records_;
};

} // namespace nickel::pub
