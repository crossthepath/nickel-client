#pragma once
// The drawing behind the draw.* functions of Luau scripts, on any ScriptCanvas (tests/script_canvas_checks.cpp).
//
// The Luau bindings in script_engine.hpp read the arguments; everything else (text alignment and shadow, projecting
// world points to the screen, box edges and faces) is here, independent of Lua, ImGui and the menu.
#include <string>

#include "../include/nickel/visual_projection.hpp"
#include "script_canvas.hpp"

namespace overlay {

// The camera of the current frame, for render3d and draw.worldToScreen (the ESP modules use the same projection).
struct ScriptProjection {
    float matrix[16]{};
    float origin[3]{};
    bool valid = false; // a fresh camera matrix was captured (less than 250 ms old)
    float width = 0;
    float height = 0;

    bool Project(const float point[3], CanvasPoint* out) const {
        if (!valid) {
            return false;
        }
        nickel::visual::Point screen{};
        if (!nickel::visual::project(matrix, origin, point, width, height, &screen)) {
            return false;
        }
        *out = {screen.x, screen.y};
        return true;
    }
};

// draw.text's font option.
inline CanvasFont FontNamed(const std::string& name) {
    if (name == "Bold" || name == "ExtraBold") {
        return CanvasFont::Bold;
    }
    return name == "SemiBold" ? CanvasFont::SemiBold : CanvasFont::Regular;
}

// draw.text: `x` is the left edge, the centre or the right edge for align Left, Center or Right; an optional shadow
// one pixel down and right.
inline void DrawAlignedText(ScriptCanvas& canvas,
                            CanvasFont font,
                            float size,
                            float x,
                            float y,
                            const char* text,
                            CanvasColor color,
                            bool shadow,
                            const std::string& align) {
    const float width = canvas.MeasureText(font, size, text);
    const float left = align == "Center" ? x - width / 2 : (align == "Right" ? x - width : x);
    if (shadow) {
        canvas.Text(font, size, {left + 1, y + 1}, text, ShadowOf(color));
    }
    canvas.Text(font, size, {left, y}, text, color);
}

// A world segment, drawn when both ends are in front of the camera (no depth test: visible through walls).
inline void DrawSegment(ScriptCanvas& canvas,
                        const ScriptProjection& camera,
                        const float from[3],
                        const float to[3],
                        CanvasColor color,
                        float thickness) {
    CanvasPoint a;
    CanvasPoint b;
    if (camera.Project(from, &a) && camera.Project(to, &b)) {
        canvas.Line(a, b, color, thickness);
    }
}

// The 8 corners of the box from `lo` to `hi`; corner i takes hi on axis k when bit k of i is set.
inline void BoxCorners(const float lo[3], const float hi[3], float corners[8][3]) {
    for (int i = 0; i < 8; ++i) {
        corners[i][0] = (i & 1) ? hi[0] : lo[0];
        corners[i][1] = (i & 2) ? hi[1] : lo[1];
        corners[i][2] = (i & 4) ? hi[2] : lo[2];
    }
}

// draw.box and draw.entityBox: the 12 edges.
inline void DrawBoxEdges(ScriptCanvas& canvas,
                         const ScriptProjection& camera,
                         const float lo[3],
                         const float hi[3],
                         CanvasColor color,
                         float thickness) {
    float corners[8][3];
    BoxCorners(lo, hi, corners);
    static const int kEdges[12][2] = {
            {0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : kEdges) {
        DrawSegment(canvas, camera, corners[edge[0]], corners[edge[1]], color, thickness);
    }
}

// draw.filledBox: the 6 faces, each drawn when its 4 corners are in front of the camera.
inline void FillBoxFaces(
        ScriptCanvas& canvas, const ScriptProjection& camera, const float lo[3], const float hi[3], CanvasColor color) {
    float corners[8][3];
    BoxCorners(lo, hi, corners);
    static const int kFaces[6][4] = {
            {0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (const auto& face : kFaces) {
        CanvasPoint q[4];
        bool visible = true;
        for (int i = 0; i < 4; ++i) {
            visible = visible && camera.Project(corners[face[i]], &q[i]);
        }
        if (visible) {
            canvas.FillQuad(q[0], q[1], q[2], q[3], color);
        }
    }
}

} // namespace overlay
