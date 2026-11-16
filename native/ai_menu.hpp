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
        return status; // the server address is not sent
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
                         [](const std::string& id) { return SoonModule(id); }};
    ai::Services services;
    services.client = std::make_unique<ai::AssistantChat>(std::make_unique<ai::AssistantClient>(config, source));
    services.tools = std::make_unique<ai::AssistantToolRunner>(deps);
    return services;
}
