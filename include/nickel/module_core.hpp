#pragma once

// Module behaviour over explicit data, with no dependency on Minecraft memory layout.
// In game, a build-specific adapter supplies and applies these values.
namespace nickel {
struct Vec3 { float x{}, y{}, z{}; };
struct Input {
    bool forward{}, backward{}, left{}, right{}, jump{}, sneak{};
    bool moving() const { return forward || backward || left || right; }
};
struct Player {
    bool present{true};
    bool on_ground{}; // true when the player has OnGroundFlagComponent
    Vec3 velocity{};
    float yaw_degrees{};
    float pitch_degrees{};
    unsigned ability_flags{};
    float flight_speed{}, flight_aux{};
};
enum class FlightMode { Normal, Motion };
struct FlightSettings { FlightMode mode{FlightMode::Normal}; float vertical_speed{.2f}; float horizontal_speed{.05f}; };

// Sets ability flags |= 3 while flying and clears both (&= ~3) on disable.
void flight_tick(Player&, const Input&, const FlightSettings&);
void flight_disable(Player&);
void jetpack_tick(Player&, float speed = .5f);
void glide_tick(Player&, float fall_speed = .1f);
void fast_stop_tick(Player&, const Input&, bool historical_component_flag);
// The adapter must locate the actual jump-delay counter.
void no_jump_delay(int& counter);
// Two separate render-event paths: vision effect and light level.
void full_bright_vision(bool& vision_enabled, float& intensity, float requested_intensity);
void full_bright_light(float& light, float requested_intensity);
// The adapter supplies the TimeOfDayEvent field.
void time_changer(float& time_of_day, float configured_time);

// Timer: saves the tick rate on enable, enforces the configured rate each tick, restores it on disable.
class Timer {
    bool active_{};
    float saved_{};
public:
    void enable(float current_rate);
    void tick(float& current_rate, float configured_rate = 20.f) const;
    void disable(float& current_rate);
    bool active() const { return active_; }
};
// Frame-rate independent smoothing used by the menu UI.
float approach(float value, float target, float speed, float delta_seconds);
float menu_opacity(float progress);
}
