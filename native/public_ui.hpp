// PUBLIC pages of the menu: the sidebar group PUBLIC with Scripts (tab 11) and Configs (tab 12), shared by players on
// nickelclient.com. Each page has the sort tabs Top Rated / Trending / Recent, a search box, a grid of cards (name,
// author, stars, downloads, summary, installed / held / update pills) with Load more, and the details side panel
// (description, version, capabilities of scripts with the sensitive ones in red, the settings a config changes,
// Install / Update / Start / Uninstall, the star). Scripts are always shown as source before they are installed, and
// they are installed held: only Start (ScriptLibrary::Start, render thread) runs them.
//
// PUBLIC is off by default (Settings > Connection > Online community (PUBLIC)). While it is off the pages show how
// to turn it on, and nothing is created, read or requested: the PublicClient and its worker thread only exist once
// the player turned PUBLIC on.
//
// Included by menu.cpp inside its anonymous namespace, after the Configs, Scripting and Schematics screens (it reuses
// UiButton, UiInput, LoadConfig and the Scripting selection). What the pages do is nickel::pub::PublicPage
// (public_page.hpp), checked without ImGui by tests/public_page_checks.cpp; this file only draws and forwards clicks.
// Offline review (probe only): NICKEL_REVIEW_PUBLIC=1 shows fixture cards without any network, 2 also opens the
// first script's source, 3 shows the rate-limited state.
namespace pub = nickel::pub;

constexpr int kPublicScriptsTab = 11;
constexpr int kPublicConfigsTab = 12;

inline bool PublicTab(int t) {
    return t == kPublicScriptsTab || t == kPublicConfigsTab;
}

inline pub::Kind PublicKindOf(int t) {
    return t == kPublicConfigsTab ? pub::Kind::Config : pub::Kind::Script;
}

// Settings > Connection > Online community (PUBLIC).
MenuOption& PublicOption() {
    return connectionOptions[2];
}

// Requests of the offline review: nothing leaves the process.
class ReviewRequests : public pub::PageRequests {
public:
    bool List(const pub::ListQuery&) override { return true; }

    bool More(pub::Kind) override { return true; }

    bool Detail(const std::string&) override { return true; }

    bool Source(const std::string&, int, const std::string&) override { return true; }

    bool File(const std::string&, int, const std::string&) override { return true; }

    bool Lookup(std::vector<std::string>) override { return true; }

    bool Revocations() override { return true; }

    bool Library() override { return true; }

    bool Star(const std::string&, bool) override { return true; }
};

struct PublicMenu {
    std::unique_ptr<pub::PublicPage> page;
    // Created when PUBLIC is first on and never destroyed: the overlay has no unload path, and a destructor at
    // process exit would join the worker thread inside DllMain.
    pub::PublicClient* client = nullptr;
    pub::CredentialTokenSource* tokens = nullptr;
    pub::ClientRequests* requests = nullptr;
    pub::PageRequests* reviewRequests = nullptr;
    bool review = false;
    pub::Snapshot snap;
    uint64_t version = 0;
    bool signedIn = false;
    std::string handle;
    char search[2][72]{};
    std::string armed; // the card whose Uninstall was clicked once
    double armedAt = 0;
    std::string edited; // the card whose Uninstall answered Modified: "Remove anyway?"
    std::shared_ptr<const pub::FileResult> linesOf;
    std::vector<std::string> lines;
    std::vector<pub::ConfigSection> sections;
};

PublicMenu& Public() {
    static PublicMenu state;
    return state;
}

pub::Card ReviewCard(const char* id,
                     pub::Kind kind,
                     const char* name,
                     const char* summary,
                     const char* author,
                     uint64_t stars,
                     uint64_t downloads,
                     const char* tag) {
    pub::Card card;
    card.id = id;
    card.kind = kind;
    card.slug = std::string("review-") + id;
    card.name = name;
    card.summary = summary;
    card.author.handle = author;
    card.author.displayName = author;
    card.author.verified = std::string(author) == "nickel";
    card.tags = {tag};
    card.license = "MIT";
    card.latest.number = 3;
    card.latest.label = "1.2.0";
    card.latest.size = 4210;
    card.latest.sha256 = std::string(64, 'a');
    card.latest.reviewed = stars > 100;
    card.latest.reviewedAt = 1794765600000LL;
    card.stats.stars = stars;
    card.stats.downloads = downloads;
    card.updatedAt = 1794765600000LL - 7200000;
    card.publishedAt = 1794765600000LL - 864000000;
    if (kind == pub::Kind::Script) {
        card.capabilities = {pub::Capability::Render, pub::Capability::ChatSend, pub::Capability::Input};
    } else {
        card.categories = {"combat", "visuals"};
        card.changedModules = 12;
    }
    return card;
}

pub::Snapshot PublicReviewSnapshot(int mode) {
    pub::Snapshot snap;
    auto scriptList = std::make_shared<pub::ListResult>();
    scriptList->query.kind = pub::Kind::Script;
    scriptList->query.sort = pub::Sort::Top;
    const char* const names[7] = {"Auto GG",
                                  "Totem Reminder",
                                  "Combo Counter Plus",
                                  "Bridge Helper",
                                  "Chat Filter",
                                  "Ping Graph",
                                  "Session Timer"};
    const char* const summaries[7] = {"Says gg in chat after a win, with a random delay and a per-server filter.",
                                      "Warns you with a sound and a title when your offhand totem pops.",
                                      "A bigger combo counter with a fade and a best-of-session record.",
                                      "Shows where the next block goes when you speed bridge.",
                                      "Hides chat lines that match your own words.",
                                      "Draws your ping over the last minute.",
                                      "Counts how long you have been playing today."};
    const char* const ids[7] = {"7k2m9x4qa1dz",
                                "abcdefghjkmn",
                                "23456789abcd",
                                "pqrstvwxyz01",
                                "9x8w7v6t5s4r",
                                "badd1gest234",
                                "badetag23456"};
    for (int i = 0; i < 7; ++i) {
        scriptList->items.push_back(ReviewCard(ids[i],
                                               pub::Kind::Script,
                                               names[i],
                                               summaries[i],
                                               i == 0 ? "nickel" : "mash",
                                               1240 / (i + 1),
                                               12412 / (i + 1),
                                               i % 2 ? "hud" : "pvp"));
    }
    scriptList->nextCursor = "next";
    scriptList->fetchedAt = 1794765600000LL;
    if (mode == 3) {
        scriptList->items.clear();
        scriptList->outcome.status = pub::Status::RateLimited;
        scriptList->outcome.retryAfter = 20;
    }
    snap.lists[0] = scriptList;
    auto configs = std::make_shared<pub::ListResult>();
    configs->query.kind = pub::Kind::Config;
    configs->items.push_back(ReviewCard("2b3c4d5e6f7g",
                                        pub::Kind::Config,
                                        "Hypixel PvP",
                                        "Reach, velocity and a clean HUD.",
                                        "mash",
                                        88,
                                        3021,
                                        "pvp"));
    snap.lists[1] = configs;
    auto detail = std::make_shared<pub::DetailResult>();
    detail->id = ids[0];
    detail->detail.card = scriptList->items.empty() ? pub::Card() : scriptList->items[0];
    detail->detail.description = "Says gg in chat after each win.\nWaits a random 1 to 3 seconds, and only on the "
                                 "servers you list in its settings.";
    detail->detail.versions.push_back(detail->detail.card.latest);
    detail->detail.versions[0].changelog = "Added the per-server filter.";
    detail->detail.pageUrl = "https://nickelclient.com/public/script/review-7k2m9x4qa1dz";
    snap.detail = detail;
    auto source = std::make_shared<pub::FileResult>();
    source->id = ids[0];
    source->number = 3;
    source->bytes = "-- Auto GG: says gg after a win\nlocal mod = nickel.module(\"Auto GG\")\n\n"
                    "mod:on(\"chat\", function(line)\n    if line:find(\"won the game\") then\n"
                    "        nickel.chat.send(\"gg\")\n    end\nend)\n";
    snap.source = source;
    return snap;
}

