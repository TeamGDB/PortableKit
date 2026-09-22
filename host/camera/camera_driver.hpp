#pragma once

#include "profile.hpp"
#include "settings/settings.hpp"

#include <optional>

namespace psprecomp {
class Runtime;
}

// The framework's side of GameProfile::camera: what the host asks the game's
// camera driver, with the answer for a game that has none. The driver itself
// is the game's, because only the game knows where its camera lives.
namespace portablekit::camera {

// Once per presented game frame, at the game's flip (never per interpolated
// present).
inline void game_camera_frame(psprecomp::Runtime &runtime) {
    const CameraDriver *driver = game().camera;
    if (driver != nullptr && driver->frame != nullptr) driver->frame(runtime);
}

// The port is driving the game's camera right now, so the game must not also
// act on the second stick.
[[nodiscard]] inline bool game_camera_driving() {
    const CameraDriver *driver = game().camera;
    return driver != nullptr && driver->driving != nullptr && driver->driving();
}

// Once per game frame, at the flip: gives the game's 3D view `aspect`, width
// over height of the picture it is drawn into, through the game's own hook.
inline void game_aspect_frame(psprecomp::Runtime &runtime, float aspect) {
    if (game().view_aspect_frame != nullptr) game().view_aspect_frame(runtime, aspect);
}

// The game is aiming under the driver: the second stick goes to the game
// stretched to full length, so the game's aim code steps at any push and the
// driver can size each step.
[[nodiscard]] inline bool game_camera_aim_boost() {
    const CameraDriver *driver = game().camera;
    return driver != nullptr && driver->aim_boost != nullptr && driver->aim_boost();
}

// A direction for the second stick, each axis -1..1.
struct StickDirection {
    float x{};
    float y{};
};

// While the game aims under the driver and the mouse has moved, the direction,
// at full length, the second stick should show the game this sample, so the
// game's aim steps the mouse's way.
[[nodiscard]] inline std::optional<StickDirection> game_camera_mouse_aim() {
    const CameraDriver *driver = game().camera;
    StickDirection direction;
    if (driver == nullptr || driver->mouse_aim == nullptr || !driver->mouse_aim(direction.x, direction.y))
        return std::nullopt;
    return direction;
}

// Where the port does not drive the camera, the mouse can only switch the
// game's own turn: -1 or +1 while it moves left or right fast enough this
// frame, 0 otherwise (and always 0 for a game without a driver).
[[nodiscard]] inline int game_camera_mouse_stock_turn() {
    const CameraDriver *driver = game().camera;
    return driver != nullptr && driver->mouse_stock_turn != nullptr ? driver->mouse_stock_turn() : 0;
}

// Full-deflection speed for the current camera: the driver's (Aim speed while
// a game aims, say), or Camera speed.
[[nodiscard]] inline float game_camera_degrees_per_second() {
    const CameraDriver *driver = game().camera;
    if (driver != nullptr && driver->degrees_per_second != nullptr) return driver->degrees_per_second();
    return settings::current().camera_speed;
}

} // namespace portablekit::camera
