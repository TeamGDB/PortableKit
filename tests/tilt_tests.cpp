// Tilt controls (input/tilt.hpp): the device's roll as tilt buttons, without
// SDL, sensors or game data.
#include "input/tilt.hpp"

#include <cmath>
#include <iostream>

namespace {
using namespace portablekit::input::tilt;
int failures{};

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr float kPi = 3.14159265358979f;
constexpr float kG = 9.80665f;
constexpr float kFrame = 1.0f / 60.0f;

// What the accelerometer reads with the device rolled `roll` degrees (right
// side down) and pitched `pitch` degrees back from flat towards the player.
Reading accel_at(float roll, float pitch) {
    const float r = roll * kPi / 180.0f;
    const float p = pitch * kPi / 180.0f;
    Reading reading;
    reading.has_accel = true;
    // "Up" in the device's axes: the X axis dips by the roll, and what is
    // left of up is shared between Y and Z by the pitch.
    reading.accel[0] = -std::sin(r) * kG;
    reading.accel[1] = std::cos(r) * std::cos(p) * kG;
    reading.accel[2] = std::cos(r) * std::sin(p) * kG;
    return reading;
}

void test_roll_from_accelerometer() {
    for (const float pitch : {0.0f, 30.0f, 60.0f, 85.0f}) {
        RollEstimator estimator;
        estimator.update(accel_at(10.0f, pitch), kFrame);
        check(estimator.valid(), "one accelerometer reading is enough");
        check(std::fabs(estimator.roll() - 10.0f) < 0.01f, "roll does not depend on the pitch");
    }
    RollEstimator estimator;
    estimator.update(accel_at(-8.0f, 45.0f), kFrame);
    check(estimator.roll() < -7.9f && estimator.roll() > -8.1f, "left side down is negative");
    RollEstimator falling;
    Reading nothing;
    nothing.has_accel = true;
    falling.update(nothing, kFrame);
    check(!falling.valid(), "a zero reading says nothing");
}

void test_roll_follows_accelerometer() {
    RollEstimator estimator;
    estimator.update(accel_at(0.0f, 40.0f), kFrame);
    for (int i = 0; i < 60; ++i) estimator.update(accel_at(12.0f, 40.0f), kFrame);
    check(std::fabs(estimator.roll() - 12.0f) < 0.5f, "a steady roll is reached within a second");
    check(estimator.rate() > -1.0f && estimator.rate() < 1.0f, "and its rate settles");
}

void test_gyro_rate() {
    // Flat gamepad (up is +Y): turning clockwise as the player sees it is a
    // negative rate about +Z and rolls the right side down.
    RollEstimator estimator;
    Reading reading = accel_at(0.0f, 0.0f);
    estimator.update(reading, kFrame);
    reading.has_gyro = true;
    reading.has_accel = false;
    reading.gyro[2] = -30.0f * kPi / 180.0f;
    for (int i = 0; i < 30; ++i) estimator.update(reading, kFrame);
    check(std::fabs(estimator.rate() - 30.0f) < 0.1f, "clockwise about the player's view is positive roll");
    check(std::fabs(estimator.roll() - 15.0f) < 0.5f, "half a second at 30 deg/s rolls 15 degrees");

    // Held upright (up is +Z-ish, a phone or a Deck at 70 degrees): the same
    // turn about the screen's normal is still roll.
    RollEstimator upright;
    Reading tilted = accel_at(0.0f, 70.0f);
    upright.update(tilted, kFrame);
    const float p = 70.0f * kPi / 180.0f;
    // A turn about the axis towards the player, which is pitched with the
    // device: its components in the device's axes.
    tilted.has_gyro = true;
    tilted.has_accel = false;
    tilted.gyro[1] = 0.0f;
    tilted.gyro[2] = -30.0f * kPi / 180.0f;
    upright.update(tilted, kFrame);
    check(upright.rate() > 30.0f * std::cos(p) * 0.99f, "a turn about the screen's normal rolls when upright");
}

void test_fusion_rejects_shake() {
    // A shake that the gyroscope says is not a turn barely moves the roll.
    RollEstimator estimator;
    Reading still = accel_at(0.0f, 30.0f);
    still.has_gyro = true;
    estimator.update(still, kFrame);
    Reading shaken = still;
    shaken.accel[0] = -0.5f * kG;  // a sideways jolt: 30 degrees if taken as roll
    estimator.update(shaken, kFrame);
    check(std::fabs(estimator.roll()) < 3.0f, "one jolt moves the fused roll only a little");
}

void test_dead_zone_and_hysteresis() {
    Mapper mapper;
    const Tuning tuning{Mode::Angle, 12.0f, 4.0f, false};
    check(mapper.update(0.0f, 0.0f, kFrame, tuning).direction == 0, "neutral presses nothing");
    check(mapper.update(3.9f, 0.0f, kFrame, tuning).direction == 0, "inside the dead zone presses nothing");
    Output out = mapper.update(4.5f, 0.0f, kFrame, tuning);
    check(out.direction == 1, "past the dead zone to the right presses right");
    check(out.strength > 0.0f && out.strength < 0.1f, "just past it is a weak tilt");
    check(mapper.update(3.0f, 0.0f, kFrame, tuning).direction == 1, "a little back inside still holds");
    check(mapper.update(2.4f, 0.0f, kFrame, tuning).direction == 0, "past the hysteresis it lets go");
    check(mapper.update(3.9f, 0.0f, kFrame, tuning).direction == 0, "and needs the dead zone again");
    out = mapper.update(-20.0f, 0.0f, kFrame, tuning);
    check(out.direction == -1 && out.strength == 1.0f, "far to the left is full left");
    check(mapper.update(5.0f, 0.0f, kFrame, tuning).direction == 1, "crossing over changes side at once");
    // A dead zone smaller than the hysteresis still lets go.
    Mapper small;
    const Tuning narrow{Mode::Angle, 12.0f, 1.0f, false};
    check(small.update(0.0f, 0.0f, kFrame, narrow).direction == 0, "neutral");
    check(small.update(1.2f, 0.0f, kFrame, narrow).direction == 1, "a narrow dead zone presses");
    check(small.update(0.2f, 0.0f, kFrame, narrow).direction == 0, "and lets go near neutral");
}

void test_recenter_and_invert() {
    Mapper mapper;
    Tuning tuning{Mode::Angle, 12.0f, 4.0f, false};
    // The first roll seen is neutral: a player holding the device at 20
    // degrees presses nothing.
    check(mapper.update(20.0f, 0.0f, kFrame, tuning).direction == 0, "the first roll is neutral");
    check(std::fabs(mapper.neutral() - 20.0f) < 0.001f, "and remembered");
    check(mapper.update(26.0f, 0.0f, kFrame, tuning).direction == 1, "rolls count from it");
    mapper.recenter();
    check(mapper.update(26.0f, 0.0f, kFrame, tuning).direction == 0, "re-centring makes the hold neutral");
    tuning.invert = true;
    check(mapper.update(31.0f, 0.0f, kFrame, tuning).direction == -1, "inverted, right presses left");
}

void test_level_horizon() {
    Mapper mapper;
    const Tuning level{Mode::AngleLevel, 12.0f, 4.0f, false};
    (void)mapper.update(10.0f, 0.0f, kFrame, level);
    Output out = mapper.update(17.0f, 0.0f, kFrame, level);
    check(std::fabs(out.level_degrees - 7.0f) < 0.001f, "the picture turns by the roll from neutral");
    check(out.direction == 1, "and the tilt still presses");
    out = mapper.update(80.0f, 0.0f, kFrame, level);
    check(out.level_degrees == 10.0f, "never further than the limit, 10 degrees by default");
    Tuning wider = level;
    wider.level_limit = 15.0f;
    check(mapper.update(80.0f, 0.0f, kFrame, wider).level_degrees == 15.0f, "the limit is tunable");
    wider.level_limit = 90.0f;
    check(mapper.update(80.0f, 0.0f, kFrame, wider).level_degrees == kMaxLevelLimit, "within its own bounds");
    Mapper plain;
    const Tuning angle{Mode::Angle, 12.0f, 4.0f, false};
    (void)plain.update(0.0f, 0.0f, kFrame, angle);
    check(plain.update(10.0f, 0.0f, kFrame, angle).level_degrees == 0.0f, "Angle alone leaves the picture");
}

void test_rate_mode() {
    Mapper mapper;
    const Tuning rate{Mode::Rate, 12.0f, 4.0f, false};
    (void)mapper.update(0.0f, 0.0f, kFrame, rate);
    // A quick turn: 0.25 s at 80 deg/s.
    Output out;
    for (int i = 0; i < 15; ++i) out = mapper.update(0.0f, 80.0f, kFrame, rate);
    check(out.direction == 1, "a quick turn to the right tilts right");
    // Bringing the device back slowly adds nothing.
    for (int i = 0; i < 60; ++i) out = mapper.update(0.0f, -15.0f, kFrame, rate);
    check(out.direction == 1, "a slow return keeps the tilt");
    // Held still, it fades.
    for (int i = 0; i < 60 * 8; ++i) out = mapper.update(0.0f, 0.0f, kFrame, rate);
    check(out.direction == 0, "held still, the tilt fades");
    // A quick turn the other way tilts left at once.
    for (int i = 0; i < 15; ++i) out = mapper.update(0.0f, -80.0f, kFrame, rate);
    check(out.direction == -1, "a quick turn to the left tilts left");
    mapper.recenter();
    check(mapper.update(0.0f, 0.0f, kFrame, rate).direction == 0, "re-centring drops the tilt");
    // Slow drift alone never tilts.
    Mapper drift;
    for (int i = 0; i < 600; ++i) out = drift.update(0.0f, 19.0f, kFrame, rate);
    check(out.direction == 0, "turning slower than the threshold never tilts");
}

void test_pulse() {
    Pulse pulse;
    int down = 0;
    for (int i = 0; i < 100; ++i) down += pulse.step(1, 1.0f, true) ? 1 : 0;
    check(down == 100, "full strength holds the button");
    Pulse half;
    down = 0;
    for (int i = 0; i < 100; ++i) down += half.step(1, 0.5f, true) ? 1 : 0;
    check(down >= 58 && down <= 62, "half strength presses about 60 frames in 100");
    Pulse fresh;
    check(fresh.step(-1, 0.0f, true), "a new press starts down");
    check(!fresh.step(0, 0.0f, true), "no direction presses nothing");
    Pulse digital;
    down = 0;
    for (int i = 0; i < 100; ++i) down += digital.step(1, 0.1f, false) ? 1 : 0;
    check(down == 100, "not proportional, any tilt holds the button");
}

} // namespace

int main() {
    test_roll_from_accelerometer();
    test_roll_follows_accelerometer();
    test_gyro_rate();
    test_fusion_rejects_shake();
    test_dead_zone_and_hysteresis();
    test_recenter_and_invert();
    test_level_horizon();
    test_rate_mode();
    test_pulse();
    if (failures != 0) {
        std::cerr << failures << " tilt test(s) failed\n";
        return 1;
    }
    std::cout << "tilt tests passed\n";
    return 0;
}
