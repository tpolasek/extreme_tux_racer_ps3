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

#ifndef WINSYS_H
#define WINSYS_H

#include "bh.h"

#define SCREENSHOT_FORMAT ".png"

struct TScreenRes {
	unsigned int width, height;
	constexpr TScreenRes(unsigned int w = 0, unsigned int h = 0) : width(w), height(h) {}
};

class CWinsys {
private:
	unsigned int numJoysticks;

	bool glStatesPushed;
	RenderWindow window;
	TScreenRes auto_resolution;
	float CalcScreenScale() const;
public:
	TScreenRes resolution;
	float scale;			// scale factor for screen, see 'use_quad_scale'

	CWinsys();

	void Init();
	void SetupVideoMode(const TScreenRes& res);
	void SetupVideoMode(int width, int height);
	void PrintJoystickInfo() const;
	void SwapBuffers() { window.display(); }
	void Quit();
	void Terminate();
	// Native 2D drawables self-render via GL.
	void draw(const Drawable2D& drawable, const RenderStates& = RenderStates::Default) { drawable.draw(); }
	void clear() { window.clear(colBackgr); }
	// Save/restore GL state around 2D overlay rendering (was beginSFML/endSFML).
	void begin2D() { if (!glStatesPushed) window.pushGLStates(); glStatesPushed = true; }
	void end2D()   { if (glStatesPushed)  window.popGLStates();  glStatesPushed = false; }
	bool PollEvent(Event& event) { return window.pollEvent(event); }
	void TakeScreenshot() const;
};


extern CWinsys Winsys;

#endif
