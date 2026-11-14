// Movement group D: Knockback Desync, Auto Jump Reset, Look TP, Auto MLG.
//
// Knockback Desync (incoming packets, GameUpdate):
// - Packets are held in "lots" (a FIFO of lots; each lot = held packets + held NetworkStackLatency requests + release
//   instant + a "latency captured" flag). Receive hook (shared_ptr<Packet>&, the packet is kept by reference counting):
//   * SetActorMotion (0x28) aimed at the local player: no lot, or the last lot already captured its latency -> the
//     packet is cancelled and a new lot is opened (release = now + Delay ms). Otherwise (open lot): with Safe Mode off,
//     Chance on and a random 0..99 below Chance Amount the packet passes, else it joins the last lot.
//   * SetActorMotion of another entity: held only in Safe Mode and with a lot present.
//   * NetworkStackLatency (0x73) asking for an answer (+0x38 == 1) with a lot present: without Safe Mode a lot that
//     already captured one lets it pass; otherwise it is cancelled, its stamp (+0x30) and +0x40 are remembered in the
//     last lot, which becomes "captured".
//   * MoveActorAbsolute (0x12), MovePlayer (0x13), UpdateAttributes (0x1d), UpdatePlayerGameType (0x97), UpdateAbilities
//     (0xbb) and, in Safe Mode only, MoveActorDelta (0x6f) join the last lot. Without Safe Mode nothing is held once the
//     last lot captured its latency, and MoveActorAbsolute is held only when it targets the local player.
// - Release (GameUpdate, with a local player): the front lots whose instant passed are replayed in order through the local
//   receive path (ReceivePacket, bypass flag so the hook does not hold them again), then every remembered latency
//   request is answered with a new NetworkStackLatency { stamp, answer = 0, +0x40 } sent to the server.
//   Without a local player the lots are dropped. Enabling drops the lots; disabling releases everything, then drops.
//
// Auto Jump Reset (incoming packets, GameUpdate):
// - Incoming SetActorMotion (0x28, motion at +0x38) or ActorEvent (0x1b, event byte +0x38 == 2 = hurt) naming the local
//   player (runtime id +0x30), with a local player that has external input data. Both instants are remembered; when a
//   motion and a hurt event were both received within 100 ms the pair is consumed and the trigger runs:
//   1. nothing if a jump is already pending; the player must move horizontally (mx^2 + mz^2 >= 0.01), stand on the ground,
//      not be in water or lava, and the block at the feet must not be a ladder / vine family block nor a cobweb;
//   2. without Face Knockback the motion must push backwards (sin(yaw) * mx > cos(yaw) * mz, body yaw);
//   3. Require Target: an entity under the crosshair;
//   4. Second Hit Only: the first hit only arms (instant stored); a second hit within 1000 ms passes (disarm); a hit
//      later than that re-arms;
//   5. Chance: random 0..99 below Chance;
//   6. Delay (random in [min, max] ms): <= 0 jumps now, otherwise the jump is done on a later GameUpdate.
// - Jump = a jump request (grounded only). With Face Knockback the yaw atan2(mx, -mz) is requested from the rotation
//   manager (mode 2, priority 3, current pitch) and the request is renewed on every GameUpdate for 100 ms.
//
// Look TP (one-shot action):
// - Ray from the player position along the head yaw / pitch for Range blocks, through the world raycast of the block source
//   (BlockSource vtable 0x198; flags false, false, maximum distance = (int)Range, accept-all filter). When it hits a
//   block, the block above the hit block must be air or replaceable; the player is then moved to the centre of that cell
//   (x + 0.5, y + height offset, z + 0.5): the position and previous position of the position state are written, the
//   velocity zeroed and the collision box translated by the same difference. Nothing is sent by the module; the move
//   reaches the server with the next PlayerAuthInput. The module turns itself off afterwards.
//
// Auto MLG (LevelTick of the client level, action sequences, rotation manager):
// - Nothing is done while: no external input data, on the ground, No Fall enabled, Creative / Spectator, flying or allowed
//   to fly, fall-distance component byte +4 set, in water or lava, gliding or riding, Slow Falling or Levitation active,
//   or the block at the feet already cushions (a liquid, cobweb, powder snow, scaffolding, vine, ladder, sweet berry bush).
// - The fall is run through the movement simulator (game_movement_sim.hpp) for up to 200 ticks, keeping every simulated
//   feet position; a failed tick drops it. Landing = first simulated tick ending on the ground; landing block = the
//   simulator's supporting block, else the floor of the position; the block above it must not cushion. Fall damage =
//   ceil((fall - 3 - JumpBoost) * m) scaled by (1 - 0.2 * Resistance, clamped 0..1) and (1 - 0.04 * min(3 * FeatherFalling
//   + sum of Protection, 20)), with m = 0.5 on a bed, 1 / 0 on slime (sneaking / not), 0.2 on hay or honey, else 1.
//   Trigger = Lethal Only (damage >= health + absorption) or Min Damage (damage >= setting).
// - Rescue item: Water Bucket (not in the Nether) then Cobweb ("web" / "cobweb"), in the hotbar. Water: top face of the
//   landing block (the cell above must be air). Cobweb planner: from the second-to-last simulated position back to the
//   fifth-to-last, the feet cell then the head cell; an air cell clear of the player, touched by a later simulated
//   position (not the last), gets the closest solid neighbour among down / north / south / west / east (its face toward the
//   cell); usable when the cell is clear of the player and the face centre is within reach, or within reach (eyes 1.62
//   above) from an earlier simulated position whose box is clear of the cell.
//   Pull From Inventory (no hotbar plan): a plan must exist for the inventory item; the stack (slot >= 9) is moved through
//   the inventory scheduler to the first empty hotbar slot, else to the last hotbar slot that is not selected; the rescue
//   is planned on a later tick from the hotbar.
//   Sequence (8000 ms): select the item (silent swap with Silent), wait until airborne, the face centre within reach, the
//   target cell clear of the player (side faces) and the rotation reached, check the item, use it on the planned face (click-on-block transaction), phase 2, switch back.
//   While planned (phase 1) the view is aimed at the face every tick and the plan is kept only while the evaluation still
//   wants a rescue, the item is in the hotbar, the target cell is air and (cobweb) the cell is inside the player or touched
//   by a later simulated position / (water) the same plan is found again.
// - Phase 2: while airborne after placing nothing happens until 3000 ms elapsed (then reset); on the ground or in water,
//   with a water bucket and Pick Up Water, after 100 ms the placed water is picked up with an empty bucket (phase 3: the
//   view is turned toward the water cell, bucket selected, used); otherwise the module resets.
// - Aim: yaw = atan2(-dx, dz), pitch = -atan2(dy, horizontal distance), rotation request mode Silent ? 2 : 0, priority 3,
//   maximum step = Aim Speed / 100 * 90 degrees.
// - Highlight: wireframe box of the rescue cell (target + face direction) in Highlight Color while a rescue is planned.
// Game offsets: actor+0x240 position state (x, y, z at +0, velocity at +0x18), actor+0x248 box (6 floats), actor+0x250
// rotation (pitch, yaw), actor+0x200 level.
#pragma once
#include "game_actions.hpp"
#include "game_auto_anchor.hpp"
#include "game_combat.hpp"
#include "game_inventory.hpp"
#include "game_lag.hpp"
#include "game_movement_sim.hpp"
#include "game_map.hpp"
#include "game_packets.hpp"
#include "game_player.hpp"
#include "game_rotation.hpp"
#include "game_sticky_aim.hpp"
#include "game_world.hpp"
#include "game_xray.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace game {
constexpr int kRotMovementD = 18;      // rotation manager module id (Auto Jump Reset and Auto MLG)
constexpr int kActionAutoMlg = 17;     // action planner module id (Auto MLG)

