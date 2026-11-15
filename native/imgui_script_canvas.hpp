#pragma once
// ScriptCanvas on an ImGui draw list (tests/script_canvas_checks.cpp compares it with direct ImDrawList calls).
//
// Each primitive is the ImDrawList call the engine made before the canvas existed, with the same arguments, so script
// drawing produces the same vertices. Text needs Nickel's fonts: the menu's subclass provides it
// (menu_script_host.hpp).
#include "imgui.h"
#include "script_canvas.hpp"

namespace overlay {

class ImGuiScriptCanvas : public ScriptCanvas {
public:
    // The draw list of the current frame; the host sets it before handing the canvas to the engine.
    ImDrawList* list = nullptr;

    void Line(CanvasPoint from, CanvasPoint to, CanvasColor color, float thickness) override {
        list->AddLine(Vec(from), Vec(to), color, thickness);
    }

    void Rect(CanvasPoint min, CanvasPoint max, CanvasColor color, float thickness) override {
        list->AddRect(Vec(min), Vec(max), color, 0, 0, thickness);
    }

    void FillRect(CanvasPoint min, CanvasPoint max, CanvasColor color, float rounding) override {
        list->AddRectFilled(Vec(min), Vec(max), color, rounding);
    }

    void FillGradient(CanvasPoint min,
                      CanvasPoint max,
                      CanvasColor topLeft,
                      CanvasColor topRight,
                      CanvasColor bottomRight,
                      CanvasColor bottomLeft) override {
        list->AddRectFilledMultiColor(Vec(min), Vec(max), topLeft, topRight, bottomRight, bottomLeft);
    }

    void Circle(CanvasPoint center, float radius, CanvasColor color, float thickness) override {
        list->AddCircle(Vec(center), radius, color, 0, thickness);
    }

    void FillCircle(CanvasPoint center, float radius, CanvasColor color) override {
        list->AddCircleFilled(Vec(center), radius, color);
    }

    void FillQuad(CanvasPoint a, CanvasPoint b, CanvasPoint c, CanvasPoint d, CanvasColor color) override {
        list->AddQuadFilled(Vec(a), Vec(b), Vec(c), Vec(d), color);
    }

protected:
    static ImVec2 Vec(CanvasPoint point) { return ImVec2(point.x, point.y); }
};

} // namespace overlay