// Called once per frame by DrawMenu, menu open or not: answers are handled (installs, revocations, updates).
void PublicTick() {
    PublicMenu& m = Public();
    if (configPath.empty()) {
        return;
    }
    static bool reviewChecked = false;
    int reviewMode = 0;
    if (!reviewChecked) {
        reviewChecked = true;
        char value[8]{};
        if (OwnProbe() && GetEnvironmentVariableA("NICKEL_REVIEW_PUBLIC", value, sizeof(value))) {
            m.review = true;
            reviewMode = std::max(1, atoi(value));
            PublicOption().value = true;
        }
    }
    const bool on = PublicOption().value;
    if (!on && !m.page) {
        return; // never on in this session: nothing created, read or requested
    }
    if (!m.page) {
        pub::PageOptions options;
        options.root = configPath.substr(0, configPath.find_last_of(L"\\/"));
        options.library = &ScriptsLibrary();
        m.page = std::make_unique<pub::PublicPage>(std::move(options));
    }
    if (on && m.review && !m.reviewRequests) {
        m.reviewRequests = new ReviewRequests();
        m.snap = PublicReviewSnapshot(reviewMode);
    }
    if (on && !m.review && !m.client) {
        m.tokens = new pub::CredentialTokenSource();
        pub::ClientConfig config;
        config.userAgent = L"Nickel-Overlay/" + Wide(pub::kRunningNickel);
        m.client = new pub::PublicClient(config, m.tokens);
        m.requests = new pub::ClientRequests(m.client);
    }
    m.page->SetRequests(on ? (m.review ? m.reviewRequests : static_cast<pub::PageRequests*>(m.requests)) : nullptr);
    if (on && m.client) {
        const uint64_t version = m.client->Version();
        if (version != m.version) {
            m.version = version;
            m.snap = m.client->Snap();
        }
        m.signedIn = m.tokens->SignedIn(&m.handle);
    }
    m.page->Tick(m.snap, on, m.signedIn, pub::detail::NowUnixMs());
    if (reviewMode > 0 && m.snap.lists[0] && !m.snap.lists[0]->items.empty()) {
        m.page->Select(pub::Kind::Script, m.snap.lists[0]->items[0].id);
        if (reviewMode == 2) {
            m.page->OpenSource(m.snap.lists[0]->items[0]);
        }
    }
    for (std::string& text : m.page->TakeNotices()) {
        if (notices.size() < 16) {
            notices.push_back({8, "PUBLIC", std::move(text)});
        }
    }
    if (!m.armed.empty() && ImGui::GetTime() - m.armedAt > 3) {
        m.armed.clear();
    }
}

// ---- small drawings (no glyph for these in the icon font)

void DrawStar(ImDrawList* d, ImVec2 c, float r, ImU32 color, bool filled) {
    ImVec2 points[10];
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.5707963f + i * 0.6283185f;
        const float radius = i % 2 ? r * 0.45f : r;
        points[i] = ImVec2(c.x + std::cos(angle) * radius, c.y + std::sin(angle) * radius);
    }
    if (filled) {
        for (int i = 0; i < 10; ++i) {
            d->AddTriangleFilled(c, points[i], points[(i + 1) % 10], color);
        }
    } else {
        d->AddPolyline(points, 10, color, ImDrawFlags_Closed, std::max(1.f, r * 0.18f));
    }
}

void DrawDownload(ImDrawList* d, ImVec2 lo, float size, ImU32 color) {
    const float k = size / 12.f;
    const float th = std::max(1.f, 1.4f * k);
    auto at = [&](float x, float y) {
        return ImVec2(lo.x + x * k, lo.y + y * k);
    };
    d->AddLine(at(6, 1), at(6, 8), color, th);
    d->AddLine(at(3, 5.5f), at(6, 8.5f), color, th);
    d->AddLine(at(9, 5.5f), at(6, 8.5f), color, th);
    d->AddLine(at(1.5f, 11), at(10.5f, 11), color, th);
}

void DrawTick(ImDrawList* d, ImVec2 lo, float size, ImU32 color) {
    if (DrawGlyph(d, nicons::k_check, lo, size, color)) {
        return;
    }
    const float k = size / 12.f;
    d->AddLine(ImVec2(lo.x + 2 * k, lo.y + 6.5f * k), ImVec2(lo.x + 5 * k, lo.y + 9.5f * k), color, 1.5f * k);
    d->AddLine(ImVec2(lo.x + 5 * k, lo.y + 9.5f * k), ImVec2(lo.x + 10 * k, lo.y + 3 * k), color, 1.5f * k);
}

// "4 hours ago" -> "4 h ago", for the narrow tiles of the side panel.
std::string ShortAgo(std::string text) {
    static const char* const kLong[] = {" minutes ago",
                                        " minute ago",
                                        " hours ago",
                                        " hour ago",
                                        " days ago",
                                        " weeks ago",
                                        " months ago",
                                        " month ago"};
    static const char* const kShort[] = {
            " min ago", " min ago", " h ago", " h ago", " d ago", " wk ago", " mo ago", " mo ago"};
    for (size_t i = 0; i < sizeof(kLong) / sizeof(kLong[0]); ++i) {
        const size_t at = text.find(kLong[i]);
        if (at != std::string::npos) {
            return text.substr(0, at) + kShort[i];
        }
    }
    return text;
}

// Up to `maxLines` lines of `text` within `width`, the last one cut with "...".
std::vector<std::string> WrapText(ImFont* font, float size, const std::string& text, float width, int maxLines) {
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find_first_of(" \n", at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string word = text.substr(at, end - at);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(font, size, candidate.c_str()) > width) {
            lines.push_back(line);
            line = word;
            if ((int)lines.size() == maxLines) {
                break;
            }
        } else {
            line = candidate;
        }
        if (end < text.size() && text[end] == '\n' && !line.empty()) {
            lines.push_back(line);
            line.clear();
            if ((int)lines.size() == maxLines) {
                break;
            }
        }
        at = end + 1;
    }
    if ((int)lines.size() < maxLines && !line.empty()) {
        lines.push_back(line);
        line.clear();
        at = text.size();
    }
    if (!lines.empty()) {
        const bool cut = at < text.size() || !line.empty();
        std::string& last = lines.back();
        if (cut || TextWidth(font, size, last.c_str()) > width) {
            last = Fit(font, size, cut ? last + " ..." : last, width);
        }
    }
    return lines;
}

// A pill (state of a card) at the top right corner `right`, returns its width in u.
float DrawBadge(ImDrawList* d, pub::Badge badge, ImVec2 right, float s, float a) {
    if (badge == pub::Badge::None) {
        return 0;
    }
    const char* text = pub::BadgeText(badge);
    const float w = TextWidth(bold, 10, text) + 12;
    ImU32 fill = Rgba(35, 35, 44, a);
    ImU32 ink = Rgba(159, 232, 191, a);
    switch (badge) {
        case pub::Badge::Revoked:
        case pub::Badge::Removed:
        case pub::Badge::Unavailable:
        case pub::Badge::NotListed:
        case pub::Badge::Incompatible:
            fill = Rgba(42, 21, 24, a);
            ink = Rgba(248, 113, 113, a);
            break;
        case pub::Badge::Withdrawn:
        case pub::Badge::Modified:
            fill = Rgba(58, 42, 18, a);
            ink = Rgba(255, 179, 92, a);
            break;
        case pub::Badge::Update:
            fill = Rgba(255, 255, 255, a);
            ink = AccDeep(a);
            break;
        case pub::Badge::Held:
            ink = Acc(a);
            break;
        default:
            break;
    }
    const ImVec2 lo(right.x - w * s, right.y);
    d->AddRectFilled(lo, ImVec2(right.x, right.y + 18 * s), fill, 0);
    Label(d, bold, 10 * s, ImVec2(lo.x + 6 * s, right.y + 3.5f * s), text, ink);
    return w;
}

