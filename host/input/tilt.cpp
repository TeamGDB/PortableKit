#include "input/tilt.hpp"

#include <algorithm>
#include <cmath>

namespace portablekit::input::tilt {
namespace {

constexpr float kDegrees = 57.29577951308232f;
constexpr float kGravity = 9.80665f;
// Seconds over which the accelerometer corrects the gyroscope's drift.
constexpr float kFusionSeconds = 0.25f;
// Seconds of smoothing on the accelerometer alone, when there is no gyroscope.
constexpr float kAccelOnlySeconds = 0.08f;

float blend(float seconds, float time_constant) {
    if (seconds <= 0.0f) return 0.0f;
    return seconds / (time_constant + seconds);
}

} // namespace

void RollEstimator::update(const Reading &reading, float seconds) {
    seconds = std::clamp(seconds, 0.0f, 0.25f);
    bool have_accel_roll = false;
    float accel_roll = 0.0f;
    if (reading.has_accel) {
        const float x = reading.accel[0];
        const float y = reading.accel[1];
        const float z = reading.accel[2];
        const float length = std::sqrt(x * x + y * y + z * z);
        // Free fall or a sensor still starting says nothing about "up".
        if (length > 0.3f * kGravity) {
            up_[0] = x / length;
            up_[1] = y / length;
            up_[2] = z / length;
            accel_roll = std::asin(std::clamp(-up_[0], -1.0f, 1.0f)) * kDegrees;
            have_accel_roll = true;
        }
    }
    bool have_gyro_rate = false;
    float gyro_rate = 0.0f;
    if (reading.has_gyro) {
        // The X axis's height above the horizon is asin(x·up); up turns in
        // the device's axes as -ω × up, so the height changes at
        // -ω·(up × x) / cos(height), and roll is minus the height.
        const float cosine = std::sqrt(std::max(1.0f - up_[0] * up_[0], 0.0f));
        if (cosine > 0.2f) {
            gyro_rate = (reading.gyro[1] * up_[2] - reading.gyro[2] * up_[1]) / cosine * kDegrees;
            have_gyro_rate = true;
        }
    }
    if (!valid_) {
        if (!have_accel_roll && !have_gyro_rate) return;
        // Without an accelerometer the start is taken as level; re-centring
        // makes it so anyway.
        roll_ = have_accel_roll ? accel_roll : 0.0f;
        rate_ = have_gyro_rate ? gyro_rate : 0.0f;
        valid_ = true;
        return;
    }
    const float previous = roll_;
    if (have_gyro_rate) {
        roll_ += gyro_rate * seconds;
        if (have_accel_roll) roll_ += (accel_roll - roll_) * blend(seconds, kFusionSeconds);
        rate_ = gyro_rate;
    } else if (have_accel_roll) {
        roll_ += (accel_roll - roll_) * blend(seconds, kAccelOnlySeconds);
        rate_ = seconds > 0.0f ? (roll_ - previous) / seconds : 0.0f;
    }
    roll_ = std::clamp(roll_, -90.0f, 90.0f);
}

Output Mapper::update(float roll, float rate, float seconds, const Tuning &tuning) {
    const float full = std::clamp(tuning.full_tilt, kMinFullTilt, kMaxFullTilt);
    const float dead = std::clamp(tuning.dead_zone, kMinDeadZone, std::min(kMaxDeadZone, full - 1.0f));
    if (recenter_) {
        recenter_ = false;
        neutral_ = roll;
        accumulated_ = 0.0f;
        held_ = 0;
    }
    Output out;
    const float from_neutral = roll - neutral_;
    float angle = from_neutral;
    if (tuning.mode == Mode::Rate) {
        // Only turning faster than holding still counts, by how much faster.
        const float speed = std::fabs(rate);
        if (speed > kRateThreshold) accumulated_ += std::copysign(speed - kRateThreshold, rate) * seconds;
        if (seconds > 0.0f) accumulated_ *= std::exp(-seconds / kRateDecaySeconds);
        // No further than a little past full, so tilting back acts at once.
        accumulated_ = std::clamp(accumulated_, -full * 1.25f, full * 1.25f);
        angle = accumulated_;
    } else if (tuning.mode == Mode::AngleLevel) {
        const float limit = std::clamp(tuning.level_limit, kMinLevelLimit, kMaxLevelLimit);
        out.level_degrees = std::clamp(from_neutral, -limit, limit);
    }
    if (tuning.invert) angle = -angle;

    const float magnitude = std::fabs(angle);
    const int side = angle > 0.0f ? 1 : -1;
    const float release = std::max(dead - kHysteresis, dead * 0.5f);
    if (held_ != 0 && (side != held_ || magnitude < release)) held_ = 0;
    if (held_ == 0 && magnitude > dead) held_ = side;
    out.direction = held_;
    if (held_ != 0) out.strength = std::clamp((magnitude - dead) / (full - dead), 0.0f, 1.0f);
    return out;
}

bool Pulse::step(int direction, float strength, bool proportional) {
    if (direction != direction_) {
        direction_ = direction;
        // A new press starts with the button down.
        phase_ = 1.0f;
    }
    if (direction == 0) return false;
    if (!proportional) return true;
    // Just past the dead zone still presses one frame in five.
    const float duty = std::clamp(0.2f + 0.8f * strength, 0.2f, 1.0f);
    phase_ += duty;
    if (phase_ >= 1.0f) {
        phase_ -= 1.0f;
        return true;
    }
    return false;
}

} // namespace portablekit::input::tilt
