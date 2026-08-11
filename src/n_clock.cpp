/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

Clock implementation - clock_gettime(CLOCK_MONOTONIC).
---------------------------------------------------------------------*/
#include "n_clock.h"

#include "bh.h"   // OS_PS3

#ifdef OS_PS3
// PS3 newlib has no clock_gettime/CLOCK_MONOTONIC; use the lv2 system clock
// (seconds + nanoseconds since epoch).
#include <sys/systime.h>
static int64_t now_us() {
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return static_cast<int64_t>(sec) * 1000000LL + static_cast<int64_t>(nsec / 1000);
}
#else
#include <time.h>
static int64_t now_us() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}
#endif

Clock::Clock() : startTime_(Duration(now_us())) {}

Duration Clock::getElapsedTime() const {
	return Duration(now_us() - startTime_.asMicroseconds());
}

Duration Clock::restart() {
	int64_t n = now_us();
	Duration elapsed(n - startTime_.asMicroseconds());
	startTime_ = Duration(n);
	return elapsed;
}
