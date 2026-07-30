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

#include "regist.h"
#include "ogl.h"
#include "textures.h"
#include "audio.h"
#include "gui.h"
#include "particles.h"
#include "font.h"
#include "game_ctrl.h"
#include "translation.h"
#include "race_select.h"
#include "winsys.h"

CRegist Regist;

static TUpDown* character;
static TWidget* textbutton;

// The single hardcoded player name.
static const std::string hardcodedName = "bunny";

void QuitRegistration() {
	Players.ResetControls();
	Players.AllocControl(0);
	g_game.player = Players.GetPlayer(0);

	g_game.character = &Char.CharList[character->GetValue()];
	State::manager.RequestEnterState(RaceSelect);
}

void CRegist::Jbutt(int button, bool pressed) {
	if (!pressed) return;
	switch (button) {
		case 0: // A / Confirm
			QuitRegistration();
			break;
		case 1: // B / Back
			State::manager.RequestQuit();
			break;
	}
}

void CRegist::Jaxis(int axis, float value) {
	static int last_dir = 0;
	if (axis == 1) {
		int dir = (value < -0.5f) ? -1 : (value > 0.5f) ? 1 : 0;
		if (dir != 0 && dir != last_dir) {
			if (character->focussed())
				character->Action(dir < 0 ? ACT_UP : ACT_DOWN);
			else
				ActionGUI(dir < 0 ? ACT_UP : ACT_DOWN);
		}
		last_dir = dir;
	}
}

static int framewidth, frameheight, arrowwidth;
static TArea area;
static double texsize;
static TLabel* sHelpCharacter;
static TFramedText* sCharFrame;

void CRegist::Enter() {
	Music.Play(param.menu_music, true);

	framewidth = (int)(Winsys.scale * 280);
	frameheight = (int)(Winsys.scale * 50);
	arrowwidth = 70*Winsys.scale;
	area = AutoAreaN(30, 80, framewidth);
	texsize = 128 * Winsys.scale;

	Char.LoadCharacterPreviews();
	int initial_character = 0;
	if (g_game.character != nullptr) {
		for (std::size_t i = 0; i < Char.CharList.size(); i++) {
			if (&Char.CharList[i] == g_game.character) {
				initial_character = (int)i;
				break;
			}
		}
	}

	ResetGUI();
	character = AddUpDown(area.left + framewidth + 8, area.top, 0,
		(int)Char.CharList.size() - 1, initial_character);
	int siz = FT.AutoSizeN(5);
	textbutton = AddTextButton(Trans.Text(60), CENTER, AutoYPosN(70), siz);

	FT.AutoSizeN(3);
	int top = AutoYPosN(24);
	sHelpCharacter = AddLabel(Trans.Text(59), area.left, top, colWhite);

	FT.AutoSizeN(4);
	sCharFrame = AddFramedText(area.left, area.top, framewidth, frameheight, 3, colMBackgr, "", FT.GetSize());

	// Hardcode the single player name as "bunny".
	Players.ResetControls();
	Players.SetSinglePlayer(hardcodedName);

	SetFocus(character);
}

void CRegist::Loop(float time_step) {
	ScopedRenderMode rm(GUI);
	Winsys.clear();

	if (param.ui_snow) {
		update_ui_snow(time_step);
		draw_ui_snow();
	}

	DrawGUIBackground(Winsys.scale);

	sCharFrame->SetString(Char.CharList[character->GetValue()].name);
	sCharFrame->Focussed(character->focussed());
	if (Char.CharList[character->GetValue()].preview != nullptr)
		Char.CharList[character->GetValue()].preview->DrawFrame(
		    area.right - texsize - 60 - arrowwidth,
		    AutoYPosN(40), texsize, texsize, 3, colWhite);

	DrawGUI();

	Winsys.SwapBuffers();
}
