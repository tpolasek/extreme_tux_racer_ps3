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

#ifndef BH_H
#define BH_H

// --------------------------------------------------------------------
//			global and or system-dependant includes
// --------------------------------------------------------------------

#include <cstdint>
#include <cstddef>
#include <string>

#include <GL/gl.h>

// Native window/graphics/audio types.
#include "n_color.h"
#include "n_geom.h"
#include "n_clock.h"
#include "n_image.h"
#include "n_texture.h"
#include "n_font.h"
#include "n_draw.h"
#include "n_window.h"

#ifndef HAVE_CONFIG_H
#	ifdef __PPU__
#		define OS_PS3
#	elif defined(__APPLE__)
#		define OS_MAC
#	elif defined(__linux__)
#		define OS_LINUX
#	endif
#endif // CONFIG_H

// Unix platform (Linux, Mac OS X, BSD, ...)
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/time.h>
#include <GL/glx.h>
#ifndef OS_PS3
#	include <pwd.h>
#endif
#define SEP "/"


/* Stencil shadows need a hardware stencil path we don't implement on PS3;
 * without it ogl.cpp falls back to the cull-face shadow path. */
#ifndef OS_PS3
#	define USE_STENCIL_BUFFER
#endif

#include "version.h"
#define WINDOW_TITLE "Extreme Tux Racer " ETR_VERSION_STRING

#include "etr_types.h"
#include "common.h"
#include "game_config.h"

extern TGameData g_game;

#endif // BH_H
