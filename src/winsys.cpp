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

#include <sys/stat.h>

#include "winsys.h"
#include "course.h"
#include "game_ctrl.h"
#include "ogl.h"
#include "translation.h"
#include "n_image.h"
#include <iostream>
#include <vector>

CWinsys Winsys;

CWinsys::CWinsys()
	: numJoysticks(0)
	, glStatesPushed(false)
	, auto_resolution(800, 600)
	, scale(1.f) {
	for (unsigned int i = 0; i < Joystick::Count; i++) {
		if (Joystick::isConnected(i))
			numJoysticks++;
		else
			break;
	}
}

float CWinsys::CalcScreenScale() const {
	if (resolution.height < 768) return 0.78f;
	else return (resolution.height / 768.f);
}

void CWinsys::SetupVideoMode(const TScreenRes& res) {
	int bpp = 32;
	switch (param.bpp_mode) {
		case 16:
		case 32:
			bpp = param.bpp_mode;
			break;
		case 0:
		default:
			param.bpp_mode = 0;
			bpp = VideoMode::getDesktopMode().bitsPerPixel;
			break;
	}
	Uint32 style = Style::Close | Style::Titlebar;
	if (param.fullscreen)
		style |= Style::Fullscreen;

	resolution = res;

	ResetRenderMode();

#ifdef USE_STENCIL_BUFFER
	ContextSettings ctx(bpp, 8, 0, 1, 2);
#else
	ContextSettings ctx(bpp, 0, 0, 1, 2);
#endif
	window.create(VideoMode(resolution.width, resolution.height, bpp), WINDOW_TITLE, style, ctx);
	Vector2u actualSize = window.getSize();
	resolution = TScreenRes(actualSize.x, actualSize.y);
	if (param.framerate)
		window.setFramerateLimit(param.framerate);

	scale = CalcScreenScale();
	if (param.use_quad_scale) scale = std::sqrt(scale);
}

void CWinsys::SetupVideoMode(int width, int height) {
	SetupVideoMode(TScreenRes(width, height));
}

void CWinsys::Init() {
	SetupVideoMode(TScreenRes(1280, 720));
}

void CWinsys::Quit() {
	SaveMessages();
	window.close();
}

void CWinsys::Terminate() {
	Quit();
	std::exit(0);
}

void CWinsys::PrintJoystickInfo() const {
	if (numJoysticks == 0) {
		std::cout << "No joystick found\n";
		return;
	}
	std::cout << '\n';
	for (unsigned int i = 0; i < numJoysticks; i++) {
		std::cout << "Joystick " << i << '\n';
		int buttons = Joystick::getButtonCount(i);
		std::cout << "Joystick has " << buttons << " button" << (buttons == 1 ? "" : "s") << '\n';
		std::cout << "Axes: ";
		if (Joystick::hasAxis(i, Joystick::R)) std::cout << "R ";
		if (Joystick::hasAxis(i, Joystick::U)) std::cout << "U ";
		if (Joystick::hasAxis(i, Joystick::V)) std::cout << "V ";
		if (Joystick::hasAxis(i, Joystick::X)) std::cout << "X ";
		if (Joystick::hasAxis(i, Joystick::Y)) std::cout << "Y ";
		if (Joystick::hasAxis(i, Joystick::Z)) std::cout << "Z ";
		std::cout << '\n';
	}
}

void CWinsys::TakeScreenshot() const {
	Vector2u size = window.getSize();
	std::vector<Uint8> buf(static_cast<std::size_t>(size.x) * size.y * 4);

	// Read framebuffer (GL origin is bottom-left; flip for PNG top-left).
	glReadPixels(0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

	Image img;
	img.create(size.x, size.y, buf.data());
	img.flipVertically();

	std::string path = param.screenshot_dir;

	const char *cpath = path.c_str();

	if (!DirExists(cpath)) {
		mkdir(cpath, 0775);
	}

	path += SEP;
	path += g_game.course->dir;
	path += '_';
	path += GetTimeString();

	path += SCREENSHOT_FORMAT;
	img.saveToFile(path);
}
