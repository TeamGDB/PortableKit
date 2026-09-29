#include "../profile.hpp"
#include "input/motion.hpp"

#include "input/tilt.hpp"
#include "settings/settings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace portablekit::input::motion {
namespace {

constexpr float kDegreesPerRadian = 57.29577951308232f;
// The tilt buttons change at most once per PSP frame when pressed part of the
// time (TiltControls::proportional).
constexpr std::uint64_t kPulseNs = 16'683'000u;

struct State {
    SDL_Gamepad *pad{};
    bool pad_accel{};
    bool pad_gyro{};
    bool pad_enabled{};
    std::string pad_name;
    // The device's own sensors, opened the first time they are needed.
    bool device_opened{};
    SDL_Sensor *device_accel{};
    SDL_Sensor *device_gyro{};
    std::string device_names;

    bool on{};
    bool reading{};
    bool recenter_down{};
    tilt::RollEstimator estimator;
    tilt::Mapper mapper;
    tilt::Pulse pulse;
    tilt::Output output;
    std::uint64_t last_ns{};
    std::uint64_t pulse_ns{};
    bool pulse_down{};
    int traced_direction{};
    std::uint64_t traced_ns{};
};

State &state() {
    static State value;
    return value;
}

bool trace() {
    static const bool value = portablekit::env("TRACE_PAD") != nullptr || portablekit::env("TRACE_TILT") != nullptr;
    return value;
}

std::string pad_label(SDL_Gamepad *pad) {
    const char *name = SDL_GetGamepadName(pad);
    return name != nullptr ? name : "gamepad";
}

void enable_pad_sensors(State &s, bool enable) {
    if (s.pad == nullptr || s.pad_enabled == enable) return;
    if (s.pad_accel) SDL_SetGamepadSensorEnabled(s.pad, SDL_SENSOR_ACCEL, enable);
    if (s.pad_gyro) SDL_SetGamepadSensorEnabled(s.pad, SDL_SENSOR_GYRO, enable);
    s.pad_enabled = enable;
}

// A phone's sensors, and a computer's where it has any. Opened once, when
// tilt controls are first on without a gamepad that has its own.
void open_device_sensors(State &s) {
    if (s.device_opened) return;
    s.device_opened = true;
    if (!SDL_InitSubSystem(SDL_INIT_SENSOR)) {
        std::cout << "[tilt] no sensor support: " << SDL_GetError() << std::endl;
        return;
    }
    int count = 0;
    SDL_SensorID *ids = SDL_GetSensors(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        const SDL_SensorType type = SDL_GetSensorTypeForID(ids[i]);
        SDL_Sensor **slot = type == SDL_SENSOR_ACCEL ? &s.device_accel : type == SDL_SENSOR_GYRO ? &s.device_gyro
                                                                                               : nullptr;
        if (slot == nullptr || *slot != nullptr) continue;
        *slot = SDL_OpenSensor(ids[i]);
        if (*slot == nullptr) {
            std::cout << "[tilt] cannot open sensor " << ids[i] << ": " << SDL_GetError() << std::endl;
            continue;
        }
        const char *name = SDL_GetSensorName(*slot);
        if (!s.device_names.empty()) s.device_names += ", ";
        s.device_names += std::string(name != nullptr ? name : "sensor") +
                          (type == SDL_SENSOR_ACCEL ? " (accelerometer)" : " (gyroscope)");
    }
    SDL_free(ids);
    if (s.device_names.empty())
        std::cout << "[tilt] this device has no accelerometer or gyroscope of its own" << std::endl;
    else
        std::cout << "[tilt] device sensors: " << s.device_names << std::endl;
}

// Quarter turns anticlockwise of the display from the device's natural
// orientation, which is the frame a device's own sensors report in.
int display_quarter_turns(SDL_Window *window) {
    if (window == nullptr) return 0;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0) return 0;
    const auto turns = [](SDL_DisplayOrientation orientation) {
        switch (orientation) {
        case SDL_ORIENTATION_LANDSCAPE: return 1;          // right side up
        case SDL_ORIENTATION_PORTRAIT_FLIPPED: return 2;
        case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return 3;  // left side up
        default: return 0;
        }
    };
    const SDL_DisplayOrientation current = SDL_GetCurrentDisplayOrientation(display);
    if (current == SDL_ORIENTATION_UNKNOWN) return 0;
    return (turns(current) - turns(SDL_GetNaturalDisplayOrientation(display)) + 4) % 4;
}

