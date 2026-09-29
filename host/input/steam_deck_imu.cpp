#include "../profile.hpp"
#include "input/steam_deck_imu.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#if defined(__linux__) && !defined(__ANDROID__)
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#endif

namespace portablekit::input::steam_deck_imu {
namespace {

[[maybe_unused]] bool switched_off() {
    const char *value = portablekit::env("TILT_DECK");
    return value != nullptr && (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0);
}

} // namespace

#if defined(__linux__) && !defined(__ANDROID__)
namespace {

// The Deck's built-in controller, and the interface of it that sends the
// controller's state: the one whose report descriptor starts with the
// vendor-defined usage page 0xFFFF (the other two are its keyboard and mouse).
constexpr const char *kDeckHidId = "000028DE:00001205";

// Its state report, as read on a Deck: a 4-byte header (01 00, type 09,
// length 0x40), then the packet number (u32) at 4, the buttons (u64) at 8,
// the two trackpads at 16, then little-endian int16 accelerometer X, Y, Z
// at 24, gyroscope X, Y, Z at 30 and an orientation quaternion at 36. 250
// reports a second.
constexpr std::size_t kReportBytes = 64;
constexpr std::uint8_t kStateReport = 0x09;
constexpr std::size_t kAccel = 24;
constexpr std::size_t kGyro = 30;
constexpr std::size_t kMotionEnd = 44;
// Full scale: ±2 g and ±2000 degrees a second over the int16 range.
constexpr float kAccelScale = 2.0f * 9.80665f / 32768.0f;
constexpr float kGyroScale = 2000.0f / 32768.0f * 0.017453292519943295f;

// The controller's settings: feature message 0x87 with (setting u8, value
// u16) triples; setting 48 is the motion unit's mode, and 0x18 asks for raw
// accelerometer (0x08) and raw gyroscope (0x10) data.
constexpr std::uint8_t kSetSettings = 0x87;
constexpr std::uint8_t kSettingImuMode = 48;
constexpr std::uint16_t kImuRaw = 0x18;
constexpr std::uint16_t kImuOff = 0x00;

// How long the motion fields may stay empty before the unit is switched on,
// and between two attempts.
constexpr auto kSilenceBeforeSwitch = std::chrono::milliseconds(300);
constexpr auto kSwitchInterval = std::chrono::seconds(1);

using Clock = std::chrono::steady_clock;

struct Deck {
    bool tried{};
    int fd{-1};
    bool writable{};
    std::string path;
    bool switched_on{};   // the mode was set here and not yet set back
    int switches{};
    bool have{};          // a report with motion data has been read
    std::uint64_t reports{};
    tilt::Reading last;
    Clock::time_point since_data{};
    Clock::time_point last_switch{};
};

Deck &deck() {
    static Deck value;
    return value;
}

bool trace() {
    static const bool value = portablekit::env("TRACE_PAD") != nullptr || portablekit::env("TRACE_TILT") != nullptr;
    return value;
}

std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// /dev/hidrawN of the Deck controller's state interface, or empty.
std::string find_device() {
    DIR *dir = opendir("/sys/class/hidraw");
    if (dir == nullptr) return {};
    std::string found;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("hidraw", 0) != 0) continue;
        const std::string base = "/sys/class/hidraw/" + name + "/device/";
        std::string uevent = read_file(base + "uevent");
        for (char &c : uevent) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (uevent.find(kDeckHidId) == std::string::npos) continue;
        const std::string descriptor = read_file(base + "report_descriptor");
        if (descriptor.size() < 3 || static_cast<std::uint8_t>(descriptor[0]) != 0x06 ||
            static_cast<std::uint8_t>(descriptor[1]) != 0xFF || static_cast<std::uint8_t>(descriptor[2]) != 0xFF)
            continue;
        found = "/dev/" + name;
        break;
    }
    closedir(dir);
    return found;
}

std::int16_t le16(const std::uint8_t *p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
}

