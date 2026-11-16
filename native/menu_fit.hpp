#pragma once
// How large the Nickel menu is drawn in the game window, and where: one pure function, no ImGui, no Windows API,
// checked by tests/menu_fit_checks.cpp. menu.cpp calls FitMenu once per frame; everything the menu draws (the menu,
// its side panels, the item profile panel, the Nickel AI bubble and chat window) and every hit area uses the scale it
// returns.
//
// The player's own size, scale and place (dragged edges, corner and title bar, kept in module-preferences.ini) are the
// input and are never changed here: a small window only changes what is drawn this frame, so the menu is as the player
// left it again once the window is large enough.
//
// The rule:
// 1. Base scale: the default menu with its side panel (1012 + 19 + 297 = 1328 x 578 units) takes at most 72 % of the
//    window's width and height; never above 1, so a window of 1920 x 1080 or larger draws the menu as before.
// 2. Scale = base x the player's scale, never below kMinReadableScale (text stays readable).
// 3. Everything must fit inside the window with an 8 px margin: a menu the player made larger than the default gives
//    back its extra width or height first, then the scale goes down to kMinReadableScale, then the size goes down to
//    the menu's minimum (820 x 430), and only a window too small even for that takes the scale lower.
// 4. Place: centred, or the player's place kept inside the margin; a side panel that would leave the window moves the
//    menu left (not while the player drags it).

#include <algorithm>
#include <cmath>

namespace nickel::menu {

constexpr float kDefaultW = 1012, kDefaultH = 578;   // units (pixels at scale 1)
constexpr float kMinW = 820, kMinH = 430, kMaxW = 1600, kMaxH = 1000;
constexpr float kPanelGap = 19, kPanelW = 297;       // a side panel right of the menu
constexpr float kMargin = 8;                         // screen pixels kept free around the menu
constexpr float kOccupancy = .72f;                   // of the window, for the default menu and its side panel
constexpr float kMinReadableScale = .7f;             // 13.5 u body text stays above 9 px
constexpr float kMinScale = .6f, kMaxScale = 1.8f;   // the player's own scale (corner drag)

struct FitInput {
    float screenW = 0, screenH = 0;
    float wantW = kDefaultW, wantH = kDefaultH, scale = 1; // the player's size (units) and scale
    float x = -1, y = -1;                                  // the player's place (screen pixels), -1: centred
    bool panel = false;                                    // a side panel shows right of the menu
    bool dragging = false;                                 // the player is moving or resizing the menu
};

struct MenuFit {
    float base = 1;  // the window's own scale (1 when large enough), before the player's scale
    float s = 1;     // pixels per unit for everything the menu draws
    float w = kDefaultW, h = kDefaultH; // units drawn this frame
    float x = 0, y = 0;                 // top-left corner (screen pixels)
};

// Step 1: the window's scale for the default menu with its side panel.
inline float BaseScale(float screenW, float screenH) {
    if (screenW <= 0 || screenH <= 0) {
        return 0;
    }
    return std::min({1.f, kOccupancy * screenW / (kDefaultW + kPanelGap + kPanelW), kOccupancy * screenH / kDefaultH});
}

inline MenuFit FitMenu(const FitInput& in) {
    MenuFit out;
    out.base = BaseScale(in.screenW, in.screenH);
    if (out.base <= 0) {
        out.s = 0;
        return out;
    }
    const float extra = in.panel ? kPanelGap + kPanelW : 0.f;
    const float roomW = std::max(1.f, in.screenW - 2 * kMargin), roomH = std::max(1.f, in.screenH - 2 * kMargin);
    float s = std::max(kMinReadableScale, out.base * std::clamp(in.scale, kMinScale, kMaxScale));
    float w = std::clamp(in.wantW, kMinW, kMaxW), h = std::clamp(in.wantH, kMinH, kMaxH);
    // 3a. the player's extra size first (not below the default)
    w = std::min(w, std::max(std::min(w, kDefaultW), roomW / s - extra));
    h = std::min(h, std::max(std::min(h, kDefaultH), roomH / s));
    // 3b. then the scale, down to the readable minimum
    s = std::min(s, std::max(kMinReadableScale, std::min(roomW / (w + extra), roomH / h)));
    // 3c. then the size, down to the menu's minimum
    w = std::min(w, std::max(kMinW, roomW / s - extra));
    h = std::min(h, std::max(kMinH, roomH / s));
    // 3d. a window too small for that: the scale goes lower
    s = std::min({s, roomW / (w + extra), roomH / h});
    out.s = s;
    out.w = w;
    out.h = h;
    // 4. place
    out.x = std::floor((in.screenW - w * s) / 2);
    out.y = std::floor((in.screenH - h * s) / 2);
    if (in.x >= 0 && in.y >= 0) {
        out.x = std::floor(std::clamp(in.x, kMargin, std::max(kMargin, in.screenW - kMargin - w * s)));
        out.y = std::floor(std::clamp(in.y, kMargin, std::max(kMargin, in.screenH - kMargin - h * s)));
    }
    if (in.panel && !in.dragging && out.x + (w + extra) * s > in.screenW - kMargin) {
        out.x = std::max(kMargin, std::floor(in.screenW - kMargin - (w + extra) * s));
    }
    return out;
}

} // namespace nickel::menu
