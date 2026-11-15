// Script drawing without the game (native/script_canvas.hpp, script_draw.hpp, imgui_script_canvas.hpp).
//
// 1. Colours: PackColor gives ImGui::ColorConvertFloat4ToU32's value; colour text is read and written exactly as the
//    menu did before scripts and menu shared one implementation.
// 2. On a recording canvas: text alignment and shadow, world segments, box edges and faces, nothing drawn without a
//    valid camera.
// 3. ImGuiScriptCanvas produces the same vertices, indices and draw commands as the ImDrawList calls the engine made
//    before the canvas existed, and allocates nothing more.
// 4. A timing of ImDrawList calls made directly and through the canvas (printed, not checked: machines differ).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_script_canvas.hpp"
#include "script_draw.hpp"

namespace {

// Heap allocations made by this process: operator new below, and ImGui's own allocator (ImGui::MemAlloc, which draw
// lists use to grow their buffers), counted through ImGui::SetAllocatorFunctions.
size_t allocations = 0;

void* CountingAlloc(size_t size, void*) {
    ++allocations;
    return std::malloc(size);
}

void CountingFree(void* memory, void*) {
    std::free(memory);
}

} // namespace

void* operator new(size_t size) {
    ++allocations;
    if (void* memory = std::malloc(size ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, size_t) noexcept {
    std::free(memory);
}

namespace {

using namespace overlay;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

// ---- 1. colours

// The menu's colour functions before this change (menu.cpp ParseColor / FormatColor).
ImVec4 ReferenceParse(const std::string& text) {
    unsigned v = text.size() == 9 ? (unsigned)strtoul(text.c_str() + 1, nullptr, 16) : 0xffffffffu;
    return ImVec4(((v >> 24) & 255) / 255.f, ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f);
}

std::string ReferenceFormat(ImVec4 c) {
    char text[16]{};
    snprintf(text,
             sizeof(text),
             "#%02x%02x%02x%02x",
             (int)std::lround(c.x * 255),
             (int)std::lround(c.y * 255),
             (int)std::lround(c.z * 255),
             (int)std::lround(c.w * 255));
    return text;
}

void Colours() {
    bool packed = true;
    const float values[] = {-1.f, -0.001f, 0.f, 0.001f, 0.25f, 0.5f, 0.501f, 0.998f, 0.999f, 1.f, 1.001f, 2.f, 300.f};
    for (float r : values) {
        for (float g : values) {
            for (float a : values) {
                const ScriptColor color{r, g, 0.75f - r * 0.1f, a};
                packed &=
                        PackColor(color) == ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a));
            }
        }
    }
    for (int i = 0; i <= 1000; ++i) {
        const float v = i / 1000.f;
        packed &= PackColor({v, v, v, v}) == ImGui::ColorConvertFloat4ToU32(ImVec4(v, v, v, v));
    }
    Check(packed, "PackColor equals ImGui::ColorConvertFloat4ToU32 (clamped, rounded, RGBA in IM_COL32 order)");

    bool shadows = true;
    for (unsigned alpha = 0; alpha < 256; ++alpha) {
        const CanvasColor color = IM_COL32(10, 200, 30, alpha);
        shadows &= ShadowOf(color) == IM_COL32(0, 0, 0, (color >> 24) * 3 / 4);
    }
    Check(shadows, "text shadow: black at 3/4 of the text's alpha, as before");

    bool parsed = true;
    for (const char* text :
         {"#11223344", "#ffffffff", "#00000000", "#FFaa0080", "#123", "", "#zzzzzzzz", "#1122334455"}) {
        const ScriptColor ours = ParseColorText(text);
        const ImVec4 before = ReferenceParse(text);
        parsed &= ours.r == before.x && ours.g == before.y && ours.b == before.z && ours.a == before.w;
    }
    Check(parsed, "colour text is read as before (including invalid text read as white)");

    bool formatted = true;
    for (float v : values) {
        const ScriptColor color{v, 1 - v, v * 0.5f, 0.3f};
        formatted &= FormatColorText(color) == ReferenceFormat(ImVec4(color.r, color.g, color.b, color.a));
    }
    Check(formatted, "colour text is written as before (#rrggbbaa)");
}

// ---- 2. a recording canvas

struct Call {
    std::string what;
    CanvasPoint a, b;
    CanvasColor color = 0;
    float value = 0;
    std::string text;
};

class RecordingCanvas final : public ScriptCanvas {
public:
    std::vector<Call> calls;

    void Line(CanvasPoint from, CanvasPoint to, CanvasColor color, float thickness) override {
        calls.push_back({"line", from, to, color, thickness, ""});
    }

    void Rect(CanvasPoint min, CanvasPoint max, CanvasColor color, float thickness) override {
        calls.push_back({"rect", min, max, color, thickness, ""});
    }

    void FillRect(CanvasPoint min, CanvasPoint max, CanvasColor color, float rounding) override {
        calls.push_back({"fill", min, max, color, rounding, ""});
    }

    void FillGradient(CanvasPoint min, CanvasPoint max, CanvasColor, CanvasColor, CanvasColor, CanvasColor) override {
        calls.push_back({"gradient", min, max, 0, 0, ""});
    }

    void Circle(CanvasPoint center, float radius, CanvasColor color, float) override {
        calls.push_back({"circle", center, {}, color, radius, ""});
    }

    void FillCircle(CanvasPoint center, float radius, CanvasColor color) override {
        calls.push_back({"fillCircle", center, {}, color, radius, ""});
    }

    void FillQuad(CanvasPoint a, CanvasPoint, CanvasPoint, CanvasPoint d, CanvasColor color) override {
        calls.push_back({"quad", a, d, color, 0, ""});
    }

    void Text(CanvasFont font, float size, CanvasPoint topLeft, const char* text, CanvasColor color) override {
        calls.push_back({"text", topLeft, {}, color, size + static_cast<float>(font) * 1000, text});
    }

    float MeasureText(CanvasFont, float size, const char* text) override {
        return size * static_cast<float>(std::strlen(text)); // every character is `size` wide
    }
};

// A camera with w = 1: screen x = width/2 * (1 + x), y = height/2 * (1 - y).
ScriptProjection TestCamera() {
    ScriptProjection camera;
    camera.matrix[0] = 1;
    camera.matrix[5] = 1;
    camera.matrix[15] = 1;
    camera.valid = true;
    camera.width = 200;
    camera.height = 100;
    return camera;
}

void Drawing() {
    RecordingCanvas canvas;
    DrawAlignedText(canvas, FontNamed("Bold"), 10, 100, 20, "abcd", IM_COL32(255, 255, 255, 200), true, "Center");
    Check(canvas.calls.size() == 2 && canvas.calls[0].text == "abcd" && canvas.calls[0].a.x == 81
                  && canvas.calls[0].a.y == 21 && canvas.calls[0].color == IM_COL32(0, 0, 0, 150),
          "draw.text with shadow: shadow first, 1 px down-right, 3/4 alpha");
    Check(canvas.calls[1].a.x == 80 && canvas.calls[1].a.y == 20 && canvas.calls[1].value == 10 + 2000,
          "draw.text align Center: x is the centre (width measured with the same font and size)");
    canvas.calls.clear();
    DrawAlignedText(canvas, FontNamed("Regular"), 10, 100, 20, "ab", 0xffffffff, false, "Right");
    DrawAlignedText(canvas, FontNamed("Nope"), 10, 100, 20, "ab", 0xffffffff, false, "Left");
    Check(canvas.calls.size() == 2 && canvas.calls[0].a.x == 80 && canvas.calls[1].a.x == 100,
          "align Right ends at x, Left starts at x; no shadow when not asked");
    Check(FontNamed("ExtraBold") == CanvasFont::Bold && FontNamed("SemiBold") == CanvasFont::SemiBold
                  && FontNamed("x") == CanvasFont::Regular,
          "font names: ExtraBold is Bold, unknown names are Regular");

    const ScriptProjection camera = TestCamera();
    const float lo[3] = {-0.5f, -0.5f, 0};
    const float hi[3] = {0.5f, 0.5f, 1};
    canvas.calls.clear();
    DrawBoxEdges(canvas, camera, lo, hi, 0xff0000ff, 2);
    Check(canvas.calls.size() == 12, "draw.box: 12 edges when the box is in front of the camera");
    canvas.calls.clear();
    FillBoxFaces(canvas, camera, lo, hi, 0x80ffffff);
    Check(canvas.calls.size() == 6, "draw.filledBox: 6 faces");

    CanvasPoint point;
    const float centre[3] = {0, 0, 0};
    Check(camera.Project(centre, &point) && point.x == 100 && point.y == 50,
          "the projection puts the origin mid-screen");

    ScriptProjection stale = camera;
    stale.valid = false;
    canvas.calls.clear();
    DrawBoxEdges(canvas, stale, lo, hi, 0xffffffff, 1);
    FillBoxFaces(canvas, stale, lo, hi, 0xffffffff);
    DrawSegment(canvas, stale, lo, hi, 0xffffffff, 1);
    Check(canvas.calls.empty() && !stale.Project(centre, &point), "no camera captured: nothing is drawn or projected");

    ScriptProjection behind = camera;
    behind.matrix[15] = 0; // w = 0 for every point: behind the near plane
    canvas.calls.clear();
    DrawBoxEdges(canvas, behind, lo, hi, 0xffffffff, 1);
    Check(canvas.calls.empty(), "points behind the camera are skipped");
}

// ---- 3. ImGuiScriptCanvas against direct ImDrawList calls

// The calls the engine made before the canvas, for one script frame.
void DirectCalls(ImDrawList* d, float t) {
    d->AddRectFilled(ImVec2(10 + t, 10), ImVec2(110, 60), IM_COL32(20, 30, 40, 200), 4);
    d->AddRect(ImVec2(10, 70), ImVec2(110 + t, 120), IM_COL32(200, 30, 40, 255), 0, 0, 2);
    d->AddLine(ImVec2(0, 0), ImVec2(300, 200 + t), IM_COL32(255, 255, 0, 255), 1.5f);
    d->AddCircle(ImVec2(150, 150), 30 + t, IM_COL32(0, 255, 0, 255), 0, 1);
    d->AddCircleFilled(ImVec2(220, 150), 20, IM_COL32(0, 0, 255, 128));
    d->AddRectFilledMultiColor(ImVec2(0, 200),
                               ImVec2(100, 260),
                               IM_COL32(255, 0, 0, 255),
                               IM_COL32(0, 255, 0, 255),
                               IM_COL32(0, 0, 255, 255),
                               IM_COL32(255, 255, 255, 255));
    d->AddQuadFilled(ImVec2(300, 300), ImVec2(340, 300), ImVec2(340, 340), ImVec2(300, 340), IM_COL32(9, 9, 9, 99));
}

void CanvasCalls(ScriptCanvas& c, float t) {
    c.FillRect({10 + t, 10}, {110, 60}, IM_COL32(20, 30, 40, 200), 4);
    c.Rect({10, 70}, {110 + t, 120}, IM_COL32(200, 30, 40, 255), 2);
    c.Line({0, 0}, {300, 200 + t}, IM_COL32(255, 255, 0, 255), 1.5f);
    c.Circle({150, 150}, 30 + t, IM_COL32(0, 255, 0, 255), 1);
    c.FillCircle({220, 150}, 20, IM_COL32(0, 0, 255, 128));
    c.FillGradient({0, 200},
                   {100, 260},
                   IM_COL32(255, 0, 0, 255),
                   IM_COL32(0, 255, 0, 255),
                   IM_COL32(0, 0, 255, 255),
                   IM_COL32(255, 255, 255, 255));
    c.FillQuad({300, 300}, {340, 300}, {340, 340}, {300, 340}, IM_COL32(9, 9, 9, 99));
}

class PlainCanvas final : public ImGuiScriptCanvas {
public:
    void Text(CanvasFont, float, CanvasPoint, const char*, CanvasColor) override {}

    float MeasureText(CanvasFont, float, const char*) override { return 0; }
};

template <class T>
bool SameBuffer(const ImVector<T>& a, const ImVector<T>& b) {
    return a.Size == b.Size && (a.Size == 0 || std::memcmp(a.Data, b.Data, sizeof(T) * a.Size) == 0);
}

bool SameCommands(const ImDrawList& a, const ImDrawList& b) {
    if (a.CmdBuffer.Size != b.CmdBuffer.Size) {
        return false;
    }
    for (int i = 0; i < a.CmdBuffer.Size; ++i) {
        const ImDrawCmd& x = a.CmdBuffer[i];
        const ImDrawCmd& y = b.CmdBuffer[i];
        if (x.ElemCount != y.ElemCount || x.IdxOffset != y.IdxOffset || x.VtxOffset != y.VtxOffset
            || std::memcmp(&x.ClipRect, &y.ClipRect, sizeof(x.ClipRect)) != 0) {
            return false;
        }
    }
    return true;
}

void BeginList(ImDrawList* list, ImTextureID texture) {
    list->_ResetForNewFrame();
    list->PushClipRectFullScreen();
    list->PushTextureID(texture);
}

void ImGuiAdapter() {
    ImGui::SetAllocatorFunctions(CountingAlloc, CountingFree);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();

    ImDrawList direct(ImGui::GetDrawListSharedData());
    ImDrawList adapted(ImGui::GetDrawListSharedData());
    PlainCanvas canvas;
    canvas.list = &adapted;
    bool same = true;
    for (int frame = 0; frame < 3; ++frame) {
        BeginList(&direct, io.Fonts->TexID);
        BeginList(&adapted, io.Fonts->TexID);
        for (int i = 0; i < 50; ++i) {
            DirectCalls(&direct, static_cast<float>(i));
            CanvasCalls(canvas, static_cast<float>(i));
        }
        same &= SameBuffer(direct.VtxBuffer, adapted.VtxBuffer) && SameBuffer(direct.IdxBuffer, adapted.IdxBuffer)
                && SameCommands(direct, adapted);
    }
    Check(same && direct.VtxBuffer.Size > 0,
          "through the canvas: same vertices, indices and draw commands as direct ImDrawList calls");

    // First frame on new lists: the buffers grow the same way through the canvas.
    auto firstFrameAllocations = [&](bool throughCanvas) {
        ImDrawList fresh(ImGui::GetDrawListSharedData());
        PlainCanvas freshCanvas;
        freshCanvas.list = &fresh;
        const size_t before = allocations;
        BeginList(&fresh, io.Fonts->TexID);
        for (int i = 0; i < 50; ++i) {
            if (throughCanvas) {
                CanvasCalls(freshCanvas, static_cast<float>(i));
            } else {
                DirectCalls(&fresh, static_cast<float>(i));
            }
        }
        return allocations - before;
    };
    const size_t directGrowth = firstFrameAllocations(false);
    const size_t canvasGrowth = firstFrameAllocations(true);
    std::printf("     allocations on a new draw list (buffers growing): direct %zu, through the canvas %zu\n",
                directGrowth,
                canvasGrowth);
    Check(directGrowth > 0 && canvasGrowth == directGrowth, "growing a new draw list: the same allocations either way");

    // Steady state (buffers already grown): count heap allocations for one more frame each way.
    auto frameAllocations = [&](bool throughCanvas) {
        BeginList(throughCanvas ? &adapted : &direct, io.Fonts->TexID);
        const size_t before = allocations;
        for (int i = 0; i < 50; ++i) {
            if (throughCanvas) {
                CanvasCalls(canvas, static_cast<float>(i));
            } else {
                DirectCalls(&direct, static_cast<float>(i));
            }
        }
        return allocations - before;
    };
    const size_t directAllocations = frameAllocations(false);
    const size_t canvasAllocations = frameAllocations(true);
    std::printf("     heap allocations for 350 primitives: direct %zu, through the canvas %zu\n",
                directAllocations,
                canvasAllocations);
    Check(canvasAllocations == directAllocations, "the canvas allocates nothing more per frame than direct calls");

    // ---- 4. timing (printed only)
    auto time = [&](bool throughCanvas) {
        const int frames = 400;
        const auto start = std::chrono::steady_clock::now();
        for (int f = 0; f < frames; ++f) {
            BeginList(throughCanvas ? &adapted : &direct, io.Fonts->TexID);
            for (int i = 0; i < 50; ++i) {
                if (throughCanvas) {
                    CanvasCalls(canvas, static_cast<float>(i));
                } else {
                    DirectCalls(&direct, static_cast<float>(i));
                }
            }
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        return ns / (frames * 50.0 * 7.0);
    };
    time(false);
    time(true); // warm both paths
    double directNs = 1e9;
    double canvasNs = 1e9;
    for (int run = 0; run < 5; ++run) {
        directNs = std::min(directNs, time(false));
        canvasNs = std::min(canvasNs, time(true));
    }
    std::printf("     time per primitive (best of 5): direct %.1f ns, through the canvas %.1f ns (%+.1f%%)\n",
                directNs,
                canvasNs,
                (canvasNs / directNs - 1) * 100);

    ImGui::EndFrame();
    ImGui::DestroyContext();
}

} // namespace

int main() {
    Colours();
    Drawing();
    ImGuiAdapter();
    std::printf("%s\n", failed ? "script canvas checks FAILED" : "script canvas checks passed");
    return failed ? 1 : 0;
}
