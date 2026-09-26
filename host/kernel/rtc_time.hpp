#pragma once

// The PSP's real-time clock arithmetic (sceRtc), with no guest memory: a
// tick is microseconds since 0001-01-01 00:00:00 in the proleptic Gregorian
// calendar, and a date-time is ScePspDateTime's fields.

#include <cstdint>
#include <optional>

namespace portablekit::rtc {

struct DateTime {
    int year{1}, month{1}, day{1}, hour{}, minute{}, second{};
    std::uint32_t microsecond{};
};

inline constexpr std::uint64_t kTicksPerSecond = 1'000'000ull;
inline constexpr std::uint64_t kTicksPerDay = 86'400ull * kTicksPerSecond;
// 0001-01-01 to 1970-01-01.
inline constexpr std::uint64_t kUnixEpochTick = 719'162ull * kTicksPerDay;

[[nodiscard]] bool is_leap_year(int year) noexcept;
// 0 for an invalid month.
[[nodiscard]] int days_in_month(int year, int month) noexcept;
// 0 Sunday .. 6 Saturday.
[[nodiscard]] int day_of_week(int year, int month, int day) noexcept;
// Days from 0001-01-01 to the date.
[[nodiscard]] std::int64_t days_from_civil(int year, int month, int day) noexcept;

// Empty when a field is out of range (sceRtcCheckValid's checks).
[[nodiscard]] std::optional<std::uint64_t> to_tick(const DateTime &date) noexcept;
[[nodiscard]] DateTime from_tick(std::uint64_t tick) noexcept;

} // namespace portablekit::rtc
