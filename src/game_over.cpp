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

#include "game_over.h"
#include "audio.h"
#include "ogl.h"
#include "view.h"
#include "course_render.h"
#include "course.h"
#include "env.h"
#include "hud.h"
#include "track_marks.h"
#include "particles.h"
#include "gui.h"
#include "font.h"
#include "spx.h"
#include "game_ctrl.h"
#include "translation.h"
#include "race_select.h"
#include "winsys.h"
#include "physics.h"
#include "tux.h"

CGameOver GameOver;

static CKeyframe *final_frame;

void QuitGameOver() {
	State::manager.RequestEnterState(RaceSelect);
}

void CGameOver::Jbutt(int button, bool pressed) {
	if (pressed && button == 0) QuitGameOver();
}


void GameOverMessage(const CControl *ctrl) {
	int fwidth = 500;

	int leftframe = (Winsys.resolution.width - fwidth) / 2;
	int topframe = 80;

	const sf::Color& backcol = colWhite;
	static const sf::Color framecol(178, 178, 255);

	if (param.use_papercut_font > 0) FT.SetSize(28);
	else FT.SetSize(22);
	if (g_game.raceaborted) {
		DrawFrameX(leftframe, topframe, fwidth, 100, 4, backcol, framecol, 0.5f);
		FT.SetColor(colDBlue);
		FT.DrawString(CENTER, topframe+30, Trans.Text(25));
	} else {
		int firstMarker = leftframe + 60;
		int secondMarker = leftframe + 310;
		DrawFrameX(leftframe, topframe, fwidth, 210, 4, backcol, framecol, 0.5f);

		if (param.use_papercut_font > 0) FT.SetSize(20);
		else FT.SetSize(14);
		FT.SetColor(colDBlue);

		std::string line = Trans.Text(84) + ":  ";
		FT.DrawString(firstMarker, topframe + 15, line);
		line = Int_StrN(g_game.score);
		line += "  pts";
		FT.DrawString(secondMarker, topframe + 15, line);

		line = Trans.Text(85) + ":  ";
		FT.DrawString(firstMarker, topframe + 40, line);
		line = Int_StrN(g_game.herring);
		FT.DrawString(secondMarker, topframe + 40, line);

		line = Trans.Text(86) + ":  ";
		FT.DrawString(firstMarker, topframe + 65, line);
		line = Float_StrN(g_game.time, 2);
		line += "  s";
		FT.DrawString(secondMarker, topframe + 65, line);

		line = Trans.Text(87) + ":  ";
		FT.DrawString(firstMarker, topframe + 90, line);
		line = Float_StrN(ctrl->way, 2);
		line += "  m";
		FT.DrawString(secondMarker, topframe + 90, line);

		line = Trans.Text(88) + ":  ";
		FT.DrawString(firstMarker, topframe + 115, line);
		line = Float_StrN(ctrl->way / g_game.time * 3.6, 2);
		line += "  km/h";
		FT.DrawString(secondMarker, topframe + 115, line);
	}
}

// =========================================================================
void CGameOver::Enter() {
	g_game.score = g_game.herring * 10;
	double timept = Course.GetDimensions().y - (g_game.time * 10);
	g_game.score += (int)timept;
	if (g_game.score < 0) g_game.score = 0;

	if (g_game.raceaborted) {
		Music.PlayTheme(g_game.theme_id, MUS_LOSTRACE);
	} else {
		Music.PlayTheme(g_game.theme_id, MUS_WONRACE);
	}

	if (g_game.raceaborted || !g_game.use_keyframe) {
		final_frame = nullptr;
	} else {
		final_frame = g_game.character->GetKeyframe(FINISH);

		if (!g_game.raceaborted) {
			const CControl *ctrl = g_game.player->ctrl;
			final_frame->Init(ctrl->cpos, -0.18);
		}
	}
	SetStationaryCamera(true);
}


void CGameOver::Loop(float time_step) {
	CControl *ctrl = g_game.player->ctrl;
	int width = Winsys.resolution.width;
	int height = Winsys.resolution.height;

	ClearRenderContext();
	Env.SetupFog();

	update_view(ctrl, 0);

	if (final_frame != nullptr) final_frame->Update(time_step);

	SetupViewFrustum(ctrl);
	Env.DrawSkybox(ctrl->viewpos);
	Env.DrawFog();
	Env.SetupLight();

	RenderCourse();
	DrawTrackmarks();
	DrawTrees();

	UpdateWind(time_step);
	UpdateSnow(time_step, ctrl);
	DrawSnow(ctrl);

	g_game.character->shape->Draw();

	{
		ScopedRenderMode rm(GUI);
		if (final_frame != nullptr) {
			if (!final_frame->active) GameOverMessage(ctrl);
		} else GameOverMessage(ctrl);
	}
	DrawHud(ctrl);
	Reshape(width, height);
	Winsys.SwapBuffers();
}
