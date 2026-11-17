// Nickel AI in the game: the real services behind the panel (ai_ui.hpp). Included by menu.cpp inside its anonymous
// namespace, after the panel's drawing, so it can use the menu's modules, tabs and options. DrawMenu installs
// MakeAiServices as nickel::ai::Factory(); the panel calls it the first time the player opens the chat window from the
// Nickel AI bubble (while the option is on), so the install key and the network client exist only from then.
//
// - The client talks to https://nickelclient.com/api/ai/v1, or to NICKEL_AI_LOCAL_SERVER (an http://127.0.0.1:<port>
//   origin) for local tests. It logs request lines and error kinds only, never the key or a body.
// - The tools change modules through the menu's ScriptHost (MenuScriptHostInstance, the same registry the Luau
//   bindings use, saved like a click on a toggle) and scripts through ScriptsLibrary(): a written script is held.
// - Navigation opens a tab or selects a module like a click in the side column would; "Nickel AI" is the chat window.
// ai_integration.hpp is included at the top of menu.cpp (outside the anonymous namespace).

namespace nickel_ai_menu_detail {
inline std::string Lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}
} // namespace nickel_ai_menu_detail

class MenuAiNavigator final : public nickel::ai::tools::UiNavigator {
public:
    bool ShowModule(int index, const std::string& id) override {
        if (index < 0 || index >= static_cast<int>(modules.size()) || modules[index].id != id || SoonModule(id)) {
            return false;
        }
        for (int i = 0; i < 6; ++i) {
            if (modules[index].category == names[i]) {
                tab = i;
                category = i;
                selected = index;
                listening = -1;
                search[0] = 0;
                return true;
            }
        }
        return false;
    }

    bool ShowTab(const std::string& wanted) override {
        const std::string name = nickel_ai_menu_detail::Lower(wanted);
        int found = -1;
        if (name == "home" || name == "settings") {
            found = 6;
        } else if (name == "nickel ai" || name == "ai" || name == "assistant") {
            Ai().bubble.Open(); // not a tab: the chat window beside the menu (already open while a tool runs)
            return true;
        } else if ((name == "public scripts" || name == "community scripts") && PublicOption().value) {
            found = 11;
        } else if ((name == "public configs" || name == "community configs") && PublicOption().value) {
            found = 12;
        } else {
            for (int i = 0; i <= 9; ++i) {
                if (Lower(names[i]) == name) {
                    found = i;
                    break;
                }
            }
        }
        if (found < 0) {
            return false;
        }
        tab = found;
        if (found < 6) {
            category = found;
        }
        selected = -1;
        listening = -1;
        search[0] = 0;
        return true;
    }

private:
    static std::string Lower(const char* text) { return nickel_ai_menu_detail::Lower(text); }
};

std::string AiGameVersion() {
    uint16_t version[4]{};
    if (!game::GameVersion(GetModuleHandleW(nullptr), version)) {
        return "";
    }
    char text[48];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", version[0], version[1], version[2], version[3]);
    return text;
}

// The player's language for the answers ("fr-FR"), from Windows.
std::string AiLanguage() {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) <= 0) {
        return "en";
    }
    std::string text;
    for (const wchar_t* c = locale; *c && text.size() < 35; ++c) {
        const wchar_t w = *c;
        if ((w >= L'a' && w <= L'z') || (w >= L'A' && w <= L'Z') || (w >= L'0' && w <= L'9') || w == L'-') {
            text.push_back(static_cast<char>(w));
        }
    }
    return text.empty() ? "en" : text;
}

class MenuAiStatus final : public nickel::ai::tools::ClientStatusSource {
public:
    nickel::ai::tools::ClientStatus Status() const override {
        nickel::ai::tools::ClientStatus status;
        status.nickelVersion = pub::kRunningNickel;
        status.gameVersion = AiGameVersion();
        game::Context context{};
        status.inWorld = game::Local(&context);
        // The server the player is on (the player asks Nickel AI about it): address and port, or a local world.
        game::Server server{};
        if (status.inWorld && game::ReadServer(&server)) {
            if (server.singleplayer) {
                status.server = "singleplayer (a local world)";
            } else if (server.address[0]) {
                server.address[sizeof(server.address) - 1] = 0;
                status.server = std::string(server.address) + ":" + std::to_string(server.port);
            }
        }
        return status;
    }
};

