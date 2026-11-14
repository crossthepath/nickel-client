#pragma once
#include <Windows.h>
#include <imgui.h>
#include <atomic>
namespace overlay {
void InitializeMenu(HMODULE module);
void SetWindow(HWND hwnd);   // game window, used to open the full-screen map from a key
void DrawMenu(float progress,ImTextureID logo);
void ToggleMenu(HWND hwnd);
bool MenuIsOpen();
void NoteGameCursor(bool lock);   // the game asked to lock (ClipCursor area, SetCursorPos) or release the cursor while the menu is open
bool Playing();   // the game owns the mouse (in the world, no game screen with a cursor)
bool CanToggleNow();   // false when another game UI is open and "Open in menus" is off
void CloseMenu(HWND hwnd);
float MenuProgress(float dt);
bool FontsRebuild();   // renderer, before NewFrame: true when sizes were added and the font texture must be recreated
bool RecordingKey();
bool MenuKeyMatches(unsigned long long wparam,long long lparam);   // the key message is the menu key (Right Shift by default)
void HandleKey(unsigned vk,long long lparam);
// Provided by the renderer: adds the game blur to a draw list and returns the blurred texture (0 = unavailable)
extern ImTextureID (*RequestBlur)(ImDrawList*);
extern ImTextureID (*RequestMotionBlur)(ImDrawList*,const float*,float,int,float);
// Provided by the renderer: one line in overlay.log (nullptr until the renderer is ready)
extern void (*WriteLog)(const char*);
// Provided by the renderer: dynamic HUD images (0 Target HUD head, 1 album art, 2 MiniMap terrain,
// 3 full-screen map, 4 faces); 0 = unavailable
extern ImTextureID (*UpdateImage)(int slot,const unsigned char* rgba,int width,int height);
extern ImTextureID (*GetImage)(int slot);
extern const char* backendName;
// Vsync Disabler: read by the renderer's Present hook
extern std::atomic<bool> vsyncDisabled;
// Mouse buttons forced by click modules: press or release for a duration (ns, 0 = a single pass);
// SyntheticButtons is applied by the renderer's GetMouseState hook.
void ForceButton(int button,bool press,long long durationNs);
void ClearButton(int button);
unsigned SyntheticButtons(unsigned buttons);
void RecordClick(int button);   // counted by the CPS HUD
int TakeSyntheticClicks(int button);
extern std::atomic<bool> streamHide;   // Stream Hide: the UI is drawn in a window excluded from screen capture
}
