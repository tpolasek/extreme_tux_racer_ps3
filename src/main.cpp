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

#include "bh.h"
#include "textures.h"
#include "ogl.h"
#include "splash_screen.h"
#include "audio.h"
#include "font.h"
#include "hud.h"
#include "winsys.h"
#include <iostream>
#include <ctime>
#include <cstring>
#include <csignal>

#ifdef OS_PS3
#include "ps3_tty.h"
#include "ps3_log.h"
#define ETR_TRACE(msg) sysTtyTrace("[etr] " msg)
#else
#define ETR_TRACE(msg) do {} while (0)
#endif

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

extern "C" int etr_run() {
	std::cout << "\n----------- Extreme Tux Racer " ETR_VERSION_STRING " ----------------";
	std::cout << "\n----------- (C) 2010-2024 Extreme Tux Racer Team  --------\n\n";
	std::cout << "\n----------- (C) GG --------\n\n";

	std::srand(std::time(nullptr));
	std::signal(SIGINT, sigint_handler);
	ETR_TRACE("InitConfig");
	InitConfig();
#if defined(DEMO_MODE) && DEMO_MODE
	ps3_perf_open((param.save_dir + SEP "etr_perf.log").c_str());
#endif
	ETR_TRACE("InitGame");
	InitGame(0, nullptr);
	ETR_TRACE("Winsys.Init");
	Winsys.Init();
	ETR_TRACE("InitOpenglExtensions");
	InitOpenglExtensions();

	// For checking the joystick and the OpgenGL version (the info is written on the console):
	//Winsys.PrintJoystickInfo();
	//PrintGLInfo ();

	// theses resources must or should be loaded before splashscreen starts
	ETR_TRACE("LoadTextureList");
	if (!Tex.LoadTextureList()) {
		ETR_TRACE("LoadTextureList FAILED");
		Winsys.Quit();
		return -1;
	}
	ETR_TRACE("LoadFontlist");
	FT.LoadFontlist();
	FT.SetFontFromSettings();
	ETR_TRACE("LoadMusicList");
	Music.LoadMusicList();
	Music.SetVolume(param.music_volume);

	ETR_TRACE("Run(SplashScreen)");
	State::manager.Run(SplashScreen);

	ETR_TRACE("shutdown");
	FreeHudResources();
#if defined(DEMO_MODE) && DEMO_MODE
	ps3_perf_close();
#endif
	Winsys.Quit();

	return 0;
}

#ifndef OS_PS3
int main(int argc, char **argv) {
	return etr_run();
}
#endif
