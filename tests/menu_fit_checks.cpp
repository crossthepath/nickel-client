// The menu's scale, size and place for the game window's size (native/menu_fit.hpp), without the game: the four
// window sizes players use (1920 x 1080, 1600 x 900, 1280 x 720, 1024 x 576), the look kept from 1920 x 1080 up,
// readable text, the menu with its side panel always inside the window, and the player's own size, scale and place
// kept.
#include <cmath>
#include <cstdio>

#include "menu_fit.hpp"

namespace {

using namespace nickel::menu;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

bool Near(float a, float b, float within = .002f) {
    return a > b - within && a < b + within;
}

// The menu and, when shown, its side panel stay inside the window with the 8 px margin.
bool Inside(const FitInput& in, const MenuFit& fit) {
    const float right = fit.x + (fit.w + (in.panel ? kPanelGap + kPanelW : 0.f)) * fit.s;
    return fit.x >= kMargin - .01f && fit.y >= kMargin - .01f && right <= in.screenW - kMargin + .01f
           && fit.y + fit.h * fit.s <= in.screenH - kMargin + .01f;
}

FitInput Window(float w, float h, bool panel = true) {
    FitInput in;
    in.screenW = w;
    in.screenH = h;
    in.panel = panel;
    return in;
}

void FourWindowSizes() {
    struct Case {
        float w, h, s;
    };
    // base = min(1, .72 x width / 1328, .72 x height / 578), never below 0.7
    const Case cases[] = {{1920, 1080, 1.f}, {1600, 900, .72f * 1600 / 1328}, {1280, 720, .7f}, {1024, 576, .7f}};
    for (const Case& c : cases) {
        for (bool panel : {true, false}) {
            const FitInput in = Window(c.w, c.h, panel);
            const MenuFit fit = FitMenu(in);
            char what[160];
            std::snprintf(what, sizeof(what), "%.0f x %.0f%s: scale %.3f, default size, inside the window", c.w, c.h,
                          panel ? " with a side panel" : "", fit.s);
            Check(Near(fit.s, c.s) && Near(fit.w, kDefaultW) && Near(fit.h, kDefaultH) && Inside(in, fit), what);
        }
    }
    const MenuFit large = FitMenu(Window(1920, 1080, false));
    Check(Near(large.s, 1) && Near(large.x, 454) && Near(large.y, 251),
          "1920 x 1080: drawn exactly as before (scale 1, 1012 x 578, centred at 454, 251)");
    Check(Near(FitMenu(Window(2560, 1440)).s, 1) && Near(FitMenu(Window(3840, 2160)).s, 1),
          "a larger window never enlarges the menu");
    Check(Near(FitMenu(Window(1920, 1009)).s, 1), "a maximised 1920 x 1080 game window (1009 high) keeps scale 1");
    const MenuFit at720 = FitMenu(Window(1280, 720));
    Check(at720.s * 13.5f >= 9.4f && at720.s * 10.5f >= 7.3f,
          "1280 x 720: body text 9.45 px, captions 7.35 px (the readable minimum, 0.7)");
    Check(at720.x + (kDefaultW + kPanelGap + kPanelW) * at720.s <= 1280 - kMargin,
          "1280 x 720: the side panel fits right of the menu (it used to overflow at scale 1)");
}

void PlayerSizeAndScaleKept() {
    // a menu made wider and taller by the player stays so where it fits
    FitInput in = Window(1920, 1080);
    in.wantW = 1400;
    in.wantH = 800;
    MenuFit fit = FitMenu(in);
    Check(Near(fit.w, 1400) && Near(fit.h, 800) && Near(fit.s, 1) && Inside(in, fit),
          "1920 x 1080: the player's larger menu is kept");
    // in a small window a menu larger than the default gives back its extra size before the scale goes down
    in.screenW = 1280;
    in.screenH = 720;
    in.wantW = 1600;
    in.wantH = 1000;
    fit = FitMenu(in);
    Check(Inside(in, fit) && Near(fit.s, .7f) && fit.w < 1600 && fit.w > kDefaultW,
          "1280 x 720: the widest menu gives back width first, text stays at 0.7");
    // the player's scale multiplies the window's, within the readable minimum and the window
    in = Window(1920, 1080);
    in.scale = 1.3f;
    fit = FitMenu(in);
    Check(Near(fit.s, 1.3f) && Inside(in, fit), "1920 x 1080: a player scale of 1.3 is kept");
    in.scale = .6f;
    Check(Near(FitMenu(in).s, kMinReadableScale), "a player scale under 0.7 is drawn at 0.7");
    in = Window(1600, 900);
    in.scale = 1.8f;
    fit = FitMenu(in);
    Check(Inside(in, fit) && fit.s > 1 && fit.s < 1.8f * fit.base,
          "1600 x 900: a large player scale stops at the window");
    // the input is never changed: a later, larger window shows the player's menu again
    in = Window(1024, 576, false);
    in.wantW = 1500;
    in.wantH = 900;
    in.scale = 1.5f;
    const FitInput copy = in;
    fit = FitMenu(in);
    Check(Inside(in, fit) && Near(copy.wantW, in.wantW) && Near(copy.scale, in.scale),
          "1024 x 576: fitted for this frame only");
    in.screenW = 2560;
    in.screenH = 1440;
    fit = FitMenu(in);
    Check(Near(fit.w, 1500) && Near(fit.h, 900) && Near(fit.s, 1.5f), "2560 x 1440: the player's menu is back as set");
}

void Place() {
    FitInput in = Window(1600, 900, false);
    MenuFit fit = FitMenu(in);
    Check(Near(fit.x, std::floor((1600 - kDefaultW * fit.s) / 2))
              && Near(fit.y, std::floor((900 - kDefaultH * fit.s) / 2)),
          "centred while never moved");
    in.x = 1500;
    in.y = 5;
    fit = FitMenu(in);
    Check(Inside(in, fit) && Near(fit.y, kMargin), "the player's place is kept inside the margin");
    in.panel = true;
    fit = FitMenu(in);
    Check(Inside(in, fit), "a side panel moves the menu left so that it fits");
    in.dragging = true;
    fit = FitMenu(in);
    Check(fit.x + kDefaultW * fit.s <= 1600 - kMargin + .01f,
          "not while the player drags the menu (only the menu is kept in)");
    // a window too small even for the smallest menu at 0.7: the scale goes lower, everything still fits
    in = Window(800, 600);
    fit = FitMenu(in);
    Check(Inside(in, fit) && fit.s < kMinReadableScale && Near(fit.w, kMinW),
          "800 x 600: below 0.7 only when nothing else fits");
    Check(FitMenu(Window(0, 0)).s == 0, "no window size yet: nothing is drawn");
}

} // namespace

int main() {
    FourWindowSizes();
    PlayerSizeAndScaleKept();
    Place();
    std::printf("%s: %d failed\n", failed == 0 ? "PASS" : "FAIL", failed);
    return failed == 0 ? 0 : 1;
}
