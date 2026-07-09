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

#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "bh.h"
#include "textures.h"
#include "ogl.h"
#include "splash_screen.h"
#include "audio.h"
#include "font.h"
#include "winsys.h"
#include <iostream>
#include <ctime>
#include <cstring>
#include <csignal>

TGameData g_game;

volatile sig_atomic_t g_sigint_received = 0;

static void sigint_handler(int) {
	g_sigint_received = 1;
}

void InitGame(int argc, char **argv) {
	(void)argc;
	(void)argv;

	g_game.player = nullptr;
	g_game.course = nullptr;
	g_game.mirrorred = false;
	g_game.character = nullptr;
	g_game.location_id = 0;
	g_game.light_id = 0;   // Sunny
	g_game.snow_id = 1;    // Light snow
	g_game.wind_id = 1;    // Breeze
	g_game.theme_id = 0;
	g_game.force_treemap = false;
	g_game.treesize = 3;
	g_game.treevar = 3;
}

int main(int argc, char **argv) {
	std::cout << "\n----------- Extreme Tux Racer " ETR_VERSION_STRING " ----------------";
	std::cout << "\n----------- (C) 2010-2024 Extreme Tux Racer Team  --------\n\n";
	std::cout << "\n----------- (C) GG --------\n\n";

	std::srand(std::time(nullptr));
	std::signal(SIGINT, sigint_handler);
	InitConfig();
	InitGame(argc, argv);
	Winsys.Init();
	InitOpenglExtensions();

	// For checking the joystick and the OpgenGL version (the info is written on the console):
	//Winsys.PrintJoystickInfo();
	//PrintGLInfo ();

	// theses resources must or should be loaded before splashscreen starts
	if (!Tex.LoadTextureList()) {
		Winsys.Quit();
		return -1;
	}
	FT.LoadFontlist();
	FT.SetFontFromSettings();
	Music.LoadMusicList();
	Music.SetVolume(param.music_volume);

	State::manager.Run(SplashScreen);

	Winsys.Quit();

	return 0;
}
