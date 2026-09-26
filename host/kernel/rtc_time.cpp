#include "rtc_time.hpp"

namespace portablekit::rtc {

bool is_leap_year(int year) noexcept { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int days_in_month(int year, int month) noexcept {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return kDays[month - 1] + (month == 2 && is_leap_year(year) ? 1 : 0);
}

std::int64_t days_from_civil(int year, int month, int day) noexcept {
    // Whole years, then whole months of this year, then days.
    const std::int64_t y = year - 1;
    std::int64_t days = y * 365 + y / 4 - y / 100 + y / 400;
    for (int m = 1; m < month; ++m) days += days_in_month(year, m);
    return days + (day - 1);
}

int day_of_week(int year, int month, int day) noexcept {
    // 0001-01-01 was a Monday.
    return static_cast<int>((days_from_civil(year, month, day) + 1) % 7);
}

std::optional<std::uint64_t> to_tick(const DateTime &date) noexcept {
    if (date.year < 1 || date.year > 9999 || date.month < 1 || date.month > 12 || date.day < 1 ||
        date.day > days_in_month(date.year, date.month) || date.hour < 0 || date.hour > 23 || date.minute < 0 ||
        date.minute > 59 || date.second < 0 || date.second > 59 || date.microsecond >= 1'000'000u)
        return std::nullopt;
    const auto days = static_cast<std::uint64_t>(days_from_civil(date.year, date.month, date.day));
    const std::uint64_t seconds =
        static_cast<std::uint64_t>(date.hour) * 3600u + static_cast<std::uint64_t>(date.minute) * 60u +
        static_cast<std::uint64_t>(date.second);
    return days * kTicksPerDay + seconds * kTicksPerSecond + date.microsecond;
}

DateTime from_tick(std::uint64_t tick) noexcept {
    DateTime date;
    std::uint64_t days = tick / kTicksPerDay;
    std::uint64_t rest = tick % kTicksPerDay;
    // Whole 400-year cycles (146097 days), then years one by one.
    date.year = 1 + static_cast<int>(days / 146'097u) * 400;
    days %= 146'097u;
    for (;;) {
        const std::uint64_t in_year = is_leap_year(date.year) ? 366u : 365u;
        if (days < in_year) break;
        days -= in_year;
        ++date.year;
    }
    date.month = 1;
    for (;;) {
        const auto in_month = static_cast<std::uint64_t>(days_in_month(date.year, date.month));
        if (days < in_month) break;
        days -= in_month;
        ++date.month;
    }
    date.day = static_cast<int>(days) + 1;
    date.microsecond = static_cast<std::uint32_t>(rest % kTicksPerSecond);
    rest /= kTicksPerSecond;
    date.second = static_cast<int>(rest % 60u);
    date.minute = static_cast<int>(rest / 60u % 60u);
    date.hour = static_cast<int>(rest / 3600u);
    return date;
}

} // namespace portablekit::rtc