bool set_imu_mode(Deck &d, std::uint16_t mode) {
    if (d.fd < 0 || !d.writable) return false;
    // Report ID 0 (the device numbers none), then the message.
    std::uint8_t buffer[kReportBytes + 1]{};
    buffer[1] = kSetSettings;
    buffer[2] = 3;
    buffer[3] = kSettingImuMode;
    buffer[4] = static_cast<std::uint8_t>(mode & 0xFF);
    buffer[5] = static_cast<std::uint8_t>(mode >> 8);
    if (ioctl(d.fd, HIDIOCSFEATURE(sizeof(buffer)), buffer) < 0) {
        std::cout << "[tilt] Steam Deck: cannot switch its motion sensors " << (mode != kImuOff ? "on" : "off")
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

void restore_at_exit() {
    Deck &d = deck();
    if (d.switched_on) set_imu_mode(d, kImuOff);
}

} // namespace

bool present() {
    return !switched_off() && !find_device().empty();
}

bool open() {
    Deck &d = deck();
    if (d.fd >= 0) return true;
    if (d.tried) return false;
    d.tried = true;
    if (switched_off()) return false;
    d.path = find_device();
    if (d.path.empty()) return false;
    d.fd = ::open(d.path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    d.writable = d.fd >= 0;
    if (d.fd < 0) d.fd = ::open(d.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (d.fd < 0) {
        std::cout << "[tilt] Steam Deck controller " << d.path << " cannot be read: " << std::strerror(errno)
                  << std::endl;
        return false;
    }
    d.have = false;
    d.reports = 0;
    d.since_data = Clock::now();
    d.last_switch = {};
    static bool registered = false;
    if (!registered) {
        registered = true;
        std::atexit(restore_at_exit);
    }
    std::cout << "[tilt] Steam Deck controller " << d.path << ": reading its gyroscope and accelerometer beside "
              << "Steam Input" << (d.writable ? "" : " (read only; the motion unit cannot be switched on)")
              << std::endl;
    return true;
}

bool read(tilt::Reading &reading) {
    Deck &d = deck();
    if (d.fd < 0) return false;
    std::uint8_t report[kReportBytes];
    float gyro[3]{};
    int gyros = 0;
    bool accel = false;
    for (;;) {
        const ssize_t n = ::read(d.fd, report, sizeof(report));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cout << "[tilt] Steam Deck controller: " << std::strerror(errno) << "; stopped reading it"
                          << std::endl;
                close();
                return false;
            }
            break;
        }
        if (static_cast<std::size_t>(n) < kMotionEnd || report[0] != 0x01 || report[2] != kStateReport) continue;
        ++d.reports;
        bool empty = true;
        for (std::size_t i = kAccel; i < kMotionEnd; ++i) empty = empty && report[i] == 0;
        if (empty) continue;
        // The controller's X, Y, Z in SDL's gamepad axes: X right, the
        // controller's Z as up out of its face, its -Y towards the player.
        const std::uint8_t *a = report + kAccel;
        const std::uint8_t *g = report + kGyro;
        d.last.accel[0] = le16(a) * kAccelScale;
        d.last.accel[1] = le16(a + 4) * kAccelScale;
        d.last.accel[2] = -le16(a + 2) * kAccelScale;
        accel = true;
        gyro[0] += le16(g) * kGyroScale;
        gyro[1] += le16(g + 4) * kGyroScale;
        gyro[2] += -le16(g + 2) * kGyroScale;
        ++gyros;
    }
    const Clock::time_point now = Clock::now();
    if (accel) {
        if (!d.have) {
            std::cout << "[tilt] Steam Deck: motion data arriving" << std::endl;
            if (trace())
                std::cout << "[tilt] Steam Deck accel " << d.last.accel[0] << ' ' << d.last.accel[1] << ' '
                          << d.last.accel[2] << " m/s2" << std::endl;
        }
        d.have = true;
        d.since_data = now;
        d.last.has_accel = true;
        d.last.has_gyro = gyros > 0;
        for (int i = 0; i < 3; ++i) d.last.gyro[i] = gyros > 0 ? gyro[i] / static_cast<float>(gyros) : 0.0f;
    } else if (now - d.since_data >= kSilenceBeforeSwitch && d.writable &&
               (d.last_switch == Clock::time_point{} || now - d.last_switch >= kSwitchInterval)) {
        // Reports arrive with empty motion fields: Steam has the motion
        // unit off. Once, and again whenever Steam turns it back off.
        d.last_switch = now;
        if (set_imu_mode(d, kImuRaw)) {
            d.switched_on = true;
            ++d.switches;
            if (d.switches <= 3 || trace())
                std::cout << "[tilt] Steam Deck: its motion sensors were off; switched them on"
                          << (d.switches > 1 ? " again (" + std::to_string(d.switches) + ")" : std::string{})
                          << std::endl;
        }
        d.have = false;
    }
    if (!d.have) return false;
    reading = d.last;
    return true;
}

void close() {
    Deck &d = deck();
    if (d.fd < 0) return;
    if (d.switched_on && set_imu_mode(d, kImuOff))
        std::cout << "[tilt] Steam Deck: switched its motion sensors back off" << std::endl;
    d.switched_on = false;
    ::close(d.fd);
    d.fd = -1;
    d.have = false;
    // Opened again when tilt controls come back on.
    d.tried = false;
}

std::string describe() {
    const Deck &d = deck();
    if (d.fd < 0) return {};
    if (d.have) return "Steam Deck: gyroscope and accelerometer";
    return d.writable ? "Steam Deck: waiting for its motion sensors" : "Steam Deck: its motion sensors are off";
}

#else

bool present() { return false; }
bool open() { return false; }
bool read(tilt::Reading &) { return false; }
void close() {}
std::string describe() { return {}; }

#endif

} // namespace portablekit::input::steam_deck_imu
