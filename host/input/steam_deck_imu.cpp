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
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <thread>
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

// Everything that touches the device runs on its own thread, so nothing here
// can hold up a frame: reads wait in poll() with a timeout, and the feature
// report that switches the motion unit (a USB control transfer the kernel may
// take a while over) is sent from there too. The main thread only takes the
// latest values under the mutex.
struct Deck {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool tried{};
    bool opened{};        // the thread runs and owns fd
    bool active{};        // tilt controls want readings
    bool quit{};
    int fd{-1};
    bool writable{};
    std::string path;
    // Written by the thread, taken by read().
    bool have{};          // motion data arrived since active
    tilt::Reading last;
    float gyro_sum[3]{};
    int gyro_count{};
    std::uint64_t reports{};
};

Deck &deck() {
    static Deck value;
    return value;
}

bool trace() {
    static const bool value = portablekit::env("TRACE_PAD") != nullptr || portablekit::env("TRACE_TILT") != nullptr;
    return value;
}

void say(const std::string &line) {
    // One write per line, as the thread logs beside the main thread.
    std::cout << (line + "\n") << std::flush;
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

// The reader thread only.
bool set_imu_mode(int fd, std::uint16_t mode) {
    // Report ID 0 (the device numbers none), then the message.
    std::uint8_t buffer[kReportBytes + 1]{};
    buffer[1] = kSetSettings;
    buffer[2] = 3;
    buffer[3] = kSettingImuMode;
    buffer[4] = static_cast<std::uint8_t>(mode & 0xFF);
    buffer[5] = static_cast<std::uint8_t>(mode >> 8);
    if (ioctl(fd, HIDIOCSFEATURE(sizeof(buffer)), buffer) < 0) {
        say(std::string("[tilt] Steam Deck: cannot switch its motion sensors ") + (mode != kImuOff ? "on" : "off") +
            ": " + std::strerror(errno));
        return false;
    }
    return true;
}

void run(Deck &d) {
    const int fd = d.fd;
    const bool writable = d.writable;
    bool switched_on = false;   // the mode was set here and not yet set back
    int switches = 0;
    bool was_active = false;
    bool announced = false;
    Clock::time_point since_data{};
    Clock::time_point last_switch{};
    std::uint8_t report[kReportBytes];
    for (;;) {
        bool active;
        {
            std::unique_lock lock(d.mutex);
            if (!d.active && !d.quit) {
                // Idle: switch the unit back off if it was switched on here,
                // then wait without touching the device.
                if (switched_on) {
                    lock.unlock();
                    if (set_imu_mode(fd, kImuOff)) say("[tilt] Steam Deck: switched its motion sensors back off");
                    switched_on = false;
                    lock.lock();
                }
                d.wake.wait(lock, [&] { return d.active || d.quit; });
            }
            if (d.quit) break;
            active = d.active;
        }
        const Clock::time_point now = Clock::now();
        if (active && !was_active) {
            // Drop what queued while idle.
            for (int i = 0; i < 256 && ::read(fd, report, sizeof(report)) > 0; ++i) {}
            since_data = now;
            last_switch = {};
            announced = false;
        }
        was_active = active;

        pollfd waiting{fd, POLLIN, 0};
        const int ready = ::poll(&waiting, 1, 100);
        if (ready < 0 && errno != EINTR) {
            say(std::string("[tilt] Steam Deck controller: ") + std::strerror(errno) + "; stopped reading it");
            break;
        }
        if (ready > 0 && (waiting.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            say("[tilt] Steam Deck controller went away; stopped reading it");
            break;
        }
        float gyro[3]{};
        float accel[3]{};
        int count = 0;
        // Bounded: the kernel keeps at most 64 reports, 250 arrive a second.
        for (int i = 0; ready > 0 && i < 256; ++i) {
            const ssize_t n = ::read(fd, report, sizeof(report));
            if (n <= 0) break;
            if (static_cast<std::size_t>(n) < kMotionEnd || report[0] != 0x01 || report[2] != kStateReport) continue;
            bool empty = true;
            for (std::size_t j = kAccel; j < kMotionEnd; ++j) empty = empty && report[j] == 0;
            if (empty) continue;
            // The controller's X, Y, Z in SDL's gamepad axes: X right, the
            // controller's Z as up out of its face, its -Y towards the player.
            const std::uint8_t *a = report + kAccel;
            const std::uint8_t *g = report + kGyro;
            accel[0] = le16(a) * kAccelScale;
            accel[1] = le16(a + 4) * kAccelScale;
            accel[2] = -le16(a + 2) * kAccelScale;
            gyro[0] += le16(g) * kGyroScale;
            gyro[1] += le16(g + 4) * kGyroScale;
            gyro[2] += -le16(g + 2) * kGyroScale;
            ++count;
        }
        const Clock::time_point after = Clock::now();
        if (count > 0) {
            since_data = after;
            if (!announced) {
                announced = true;
                std::string line = "[tilt] Steam Deck: motion data arriving";
                if (trace())
                    line += " (accel " + std::to_string(accel[0]) + ' ' + std::to_string(accel[1]) + ' ' +
                            std::to_string(accel[2]) + " m/s2)";
                say(line);
            }
            std::lock_guard lock(d.mutex);
            d.have = true;
            d.last.has_accel = true;
            std::memcpy(d.last.accel, accel, sizeof(accel));
            for (int i = 0; i < 3; ++i) d.gyro_sum[i] += gyro[i];
            d.gyro_count += count;
            d.reports += static_cast<std::uint64_t>(count);
        } else if (after - since_data >= kSilenceBeforeSwitch) {
            {
                std::lock_guard lock(d.mutex);
                d.have = false;
            }
            // Reports arrive with empty motion fields, or none at all: Steam
            // has the motion unit off. Once, and again whenever Steam turns
            // it back off.
            if (writable && (last_switch == Clock::time_point{} || after - last_switch >= kSwitchInterval)) {
                last_switch = after;
                if (set_imu_mode(fd, kImuRaw)) {
                    switched_on = true;
                    ++switches;
                    announced = false;
                    if (switches <= 3 || trace())
                        say(std::string("[tilt] Steam Deck: its motion sensors were off; switched them on") +
                            (switches > 1 ? " again (" + std::to_string(switches) + ")" : std::string{}));
                }
            }
        }
    }
    if (switched_on) set_imu_mode(fd, kImuOff);
    ::close(fd);
    std::lock_guard lock(d.mutex);
    d.have = false;
    d.fd = -1;
}

void stop_at_exit() {
    Deck &d = deck();
    {
        std::lock_guard lock(d.mutex);
        d.quit = true;
    }
    d.wake.notify_all();
    // Within a poll timeout, and the unit switched back off if it was
    // switched on here.
    if (d.thread.joinable()) d.thread.join();
}

} // namespace

bool present() {
    return !switched_off() && !find_device().empty();
}

bool open() {
    Deck &d = deck();
    if (d.opened) {
        {
            std::lock_guard lock(d.mutex);
            if (d.fd < 0) return false;  // the thread stopped: the device went away
            if (d.active) return true;
            d.active = true;
        }
        d.wake.notify_all();
        return true;
    }
    if (d.tried) return false;
    d.tried = true;
    if (switched_off()) return false;
    d.path = find_device();
    if (d.path.empty()) return false;
    d.fd = ::open(d.path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    d.writable = d.fd >= 0;
    if (d.fd < 0) d.fd = ::open(d.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (d.fd < 0) {
        say("[tilt] Steam Deck controller " + d.path + " cannot be read: " + std::strerror(errno));
        return false;
    }
    d.active = true;
    d.opened = true;
    d.thread = std::thread(run, std::ref(d));
    std::atexit(stop_at_exit);
    say("[tilt] Steam Deck controller " + d.path + ": reading its gyroscope and accelerometer beside Steam Input" +
        (d.writable ? "" : " (read only; the motion unit cannot be switched on)"));
    return true;
}

bool read(tilt::Reading &reading) {
    Deck &d = deck();
    std::lock_guard lock(d.mutex);
    if (d.fd < 0 || !d.active || !d.have) return false;
    // The mean rate over the reports since the last call; the last one again
    // when none arrived in between.
    if (d.gyro_count > 0) {
        for (int i = 0; i < 3; ++i) {
            d.last.gyro[i] = d.gyro_sum[i] / static_cast<float>(d.gyro_count);
            d.gyro_sum[i] = 0.0f;
        }
        d.gyro_count = 0;
        d.last.has_gyro = true;
    }
    reading = d.last;
    return true;
}

void close() {
    Deck &d = deck();
    {
        std::lock_guard lock(d.mutex);
        if (!d.active) return;
        d.active = false;
        d.have = false;
        d.gyro_count = 0;
        for (float &v : d.gyro_sum) v = 0.0f;
    }
    d.wake.notify_all();
}

std::string describe() {
    Deck &d = deck();
    std::lock_guard lock(d.mutex);
    if (d.fd < 0 || !d.active) return {};
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
