/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types (SFML removal)

Clock - monotonic timer, replaces sf::Clock. Backed by clock_gettime.
---------------------------------------------------------------------*/
#ifndef N_CLOCK_H
#define N_CLOCK_H

#include "n_geom.h" // Duration

class Clock {
	Duration startTime_;
public:
	Clock();
	/// Returns elapsed time since the clock started / last restart.
	Duration getElapsedTime() const;
	/// Resets the clock and returns the elapsed time since the last reset.
	Duration restart();
};

#endif // N_CLOCK_H
