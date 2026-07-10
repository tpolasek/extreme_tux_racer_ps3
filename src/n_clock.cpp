/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

Clock implementation - clock_gettime(CLOCK_MONOTONIC).
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_clock.h"

#include <time.h>

static int64_t now_us() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

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