const overlay::ScriptInfo* FindScriptInfo(const std::vector<ScriptInfo>& known, const std::string& name) {
    const int index = FindScript(known, name);
    return index >= 0 ? &known[index] : nullptr;
}

pub::Badge CardBadge(const pub::Card& card, const std::vector<ScriptInfo>& known) {
    PublicMenu& m = Public();
    const pub::LocalState* local = m.page->LocalOf(card.kind, card.id);
    const overlay::ScriptInfo* info =
            local != nullptr && card.kind == pub::Kind::Script ? FindScriptInfo(known, local->record.Name()) : nullptr;
    return pub::BadgeOf(local != nullptr ? local->state : pub::Local::None,
                        info,
                        pub::CompatibleWith(card.latest.nickelMin, m.page->Running()));
}

// The card shown in the side panel: from the details answer when it is this one, else from the list.
const pub::Card* SelectedCard(pub::Kind kind, const pub::DetailResult** detailOut) {
    PublicMenu& m = Public();
    const std::string& id = m.page->View(kind).selected;
    *detailOut = nullptr;
    if (id.empty()) {
        return nullptr;
    }
    if (m.snap.detail && m.snap.detail->id == id) {
        *detailOut = m.snap.detail.get();
        if (m.snap.detail->outcome.Ok() && !m.snap.detail->loading) {
            return &m.snap.detail->detail.card;
        }
    }
    if (const pub::ListResult* list = m.snap.List(kind)) {
        for (const pub::Card& card : list->items) {
            if (card.id == id) {
                return &card;
            }
        }
    }
    return nullptr;
}

void TurnPublicOn() {
    PublicOption().value = true;
    WritePrivateProfileStringW(L"menu", Wide(PublicOption().key).c_str(), L"1", configPath.c_str());
}

// ---- the page (main panel)

void DrawPublicOff(ImVec2 origin, float s, float opacity, pub::Kind kind) {
    const float U = 242.f / 210.f;
    const float left = 257;
    const float areaW = menuW - 22 - left;
    auto* d = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) {
        return ImVec2(origin.x + x * s, origin.y + y * s);
    };
    auto centred = [&](ImFont* font, float size, const char* text, float y, ImU32 color) {
        Label(d, font, size * s, P(left + (areaW - TextWidth(font, size, text)) / 2, y), text, color);
    };
    const float y = std::max(120.f, menuH / 2 - 90);
    centred(bold, 17, "PUBLIC is off", y, Rgba(255, 255, 255, opacity));
    centred(regular,
            13,
            kind == pub::Kind::Script ? "Browse and install the scripts players share on nickelclient.com."
                                      : "Browse and install the configs players share on nickelclient.com.",
            y + 30,
            Rgba(148, 148, 160, opacity));
    centred(regular, 13, "While it is off, Nickel sends nothing to the site.", y + 50, Rgba(148, 148, 160, opacity));
    if (UiButton(d,
                 "public-turn-on",
                 "Turn on",
                 P(left + (areaW - 110 * U) / 2, y + 84),
                 110 * U,
                 32 * U,
                 1,
                 true,
                 opacity,
                 s)) {
        TurnPublicOn();
    }
    centred(regular,
            12,
            "Also in Settings > Connection > Online community (PUBLIC).",
            y + 84 + 32 * U + 14,
            Rgba(120, 120, 132, opacity));
}

void DrawPublicCard(ImDrawList* d,
                    const pub::Card& card,
                    ImVec2 p,
                    float w,
                    float h,
                    float s,
                    float a,
                    bool chosen,
                    pub::Badge badge,
                    bool starred) {
    auto& io = ImGui::GetIO();
    const ImVec2 hi(p.x + w * s, p.y + h * s);
    const float hover =
            Animate("pub-card" + card.id, opened && ImGui::IsMouseHoveringRect(p, hi) ? 1.f : 0.f, io.DeltaTime);
    const bool dim = badge == pub::Badge::Incompatible;
    const float fade = a * (dim ? 0.55f : 1.f);
    d->AddRectFilled(p, hi, Rgba(15 + (int)(5 * hover), 15 + (int)(5 * hover), 20 + (int)(7 * hover), fade), 0);
    d->AddRect(p, hi, chosen ? Acc(0.6f * a) : Rgba(26, 26, 33, a), 0, 0, 1 * s);
    auto R = [&](float x, float y) {
        return ImVec2(p.x + x * s, p.y + y * s);
    };
    const bool installed = badge != pub::Badge::None && badge != pub::Badge::Incompatible;
    d->AddRectFilled(R(12, 12), R(46, 46), installed ? AccDeep(fade) : Rgba(23, 23, 29, fade), 0);
    char letter[2] = {(char)std::toupper((unsigned char)(card.name.empty() ? '?' : card.name[0])), 0};
    Label(d,
          semibold,
          15 * s,
          R(12 + (34 - TextWidth(semibold, 15, letter)) / 2, 20.5f),
          letter,
          installed ? Rgba(255, 255, 255, fade) : Rgba(148, 148, 160, fade));
    const float pill = DrawBadge(d, badge, R(w - 12, 12), s, a);
    Label(d,
          semibold,
          14.5f * s,
          R(56, 12),
          Fit(semibold, 14.5f, card.name, w - 56 - 12 - pill - 6).c_str(),
          Rgba(255, 255, 255, fade));
    const std::string by = "by " + (card.author.displayName.empty() ? card.author.handle : card.author.displayName);
    const std::string byFit = Fit(regular, 12, by, w - 56 - 30);
    Label(d, regular, 12 * s, R(56, 31), byFit.c_str(), Rgba(148, 148, 160, fade));
    if (card.author.verified) {
        DrawTick(d, R(56 + TextWidth(regular, 12, byFit.c_str()) + 4, 32.5f), 11 * s, Rgba(255, 255, 255, fade));
    }
    const std::vector<std::string> lines = WrapText(regular, 12, card.summary, w - 24, 2);
    for (size_t i = 0; i < lines.size(); ++i) {
        Label(d, regular, 12 * s, R(12, 56 + 16 * (float)i), lines[i].c_str(), Rgba(148, 148, 160, fade));
    }
    const float row = h - 24;
    DrawStar(d, R(18, row + 6), 5.5f * s, starred ? Acc(fade) : Rgba(148, 148, 160, fade), starred);
    const std::string stars = pub::FormatCount(card.stats.stars);
    Label(d, semibold, 11.5f * s, R(27, row), stars.c_str(), Rgba(224, 224, 230, fade));
    float x = 27 + TextWidth(semibold, 11.5f, stars.c_str()) + 12;
    DrawDownload(d, R(x, row), 11 * s, Rgba(148, 148, 160, fade));
    const std::string downloads = pub::FormatCount(card.stats.downloads);
    Label(d, semibold, 11.5f * s, R(x + 15, row), downloads.c_str(), Rgba(224, 224, 230, fade));
    x += 15 + TextWidth(semibold, 11.5f, downloads.c_str()) + 12;
    const std::string version = pub::VersionText(card.latest);
    float tagLeft = w - 12;
    if (!card.tags.empty()) {
        const float tw = TextWidth(regular, 11, card.tags[0].c_str()) + 12;
        tagLeft = w - 12 - tw;
        if (tagLeft > x + 40) {
            d->AddRectFilled(R(tagLeft, row - 2), R(w - 12, row + 15), Rgba(23, 23, 29, fade), 0);
            Label(d, regular, 11 * s, R(tagLeft + 6, row), card.tags[0].c_str(), Rgba(148, 148, 160, fade));
        } else {
            tagLeft = w - 12;
        }
    }
    if (card.latest.reviewed) {
        DrawTick(d, R(x, row), 10 * s, Rgba(159, 232, 191, fade));
        x += 13;
    }
    Label(d,
          regular,
          11.5f * s,
          R(x, row),
          Fit(regular, 11.5f, version, std::max(0.f, tagLeft - 8 - x)).c_str(),
          Rgba(120, 120, 132, fade));
}