// A vector in the device's natural axes, in the screen's.
void to_screen(float v[3], int quarter_turns) {
    const float x = v[0];
    const float y = v[1];
    switch (quarter_turns) {
    case 1: v[0] = -y; v[1] = x; break;
    case 2: v[0] = -x; v[1] = -y; break;
    case 3: v[0] = y; v[1] = -x; break;
    default: break;
    }
}

// The pad's sensors when it has them, else the device's.
bool read_sensors(State &s, SDL_Window *window, tilt::Reading &reading) {
    if (s.pad != nullptr && (s.pad_accel || s.pad_gyro)) {
        reading.has_accel = s.pad_accel && SDL_GetGamepadSensorData(s.pad, SDL_SENSOR_ACCEL, reading.accel, 3);
        reading.has_gyro = s.pad_gyro && SDL_GetGamepadSensorData(s.pad, SDL_SENSOR_GYRO, reading.gyro, 3);
        return reading.has_accel || reading.has_gyro;
    }
    open_device_sensors(s);
    if (s.device_accel == nullptr && s.device_gyro == nullptr) return false;
    const int turns = display_quarter_turns(window);
    if (s.device_accel != nullptr) {
        reading.has_accel = SDL_GetSensorData(s.device_accel, reading.accel, 3);
        if (reading.has_accel) to_screen(reading.accel, turns);
    }
    if (s.device_gyro != nullptr) {
        reading.has_gyro = SDL_GetSensorData(s.device_gyro, reading.gyro, 3);
        if (reading.has_gyro) to_screen(reading.gyro, turns);
    }
    return reading.has_accel || reading.has_gyro;
}

tilt::Tuning tuning_now() {
    const settings::Settings &player = settings::current();
    tilt::Tuning tuning;
    tuning.mode = player.tilt_mode;
    tuning.full_tilt = player.tilt_full;
    tuning.dead_zone = player.tilt_dead_zone;
    tuning.invert = player.tilt_invert;
    tuning.level_limit = player.tilt_level_limit;
    return tuning;
}

} // namespace

void gamepad_changed(SDL_Gamepad *pad) {
    State &s = state();
    if (s.pad == pad) return;
    s.pad = pad;
    s.pad_enabled = false;
    s.pad_accel = pad != nullptr && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    s.pad_gyro = pad != nullptr && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
    s.pad_name = pad != nullptr ? pad_label(pad) : std::string{};
    // Another sensor, another neutral.
    s.estimator.reset();
    s.mapper.recenter();
    if (portablekit::game().tilt == nullptr || pad == nullptr) return;
    if (s.pad_accel || s.pad_gyro) {
        std::cout << "[tilt] " << s.pad_name << ": "
                  << (s.pad_gyro && s.pad_accel ? "gyroscope and accelerometer"
                      : s.pad_gyro              ? "gyroscope only"
                                                : "accelerometer only")
                  << " (" << SDL_GetGamepadSensorDataRate(pad, s.pad_gyro ? SDL_SENSOR_GYRO : SDL_SENSOR_ACCEL)
                  << " Hz)" << std::endl;
    } else {
        std::cout << "[tilt] " << s.pad_name << " has no motion sensors";
        // Steam Input hands the game a virtual pad without them, the Steam
        // Deck's own included.
        const SDL_GamepadType type = SDL_GetGamepadType(pad);
        if (s.pad_name.find("Steam") != std::string::npos || type == SDL_GAMEPAD_TYPE_XBOX360)
            std::cout << "; under Steam Input, turn Steam Input off for this game to use the pad's own gyroscope, "
                         "or map the gyroscope to a stick in Steam's controller settings";
        std::cout << std::endl;
    }
}

