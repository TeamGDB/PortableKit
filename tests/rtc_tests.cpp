// sceRtc's calendar arithmetic: ticks from 0001-01-01, dates, weekdays.

#include "kernel/rtc_time.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

} // namespace

int main() {
    using namespace portablekit::rtc;
    check(to_tick({1970, 1, 1, 0, 0, 0, 0}) == kUnixEpochTick, "the Unix epoch is 719162 days in");
    check(to_tick({1, 1, 1, 0, 0, 0, 0}) == 0u, "tick 0 is 0001-01-01 00:00:00");
    const DateTime date{2008, 2, 29, 23, 59, 58, 123456};
    const auto tick = to_tick(date);
    check(tick.has_value(), "a leap day is valid");
    const DateTime back = from_tick(*tick);
    check(back.year == 2008 && back.month == 2 && back.day == 29 && back.hour == 23 && back.minute == 59 &&
              back.second == 58 && back.microsecond == 123456u,
          "a tick converts back to the same date and time");
    check(!to_tick({2007, 2, 29, 0, 0, 0, 0}) && !to_tick({2008, 13, 1, 0, 0, 0, 0}) &&
              !to_tick({2008, 1, 1, 24, 0, 0, 0}),
          "invalid dates are refused");
    check(day_of_week(1970, 1, 1) == 4 && day_of_week(2026, 9, 26) == 6 && day_of_week(1, 1, 1) == 1,
          "weekdays: 1970-01-01 Thursday, 2026-09-26 Saturday, 0001-01-01 Monday");
    check(days_in_month(2000, 2) == 29 && days_in_month(1900, 2) == 28 && days_in_month(2026, 4) == 30,
          "February in leap and non-leap centuries");
    const DateTime far = from_tick(*to_tick({9999, 12, 31, 12, 0, 0, 0}));
    check(far.year == 9999 && far.month == 12 && far.day == 31 && far.hour == 12, "the last year the PSP allows");
    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