void DrawPublicSource(ImVec2 origin, float s, float opacity, pub::Kind kind) {
    PublicMenu& m = Public();
    pub::PublicPage& page = *m.page;
    const float U = 242.f / 210.f;
    const float left = 257;
    const float right = menuW - 22;
    const float top = 72;
    const float areaW = right - left;
    auto* d = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) {
        return ImVec2(origin.x + x * s, origin.y + y * s);
    };
    const pub::DetailResult* detail = nullptr;
    const pub::Card* found = SelectedCard(kind, &detail);
    if (found == nullptr) {
        page.CloseSource(kind);
        return;
    }
    const pub::Card card = *found;
    if (UiButton(d, "pub-source-back", "Back", P(left, top), 64 * U, 32 * U, 0, true, opacity, s)) {
        page.CloseSource(kind);
        return;
    }
    const std::shared_ptr<const pub::FileResult> source = m.snap.source;
    const bool ours = source && source->id == card.id && source->number == card.latest.number;
    if (ours && source != m.linesOf && !source->loading) {
        m.linesOf = source;
        m.lines = pub::SourceLines(source->bytes);
        m.sections = kind == pub::Kind::Config ? pub::ConfigSummary(source->bytes) : std::vector<pub::ConfigSection>();
    }
    const bool ready = ours && !source->loading && source->outcome.Ok() && m.linesOf == source;
    // install / update button on the right
    const pub::LocalState* local = page.LocalOf(kind, card.id);
    const pub::Local state = local != nullptr ? local->state : pub::Local::None;
    const pub::PendingInstall& pending = page.Pending();
    const bool busy = pending.step == pub::Step::Downloading && pending.card.id == card.id;
    const bool update =
            state == pub::Local::UpdateAvailable || state == pub::Local::Withdrawn || state == pub::Local::Modified;
    const bool installed = state != pub::Local::None && !update;
    const bool compatible = pub::CompatibleWith(card.latest.nickelMin, page.Running());
    const char* action = busy ? "Installing..." : installed ? "Installed" : update ? "Update" : "Install";
    if (UiButton(d,
                 "pub-source-install",
                 action,
                 P(right - 110 * U, top),
                 110 * U,
                 32 * U,
                 1,
                 ready && !busy && !installed && compatible,
                 opacity,
                 s)) {
        page.Install(card, update);
    }
    {
        std::string line = card.name + "  \xc2\xb7  " + pub::VersionText(card.latest);
        if (ready) {
            line += "  \xc2\xb7  " + std::to_string(m.lines.size()) + (m.lines.size() == 1 ? " line" : " lines");
        }
        line += "  \xc2\xb7  sha256 " + card.latest.sha256.substr(0, 4) + "..."
                + card.latest.sha256.substr(card.latest.sha256.size() >= 4 ? card.latest.sha256.size() - 4 : 0);
        Label(d,
              regular,
              12.5f * s,
              P(left + 64 * U + 12, top + 16 * U - 9),
              Fit(regular, 12.5f, line, areaW - 64 * U - 110 * U - 24).c_str(),
              Rgba(214, 214, 222, opacity));
    }
    float y = top + 42 * U;
    // what the script can do, or what the config changes, above the text
    if (kind == pub::Kind::Script) {
        float x = left;
        if (card.capabilities.empty()) {
            Label(d, regular, 12 * s, P(x, y + 3), "No capabilities declared.", Rgba(148, 148, 160, opacity));
        }
        for (const pub::Capability c : card.capabilities) {
            const bool risky = pub::Sensitive(c);
            const std::string text = std::string(risky ? "! " : "") + pub::CapabilityName(c);
            const float w = TextWidth(semibold, 11, text.c_str()) + 14;
            if (x + w > right) {
                break;
            }
            d->AddRectFilled(
                    P(x, y), P(x + w, y + 20), risky ? Rgba(42, 21, 24, opacity) : Rgba(23, 23, 29, opacity), 0);
            Label(d,
                  semibold,
                  11 * s,
                  P(x + 7, y + 4),
                  text.c_str(),
                  risky ? Rgba(248, 113, 113, opacity) : Rgba(170, 170, 182, opacity));
            x += w + 6;
        }
        y += 30;
    } else if (ready) {
        int known = 0;
        int settings = 0;
        std::string names;
        for (const pub::ConfigSection& section : m.sections) {
            if (Module* module = ModuleById(section.name.c_str())) {
                ++known;
                settings += section.settings;
                if (section.enabled == 1) {
                    names += (names.empty() ? "" : ", ") + module->name;
                }
            }
        }
        const std::string summary = std::to_string(known) + (known == 1 ? " module, " : " modules, ")
                                    + std::to_string(settings) + (settings == 1 ? " setting" : " settings")
                                    + (names.empty() ? std::string() : ". Turns on: " + names);
        Label(d,
              regular,
              12 * s,
              P(left, y + 3),
              Fit(regular, 12, summary, areaW).c_str(),
              Rgba(214, 214, 222, opacity));
        y += 30;
    }
    const ImVec2 lo = P(left, y);
    const ImVec2 size((right - left) * s, (menuH - 20 - y) * s);
    d->AddRectFilled(lo, ImVec2(lo.x + size.x, lo.y + size.y), Rgba(8, 8, 11, opacity), 0);
    if (!ours || source->loading) {
        Label(d, regular, 13 * s, P(left + 16, y + 16), "Loading the source...", Rgba(148, 148, 160, opacity));
        return;
    }
    if (!source->outcome.Ok()) {
        const std::string text = source->outcome.status == pub::Status::DigestMismatch
                                         ? "The file did not match its published hash. Nothing is shown."
                                         : std::string("The source could not be loaded (")
                                                   + pub::StatusName(source->outcome.status) + ").";
        Label(d, regular, 13 * s, P(left + 16, y + 16), text.c_str(), Rgba(246, 160, 171, opacity));
        if (UiButton(d, "pub-source-retry", "Retry", P(left + 16, y + 40), 72 * U, 28 * U, 0, true, opacity, s)) {
            page.OpenSource(card, true);
        }
        return;
    }
    // the text, one line per row, only the visible rows built, horizontal scroll for long lines
    ImGui::SetCursorScreenPos(ImVec2(lo.x + 8 * s, lo.y + 6 * s));
    ImGui::BeginChild("##pub-source-text",
                      ImVec2(size.x - 8 * s, size.y - 12 * s),
                      false,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar);
    WindowFont(s * 12.5f * FontEmFactor);
    ImGuiListClipper clipper;
    clipper.Begin((int)m.lines.size());
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            char number[16];
            snprintf(number, sizeof(number), "%5d  ", i + 1);
            ImGui::TextColored(ImVec4(96 / 255.f, 96 / 255.f, 106 / 255.f, 1), "%s", number);
            ImGui::SameLine(0, 0);
            const std::string& text = m.lines[i];
            size_t first = text.find_first_not_of(' ');
            const bool comment =
                    first != std::string::npos && text.compare(first, 2, "--") == 0 && kind == pub::Kind::Script;
            const bool header = kind == pub::Kind::Config && first != std::string::npos && text[first] == '[';
            const ImVec4 color = comment  ? ImVec4(120 / 255.f, 120 / 255.f, 132 / 255.f, 1)
                                 : header ? themeLive.accent
                                          : ImVec4(224 / 255.f, 224 / 255.f, 230 / 255.f, 1);
            ImGui::TextColored(color, "%s", text.c_str());
        }
    }
    EndChildWindow();
}

