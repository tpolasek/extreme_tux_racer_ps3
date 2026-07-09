/* --------------------------------------------------------------------
EXTREME TUXRACER

Copyright (C) 1999-2001 Jasmin F. Patry (Tuxracer)
Copyright (C) 2010 Extreme Tux Racer Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
---------------------------------------------------------------------*/

#ifndef ETR_TYPES_H
#define ETR_TYPES_H

#include "vectors.h"

enum TViewMode {
	BEHIND,
	FOLLOW,
	ABOVE,
	NUM_VIEW_MODES
};

struct TPlayer;
struct TCourse;
struct TCharacter;

struct TGameData {
	float time_step;
	double finish_brake;
	int treesize;
	int treevar;
	bool finish;
	bool use_keyframe;
	bool force_treemap;

	// course params
	bool mirrorred;
	TPlayer* player;
	TCharacter* character;
	TCourse* course;
	std::size_t location_id;
	std::size_t light_id;
	int snow_id;
	int wind_id;
	std::size_t theme_id;

	// race results (better in player.ctrl ?)
	float time;				// reached time
	int score;				// reached score
	int herring;			// catched herrings during the race
	bool raceaborted;
};

class CControl;

#endif
