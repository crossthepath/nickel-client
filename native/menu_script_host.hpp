// The menu's implementation of ScriptHost (script_host.hpp): what the Luau engine may use of the menu.
//
// menu.cpp includes this file inside its anonymous namespace, right before script_engine.hpp; it is the only place
// where the engine's needs meet the menu's globals (the module list, Save, the preferences file, the key bindings,
// notifications). InitializeMenu passes MenuScriptHostInstance() to SetScriptHost before any script runs.
#include "script_keys.hpp" // ScriptKeyDown

// Script drawing on an ImGui draw list, with Nickel's fonts for text (Label, TextWidth).
class MenuScriptCanvas final : public ImGuiScriptCanvas {
public:
    void Text(CanvasFont font, float size, CanvasPoint topLeft, const char* text, CanvasColor color) override {
        Label(list, FontOf(font), size, ImVec2(topLeft.x, topLeft.y), text, color);
    }

    float MeasureText(CanvasFont font, float size, const char* text) override {
        return TextWidth(FontOf(font), size, text);
    }

private:
    // Read at each call: the fonts are rebuilt when the renderer restarts.
    static ImFont* FontOf(CanvasFont font) {
        return font == CanvasFont::Bold ? bold : (font == CanvasFont::SemiBold ? semibold : regular);
    }
};

class MenuScriptHost final : public ScriptHost {
public:
    int Count() const override { return static_cast<int>(modules.size()); }

    Module& At(int index) override { return modules[index]; }

    // The last module with this id, as the engine always searched (ids are unique in practice).
    int Find(const std::string& id) const override {
        int found = -1;
        for (int i = 0; i < Count(); ++i) {
            if (modules[i].id == id) {
                found = i;
            }
        }
        return found;
    }

    // Pointers into `modules` are kept (ModuleById caches them), so the list only grows within the capacity reserved
    // in InitializeMenu and is never reallocated.
    int Add(const std::string& id) override {
        if (modules.size() >= modules.capacity()) {
            return -1;
        }
        Module fresh{};
        fresh.id = id;
        modules.push_back(fresh);
        return Count() - 1;
    }

    void SaveModule(int index, const Setting* changed) override { Save(modules[index], changed); }

    std::string ReadPreference(const std::string& section, const std::string& key, size_t maxLength) const override {
        std::wstring text(maxLength + 1, L'\0');
        GetPrivateProfileStringW(Wide(section).c_str(),
                                 Wide(key).c_str(),
                                 L"",
                                 text.data(),
                                 static_cast<DWORD>(text.size()),
                                 configPath.c_str());
        return Utf8(text.c_str());
    }

    int ReadPreferenceInt(const std::string& section, const std::string& key, int fallback) const override {
        return static_cast<int>(
                GetPrivateProfileIntW(Wide(section).c_str(), Wide(key).c_str(), fallback, configPath.c_str()));
    }

    int KeyOf(const std::string& id) const override {
        const auto bound = keycodes.find(id);
        return bound == keycodes.end() ? 0 : bound->second;
    }

    void BindKey(const std::string& id, const std::string& name, int vk) override {
        keybinds[id] = name;
        keycodes[id] = vk;
    }

    bool GameKeyDown(int vk) const override {
        const HWND game = gameWindowForKeys ? GetAncestor(gameWindowForKeys, GA_ROOT) : nullptr;
        return ScriptKeyDown(vk, GetForegroundWindow(), game, GetAsyncKeyState);
    }

    bool HotkeysAllowed() const override { return !opened && KeysAllowed(); }

    void OnModuleRemoved(int index) override {
        if (selected == index) {
            selected = -1;
        }
    }

    void Notify(const std::string& title, const std::string& text) override {
        if (notices.size() < 16) {
            notices.push_back({8, title, text});
        }
    }

    void Log(const std::string& line) override {
        if (WriteLog) {
            WriteLog(line.c_str());
        }
    }

    // World drawing goes to ImGui's background list; HUD drawing to the HUD list, which is never rendered while the
    // HUD is hidden (outside play), so script HUD state keeps running.
    ScriptCanvas& Canvas(CanvasLayer layer) override {
        MenuScriptCanvas& canvas = layer == CanvasLayer::World ? world_ : hud_;
        canvas.list = layer == CanvasLayer::World ? ImGui::GetBackgroundDrawList() : HudList();
        return canvas;
    }

    CanvasPoint DisplaySize() const override {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        return {size.x, size.y};
    }

    HudFrame PlaceHud(const std::string& id, float width, float height) override {
        HudPlace& place = Place(id.c_str(), 20, 200);
        const HudFrame frame{place.x, place.y, place.scale};
        place.lo = ImVec2(frame.x, frame.y);
        place.hi = ImVec2(frame.x + width * frame.scale, frame.y + height * frame.scale);
        return frame;
    }

private:
    MenuScriptCanvas world_;
    MenuScriptCanvas hud_;
};

ScriptHost& MenuScriptHostInstance() {
    static MenuScriptHost host;
    return host;
}