void DrawPublic(ImVec2 origin, float s, float opacity) {
    PublicMenu& m = Public();
    const pub::Kind kind = PublicKindOf(tab);
    const int k = static_cast<int>(kind);
    if (!PublicOption().value || !m.page) {
        DrawPublicOff(origin, s, opacity, kind);
        return;
    }
    pub::PublicPage& page = *m.page;
    page.Shown(kind);
    pub::KindView& view = page.View(kind);
    if (view.sourceOpen) {
        DrawPublicSource(origin, s, opacity, kind);
        return;
    }
    const float U = 242.f / 210.f;
    const float left = 257;
    const float right = menuW - 22;
    const float top = 72;
    const float areaW = right - left;
    auto& io = ImGui::GetIO();
    auto* d = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) {
        return ImVec2(origin.x + x * s, origin.y + y * s);
    };
    static float age = 0;
    static int lastFrame = -2;
    if (ImGui::GetFrameCount() != lastFrame + 1 || opacity < .05f) {
        age = 0;
    }
    lastFrame = ImGui::GetFrameCount();
    age += io.DeltaTime;
    // header, right side: account and installed items
    {
        std::string account = m.review ? "Not signed in" : m.signedIn ? "Signed in" : "Not signed in";
        if (m.signedIn && !m.handle.empty()) {
            account = "Signed in as @" + m.handle;
        }
        const float w = TextWidth(regular, 12.5f, account.c_str());
        Label(d, regular, 12.5f * s, P(menuW - 24 - w, 31), account.c_str(), Rgba(148, 148, 160, opacity));
        size_t updates = 0;
        const size_t installed = page.InstalledCount(kind);
        if (page.AnyUpdate(kind)) {
            updates = 1;
        }
        if (installed > 0) {
            const std::string mark = std::to_string(installed) + " installed" + (updates ? " \xc2\xb7 update" : "");
            const float mw = TextWidth(semibold, 12.5f, mark.c_str()) + 12;
            const float x = menuW - 24 - w - 14 - mw;
            d->AddRectFilled(P(x, 29), P(x + mw, 48), Rgba(255, 255, 255, opacity), 0);
            Label(d, semibold, 12.5f * s, P(x + 6, 31), mark.c_str(), AccDeep(opacity));
        }
    }
    // toolbar: search, the three sorts, Refresh
    const pub::ListState state = page.StateOf(kind, m.snap, true);
    {
        const float sortW = 84 * U;
        const float refreshW = 76 * U;
        const float searchW = areaW - 3 * sortW - refreshW - 3 * 6 * U - 8 * U;
        ImGui::PushID(k);
        const bool entered = UiInput("##pub-search",
                                     kind == pub::Kind::Script ? "Search scripts" : "Search configs",
                                     m.search[k],
                                     sizeof(m.search[k]),
                                     P(left, top),
                                     searchW,
                                     32 * U,
                                     34 * U,
                                     s,
                                     opacity);
        page.Type(kind, m.search[k]);
        if (entered) {
            page.Submit(kind);
        }
        if (!DrawGlyph(d,
                       nicons::k_search,
                       P(left + 17 * U - 6.5f, top + 16 * U - 6.5f),
                       15 * s,
                       Rgba(120, 120, 132, opacity))) {
            d->AddCircle(
                    P(left + 17 * U - 1.2f, top + 16 * U - 1.2f), 5 * s, Rgba(120, 120, 132, opacity), 0, 1.5f * s);
        }
        const char* sorts[3] = {"Top Rated", "Trending", "Recent"};
        const pub::Sort values[3] = {pub::Sort::Top, pub::Sort::Trending, pub::Sort::Recent};
        float x = left + searchW + 8 * U;
        for (int i = 0; i < 3; ++i) {
            const bool active = view.sort == values[i];
            if (UiButton(d,
                         std::string("pub-sort") + sorts[i],
                         sorts[i],
                         P(x, top),
                         sortW,
                         32 * U,
                         active ? 1 : 0,
                         true,
                         opacity,
                         s)) {
                page.SetSort(kind, values[i]);
            }
            x += sortW + 6 * U;
        }
        const std::string refresh = state.retryIn > 0 ? std::to_string(state.retryIn) + " s" : "Refresh";
        if (UiButton(d,
                     "pub-refresh",
                     refresh.c_str(),
                     P(right - refreshW, top),
                     refreshW,
                     32 * U,
                     0,
                     state.retryIn == 0,
                     opacity,
                     s)) {
            page.Retry(kind);
        }
        ImGui::PopID();
    }
    float listTop = top + 42 * U;
    auto centred = [&](const std::string& text, float y, ImU32 color, ImFont* font = nullptr, float size = 13) {
        ImFont* f = font != nullptr ? font : regular;
        const std::string fitted = Fit(f, size, text, areaW - 20);
        Label(d, f, size * s, P(left + (areaW - TextWidth(f, size, fitted.c_str())) / 2, y), fitted.c_str(), color);
    };
    const pub::ListResult* list = m.snap.List(kind);
    const float mid = listTop + std::max(60.f, (menuH - 20 - listTop) / 2 - 70);
    switch (state.view) {
        case pub::ListView::Ready:
            break;
        case pub::ListView::Loading:
            break;
        case pub::ListView::Empty:
            centred(state.message, mid, Rgba(148, 148, 160, opacity));
            return;
        case pub::ListView::NoMatch:
        case pub::ListView::BadQuery:
            centred(state.message, mid, Rgba(148, 148, 160, opacity));
            if (UiButton(d,
                         "pub-clear",
                         "Clear search",
                         P(left + (areaW - 110 * U) / 2, mid + 28),
                         110 * U,
                         30 * U,
                         0,
                         true,
                         opacity,
                         s)) {
                m.search[k][0] = 0;
                page.ClearSearch(kind);
            }
            return;
        default: {
            centred(state.message, mid, Rgba(230, 230, 235, opacity), semibold, 14);
            if (!state.detail.empty()) {
                centred(state.detail, mid + 24, Rgba(148, 148, 160, opacity), regular, 12);
            }
            const std::string retry = state.retryIn > 0 ? "Retry in " + std::to_string(state.retryIn) + " s" : "Retry";
            if (UiButton(d,
                         "pub-retry",
                         retry.c_str(),
                         P(left + (areaW - 110 * U) / 2, mid + 52),
                         110 * U,
                         30 * U,
                         1,
                         state.retryIn == 0,
                         opacity,
                         s)) {
                page.Retry(kind);
            }
            return;
        }
    }
    if (state.view == pub::ListView::Ready && !state.message.empty()) {
        d->AddRectFilled(P(left, listTop), P(right, listTop + 26), Rgba(58, 42, 18, .8f * opacity), 0);
        Label(d,
              regular,
              12 * s,
              P(left + 10, listTop + 6),
              Fit(regular, 12, state.message, areaW - 100).c_str(),
              Rgba(255, 179, 92, opacity));
        if (UiButton(d,
                     "pub-note-retry",
                     "Retry",
                     P(right - 70, listTop + 3),
                     64,
                     20,
                     0,
                     state.retryIn == 0,
                     opacity,
                     s)) {
            page.Retry(kind);
        }
        listTop += 34;
    }
    // the grid: columns of at least 220 u, 120 u cards at a 130 u pitch, only the visible rows built
    const int columns = std::max(1, (int)((areaW + 10) / 230));
    const float cardW = (areaW - (columns - 1) * 10) / columns;
    const float cardH = 120;
    const float pitch = 130;
    const float viewH = menuH - 20 - listTop;
    ImGui::SetCursorScreenPos(P(left, listTop));
    ImGui::BeginChild("##pub-grid",
                      ImVec2(areaW * s, viewH * s),
                      false,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    auto* g = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    if (state.view == pub::ListView::Loading) {
        const float pulse = 0.6f + 0.4f * (0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.2f));
        for (int i = 0; i < 6; ++i) {
            const ImVec2 p(base.x + (i % columns) * (cardW + 10) * s, base.y + (i / columns) * pitch * s);
            g->AddRectFilled(p, ImVec2(p.x + cardW * s, p.y + cardH * s), Rgba(20, 20, 27, pulse * opacity), 0);
            g->AddRectFilled(ImVec2(p.x + 12 * s, p.y + 12 * s),
                             ImVec2(p.x + 46 * s, p.y + 46 * s),
                             Rgba(28, 28, 36, pulse * opacity),
                             0);
            g->AddRectFilled(ImVec2(p.x + 56 * s, p.y + 16 * s),
                             ImVec2(p.x + (cardW * .6f) * s, p.y + 28 * s),
                             Rgba(28, 28, 36, pulse * opacity),
                             0);
            g->AddRectFilled(ImVec2(p.x + 12 * s, p.y + 60 * s),
                             ImVec2(p.x + (cardW - 12) * s, p.y + 70 * s),
                             Rgba(28, 28, 36, pulse * opacity),
                             0);
        }
        EndChildWindow();
        return;
    }
    const std::vector<ScriptInfo> known =
            kind == pub::Kind::Script ? ScriptsLibrary().List() : std::vector<ScriptInfo>();
    const size_t count = list != nullptr ? list->items.size() : 0;
    const int rows = (int)((count + columns - 1) / columns);
    const float scrollY = ImGui::GetScrollY();
    const int firstRow = std::max(0, (int)(scrollY / (pitch * s)) - 2);
    const int lastRow = (int)((scrollY + viewH * s) / (pitch * s)) + 2;
    for (int row = firstRow; row <= std::min(lastRow, rows - 1); ++row) {
        for (int col = 0; col < columns; ++col) {
            const size_t index = (size_t)row * columns + col;
            if (index >= count) {
                break;
            }
            const pub::Card& card = list->items[index];
            const float rest = 1 - std::clamp((age - .03f * std::min((int)index, 12)) / .22f, 0.f, 1.f);
            const float e = 1 - rest * rest * rest;
            const ImVec2 p(base.x + col * (cardW + 10) * s, base.y + (row * pitch + (1 - e) * 10) * s);
            const bool starred = pub::PublicPage::Starred(m.snap, card.id);
            DrawPublicCard(g,
                           card,
                           p,
                           cardW,
                           cardH,
                           s,
                           opacity * e,
                           view.selected == card.id,
                           CardBadge(card, known),
                           starred);
            ImGui::PushID(card.id.c_str());
            if (Hit("##pub-card", p, ImVec2(cardW * s, cardH * s))) {
                page.Select(kind, view.selected == card.id ? std::string() : card.id);
            }
            ImGui::PopID();
        }
    }
    float end = rows * pitch;
    if (list != nullptr && !list->nextCursor.empty() && !list->fromCache) {
        const bool more = list->loadingMore;
        const ImVec2 at(base.x + (areaW - 120 * U) / 2 * s, base.y + (end + 4) * s);
        if (UiButton(g, "pub-more", more ? "Loading..." : "Load more", at, 120 * U, 30 * U, 0, !more, opacity, s)) {
            page.LoadMore(kind);
        }
        end += 4 + 30 * U + 10;
    }
    ImGui::SetCursorPosY(end * s);
    ImGui::Dummy(ImVec2(1, 1));
    const float listScroll = ImGui::GetScrollY();
    const float listLimit = ImGui::GetScrollMaxY();
    FadeEdges(s, opacity);
    EndChildWindow();
    if (listLimit > 0) {
        const float total = viewH + listLimit / s;
        const float bar = std::max(30.f, viewH * viewH / total);
        const float barTop = listTop + (viewH - bar) * (listScroll / listLimit);
        d->AddRectFilled(P(right + 3, barTop + 6), P(right + 6, barTop + bar - 6), Acc(.85f * opacity), 0);
    }
}