std::uint32_t sample(SDL_Gamepad *pad, SDL_Window *window, bool recenter_button) {
    State &s = state();
    const TiltControls *controls = portablekit::game().tilt;
    const bool on = controls != nullptr && settings::current().tilt;
    if (pad != s.pad) gamepad_changed(pad);
    if (!on) {
        if (s.on) {
            enable_pad_sensors(s, false);
            s.on = false;
            s.reading = false;
            s.output = {};
        }
        return 0u;
    }
    if (!s.on) {
        s.on = true;
        s.estimator.reset();
        s.mapper.recenter();
        s.last_ns = 0u;
    }
    enable_pad_sensors(s, true);

    if (recenter_button && !s.recenter_down) {
        s.mapper.recenter();
        if (trace()) std::cout << "[tilt] re-centred by the gamepad" << std::endl;
    }
    s.recenter_down = recenter_button;

    const std::uint64_t now = SDL_GetTicksNS();
    const float seconds = s.last_ns == 0u ? 0.0f : static_cast<float>(now - s.last_ns) * 1e-9f;
    s.last_ns = now;
    tilt::Reading reading;
    s.reading = read_sensors(s, window, reading);
    if (!s.reading) {
        s.output = {};
        return 0u;
    }
    s.estimator.update(reading, seconds);
    if (!s.estimator.valid()) return 0u;
    const tilt::Tuning tuning = tuning_now();
    s.output = s.mapper.update(s.estimator.roll(), s.estimator.rate(), seconds, tuning);

    bool down = s.output.direction != 0;
    if (controls->proportional) {
        if (s.output.direction != s.traced_direction || now - s.pulse_ns >= kPulseNs) {
            s.pulse_down = s.pulse.step(s.output.direction, s.output.strength, true);
            s.pulse_ns = now;
        }
        down = s.pulse_down;
    }
    if (trace() && (s.output.direction != s.traced_direction || now - s.traced_ns >= 1'000'000'000u)) {
        std::cout << "[tilt] roll " << std::lround(s.estimator.roll() * 10.0f) / 10.0f << " neutral "
                  << std::lround(s.mapper.neutral() * 10.0f) / 10.0f << " rate " << std::lround(s.estimator.rate())
                  << " deg/s -> "
                  << (s.output.direction < 0 ? "left" : s.output.direction > 0 ? "right" : "none")
                  << " strength " << std::lround(s.output.strength * 100.0f) << "%";
        if (tuning.mode == tilt::Mode::AngleLevel) std::cout << " level " << s.output.level_degrees;
        std::cout << std::endl;
        s.traced_ns = now;
    }
    s.traced_direction = s.output.direction;
    if (!down) return 0u;
    return s.output.direction < 0 ? controls->left : controls->right;
}

void recenter() { state().mapper.recenter(); }

float level_degrees() {
    const State &s = state();
    return s.on && s.reading ? s.output.level_degrees : 0.0f;
}

std::string source() {
    State &s = state();
    if (s.pad != nullptr && (s.pad_accel || s.pad_gyro))
        return s.pad_name + (s.pad_gyro && s.pad_accel ? ": gyroscope and accelerometer"
                             : s.pad_gyro              ? ": gyroscope only"
                                                       : ": accelerometer only");
    if (!s.device_names.empty()) return "This device: " + s.device_names;
    if (s.pad != nullptr) return s.pad_name + " has no motion sensors";
    if (s.device_opened) return "No motion sensors found";
    return "Not read yet";
}

bool reading(float &roll_from_neutral) {
    const State &s = state();
    if (!s.on || !s.reading || !s.estimator.valid()) return false;
    roll_from_neutral = s.estimator.roll() - s.mapper.neutral();
    return true;
}

} // namespace portablekit::input::motion
