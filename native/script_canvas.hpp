#pragma once
// What Luau scripts draw on, without ImGui (tests/script_canvas_checks.cpp).
//
// The engine draws script HUDs and world overlays (render2d / render3d) through ScriptCanvas; the menu implements it
// on ImGui draw lists (imgui_script_canvas.hpp, menu_script_host.hpp), tests implement it with a recorder. The
// primitives are exactly the ones scripts can use, with ImGui's argument order and colour packing, so the ImGui
// implementation is a direct call and adds nothing per primitive but one virtual call.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace overlay {

struct CanvasPoint {
    float x = 0;
    float y = 0;
};

// A colour as scripts give it: red, green, blue and alpha from 0 to 1.
struct ScriptColor {
    float r = 1;
    float g = 1;
    float b = 1;
    float a = 1;
};

// A packed colour, as ImGui stores it (IM_COL32): red in the low byte, alpha in the high byte.
using CanvasColor = uint32_t;

// Same result as ImGui::ColorConvertFloat4ToU32: each channel clamped to 0..1, scaled to 255 and rounded.
inline CanvasColor PackColor(const ScriptColor& color) {
    auto channel = [](float value) {
        const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<uint32_t>(static_cast<int>(clamped * 255.0f + 0.5f));
    };
    return channel(color.r) | channel(color.g) << 8 | channel(color.b) << 16 | channel(color.a) << 24;
}

// The shadow drawn under script text: black at three quarters of the text's alpha.
inline CanvasColor ShadowOf(CanvasColor color) {
    return ((color >> 24) * 3 / 4) << 24;
}

// A colour setting's text, "#RRGGBBAA"; any other length reads as opaque white. The menu reads colours the same way.
inline ScriptColor ParseColorText(const std::string& text) {
    const unsigned value =
            text.size() == 9 ? static_cast<unsigned>(std::strtoul(text.c_str() + 1, nullptr, 16)) : 0xffffffffu;
    return {((value >> 24) & 255) / 255.f,
            ((value >> 16) & 255) / 255.f,
            ((value >> 8) & 255) / 255.f,
            (value & 255) / 255.f};
}

// The text of a colour setting, "#rrggbbaa" (lower case), as the menu writes it.
inline std::string FormatColorText(const ScriptColor& color) {
    char text[16]{};
    std::snprintf(text,
                  sizeof(text),
                  "#%02x%02x%02x%02x",
                  static_cast<int>(std::lround(color.r * 255)),
                  static_cast<int>(std::lround(color.g * 255)),
                  static_cast<int>(std::lround(color.b * 255)),
                  static_cast<int>(std::lround(color.a * 255)));
    return text;
}

// Nickel's text weights (draw.text's font option: Regular, SemiBold, Bold; ExtraBold is drawn Bold).
enum class CanvasFont : uint8_t {
    Regular,
    SemiBold,
    Bold,
};

class ScriptCanvas {
public:
    virtual ~ScriptCanvas() = default;

    virtual void Line(CanvasPoint from, CanvasPoint to, CanvasColor color, float thickness) = 0;

    // Outline of the rectangle from `min` to `max`, square corners.
    virtual void Rect(CanvasPoint min, CanvasPoint max, CanvasColor color, float thickness) = 0;

    virtual void FillRect(CanvasPoint min, CanvasPoint max, CanvasColor color, float rounding) = 0;

    // Corner colours in ImGui's order: top left, top right, bottom right, bottom left.
    virtual void FillGradient(CanvasPoint min,
                              CanvasPoint max,
                              CanvasColor topLeft,
                              CanvasColor topRight,
                              CanvasColor bottomRight,
                              CanvasColor bottomLeft) = 0;

    virtual void Circle(CanvasPoint center, float radius, CanvasColor color, float thickness) = 0;

    virtual void FillCircle(CanvasPoint center, float radius, CanvasColor color) = 0;

    virtual void FillQuad(CanvasPoint a, CanvasPoint b, CanvasPoint c, CanvasPoint d, CanvasColor color) = 0;

    // Text in Nickel's fonts, `size` in menu units; `topLeft` is the top-left corner of the text.
    virtual void Text(CanvasFont font, float size, CanvasPoint topLeft, const char* text, CanvasColor color) = 0;

    // Width of `text` drawn with Text at this size.
    virtual float MeasureText(CanvasFont font, float size, const char* text) = 0;
};

// Where a script HUD element is drawn this frame: its top-left corner and its scale (HUD layout editor).
struct HudFrame {
    float x = 0;
    float y = 0;
    float scale = 1;
};

enum class CanvasLayer : uint8_t {
    World, // render3d: over the game, every frame
    Hud,   // render2d: the HUD, hidden outside play (chat, pause, inventory...)
};

} // namespace overlay
