// Shared ABI between Nickel.Overlay.dll and our own-process probe executable.
// Plain POD only; bump the version on any layout change.
#pragma once
#include <Windows.h>
struct NickelProbeState {
 DWORD size;
 DWORD version;
 BOOL rendererReady;
 BOOL menuOpen;
 ULONGLONG submittedFrames;
};
constexpr DWORD NickelProbeStateVersion=1;
static_assert(sizeof(NickelProbeState)==24,"NickelProbeState layout changed");
