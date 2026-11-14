#include "nickel/module_core.hpp"
#ifdef _WIN32
#define NICKEL_EXPORT extern "C" __declspec(dllexport)
#else
#define NICKEL_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Loaded by our own controller, never injected into Minecraft.
NICKEL_EXPORT int nickel_glide_velocity(float current_y, int on_ground,
                                         float fall_speed, float* output) noexcept {
    if (!output) return 0;
    try {
        nickel::Player player;
        player.on_ground = on_ground != 0;
        player.velocity.y = current_y;
        nickel::glide_tick(player, fall_speed);
        *output = player.velocity.y;
        return 1;
    } catch (...) { return 0; }
}
