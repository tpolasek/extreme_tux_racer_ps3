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

#include "race_select.h"
#include "ogl.h"
#include "textures.h"
#include "particles.h"
#include "audio.h"
#include "env.h"
#include "course.h"
#include "gui.h"
#include "font.h"
#include "translation.h"
#include "spx.h"
#include "loading.h"
#include "winsys.h"

CRaceSelect RaceSelect;

static TUpDown* course;
static TFramedText* courseName;
static TWidget* textbutton;

void SetRaceConditions() {
	// Map attributes are hardcoded to defaults: Sunny, Light snow, Breeze, mirror Off.
	g_game.course = &(*Course.currentCourseList)[course->GetValue()];
	g_game.theme_id = (*Course.currentCourseList)[course->GetValue()].music_theme;
	State::manager.RequestEnterState(Loading);
}

void CRaceSelect::Jbutt(int button, bool pressed) {
	if (!pressed) return;
	switch (button) {
		case 0: // A / Confirm
			SetRaceConditions();
			break;
		case 1: // B / Back
			break;
	}
}

void CRaceSelect::Jaxis(int axis, float value) {
	static int last_vert = 0;
	static int last_horiz = 0;

	if (axis == 1) { // vertical
		int dir = (value < -0.5f) ? -1 : (value > 0.5f) ? 1 : 0;
		if (dir != 0 && dir != last_vert) {
			if (course->focussed())
				course->Action(dir < 0 ? ACT_UP : ACT_DOWN);
			else
				ActionGUI(dir < 0 ? ACT_UP : ACT_DOWN);
		}
		last_vert = dir;
	} else if (axis == 0) { // horizontal
		int dir = (value < -0.5f) ? -1 : (value > 0.5f) ? 1 : 0;
		if (dir != 0 && dir != last_horiz) {
			if (!course->focussed())
				ActionGUI(dir < 0 ? ACT_LEFT : ACT_RIGHT);
		}
		last_horiz = dir;
	}
}

// --------------------------------------------------------------------
static TArea area;
static int framewidth, frameheight, frametop;
static int prevtop, prevwidth, prevheight;
static int prevleft;

void CRaceSelect::Enter() {
	Music.Play(param.menu_music, true);

	framewidth = 550 * Winsys.scale;
	frameheight = 50 * Winsys.scale;
	frametop = AutoYPosN(30);

	area = AutoAreaN(30, 80, framewidth);
	prevheight = 288 * Winsys.scale;
	prevwidth = 384 * Winsys.scale;
	prevleft = area.left + (framewidth - prevwidth) / 2;
	prevtop = AutoYPosN(44);
	ResetGUI();

	int siz = FT.AutoSizeN(5);
	int len1 = FT.GetTextWidth(Trans.Text(13));
	textbutton = AddTextButton(Trans.Text(13), area.right-len1-50, AutoYPosN(85), siz);
	FT.AutoSizeN(4);

	course = AddUpDown(area.left + framewidth + 8, frametop + frameheight + 20, 0, (int)Course.currentCourseList->size() - 1, g_game.course ? (int)Course.GetCourseIdx(g_game.course) : 0);
	courseName = AddFramedText(area.left, frametop + frameheight + 20, framewidth, frameheight, 3, colMBackgr, "", FT.GetSize(), true);

	SetFocus(course);
}

void CRaceSelect::Loop(float time_step) {
	ScopedRenderMode rm(GUI);
	Winsys.clear();

	if (param.ui_snow) {
		update_ui_snow(time_step);
		draw_ui_snow();
	}

	DrawGUIBackground(Winsys.scale);

	courseName->Focussed(course->focussed());
	courseName->SetString((*Course.currentCourseList)[course->GetValue()].name);

	if ((*Course.currentCourseList)[course->GetValue()].preview)
		(*Course.currentCourseList)[course->GetValue()].preview->DrawFrame(prevleft, prevtop, prevwidth, prevheight, 3, colWhite);

	if (g_game.force_treemap) {
		FT.AutoSizeN(4);
		static const sf::String forcetrees = "Load trees.png";
		std::string sizevar = "Size: ";
		sizevar += Int_StrN(g_game.treesize);
		sizevar += " Variation: ";
		sizevar += Int_StrN(g_game.treevar);
		FT.SetColor(colYellow);
		FT.DrawString(CENTER, AutoYPosN(85), forcetrees);
		FT.DrawString(CENTER, AutoYPosN(90), sizevar);
	}

	DrawGUI();

	Winsys.SwapBuffers();
}