// ---- shared helpers
inline int MdRandomPercent() {   // uniform 0..99
    static std::mt19937_64 gen{std::random_device{}()};
    return (int)(gen() % 100);
}
inline bool MdHasComponent(const Context& c, uint32_t hash) {
    uintptr_t storage{};
    uint32_t packed{};
    return Storage(c.registry, hash, &storage) && Packed(storage, c.entity, &packed);
}
inline bool MdInWaterOrLava(const Context& c) { return MdHasComponent(c, 0x78e89f39u) || MdHasComponent(c, 0x832a2768u); }
inline bool MdInWater(const Context& c) { return MdHasComponent(c, 0x78e89f39u); }
inline bool MdInLava(const Context& c) { return MdHasComponent(c, 0x832a2768u); }
inline bool MdBlockName(uint64_t block, std::string* name) { return block && XrayBlockName((uintptr_t)block, name); }
inline bool MdContains(const std::string& hay, const char* part) { return hay.find(part) != std::string::npos; }
// Block at the feet of the player (floor of the feet position).
inline uint64_t MdFeetBlock(const Context& c, uint64_t region, int32_t* cell) {
    float feet[3]{};
    if (!ActorFeet(c.actor, feet)) return 0;
    cell[0] = (int32_t)std::floor(feet[0]);
    cell[1] = (int32_t)std::floor(feet[1]);
    cell[2] = (int32_t)std::floor(feet[2]);
    return RegionBlock(c, region, cell);
}
// Rotation request toward the given yaw / pitch (modes as in the rotation manager).
inline void MdSubmitRotation(int mode, float yaw, float pitch) {
    RotRequest r;
    r.module = kRotMovementD;
    r.mode = mode;
    r.priority = 3;
    r.yaw = yaw;
    r.pitch = pitch;
    RotSubmit(std::move(r));
}

// ============================================================ Knockback Desync
struct KdLatency { uint64_t stamp = 0; uint32_t extra = 0; };
struct KdLot {
    std::vector<GamePacket> packets;
    std::vector<KdLatency> latency;
    int64_t release = 0;
    bool captured = false;
};
struct KnockbackDesyncShared {
    std::atomic<bool> enabled{false}, safe{false}, chance{false}, resetPending{false}, releasePending{false}, fault{false};
    std::atomic<int> delay{500}, amount{100};
    std::recursive_mutex guard;
    std::deque<KdLot> lots;
    bool replaying = false;
};
inline KnockbackDesyncShared& KnockbackDesync() { static auto* s = new KnockbackDesyncShared; return *s; }

