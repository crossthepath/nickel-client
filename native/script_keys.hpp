#pragma once
// client.isKeyDown for scripts (no game access: tests/script_keys_checks.cpp).
#include <windows.h>

// The state of key `code` from `state` (GetAsyncKeyState in the game), reported only while Minecraft's top-level
// window `gameRoot` is the foreground window: GetAsyncKeyState reads the whole keyboard, so without this a script
// could read keys typed in other applications. Unknown game window: nothing is reported. Same focus rule as the HUD
// (UpdateHudShown in menu.cpp).
inline bool ScriptKeyDown(int code,HWND foreground,HWND gameRoot,SHORT(WINAPI* state)(int)){
 if(!code||!gameRoot||foreground!=gameRoot)return false;
 return (state(code)&0x8000)!=0;}