// ---- the side panel

void DrawPublicPanel(ImVec2 origin, float s, float opacity) {
    PublicMenu& m = Public();
    const pub::Kind kind = PublicKindOf(tab);
    const float U = 242.f / 210.f;
    const float x0 = 16;
    const float width = 297 - 32;
    auto P = [&](float x, float y) {
        return ImVec2(origin.x + x * s, origin.y + y * s);
    };
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(ImVec2(297 * s, menuH * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);
    ImGui::Begin("##NickelPublicPanel",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                         | ImGuiWindowFlags_NoBackground | (opened.load() ? 0 : ImGuiWindowFlags_NoInputs));
    auto* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(origin, P(297, menuH), Rgba(13, 13, 15, .9f * opacity), 0);
    d->AddRect(origin, P(297, menuH), Rgba(60, 60, 72, .5f * opacity), 0);
    WindowFont(s * 15.f * FontEmFactor);
    const float a = opacity;
    float y = 16;
    auto take = [&](float height, float gap) {
        const float at = y;
        y += (height + gap) * U;
        return at;
    };
    auto finish = [&] {
        EndWindow();
        ImGui::PopStyleVar(2);
    };
    auto note = [&](const std::string& text, ImU32 color) {
        for (const std::string& line : WrapText(regular, 12 * U, text, width, 4)) {
            const float at = take(15, 1);
            Label(d, regular, 12 * U * s, P(x0, at), line.c_str(), color);
        }
        y += 6 * U;
    };
    if (!PublicOption().value || !m.page) {
        note("Turn PUBLIC on to browse what players share.", Rgba(148, 148, 160, a));
        finish();
        return;
    }
    pub::PublicPage& page = *m.page;
    const pub::DetailResult* detail = nullptr;
    const pub::Card* found = SelectedCard(kind, &detail);
    if (found == nullptr) {
        if (detail != nullptr && !detail->loading && !detail->outcome.Ok()) {
            note(detail->outcome.status == pub::Status::Gone ? "This item was removed from PUBLIC."
                                                             : std::string("The details could not be loaded (")
                                                                       + pub::StatusName(detail->outcome.status) + ").",
                 Rgba(246, 160, 171, a));
        } else {
            note(kind == pub::Kind::Script ? "Select a script to see its details."
                                           : "Select a config to see its details.",
                 Rgba(148, 148, 160, a));
        }
        if (!m.signedIn) {
            note("Sign in through the Nickel launcher (Settings > Account) to star creations.", Rgba(120, 120, 132, a));
        }
        finish();
        return;
    }
    const pub::Card card = *found;
    const pub::LocalState* localState = page.LocalOf(kind, card.id);
    const pub::Local local = localState != nullptr ? localState->state : pub::Local::None;
    const pub::InstallRecord record = localState != nullptr ? localState->record : pub::InstallRecord();
    const std::vector<ScriptInfo> known =
            kind == pub::Kind::Script ? ScriptsLibrary().List() : std::vector<ScriptInfo>();
    const overlay::ScriptInfo* script =
            local != pub::Local::None && kind == pub::Kind::Script ? FindScriptInfo(known, record.Name()) : nullptr;
    const bool compatible = pub::CompatibleWith(card.latest.nickelMin, page.Running());
    // name, author, version
    {
        const float at = take(22, 4);
        Label(d,
              bold,
              17 * U * s,
              P(x0, at + 11 * U - .7f * 17 * U),
              Fit(bold, 17 * U, card.name, width).c_str(),
              Rgba(255, 255, 255, a));
    }
    {
        const float at = take(16, 8);
        std::string by = "by " + (card.author.displayName.empty() ? card.author.handle : card.author.displayName);
        by = Fit(regular, 12 * U, by, width - 90);
        Label(d, regular, 12 * U * s, P(x0, at), by.c_str(), Rgba(148, 148, 160, a));
        float x = x0 + TextWidth(regular, 12 * U, by.c_str()) + 5;
        if (card.author.verified) {
            DrawTick(d, P(x, at + 1.5f), 12 * s, Rgba(255, 255, 255, a));
            x += 16;
        }
        if (card.author.kind == pub::AuthorKind::Legacy) {
            Label(d, regular, 11 * s, P(x, at + 1), "unverified", Rgba(120, 120, 132, a));
        }
        const std::string version = pub::VersionText(card.latest);
        Label(d,
              regular,
              12 * U * s,
              P(x0 + width - TextWidth(regular, 12 * U, version.c_str()), at),
              version.c_str(),
              Rgba(148, 148, 160, a));
    }
    {
        const float at = take(16, 10);
        const bool starred = pub::PublicPage::Starred(m.snap, card.id);
        DrawStar(d, P(x0 + 6, at + 7), 6 * s, starred ? Acc(a) : Rgba(148, 148, 160, a), starred);
        const std::string line = pub::FormatCount(card.stats.stars) + (card.stats.stars == 1 ? " star" : " stars")
                                 + "  \xc2\xb7  " + pub::FormatCount(card.stats.downloads)
                                 + (card.stats.downloads == 1 ? " download" : " downloads");
        Label(d, regular, 12 * U * s, P(x0 + 17, at), line.c_str(), Rgba(214, 214, 222, a));
    }
    // three tiles: updated, size, needs
    {
        const float at = take(52, 10);
        const std::string values[3] = {ShortAgo(pub::FormatAgo(card.updatedAt, page.Now())),
                                       pub::FormatSize(card.latest.size),
                                       card.latest.nickelMin.empty() ? std::string("any") : card.latest.nickelMin};
        const char* terms[3] = {"UPDATED", "SIZE", "NEEDS"};
        const float w = (width - 2 * 6 * U) / 3;
        for (int i = 0; i < 3; ++i) {
            const float x = x0 + i * (w + 6 * U);
            d->AddRectFilled(P(x, at), P(x + w, at + 52 * U), Rgba(0, 0, 0, .22f * a), 0);
            Label(d, bold, 9.5f * U * s, P(x + 9 * U, at + 9 * U), terms[i], Rgba(120, 120, 132, a));
            Label(d,
                  bold,
                  13 * U * s,
                  P(x + 9 * U, at + 27 * U),
                  Fit(bold, 13 * U, values[i], w - 14 * U).c_str(),
                  !compatible && i == 2 ? Rgba(248, 113, 113, a) : Rgba(230, 230, 235, a));
        }
    }
    // actions
    const pub::PendingInstall& pending = page.Pending();
    const bool mine = pending.step != pub::Step::None && pending.card.id == card.id;
    const float half = (width - 6 * U) / 2;
    auto two = [&](const char* id1,
                   const char* label1,
                   int style1,
                   bool on1,
                   const char* id2,
                   const char* label2,
                   int style2,
                   bool on2) {
        const float at = take(32, 6);
        const bool first = UiButton(d, id1, label1, P(x0, at), half, 32 * U, style1, on1, a, s);
        const bool second = UiButton(d, id2, label2, P(x0 + half + 6 * U, at), half, 32 * U, style2, on2, a, s);
        return first ? 1 : second ? 2 : 0;
    };
    auto one = [&](const char* id, const char* label, int style, bool on) {
        const float at = take(32, 6);
        return UiButton(d, id, label, P(x0, at), width, 32 * U, style, on, a, s);
    };
    const bool updateAvailable = local == pub::Local::UpdateAvailable;
    if (mine && pending.step == pub::Step::Downloading) {
        one("pub-installing", "Installing...", 1, false);
    } else if (mine && pending.step == pub::Step::NameConflict) {
        note(pending.message, Rgba(255, 179, 92, a));
        const int pressed = two("pub-keep-both", "Keep both", 1, true, "pub-replace", "Replace", 3, true);
        if (pressed == 1) {
            page.Resolve(pub::OnConflict::KeepBoth);
        } else if (pressed == 2) {
            page.Resolve(pub::OnConflict::Replace);
        }
        if (one("pub-conflict-cancel", "Cancel", 0, true)) {
            page.CancelPending();
        }
    } else if (mine && pending.step == pub::Step::Modified) {
        note(pending.message, Rgba(255, 179, 92, a));
        const int pressed =
                two("pub-replace-edits", "Replace my edits", 3, true, "pub-modified-cancel", "Cancel", 0, true);
        if (pressed == 1) {
            page.ReplaceEdits();
        } else if (pressed == 2) {
            page.CancelPending();
        }
    } else if (local == pub::Local::None) {
        const char* view = kind == pub::Kind::Script ? "View source" : "Preview settings";
        const int pressed = two("pub-install", "Install", 1, compatible, "pub-view", view, 0, true);
        if (pressed == 1) {
            if (kind == pub::Kind::Script) {
                page.OpenSource(card); // a script's source is always shown before it is installed
            } else {
                page.Install(card);
            }
        } else if (pressed == 2) {
            page.OpenSource(card);
        }
        if (!compatible) {
            note("This needs Nickel " + card.latest.nickelMin + " or newer.", Rgba(248, 113, 113, a));
        }
    } else {
        if (kind == pub::Kind::Script) {
            const bool running = script != nullptr && script->state == ScriptState::Running;
            if (updateAvailable || local == pub::Local::Withdrawn) {
                const int pressed = two("pub-review-update",
                                        local == pub::Local::Withdrawn ? "Current version" : "Review update",
                                        1,
                                        compatible,
                                        "pub-skip",
                                        "Skip version",
                                        0,
                                        updateAvailable);
                if (pressed == 1) {
                    page.OpenSource(card);
                } else if (pressed == 2) {
                    page.SkipVersion(card);
                }
            }
            if (running) {
                const int pressed =
                        two("pub-open-scripting", "Open in Scripting", 1, true, "pub-stop", "Stop", 0, true);
                if (pressed == 1) {
                    tab = 8;
                    scriptingSelected = script->serial;
                } else if (pressed == 2) {
                    page.Stop(record.Name());
                }
            } else {
                // Start is the only way an installed script runs; a revoked one stays held
                const int pressed = two("pub-start",
                                        "Start",
                                        1,
                                        script != nullptr && local != pub::Local::Revoked,
                                        "pub-view",
                                        "View source",
                                        0,
                                        true);
                if (pressed == 1) {
                    page.Start(record.Name());
                } else if (pressed == 2) {
                    page.OpenSource(card);
                }
            }
        } else {
            if (updateAvailable || local == pub::Local::Withdrawn) {
                const int pressed = two("pub-update",
                                        local == pub::Local::Withdrawn ? "Current version" : "Update",
                                        1,
                                        compatible,
                                        "pub-skip",
                                        "Skip version",
                                        0,
                                        updateAvailable);
                if (pressed == 1) {
                    page.Install(card, true);
                } else if (pressed == 2) {
                    page.SkipVersion(card);
                }
            }
            const int pressed = two("pub-load",
                                    "Load config",
                                    1,
                                    local != pub::Local::Revoked,
                                    "pub-view",
                                    "Preview settings",
                                    0,
                                    true);
            if (pressed == 1) {
                LoadConfig(record.Name());
                notices.push_back({8, "PUBLIC", "Loaded config " + record.Name() + "."});
            } else if (pressed == 2) {
                page.OpenSource(card);
            }
        }
        if (m.edited == card.id) {
            note("You edited this file. Remove it anyway?", Rgba(255, 179, 92, a));
            const int pressed =
                    two("pub-remove-anyway", "Remove anyway", 3, true, "pub-remove-cancel", "Cancel", 0, true);
            if (pressed == 1) {
                page.Uninstall(kind, card.id, true);
                m.edited.clear();
            } else if (pressed == 2) {
                m.edited.clear();
            }
        } else {
            const bool armed = m.armed == card.id;
            if (one("pub-uninstall", armed ? "Click again to uninstall" : "Uninstall", 3, true)) {
                if (!armed) {
                    m.armed = card.id;
                    m.armedAt = ImGui::GetTime();
                } else {
                    m.armed.clear();
                    if (page.Uninstall(kind, card.id, false) == pub::InstallStatus::Modified) {
                        m.edited = card.id;
                    }
                }
            }
        }
    }
    if (mine && pending.step == pub::Step::Failed) {
        note(pending.message, Rgba(246, 160, 171, a));
    }
    // star: only with an account (the launcher signs in)
    if (m.signedIn) {
        const bool starred = pub::PublicPage::Starred(m.snap, card.id);
        const bool waiting = m.snap.star && m.snap.star->id == card.id && m.snap.star->loading;
        if (one("pub-star", starred ? "Starred" : "Star", starred ? 1 : 0, !waiting)) {
            page.Star(card.id, !starred);
        }
    } else {
        note("Sign in through the Nickel launcher (Settings > Account) to star creations.", Rgba(120, 120, 132, a));
    }
    {
        const float at = take(1, 10);
        d->AddLine(P(x0, at), P(x0 + width, at), Rgba(255, 255, 255, .07f * a), U * s);
    }
    // the rest scrolls: state, capabilities or changes, description, tags, licence, what's new, review
    ImGui::SetCursorScreenPos(P(x0, y));
    ImGui::BeginChild("##pub-detail-scroll",
                      ImVec2((width + 8) * s, std::max(40.f, menuH - y - 12) * s),
                      false,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    auto* c = ImGui::GetWindowDrawList();
    WindowFont(s * 12.5f * FontEmFactor);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width * s);
    auto heading = [&](const char* text, ImU32 color) {
        ImGui::Dummy(ImVec2(1, 4 * s));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        Label(c, bold, 10.5f * U * s, at, text, color);
        ImGui::Dummy(ImVec2(1, 16 * U * s));
    };
    auto text = [&](const std::string& value, ImVec4 color) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.x, color.y, color.z, color.w * a));
        ImGui::TextWrapped("%s", value.c_str());
        ImGui::PopStyleColor();
    };
    const ImVec4 grey(148 / 255.f, 148 / 255.f, 160 / 255.f, 1);
    const ImVec4 light(214 / 255.f, 214 / 255.f, 222 / 255.f, 1);
    const ImVec4 red(246 / 255.f, 160 / 255.f, 171 / 255.f, 1);
    const ImVec4 amber(1, 179 / 255.f, 92 / 255.f, 1);
    if (*pub::LocalReason(local)) {
        text(pub::LocalReason(local), local == pub::Local::Modified || local == pub::Local::Withdrawn ? amber : red);
    }
    if (local == pub::Local::Revoked) {
        for (const pub::RevokedInstall& revoked : page.Installs().Revoked()) {
            if (revoked.id == card.id && !revoked.reason.empty()) {
                text("Reason: " + revoked.reason, red);
            }
        }
    }
    if (kind == pub::Kind::Script) {
        heading("CAN", Rgba(120, 120, 132, a));
        if (card.capabilities.empty()) {
            text("Nothing declared.", grey);
        }
        for (const pub::Capability cap : card.capabilities) {
            const bool risky = pub::Sensitive(cap);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (risky) {
                c->AddRectFilled(
                        ImVec2(at.x, at.y + 2 * s), ImVec2(at.x + 12 * s, at.y + 14 * s), Rgba(42, 21, 24, a), 0);
                Label(c, bold, 10 * s, ImVec2(at.x + 4 * s, at.y + 2.5f * s), "!", Rgba(248, 113, 113, a));
            } else {
                c->AddCircleFilled(ImVec2(at.x + 6 * s, at.y + 8 * s), 2 * s, Rgba(120, 120, 132, a));
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x + 18 * s, at.y));
            text(pub::CapabilityText(cap), risky ? red : light);
            ImGui::SetCursorScreenPos(ImVec2(at.x, ImGui::GetCursorScreenPos().y));
        }
        if (pub::ChatLeakWarning(card.capabilities)) {
            text("It reads your keys and can send chat: it may post your name, server or keys in chat.", red);
        }
    } else {
        heading("CHANGES", Rgba(120, 120, 132, a));
        std::string changes;
        if (card.changedModules >= 0) {
            changes += std::to_string(card.changedModules) + (card.changedModules == 1 ? " module" : " modules");
        }
        if (card.changedKeybinds >= 0) {
            changes += (changes.empty() ? "" : ", ") + std::to_string(card.changedKeybinds)
                       + (card.changedKeybinds == 1 ? " keybind" : " keybinds");
        }
        std::string categories;
        for (const std::string& category : card.categories) {
            std::string word = category;
            if (!word.empty()) {
                word[0] = (char)std::toupper((unsigned char)word[0]);
            }
            categories += (categories.empty() ? "" : " \xc2\xb7 ") + word;
        }
        if (!categories.empty()) {
            changes += (changes.empty() ? "" : ", ") + categories;
        }
        text(changes.empty() ? "Open Preview settings to see what it changes." : changes, light);
    }
    if (detail != nullptr && detail->outcome.Ok() && !detail->loading) {
        if (!detail->detail.description.empty()) {
            heading("DESCRIPTION", Rgba(120, 120, 132, a));
            text(detail->detail.description, light);
        }
        if (!detail->detail.versions.empty() && detail->detail.versions[0].number == card.latest.number
            && !detail->detail.versions[0].changelog.empty()) {
            heading("WHAT'S NEW", Rgba(120, 120, 132, a));
            text(detail->detail.versions[0].changelog, light);
        }
    } else if (detail != nullptr && detail->loading) {
        ImGui::Dummy(ImVec2(1, 6 * s));
        text("Loading the description...", grey);
    } else if (!card.summary.empty()) {
        heading("DESCRIPTION", Rgba(120, 120, 132, a));
        text(card.summary, light);
    }
    if (!card.tags.empty()) {
        heading("TAGS", Rgba(120, 120, 132, a));
        std::string tags;
        for (const std::string& tag : card.tags) {
            tags += (tags.empty() ? "" : "  \xc2\xb7  ") + tag;
        }
        text(tags, light);
    }
    heading("LICENSE", Rgba(120, 120, 132, a));
    text(card.license.empty() || card.license == "NOASSERTION" ? "No licence given" : card.license, light);
    ImGui::Dummy(ImVec2(1, 8 * s));
    if (card.latest.reviewed) {
        const std::string day = pub::FormatDay(card.latest.reviewedAt);
        text("Reviewed by the Nickel team" + (day.empty() ? std::string() : " on " + day) + ".",
             ImVec4(159 / 255.f, 232 / 255.f, 191 / 255.f, 1));
    } else {
        text(kind == pub::Kind::Script ? "Not reviewed: read the source before starting it." : "Not reviewed.", amber);
    }
    ImGui::Dummy(ImVec2(1, 8 * s));
    {
        std::string url = detail != nullptr && detail->outcome.Ok() ? detail->detail.pageUrl : std::string();
        if (url.rfind("https://nickelclient.com/", 0) != 0) {
            url = std::string("https://nickelclient.com/public/") + pub::KindName(kind) + "/" + card.slug;
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (UiButton(c,
                     "pub-open-site",
                     "Open on nickelclient.com",
                     at,
                     width,
                     30 * U,
                     0,
                     pub::ValidSlug(card.slug),
                     a,
                     s)) {
            ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + 30 * U * s));
        ImGui::Dummy(ImVec2(1, 10 * s));
    }
    ImGui::PopTextWrapPos();
    FadeEdges(s, opacity);
    EndChildWindow();
    finish();
}