// The game's packet capture for capture_packets / read_packets: the hook goes in on the first capture.
class MenuAiPackets final : public nickel::ai::tools::PacketSource {
public:
    bool Ready(std::string* why) override {
        if (game::InstallCaptureHook(WriteLog) == 1) {
            return true;
        }
        *why = "Packet capture is not available in this game version";
        return false;
    }
    nickel::packets::Capture& Capture() override { return nickel::packets::Shared(); }
    int64_t NowMs() override { return game::CaptureNowMs(); }
};

// get_server_info and get_world_state: the server session kept since the join (packet_capture.hpp) and what the
// game shows around the player (the readers of the script engine).
class MenuAiGame final : public nickel::ai::tools::GameSource {
public:
    nickel::json::Value ServerInfo(const nickel::json::Value& in) override {
        namespace json = nickel::json;
        json::Value body = json::Value::MakeObject();
        game::Server server{};
        game::Context context{};
        const bool inWorld = game::Local(&context);
        body.Set("in_world", json::Value::MakeBool(inWorld));
        if (inWorld && game::ReadServer(&server)) {
            server.address[sizeof(server.address) - 1] = 0;
            body.Set("singleplayer", json::Value::MakeBool(server.singleplayer));
            if (!server.singleplayer) {
                body.Set("address", json::Value::MakeString(server.address));
                body.Set("port", json::Value::MakeInt(server.port));
            }
        }
        body.Set("transport", json::Value::MakeString(game::OnRakNet() ? "RakNet" : "NetherNet or local"));
        const auto view = nickel::packets::SharedSession().View(game::CaptureNowMs());
        body.Set("session_seconds", json::Value::MakeInt(view.sinceMs / 1000));
        body.Set("session_recorded", json::Value::MakeBool(view.active));
        auto counts = [](const std::vector<std::pair<int, uint64_t>>& list, size_t most) {
            json::Value array = json::Value::MakeArray();
            for (size_t i = 0; i < list.size() && i < most; ++i) {
                json::Value item = json::Value::MakeObject();
                item.Set("id", json::Value::MakeInt(list[i].first));
                item.Set("count", json::Value::MakeInt(static_cast<int64_t>(list[i].second)));
                array.Push(std::move(item));
            }
            return array;
        };
        body.Set("packets_received", counts(view.in, 80));
        body.Set("packets_sent", counts(view.out, 40));
        int64_t maxBytes = 160;
        if (const auto* v = in.Find("max_bytes"); v != nullptr) v->GetInt64(&maxBytes);
        maxBytes = std::clamp<int64_t>(maxBytes, 0, 8192);
        std::vector<int> only;
        if (const auto* ids = in.Find("ids"); ids != nullptr && ids->IsArray()) {
            for (size_t i = 0; i < ids->size() && i < 32; ++i) only.push_back(static_cast<int>(ids->At(i).AsInt64(-1)));
        }
        std::vector<json::Value> kept;
        for (const auto& r : view.kept) {
            if (!only.empty() && std::find(only.begin(), only.end(), r.id) == only.end()) continue;
            json::Value item = json::Value::MakeObject();
            item.Set("id", json::Value::MakeInt(r.id));
            item.Set("seconds_after_join", json::Value::MakeInt(r.atMs / 1000));
            item.Set("size", json::Value::MakeInt(r.size));
            item.Set("hex", json::Value::MakeString(nickel::packets::Hex(r.payload, static_cast<size_t>(maxBytes))));
            if (r.truncated || r.payload.size() > static_cast<size_t>(maxBytes)) item.Set("cut", json::Value::MakeBool(true));
            kept.push_back(std::move(item));
        }
        nickel::ai::tools::detail::SetBounded(body, "kept_packets", kept);
        return body;
    }

