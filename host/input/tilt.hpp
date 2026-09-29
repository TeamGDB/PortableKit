#pragma once

#include <cstdint>

// Tilt controls: the device's roll, from its motion sensors, as a game's
// tilt buttons (LocoRoco tilts its world with L and R). Plain arithmetic, no
// SDL, so it is tested without hardware; input/motion.hpp reads the sensors.
//
// Axes are the screen's, as SDL gives them for a gamepad: +X to the right,
// +Y up, +Z towards the player. The accelerometer reads the reaction to
// gravity (a device at rest reads +9.8 m/s² straight up), the gyroscope
// radians a second, anticlockwise positive looking down each axis.
//
// Roll is the angle of the device's X axis below the horizon, in degrees:
// positive with the right side down. Measured that way it does not depend on
// how far the device is pitched towards the player, so a gamepad held flat
// and a phone or a Steam Deck held upright read the same.
namespace portablekit::input::tilt {

enum class Mode {
    // The roll from the calibrated neutral presses the tilt buttons.
    Angle,
    // As Angle, and the picture is turned against the roll so that the
    // game's horizon stays level with the real one (presentation only).
    AngleLevel,
    // Turning the device tilts: its rate of roll is added up into a tilt that
    // stays when the turning stops and fades back slowly. Turning slower than
    // kRateThreshold adds nothing, so the device can be brought back to a
    // comfortable hold without losing the tilt.
    Rate,
};

struct Tuning {
    Mode mode{Mode::Angle};
    // Roll, in degrees from neutral, at which the tilt is full.
    float full_tilt{12.0f};
    // Roll, in degrees from neutral, before anything is pressed.
    float dead_zone{4.0f};
    // Roll left presses right, and the other way round.
    bool invert{};
    // Angle + level horizon: the picture is turned at most this far, in
    // degrees; the further it turns, the more it is zoomed to cover the
    // screen, so the cap keeps the crop small.
    float level_limit{10.0f};
};

inline constexpr float kMinFullTilt = 5.0f;
inline constexpr float kMaxFullTilt = 45.0f;
inline constexpr float kMinDeadZone = 1.0f;
inline constexpr float kMaxDeadZone = 15.0f;
// A pressed side is released only this far inside the dead zone, so a hand
// resting at its edge does not flicker the button.
inline constexpr float kHysteresis = 1.5f;
// Rate mode: degrees a second of roll that count as holding still.
inline constexpr float kRateThreshold = 20.0f;
// Rate mode: seconds for the added-up tilt to fall to a third by itself.
inline constexpr float kRateDecaySeconds = 3.0f;
inline constexpr float kMinLevelLimit = 2.0f;
inline constexpr float kMaxLevelLimit = 20.0f;

// One reading of the sensors. Either may be missing: a pad with only a
// gyroscope, a phone with only an accelerometer.
struct Reading {
    bool has_accel{};
    float accel[3]{};  // m/s², screen axes
    bool has_gyro{};
    float gyro[3]{};   // rad/s, screen axes
};

// Fuses the accelerometer's roll, steady but shaken by every movement, with
// the gyroscope's rate of roll, smooth but drifting (a complementary filter).
class RollEstimator {
public:
    // Takes a reading `seconds` after the previous one.
    void update(const Reading &reading, float seconds);
    void reset() { *this = RollEstimator{}; }
    [[nodiscard]] bool valid() const { return valid_; }
    // Degrees, right side down positive.
    [[nodiscard]] float roll() const { return roll_; }
    // Degrees a second, the same sign.
    [[nodiscard]] float rate() const { return rate_; }

private:
    bool valid_{};
    float roll_{};
    float rate_{};
    // The last direction of "up" in the device's axes; straight up until an
    // accelerometer says otherwise.
    float up_[3]{0.0f, 1.0f, 0.0f};
};

// What the roll asks of the game.
struct Output {
    // -1 tilts left, +1 right, 0 neither.
    int direction{};
    // How far past the dead zone, 0 to 1 (1 at full tilt).
    float strength{};
    // For Angle + level horizon: degrees to turn the picture anticlockwise,
    // which is the roll from neutral up to Tuning::level_limit. 0 in the
    // other modes.
    float level_degrees{};
};

// The roll, from its neutral, as a direction to tilt: the dead zone, the
// hysteresis, inversion, re-centring and the rate mode.
class Mapper {
public:
    // The next roll taken becomes neutral (and Rate's tilt drops to none).
    void recenter() { recenter_ = true; }
    [[nodiscard]] Output update(float roll, float rate, float seconds, const Tuning &tuning);
    [[nodiscard]] float neutral() const { return neutral_; }

private:
    bool recenter_{true};
    float neutral_{};
    float accumulated_{};
    int held_{};
};

// Presses one of the tilt buttons for a direction and a strength, a frame at
// a time. At full strength the button is held; below it the button is held
// for that share of the frames, spread evenly, so a game that eases its tilt
// towards the pressed side sees part of the tilt. Strength 0 with a
// direction still presses a little, the dead zone having been passed.
class Pulse {
public:
    // True when the button is down this frame.
    [[nodiscard]] bool step(int direction, float strength, bool proportional);

private:
    float phase_{};
    int direction_{};
};

} // namespace portablekit::input::tilt
