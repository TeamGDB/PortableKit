#pragma once

#include "profile.hpp"

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

} // namespace portablekit::camera