    nickel::json::Value WorldState(const nickel::json::Value& in) override {
        namespace json = nickel::json;
        json::Value body = json::Value::MakeObject();
        game::Context context{};
        if (!game::Local(&context)) {
            body.Set("in_world", json::Value::MakeBool(false));
            return body;
        }
        body.Set("in_world", json::Value::MakeBool(true));
        int dimension = -1;
        static const char* names[] = {"Overworld", "Nether", "TheEnd"};
        if (game::ReadDimension(&dimension) && dimension >= 0 && dimension < 3) {
            body.Set("dimension", json::Value::MakeString(names[dimension]));
        }
        float health{}, maxHealth{};
        if (game::ReadHealth(&health, &maxHealth)) {
            body.Set("health", json::Value::MakeNumber(health));
            body.Set("max_health", json::Value::MakeNumber(maxHealth));
        }
        int64_t radius = 64;
        if (const auto* v = in.Find("radius"); v != nullptr) v->GetInt64(&radius);
        radius = std::clamp<int64_t>(radius, 4, 256);
        static struct ScriptEntity list[512];   // "struct": script_api21.hpp has a function of that name
        const int n = ReadScriptEntities(list, 512, static_cast<float>(radius), false);
        float me[3] = {0, 0, 0};
        for (int i = 0; i < n; ++i) {
            if (list[i].local) {
                std::copy(list[i].pos, list[i].pos + 3, me);
                json::Value pos = json::Value::MakeArray();
                for (float v : list[i].pos) pos.Push(json::Value::MakeNumber(std::round(v * 10) / 10));
                body.Set("position", std::move(pos));
            }
        }
        std::vector<json::Value> players;
        std::map<std::string, std::pair<int, float>> others;   // name or type -> count, nearest distance
        for (int i = 0; i < n; ++i) {
            if (list[i].local) continue;
            const float dx = list[i].pos[0] - me[0], dy = list[i].pos[1] - me[1], dz = list[i].pos[2] - me[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            list[i].name[sizeof(list[i].name) - 1] = 0;
            if (list[i].player) {
                json::Value p = json::Value::MakeObject();
                p.Set("name", json::Value::MakeString(list[i].name));
                p.Set("distance", json::Value::MakeNumber(std::round(distance * 10) / 10));
                players.push_back(std::move(p));
            } else {
                const std::string key = list[i].name[0] ? list[i].name : "entity." + std::to_string(list[i].type & 0xff);
                auto& entry = others[key];
                entry.second = entry.first ? std::min(entry.second, distance) : distance;
                entry.first += 1;
            }
        }
        nickel::ai::tools::detail::SetBounded(body, "players_nearby", players, 6000);
        std::vector<json::Value> entities;
        for (const auto& [name, entry] : others) {
            json::Value e = json::Value::MakeObject();
            e.Set("name", json::Value::MakeString(name));
            e.Set("count", json::Value::MakeInt(entry.first));
            e.Set("nearest", json::Value::MakeNumber(std::round(entry.second * 10) / 10));
            entities.push_back(std::move(e));
        }
        nickel::ai::tools::detail::SetBounded(body, "entities_nearby", entities, 12000);
        body.Set("radius", json::Value::MakeInt(radius));
        return body;
    }
};

void AiLog(const std::string& line) {
    if (WriteLog) {
        WriteLog(("Nickel AI: " + line).c_str());
    }
}

nickel::ai::Services MakeAiServices() {
    namespace ai = nickel::ai;
    // Kept for the life of the overlay, like PUBLIC's: the client may still read it on its worker while it stops.
    static pub::InstallIdentity* identity = new pub::InstallIdentity(pub::InstallKeyPath(PublicRoot()));
    static ai::InstallIdentitySource* source = new ai::InstallIdentitySource(*identity);
    static MenuAiNavigator navigator;
    static MenuAiStatus status;
    static ai::tools::RegistrySettingsWriter writer(MenuScriptHostInstance());
    static MenuAiPackets packets;
    static MenuAiGame gameInfo;

    ai::ClientConfig config;
    config.userAgent = L"Nickel-Overlay/" + Wide(pub::kRunningNickel);
    config.client.nickelVersion = pub::kRunningNickel;
    config.client.gameVersion = AiGameVersion();
    config.client.language = AiLanguage();
    config.log = AiLog;
    if (ai::UseLocalTestServerFromEnvironment(config)) {
        AiLog("local test server " + config.baseUrl);
    }
    ai::tools::Deps deps{MenuScriptHostInstance(), ScriptsLibrary(), writer, navigator, status, &defaults,
                         [](const std::string& id) { return SoonModule(id); }, &packets, &gameInfo};
    ai::Services services;
    services.client = std::make_unique<ai::AssistantChat>(std::make_unique<ai::AssistantClient>(config, source));
    services.tools = std::make_unique<ai::AssistantToolRunner>(deps);
    return services;
}