inline void KdClear(KnockbackDesyncShared& s) {   // drops everything without replaying
    for (auto& lot : s.lots) for (auto& p : lot.packets) ReleasePacket(p);
    s.lots.clear();
}
// Copies the receive hook's shared_ptr<Packet>& into the lot (the caller then cancels the packet).
inline bool KdHold(KdLot& lot, void* shared) {
    uint64_t object{}, control{};
    if (!Read((uintptr_t)shared, &object) || !Read((uintptr_t)shared + 8, &control) || !object || !control) return false;
    InterlockedIncrement(reinterpret_cast<volatile long*>(control) + 2);
    GamePacket p;
    p.object = object;
    p.control = control;
    lot.packets.push_back(p);
    return true;
}
inline void KdAnswerLatency(const Context& c, const KdLatency& l) {
    GamePacket p = CreatePacket(c, 0x73);
    if (p.object) {
        *reinterpret_cast<uint64_t*>(p.object + 0x30) = l.stamp;
        *reinterpret_cast<uint8_t*>(p.object + 0x38) = 0;
        *reinterpret_cast<uint32_t*>(p.object + 0x40) = l.extra;
        SendPacket(c, p.object);
    }
    ReleasePacket(p);
}
// Releases the front lots (all of them with 'all'): packets replayed first, then latency answers.
inline void KdRelease(KnockbackDesyncShared& s, bool all) {
    Context c{};
    const bool have = Client(&c);
    const int64_t now = LagNs();
    while (!s.lots.empty()) {
        if (!all && now < s.lots.front().release) return;
        KdLot lot = std::move(s.lots.front());
        s.lots.pop_front();
        for (auto& p : lot.packets) {
            if (have) {
                s.replaying = true;
                ReceivePacket(c, &p);
                s.replaying = false;
            }
            ReleasePacket(p);
        }
        if (have) for (auto& l : lot.latency) KdAnswerLatency(c, l);
    }
}
inline bool KdReceiveWork(void* shared) {
    auto& s = KnockbackDesync();
    if (!s.enabled) return false;
    uint64_t object{};
    if (!Read((uintptr_t)shared, &object) || !object) return false;
    std::lock_guard<std::recursive_mutex> lock(s.guard);
    if (s.replaying) return false;
    Context c{};
    if (!Local(&c)) { KdClear(s); return false; }
    const bool safe = s.safe;
    const int id = PacketIdOf((uintptr_t)object);
    uint64_t mine{};
    const bool haveMine = RuntimeId(c.actor, &mine);
    if (id == 0x28) {
        uint64_t who{};
        const bool local = haveMine && Read(object + 0x30, &who) && who == mine;
        if (local) {
            if (s.lots.empty() || s.lots.back().captured) {
                KdLot lot;
                lot.release = LagNs() + (int64_t)s.delay.load() * 1000000;
                if (!KdHold(lot, shared)) return false;
                s.lots.push_back(std::move(lot));
                return true;
            }
            if (!safe && s.chance && MdRandomPercent() < s.amount.load()) return false;
            return KdHold(s.lots.back(), shared);
        }
        if (!safe || s.lots.empty()) return false;
        return KdHold(s.lots.back(), shared);
    }
    if (id == 0x73) {
        uint8_t ask{};
        if (!Read(object + 0x38, &ask) || ask != 1 || s.lots.empty()) return false;
        KdLot& last = s.lots.back();
        if (!safe && last.captured) return false;
        KdLatency l;
        Read(object + 0x30, &l.stamp);
        Read(object + 0x40, &l.extra);
        last.latency.push_back(l);
        last.captured = true;
        return true;
    }
    bool listed = id == 0x12 || id == 0x13 || id == 0x1d || id == 0x97 || id == 0xbb || (id == 0x6f && safe);
    if (!listed || s.lots.empty()) return false;
    KdLot& last = s.lots.back();
    if (!safe) {
        if (last.captured) return false;
        if (id == 0x12) {
            uint64_t who{};
            if (!haveMine || !Read(object + 0x30, &who) || who != mine) return false;
        }
    }
    return KdHold(last, shared);
}
inline bool KdReceive(void* shared) {
    auto& s = KnockbackDesync();
    if (s.fault) return false;
    __try { return KdReceiveWork(shared); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s.fault = true; return false; }
}
inline void KdTickWork() {
    auto& s = KnockbackDesync();
    std::lock_guard<std::recursive_mutex> lock(s.guard);
    if (s.resetPending.exchange(false)) KdClear(s);
    if (s.releasePending.exchange(false)) { KdRelease(s, true); KdClear(s); return; }
    if (s.lots.empty()) return;
    Context c{};
    if (!Local(&c)) { KdClear(s); return; }
    KdRelease(s, false);
}
inline void KdTick() {
    auto& s = KnockbackDesync();
    if (s.fault) return;
    __try { KdTickWork(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s.fault = true; }
}

// ============================================================ Auto Jump Reset
struct AjrShared {
    std::atomic<bool> enabled{false}, requireTarget{false}, secondHit{false}, face{false}, resetPending{false}, fault{false};
    std::atomic<int> chance{100};
    std::atomic<float> delayLow{0.f}, delayHigh{50.f};
    std::mutex guard;
    // state (guard)
    float motion[3]{};
    int64_t pendAt = 0, firstAt = 0, hurtAt = 0, motionAt = 0, rotAt = 0;
    bool pendValid = false, firstValid = false, hurtValid = false, motionValid = false, rotActive = false, jumpPending = false, armed = false;
    float delayMs = 0, yaw = 0;
};
inline AjrShared& AutoJumpReset() { static auto* s = new AjrShared; return *s; }
inline void AjrReset(AjrShared& s) {
    s.motion[0] = s.motion[1] = s.motion[2] = 0;
    s.pendAt = s.firstAt = s.hurtAt = s.motionAt = s.rotAt = 0;
    s.pendValid = s.firstValid = s.hurtValid = s.motionValid = s.rotActive = s.jumpPending = s.armed = false;
    s.delayMs = s.yaw = 0;
}
inline bool AjrLadderFamily(const std::string& n) {
    return n == "minecraft:ladder" || n == "minecraft:vine" || n == "minecraft:cave_vines" || n == "minecraft:cave_vines_body_with_berries" ||
           n == "minecraft:cave_vines_head_with_berries" || n == "minecraft:twisting_vines" || n == "minecraft:weeping_vines";
}
// Grounded, not in water / lava, feet not in a ladder or cobweb block, and horizontal motion large enough.
inline bool AjrCanJump(const Context& c, const float* m) {
    if (!(0.009999999776482582f <= m[2] * m[2] + m[0] * m[0])) return false;
    if (!ActionOnGround(c.actor) || MdInWater(c) || MdInLava(c)) return false;
    const uint64_t region = ActorRegion(c);
    if (!region) return false;
    int32_t cell[3];
    std::string name;
    const uint64_t block = MdFeetBlock(c, region, cell);
    if (!MdBlockName(block, &name)) return true;   // unreadable name: not a ladder / cobweb
    return !(AjrLadderFamily(name) || name == "minecraft:web");
}
// The motion pushes the player backwards (he looks toward the attacker).
inline bool AjrPushedBack(const Context& c, const float* m) {
    float pitch{}, yaw{};
    if (!RotRead(c.actor, &pitch, &yaw)) return false;
    const float r = yaw * 0.01745329238474369f;
    return std::cos(r) * m[2] < std::sin(r) * m[0];
}
inline void AjrRotate(const Context& c, float yaw) {
    float pitch{}, current{};
    if (!RotRead(c.actor, &pitch, &current)) return;
    MdSubmitRotation(2, yaw, pitch);
}
inline void AjrJump(const Context& c, AjrShared& s) {
    if (!JumpRequest(c.actor)) return;
    if (s.face) {
        s.yaw = std::atan2(s.motion[0], -s.motion[2]) * 57.295780181884766f;
        s.rotAt = ActionNs();
        s.rotActive = true;
        AjrRotate(c, s.yaw);
    }
}
inline void AjrTrigger(const Context& c, AjrShared& s) {
    if (s.jumpPending) return;
    if (!AjrCanJump(c, s.motion)) return;
    if (!s.face && !AjrPushedBack(c, s.motion)) return;
    if (s.requireTarget) {
        Target t{};
        if (!ReadCrosshairTarget(&t)) return;
    }
    if (s.secondHit) {
        const int64_t now = ActionNs();
        if (!s.armed) { s.armed = true; s.firstAt = now; s.firstValid = true; return; }
        if (s.firstValid && 1000.0 < (double)(now - s.firstAt) / 1000000.0) { s.armed = true; s.firstAt = now; s.firstValid = true; return; }
        s.armed = false;
    }
    if (!(MdRandomPercent() < s.chance.load())) return;
    s.delayMs = ActionRandom(s.delayLow, s.delayHigh);
    if (s.delayMs <= 0.f) { AjrJump(c, s); return; }
    s.pendAt = ActionNs();
    s.pendValid = true;
    s.jumpPending = true;
}
inline bool AjrReceiveWork(void* shared) {
    auto& s = AutoJumpReset();
    if (!s.enabled) return false;
    uint64_t object{};
    if (!Read((uintptr_t)shared, &object) || !object) return false;
    const int id = PacketIdOf((uintptr_t)object);
    int kind = 0;
    if (id == 0x28) kind = 1;
    else if (id == 0x1b) {
        uint8_t type{};
        if (!Read(object + 0x38, &type) || type != 2) return false;
        kind = 2;
    } else return false;
    std::lock_guard<std::mutex> lock(s.guard);
    Context c{};
    if (!Local(&c) || !StickyHasInput(c)) { AjrReset(s); return false; }
    uint64_t mine{}, who{};
    if (!RuntimeId(c.actor, &mine) || !Read(object + 0x30, &who) || who != mine) return false;
    const int64_t now = ActionNs();
    if (kind == 1) {
        Read(object + 0x38, &s.motion[0]);
        Read(object + 0x3c, &s.motion[1]);
        Read(object + 0x40, &s.motion[2]);
        s.motionAt = now;
        s.motionValid = true;
    } else {
        s.hurtAt = now;
        s.hurtValid = true;
    }
    if (s.motionValid && (double)(now - s.motionAt) / 1000000.0 <= 100.0 && s.hurtValid && (double)(now - s.hurtAt) / 1000000.0 <= 100.0) {
        s.motionValid = false;
        s.hurtValid = false;
        AjrTrigger(c, s);
    }
    return false;   // never cancels
}
inline bool AjrReceive(void* shared) {
    auto& s = AutoJumpReset();
    if (s.fault) return false;
    __try { return AjrReceiveWork(shared); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s.fault = true; return false; }
}
inline void AjrTickWork() {
    auto& s = AutoJumpReset();
    std::lock_guard<std::mutex> lock(s.guard);
    if (s.resetPending.exchange(false)) AjrReset(s);
    if (!s.enabled) return;
    Context c{};
    if (!Local(&c) || !StickyHasInput(c)) { AjrReset(s); return; }
    const int64_t now = ActionNs();
    if (s.rotActive && (double)(now - s.rotAt) / 1000000.0 <= 100.0) AjrRotate(c, s.yaw);
    if (s.jumpPending && s.pendValid && (double)s.delayMs < (double)(now - s.pendAt) / 1000000.0) {
        s.jumpPending = false;
        AjrJump(c, s);
    }
}
inline void AjrTick() {
    auto& s = AutoJumpReset();
    if (s.fault) return;
    __try { AjrTickWork(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s.fault = true; }
}

// ============================================================ Look TP
struct LookTpShared {
    std::atomic<float> range{128.f};
    std::atomic<bool> request{false}, done{false}, fault{false};
};
inline LookTpShared& LookTp() { static auto* s = new LookTpShared; return *s; }
// Local teleport: position and previous position set, velocity cleared, collision box translated.
inline bool LookTpMove(uintptr_t actor, const float* pos) {
    uint64_t state{}, box{};
    if (!Read(actor + 0x240, &state) || !state || !Read(actor + 0x248, &box) || !box) return false;
    float old[3]{};
    if (!Floats(state, old, 3)) return false;
    const float d[3] = {pos[0] - old[0], pos[1] - old[1], pos[2] - old[2]};
    memcpy(reinterpret_cast<void*>(state), pos, 12);
    memcpy(reinterpret_cast<void*>(state + 0xc), pos, 12);
    memset(reinterpret_cast<void*>(state + 0x18), 0, 12);
    float* b = reinterpret_cast<float*>(box);
    b[0] += d[0]; b[1] += d[1]; b[2] += d[2];
    b[3] += d[0]; b[4] += d[1]; b[5] += d[2];
    return true;
}
inline void LookTpActivate() {
    Context c{};
    if (!Local(&c) || !StickyHasInput(c)) return;
    const uint64_t region = ActorRegion(c);
    if (!region) return;
    const float R = LookTp().range;
    float eye[3]{}, pitch{}, yaw{};
    if (!ActorPos(c.actor, eye) || !RotRead(c.actor, &pitch, &yaw) || !ReadYaw(&yaw)) return;
    const float p = pitch * 0.01745329238474369f, y = yaw * 0.01745329238474369f;
    const float to[3] = {eye[0] - std::sin(y) * std::cos(p) * R, eye[1] - std::sin(p) * R, eye[2] + std::cos(y) * std::cos(p) * R};
    const uint64_t clipOf = Virtual0(c, region, 0x198);
    if (!clipOf) return;
    using Clip = void*(*)(uint64_t, void*, const float*, const float*, bool, bool, int, bool, bool, uint64_t, const std::function<bool()>*, bool);
    static const std::function<bool()> accept = [] { return true; };
    alignas(16) uint8_t res[0x200]{};
    reinterpret_cast<Clip>(clipOf)(region, res, eye, to, false, false, (int)R, false, false, c.actor, &accept, false);
    uint64_t weak{};
    memcpy(&weak, res + 0x40, 8);
    if (weak && InterlockedDecrement((volatile long*)(weak + 0xc)) == 0) {
        uint64_t t{}, rel{};
        if (Read(weak, &t) && Read(t + 8, &rel) && rel) reinterpret_cast<void(*)(uint64_t)>(rel)(weak);
    }
    int type = -1;
    memcpy(&type, res + 0x18, 4);
    if (type != 0) return;
    int32_t hit[3];
    memcpy(hit, res + 0x20, 12);
    const int32_t above[3] = {hit[0], hit[1] + 1, hit[2]};
    if (!BlockReplaceable(RegionBlock(c, region, above))) return;
    uintptr_t offsets{};
    float lift{};
    if (!Element(c.registry, c.entity, "OffsetsComponent", 0x44, &offsets) || !Read(offsets, &lift)) return;
    const float dest[3] = {(float)above[0] + 0.5f, lift + (float)above[1], (float)above[2] + 0.5f};
    LookTpMove(c.actor, dest);
}
inline void LookTpTickWork() {
    auto& s = LookTp();
    if (!s.request.exchange(false)) return;
    LookTpActivate();
    s.done = true;   // the menu turns the module off
}
inline void LookTpTick() {
    auto& s = LookTp();
    if (s.fault) return;
    __try { LookTpTickWork(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s.fault = true; s.done = true; }
}

// ============================================================ Auto MLG
struct AutoMlgShared {
    std::atomic<bool> enabled{false}, water{true}, cobweb{true}, pickUp{false}, pull{false}, silent{true}, highlight{true}, resetPending{false};
    std::atomic<bool> noFall{false};             // No Fall enabled: Auto MLG stays idle (exclusive with it)
    std::atomic<int> trigger{0};                 // 0 Lethal Only, 1 Min Damage
    std::atomic<float> minDamage{6.f}, aim{100.f};
    float color[4]{0.3f, 0.75f, 1.f, 0.3f};      // guarded by 'guard'
    std::mutex guard;
    // runtime (guard)
    int phase = 0;                               // 0 idle, 1 rescue planned, 2 placed, 3 picking the water up
    int32_t target[3]{};                         // block whose face is used
    uint8_t face = 1;
    std::string item;
    int64_t placedAt = 0, pickAt = 0;
    bool placed = false, pickStarted = false, resetting = false;
    int32_t cell[3]{};                           // rescue cell (target + face direction), highlighted
    bool cellValid = false;
    MsShapes shapes;                             // simulator collision buffer (game thread only)
    volatile long fault = 0;
};
inline AutoMlgShared& AutoMlg() { static auto* s = new AutoMlgShared; return *s; }

// Face tables: 0 down, 1 up, 2 north (z-1), 3 south (z+1), 4 west (x-1), 5 east (x+1).
inline const int32_t* MlgNormal(int f) {
    static const int32_t n[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    return n[f < 6 ? f : 1];
}
inline void MlgFacePoint(const int32_t* block, int face, float* point) {   // centre of the face
    static const float x[6] = {.5f, .5f, .5f, .5f, 0.f, 1.f}, y[6] = {0.f, 1.f, .5f, .5f, .5f, .5f}, z[6] = {.5f, .5f, 0.f, 1.f, .5f, .5f};
    if (face >= 6) { point[0] = (float)block[0] + .5f; point[1] = (float)block[1] + .5f; point[2] = (float)block[2] + .5f; return; }
    point[0] = (float)block[0] + x[face];
    point[1] = (float)block[1] + y[face];
    point[2] = (float)block[2] + z[face];
}
inline bool MlgCushion(uint64_t block) {   // liquid, or name containing web, powder_snow, scaffolding, vine, ladder, sweet_berry
    uint8_t a{}, b{};
    if (!BlockFlags(block, &a, &b)) return false;
    if (b & 2) return true;
    std::string n;
    if (!MdBlockName(block, &n)) return false;
    return MdContains(n, "web") || MdContains(n, "powder_snow") || MdContains(n, "scaffolding") || MdContains(n, "vine") ||
           MdContains(n, "ladder") || MdContains(n, "sweet_berry");
}
// Rescue items: water bucket (used on the top face of the landing block) and cobweb (placed by the planner).
struct MlgItem { const char* name; const char* alt; bool planner; };
inline const MlgItem& MlgWater() { static const MlgItem i{"water_bucket", nullptr, false}; return i; }
inline const MlgItem& MlgWeb() { static const MlgItem i{"web", "cobweb", true}; return i; }
inline const MlgItem* MlgItemNamed(const std::string& n) { if (n == "water_bucket") return &MlgWater(); if (n == "web") return &MlgWeb(); return nullptr; }
inline bool MlgIs(uint64_t stack, const MlgItem& it) { return ActionNameIs(stack, it.name) || (it.alt && ActionNameIs(stack, it.alt)); }
inline bool MlgInHotbar(const Context& c, const MlgItem& it) { int slot{}; return ActionFind(c, c.actor, [&](uint64_t st) { return MlgIs(st, it); }, &slot); }
inline int MlgInInventory(const Context& c, const MlgItem& it) {   // first slot >= 9 holding the item, -1 if none
    const uint64_t box = ActionContainer(c.actor);
    const int n = ActionSize(c, box);
    for (int slot = 9; slot < n; ++slot) if (MlgIs(ActionStackAt(c, box, slot), it)) return slot;
    return -1;
}
inline bool MlgAir(const Context& c, uint64_t region, const int32_t* p) { uint8_t a{}, b{}; return BlockFlags(RegionBlock(c, region, p), &a, &b) && a == 0; }
inline bool MlgActorBox(uintptr_t actor, float* bb) { uint64_t p{}; return Read(actor + 0x248, &p) && p && Floats(p, bb, 6); }
inline bool MlgCellInBox(const float* bb, const int32_t* t) {
    const float x = (float)t[0], y = (float)t[1], z = (float)t[2];
    return x < bb[3] && y < bb[4] && z < bb[5] && bb[0] < x + 1.f && bb[1] < y + 1.f && bb[2] < z + 1.f;
}
inline float MlgDistance(const float* from, const float* to) {
    const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    return std::sqrt(dz * dz + dy * dy + dx * dx);
}

// Evaluation of the fall (every tick in phases 0 and 1).
struct MlgEval {
    bool want = false;
    std::vector<std::array<float, 3>> path;   // simulated feet positions, one per tick
    int32_t landing[3]{};
};
// First simulated position whose box (current dimensions) touches the cell.
inline bool MlgFirstTouch(uintptr_t actor, const std::vector<std::array<float, 3>>& path, const int32_t* cell, size_t* index) {
    float bb[6];
    if (!MlgActorBox(actor, bb)) return false;
    const float hw = (bb[3] - bb[0]) * .5f, h = bb[4] - bb[1], hd = (bb[5] - bb[2]) * .5f;
    const float cx = (float)cell[0], cy = (float)cell[1], cz = (float)cell[2];
    for (size_t i = 0; i < path.size(); ++i) {
        const auto& p = path[i];
        if (cx < hw + p[0] && cy < p[1] + 0.f + h && cz < hd + p[2] && p[0] - hw < cx + 1.f && p[1] < cy + 1.f && p[2] - hd < cz + 1.f) { *index = i; return true; }
    }
    return false;
}
// Placement plan: block + face to use.
struct MlgPlan { bool ok = false; int32_t block[3]{}; uint8_t face = 1; };
inline MlgPlan MlgPlanFor(const Context& c, uint64_t region, const MlgItem& it, const MlgEval& ev) {
    MlgPlan out;
    if (!region) return out;
    if (!it.planner) {   // top face of the landing block, the cell above must be air
        const int32_t above[3] = {ev.landing[0], ev.landing[1] + 1, ev.landing[2]};
        if (!MlgAir(c, region, above)) return out;
        memcpy(out.block, ev.landing, 12);
        out.face = 1;
        out.ok = true;
        return out;
    }
    const size_t n = ev.path.size();
    if (n < 2) return out;
    const size_t last = n - 1, start = last > 4 ? n - 5 : 0;
    if (!(start < last)) return out;
    float eye[3]{};
    if (!ActorPos(c.actor, eye)) return out;
    const float reach = AbrReach(c, c.actor);
    static const int order[5] = {0, 2, 3, 4, 5};
    for (size_t i = n - 2;; --i) {
        const auto& p = ev.path[i];
        const int fx = MsToInt(std::floor(p[0])), fy = MsToInt(std::floor(p[1])), fz = MsToInt(std::floor(p[2]));
        const int32_t cells[2][3] = {{fx, fy + 0, fz + 0}, {fx + 0, fy + 1, fz}};
        for (const auto& C : cells) {
            if (!MlgAir(c, region, C)) continue;
            float bb[6];
            if (!MlgActorBox(c.actor, bb)) return out;
            if (MlgCellInBox(bb, C)) continue;
            size_t touch{};
            if (!MlgFirstTouch(c.actor, ev.path, C, &touch) || !(touch < last)) continue;
            bool have = false;
            int32_t best[3]{};
            uint8_t bestFace = 1;
            for (int f : order) {
                const int32_t* d = MlgNormal(f);
                const int32_t nb[3] = {C[0] + d[0], C[1] + d[1], C[2] + d[2]};
                if (!BlockSolid(RegionBlock(c, region, nb))) continue;
                if (!have) { have = true; memcpy(best, nb, 12); bestFace = (uint8_t)(f ^ 1); continue; }
                float a[3], b[3];
                MlgFacePoint(nb, f ^ 1, a);
                MlgFacePoint(best, bestFace, b);
                if (MlgDistance(eye, a) < MlgDistance(eye, b)) { memcpy(best, nb, 12); bestFace = (uint8_t)(f ^ 1); }
            }
            if (!have) continue;
            const int32_t* d = MlgNormal(bestFace);
            const int32_t T[3] = {best[0] + d[0], best[1] + d[1], best[2] + d[2]};
            float point[3];
            MlgFacePoint(best, bestFace, point);
            if (!MlgCellInBox(bb, T) && MlgDistance(eye, point) <= reach) { memcpy(out.block, best, 12); out.face = bestFace; out.ok = true; return out; }
            const float hw = (bb[3] - bb[0]) * .5f, h = bb[4] - bb[1], hd = (bb[5] - bb[2]) * .5f;
            const float tx = (float)T[0], ty = (float)T[1], tz = (float)T[2];
            for (size_t k = 0; k < touch; ++k) {
                const auto& q = ev.path[k];
                const bool overlap = !(hw + q[0] <= tx || q[1] + 0.f + h <= ty) && !(hd + q[2] <= tz || tx + 1.f <= q[0] - hw || ty + 1.f <= q[1] || tz + 1.f <= q[2] - hd);
                if (overlap) continue;
                const float from[3] = {q[0] + 0.f, q[1] + 1.6200000047683716f, q[2] + 0.f};
                if (MlgDistance(from, point) <= reach) { memcpy(out.block, best, 12); out.face = bestFace; out.ok = true; return out; }
            }
        }
        if (i <= start) break;
    }
    return out;
}

// Expected damage of a fall (damage formula of the game, armor and effects applied).
struct MlgFallInput { float multiplier = 1.f; int jump = 0, featherFalling = 0, protection = 0, resistance = 0; };
inline float AutoMlgDamage(float fall, const MlgFallInput& in) {
    float d = std::ceil(((fall - 3.f) - (float)in.jump) * in.multiplier);
    if (!(d > 0.f)) return 0.f;
    float resist = (float)in.resistance * -0.2f + 1.f;
    resist = resist < 0.f ? 0.f : (resist > 1.f ? 1.f : resist);
    float epf = (float)(in.featherFalling * 3 + in.protection);
    if (epf > 20.f) epf = 20.f;
    return (1.f - 0.03999999910593033f * epf) * d * resist;
}
inline float MlgLandingMultiplier(const Context& c, uint64_t block) {
    std::string n;
    if (!MdBlockName(block, &n)) return 1.f;
    if (n == "minecraft:bed") return 0.5f;
    if (n == "minecraft:slime") {   // slime: no damage unless sneaking (actor data flag 1)
        uintptr_t f{};
        uint64_t flags{};
        return Element(c.registry, c.entity, "ActorDataFlagComponent", 0x18, &f) && Read(f, &flags) && (flags & (1ull << 1)) ? 1.f : 0.f;
    }
    if (n == "minecraft:hay_block" || n == "minecraft:honey_block") return 0.2f;
    return 1.f;
}
inline MlgFallInput MlgReadInput(const Context& c, uint64_t landing) {
    MlgFallInput in;
    in.multiplier = MlgLandingMultiplier(c, landing);
    in.jump = EffectLevel(c.registry, c.entity, 8) + 1;
    in.resistance = EffectLevel(c.registry, c.entity, 11) + 1;
    if (in.jump < 0) in.jump = 0;
    if (in.resistance < 0) in.resistance = 0;
    const uint64_t armor = EquipmentContainer(c, c.actor, 1);
    if (armor) {
        const int n = ActionSize(c, armor);
        for (int i = 0; i < n && i < 16; ++i) {
            const uint64_t stack = ActionStackAt(c, armor, i);
            if (!ActionItem(stack)) continue;
            const int p = EnchantLevel(c, stack, 0);
            in.protection += p > 0 ? p : 0;
            if (i == 3) { const int f = EnchantLevel(c, stack, 2); in.featherFalling = f > 0 ? f : 0; }
        }
    }
    return in;
}

// Conditions under which nothing is done.
inline bool MlgBlocked(const Context& c, uint64_t region) {
    if (!StickyHasInput(c) || ActionOnGround(c.actor)) return true;
    if (AutoMlg().noFall) return true;   // No Fall active (exclusive)
    uintptr_t storage{}, element{};
    uint32_t mode{};
    if (Storage(c.registry, 0x88d3eddfu, &storage) && InStorage(storage, c.entity, 4, &element) && Read(element, &mode) && (mode == 1 || mode == 6)) return true;   // Creative / Spectator
    uint32_t abilities{};
    if (Storage(c.registry, 0x6de78dc5u, &storage) && InStorage(storage, c.entity, 0x10, &element) && Read(element + 4, &abilities) && (abilities & 5)) return true;   // flying / may fly
    uint8_t fallFlag{};
    if (Storage(c.registry, 0xce6b34f6u, &storage) && InStorage(storage, c.entity, 8, &element) && Read(element + 4, &fallFlag) && fallFlag) return true;   // fall distance component byte +4
    if (MdInWater(c) || MdInLava(c)) return true;
    uintptr_t f{};
    uint64_t flags{};
    if (Element(c.registry, c.entity, "ActorDataFlagComponent", 0x18, &f) && Read(f, &flags) && (flags & ((1ull << 32) | (1ull << 2)))) return true;   // gliding / riding
    if (EffectLevel(c.registry, c.entity, 27) >= 0 || EffectLevel(c.registry, c.entity, 24) >= 0) return true;   // slow falling / levitation
    if (!region) return true;
    int32_t cell[3];
    return MlgCushion(MdFeetBlock(c, region, cell));
}
// Full evaluation: guards, 200-tick fall simulation, landing block, damage, trigger.
inline bool MlgEvaluate(const Context& c, AutoMlgShared& s, MlgEval* ev) {
    ev->want = false;
    ev->path.clear();
    const uint64_t region = ActorRegion(c);
    if (MlgBlocked(c, region)) return false;
    MsSim sim;
    if (!MsInit(c, region, &s.shapes, sim)) return false;
    ev->path.reserve(200);
    bool landed = false;
    float fall = 0.f;
    for (int tick = 0; tick < 200; ++tick) {
        MsOutput out;
        MsTick(sim.env, out, sim.state, sim.input);
        if (MsOutFailed(out)) return false;
        const float* p = MsOutPos(out);
        ev->path.push_back({p[0], p[1], p[2]});
        if (MsOutLanded(out)) {
            const uint8_t* st = sim.state.m;
            if (st[0xec] == 1) memcpy(ev->landing, st + 0xe0, 12);
            else {
                float pos[3];
                memcpy(pos, st + 0x4c, 12);
                ev->landing[0] = (int32_t)std::floor(pos[0]);
                ev->landing[1] = (int32_t)std::floor(pos[1]);
                ev->landing[2] = (int32_t)std::floor(pos[2]);
            }
            fall = MsOutFall(out);
            landed = true;
            break;
        }
    }
    if (!landed) return false;
    const int32_t above[3] = {ev->landing[0], ev->landing[1] + 1, ev->landing[2]};
    if (MlgCushion(RegionBlock(c, region, above))) return false;
    const float damage = AutoMlgDamage(fall, MlgReadInput(c, RegionBlock(c, region, ev->landing)));
    if (!(damage > 0.f)) return false;
    if (s.trigger.load() == 1) {
        if (!(s.minDamage.load() <= damage)) return false;
    } else {
        float health{}, maxHealth{}, absorption{}, maxAbsorption{};
        const bool haveHealth = Attribute(c.registry, c.entity, 6, &health, &maxHealth);
        if (!Attribute(c.registry, c.entity, 0xd, &absorption, &maxAbsorption)) absorption = 0.f;
        if (haveHealth && damage < health + absorption) return false;
    }
    ev->want = true;
    return true;
}

// Turn toward a point: yaw = atan2(-dx, dz), pitch = -atan2(dy, horizontal distance).
inline void MlgAim(const Context& c, const float* point, bool persistent) {
    float eye[3]{};
    if (!ActorPos(c.actor, eye)) return;
    const float dx = point[0] - eye[0], dy = point[1] - eye[1], dz = point[2] - eye[2];
    RotRequest r;
    r.module = kRotMovementD;
    r.yaw = std::atan2(-dx, dz) * 57.295780181884766f;
    r.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * -57.295780181884766f;
    r.mode = AutoMlg().silent ? 2 : 0;
    r.priority = 3;
    r.hasMax = true;
    r.maxStep = AutoMlg().aim.load() / 100.f * 90.f;
    if (persistent) RotSubmitPersistent(std::move(r), 0); else RotSubmit(std::move(r));
}
inline void AutoMlgReset(bool cancel = true) {
    auto& s = AutoMlg();
    {
        std::lock_guard<std::mutex> lock(s.guard);
        if (s.resetting) return;
        s.resetting = true;
        s.phase = 0;
        s.placed = s.pickStarted = s.cellValid = false;
        s.item.clear();
    }
    if (cancel) { ActionCancel(kActionAutoMlg); RotRelease(kRotMovementD); }
    std::lock_guard<std::mutex> lock(s.guard);
    s.resetting = false;
}
inline bool MlgInReach(const Context& c, const int32_t* block, int face) {
    float point[3], eye[3]{};
    MlgFacePoint(block, face, point);
    if (!ActorPos(c.actor, eye)) return false;
    return MlgDistance(eye, point) <= AbrReach(c, c.actor);
}
// Rescue sequence: select the item, wait (airborne, in reach, target cell clear of the player, rotation reached),
// check the item, use it, phase 2, switch back. 8000 ms limit.
inline void MlgSubmitRescue(const MlgItem& it, const MlgPlan& plan) {
    auto& s = AutoMlg();
    const std::array<int32_t, 3> block = {plan.block[0], plan.block[1], plan.block[2]};
    const int face = plan.face;
    const MlgItem item = it;
    ActionSequence q;
    q.module = kActionAutoMlg;
    q.silent = s.silent ? 1 : 0;
    q.conditions.push_back([](uintptr_t, int64_t start) { return ActionMs(start) <= 8000.0; });
    q.steps.push_back([item](ActionCtx& x) {
        int slot{};
        if (!ActionFind(*x.c, x.actor, [&](uint64_t st) { return MlgIs(st, item); }, &slot)) return 2;
        ActionSwitch(x, slot);
        return 1;
    });
    q.steps.push_back([block, face](ActionCtx& x) {
        if (ActionOnGround(x.actor)) return 0;
        if (!MlgInReach(*x.c, block.data(), face)) return 0;
        if (face != 1) {
            const int32_t* d = MlgNormal(face);
            const int32_t t[3] = {block[0] + d[0], block[1] + d[1], block[2] + d[2]};
            float bb[6];
            if (MlgActorBox(x.actor, bb) && MlgCellInBox(bb, t)) return 0;
        }
        return RotReadyFor(kRotMovementD, 0.01f) ? 1 : 0;
    });
    q.steps.push_back([item](ActionCtx& x) { return MlgInHotbar(*x.c, item) ? 1 : 2; });
    // The item is used on the planned face (click-on-block transaction). A plain item use, as in the reference, only
    // reached the server as a use in the air (no block, face -1): PowerNukkitX then placed nothing and the fall hurt.
    q.steps.push_back(StepUseOnBlock(block, (uint8_t)face));
    q.steps.push_back([](ActionCtx&) {   // placed: phase 2
        auto& a = AutoMlg();
        {
            std::lock_guard<std::mutex> lock(a.guard);
            a.phase = 2;
            a.pickStarted = false;
            a.placedAt = ActionNs();
            a.placed = true;
        }
        RotRelease(kRotMovementD);
        return 1;
    });
    q.steps.push_back(StepSwitchBack());
    ActionSubmit(std::move(q));
}
// Pull From Inventory: the stack moves to the first empty hotbar slot, else to the last hotbar slot that is not selected.
inline void MlgPull(const MlgItem& it) {
    const MlgItem item = it;
    InvSubmit(kActionAutoMlg, [item](uintptr_t actor) {
        Context k{};
        if (!Local(&k) || k.actor != actor || !AutoMlg().enabled) return;
        if (MlgInHotbar(k, item)) return;
        const int from = MlgInInventory(k, item);
        if (from < 0) return;
        const uint64_t box = ActionContainer(actor);
        const int size = ActionSize(k, box);
        const int hot = size < 9 ? size : 9;
        int dest = -1;
        for (int i = 0; i < hot; ++i) if (!ActionItem(ActionStackAt(k, box, i))) { dest = i; break; }
        if (dest < 0) {
            int32_t* sel = ActionSelected(actor);
            for (int i = hot - 1; i >= 0; --i) if (!sel || i != *sel) { dest = i; break; }
        }
        if (dest < 0) return;
        uint8_t count{};
        Read(ActionStackAt(k, box, from) + 0x22, &count);
        InvMoveStack(k, actor, 0xc, from, 0xc, dest, count ? count : 1);
    });
}
inline void MlgStartPickup(const int32_t* cell) {
    auto& s = AutoMlg();
    {
        std::lock_guard<std::mutex> lock(s.guard);
        s.phase = 3;
        memcpy(s.target, cell, 12);
        s.face = 1;
    }
    ActionSequence q;
    q.module = kActionAutoMlg;
    q.silent = s.silent ? 1 : 0;
    q.conditions.push_back([](uintptr_t, int64_t start) { return ActionMs(start) <= 1000.0; });
    q.steps.push_back(StepFindName("bucket"));
    q.steps.push_back(StepUse(kRotMovementD));
    q.done = [](int) { AutoMlgReset(false); RotRelease(kRotMovementD); };
    ActionSubmit(std::move(q));
}
inline void AutoMlgTickWork() {
    auto& s = AutoMlg();
    if (s.resetPending.exchange(false)) AutoMlgReset();
    int phase;
    {
        std::lock_guard<std::mutex> lock(s.guard);
        phase = s.phase;
    }
    if (!s.enabled) { if (phase) AutoMlgReset(); MsShapesFree(s.shapes); return; }
    Context c{};
    if (!Local(&c)) { if (phase) AutoMlgReset(); return; }
    {   // client level tick only
        uint64_t level{};
        Read(c.actor + 0x200, &level);
        if ((uint64_t)currentTickLevel != level) return;
    }
    if (phase == 3) {
        int32_t block[3];
        uint8_t face;
        {
            std::lock_guard<std::mutex> lock(s.guard);
            memcpy(block, s.target, 12);
            face = s.face;
        }
        float point[3];
        MlgFacePoint(block, face, point);
        MlgAim(c, point, false);
        return;
    }
    if (phase == 2) {
        int32_t block[3];
        uint8_t face;
        std::string item;
        bool placed, started;
        int64_t placedAt, pickAt;
        {
            std::lock_guard<std::mutex> lock(s.guard);
            memcpy(block, s.target, 12);
            face = s.face;
            item = s.item;
            placed = s.placed;
            placedAt = s.placedAt;
            started = s.pickStarted;
            pickAt = s.pickAt;
        }
        if (!ActionOnGround(c.actor) && !MdInWater(c)) {
            if (!placed) return;
            if (ActionMs(placedAt) <= 3000.0) return;
            AutoMlgReset();
            return;
        }
        if (item == "water_bucket" && s.pickUp) {
            if (!started) { std::lock_guard<std::mutex> lock(s.guard); s.pickStarted = true; s.pickAt = ActionNs(); return; }
            if (ActionMs(pickAt) <= 100.0) return;
            const int32_t* n = MlgNormal(face);
            const int32_t cell[3] = {block[0] + n[0], block[1] + n[1], block[2] + n[2]};
            const uint64_t region = ActorRegion(c);
            uint8_t a{}, b{};
            if (region && BlockFlags(RegionBlock(c, region, cell), &a, &b) && (b & 2) && MlgInReach(c, block, face)) { MlgStartPickup(cell); return; }
        }
        AutoMlgReset();
        return;
    }
    // phases 0 and 1
    MlgEval ev;
    const bool want = MlgEvaluate(c, s, &ev);
    const uint64_t region = ActorRegion(c);
    if (phase == 1) {   // the planned rescue is kept only while it can still be done
        int32_t block[3];
        uint8_t face;
        std::string name;
        {
            std::lock_guard<std::mutex> lock(s.guard);
            memcpy(block, s.target, 12);
            face = s.face;
            name = s.item;
        }
        const MlgItem* it = MlgItemNamed(name);
        bool keep = want && it && region && MlgInHotbar(c, *it);
        if (keep) {
            const int32_t* d = MlgNormal(face);
            const int32_t t[3] = {block[0] + d[0], block[1] + d[1], block[2] + d[2]};
            if (!MlgAir(c, region, t)) keep = false;
            else if (it->planner) {
                float bb[6];
                if (!(MlgActorBox(c.actor, bb) && MlgCellInBox(bb, t))) {
                    size_t touch{};
                    if (!MlgFirstTouch(c.actor, ev.path, t, &touch) || ev.path.size() <= touch + 1) keep = false;
                }
            } else {
                const MlgPlan p = MlgPlanFor(c, region, *it, ev);
                keep = p.ok && p.face == face && !memcmp(p.block, block, 12);
            }
        }
        if (!keep) AutoMlgReset();
        {
            std::lock_guard<std::mutex> lock(s.guard);
            phase = s.phase;
        }
    }
    if (!want) return;
    if (phase == 0) {
        int dimension = 0;
        const bool nether = ReadDimension(&dimension) && dimension == 1;
        const MlgItem* chosen = nullptr;
        MlgPlan plan;
        if (s.water && !nether && MlgInHotbar(c, MlgWater())) { plan = MlgPlanFor(c, region, MlgWater(), ev); if (plan.ok) chosen = &MlgWater(); }
        if (!chosen && s.cobweb && MlgInHotbar(c, MlgWeb())) { plan = MlgPlanFor(c, region, MlgWeb(), ev); if (plan.ok) chosen = &MlgWeb(); }
        if (!chosen) {
            if (s.pull) {
                const MlgItem* pull = nullptr;
                if (s.water && !nether && MlgInInventory(c, MlgWater()) >= 0 && MlgPlanFor(c, region, MlgWater(), ev).ok) pull = &MlgWater();
                else if (s.cobweb && MlgInInventory(c, MlgWeb()) >= 0 && MlgPlanFor(c, region, MlgWeb(), ev).ok) pull = &MlgWeb();
                if (pull) MlgPull(*pull);
            }
            return;
        }
        MlgSubmitRescue(*chosen, plan);
        {
            std::lock_guard<std::mutex> lock(s.guard);
            s.phase = 1;
            memcpy(s.target, plan.block, 12);
            s.face = plan.face;
            s.item = chosen->name;
            s.placed = false;
            const int32_t* d = MlgNormal(plan.face);
            s.cell[0] = plan.block[0] + d[0];
            s.cell[1] = plan.block[1] + d[1];
            s.cell[2] = plan.block[2] + d[2];
            s.cellValid = true;
        }
        phase = 1;
    }
    if (phase == 1) {   // aim at the planned face every tick
        int32_t block[3];
        uint8_t face;
        {
            std::lock_guard<std::mutex> lock(s.guard);
            memcpy(block, s.target, 12);
            face = s.face;
        }
        float point[3];
        MlgFacePoint(block, face, point);
        MlgAim(c, point, false);
    }
}
inline void AutoMlgTick() {
    auto& s = AutoMlg();
    if (s.fault) return;
    __try { AutoMlgTickWork(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&s.fault, 1); if (actionLog) actionLog("Auto MLG: fault caught, module stopped until restart"); }
}
// Highlight: wireframe box of the rescue cell.
inline bool AutoMlgLines(const float*, std::vector<WorldLine>& out) {
    auto& s = AutoMlg();
    if (!s.enabled || !s.highlight) return true;
    float color[4];
    int32_t cell[3];
    {
        std::lock_guard<std::mutex> lock(s.guard);
        if (s.phase == 0 || !s.cellValid) return true;
        memcpy(color, s.color, 16);
        memcpy(cell, s.cell, 12);
    }
    const float lo[3] = {(float)cell[0], (float)cell[1], (float)cell[2]}, hi[3] = {lo[0] + 1.f, lo[1] + 1.f, lo[2] + 1.f};
    static const int pick[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& e : edges) {
        WorldLine line{};
        for (int j = 0; j < 3; ++j) {
            line.a[j] = pick[e[0]][j] ? hi[j] : lo[j];
            line.b[j] = pick[e[1]][j] ? hi[j] : lo[j];
        }
        memcpy(line.colorA, color, 16);
        memcpy(line.colorB, color, 16);
        line.thickness = 1.5f;
        line.depth = true;
        out.push_back(line);
    }
    return true;
}

// ============================================================ dispatchers
// Incoming packets (receive hook, shared_ptr<Packet>&): true = packet held / not handled now.
inline bool MovementDReceive(void* shared) {
    if (KnockbackDesync().replaying) return false;   // packets replayed by Knockback Desync are not broadcast again
    AjrReceive(shared);        // reads only, never cancels
    return KdReceive(shared);
}
// GameUpdate subscribers.
inline void MovementDGameUpdate() {
    KdTick();
    AjrTick();
    LookTpTick();
}
}
