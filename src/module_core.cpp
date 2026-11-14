#include "nickel/module_core.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nickel {
namespace {
constexpr float radians = .01745329251994329577f;
void checked(float value, float lo, float hi) {
    if (!std::isfinite(value) || value < lo || value > hi)
        throw std::invalid_argument("setting outside recovered range");
}
}
void flight_tick(Player& player, const Input& input, const FlightSettings& settings) {
    checked(settings.vertical_speed, 0.f, 1.f);
    checked(settings.horizontal_speed, 0.f, 1.f);
    if (!player.present || player.on_ground) return;
    player.ability_flags |= 3u;
    player.velocity.y = input.jump ? settings.vertical_speed : (input.sneak ? -settings.vertical_speed : 0.f);
    if (settings.mode == FlightMode::Normal) {
        player.flight_speed = settings.horizontal_speed;
        player.flight_aux = .05f;
        return;
    }
    player.flight_speed = player.flight_aux = 0.f;
    float forward = static_cast<float>(input.forward) - static_cast<float>(input.backward);
    float side = static_cast<float>(input.right) - static_cast<float>(input.left);
    float length = std::sqrt(forward * forward + side * side);
    if (length == 0.f) { player.velocity.x = player.velocity.z = 0.f; return; }
    forward /= length;
    side /= length;
    const float yaw = (player.yaw_degrees + 90.f) * radians;
    player.velocity.x = settings.horizontal_speed * (std::cos(yaw) * forward - std::sin(yaw) * side);
    player.velocity.z = settings.horizontal_speed * (std::cos(yaw) * side + std::sin(yaw) * forward);
}
void flight_disable(Player& player) {
    if (player.present) player.ability_flags &= ~3u;
}
void jetpack_tick(Player& player, float speed) {
    checked(speed, .05f, 5.f);
    if (!player.present) return;
    const float pitch = player.pitch_degrees * radians;
    const float yaw = player.yaw_degrees * radians;
    player.velocity = {-std::sin(yaw) * std::cos(pitch) * speed,
                       -std::sin(pitch) * speed, std::cos(yaw) * std::cos(pitch) * speed};
}
void glide_tick(Player& player, float fall_speed) {
    checked(fall_speed, 0.f, 1.f);
    if (!player.present || player.on_ground) return;
    if (player.velocity.y < -fall_speed) player.velocity.y = -fall_speed;
}
void fast_stop_tick(Player& player, const Input& input, bool component_flag) {
    if (player.present && !input.moving() && component_flag)
        player.velocity.x = player.velocity.z = 0.f;
}
void no_jump_delay(int& counter) { counter = 0; }
void full_bright_vision(bool& enabled, float& intensity, float requested) {
    if (!std::isfinite(requested)) throw std::invalid_argument("non-finite intensity");
    enabled = true;
    intensity = requested;
}
void full_bright_light(float& light, float requested) {
    if (!std::isfinite(requested)) throw std::invalid_argument("non-finite intensity");
    light = std::max(light, requested);
}
void time_changer(float& time, float configured) { checked(configured, 0.f, 1.f); time = configured; }
void Timer::enable(float current) {
    if (!std::isfinite(current)) throw std::invalid_argument("non-finite tick rate");
    if (!active_) { saved_ = current; active_ = true; }
}
void Timer::tick(float& current, float configured) const {
    checked(configured, 0.f, 100.f);
    if (active_) current = configured;
}
void Timer::disable(float& current) { if (active_) { current = saved_; active_ = false; } }
float approach(float value, float target, float speed, float dt) {
    if (dt < 0.f || speed < 0.f || !std::isfinite(dt) || !std::isfinite(speed))
        throw std::invalid_argument("invalid animation interval");
    value += (target - value) * (1.f - std::exp(-speed * dt));
    return std::fabs(target - value) < .001f ? target : value;
}
float menu_opacity(float progress) {
    progress = std::clamp(progress, 0.f, 1.f);
    return progress * progress * (3.f - 2.f * progress);
}
}
