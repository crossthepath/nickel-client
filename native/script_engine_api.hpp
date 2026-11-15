#pragma once
// What the rest of the overlay may use of the Luau engine (script_engine.hpp, script_api21.hpp).
//
// menu.cpp includes this header inside its anonymous namespace, before any use; the definitions follow in
// script_engine.hpp. The menu, the HUD and the game hooks call only these functions: they never read the engine's
// script list, its lock or its flags directly (tools/check_boundaries.py fails the build if they start to).
// The Scripting tab (script_ui.hpp) still reads the engine's state directly; it moves behind this interface later.

using ScriptEvent = nickel::script::Event;
using ScriptEventMask = nickel::script::EventMask;

// ---- state, for wiring the game hooks (render thread, every frame)

// True when at least one script is running.
bool ScriptsAnyRunning();

// The events that at least one enabled module of a running script handles (bits from nickel::script::Bit).
ScriptEventMask ScriptsWantedEvents();

// True when an enabled module of a running script handles `event`.
bool ScriptsWant(ScriptEvent event);

// True once a script module has submitted a rotation (the shared rotation manager must stay attached).
bool ScriptsWantRotations();

// True when an enabled module handles render2d or render3d during the last frame (world projection needed).
bool ScriptsWantCamera();

// ---- entry points, called by the menu and by the game hooks

// A key pressed with the menu closed (render thread).
void ScriptsKey(unsigned vk);

// Toggles, timers, update, render2d and render3d; adds the ids of the script HUD elements drawn to `drawn`
// (render thread).
void ScriptsFrame(std::vector<std::string>& drawn);

// Game tick: queued messages and player writes, then the tick event (game thread).
void ScriptsTick();

// Outgoing and incoming packets; true cancels the packet (game thread).
bool ScriptsSend(uintptr_t packet);
bool ScriptsReceive(uintptr_t packet);

// The local player attacks `target`; true cancels the attack (game thread).
bool ScriptsAttack(uintptr_t local, uintptr_t target);

// Jump and movement hooks (game thread).
void ScriptsJumpPre(uint64_t arg7, uint64_t arg8);
void ScriptsJumpPost(uint64_t arg8);
bool ScriptsJumpCancel();
void ScriptsMoveGuarded(uint64_t input, uint64_t state, uint64_t movement);
