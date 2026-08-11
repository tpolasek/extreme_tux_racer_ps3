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
#include "highscore.h"
#include "textures.h"

#include <algorithm>
#include <cmath>

CGameOver GameOver;

static CKeyframe *final_frame;

namespace {

const int NUM_FISH_BURSTS = 7;
const int FISH_PER_BURST = 40;
const int NUM_CELEBRATION_FISH = NUM_FISH_BURSTS * FISH_PER_BURST;
const float FISH_BURST_INTERVAL = 0.7f;
const float FISH_LIFETIME = 1.8f;
const float FISH_BURST_DURATION = 5.0f;

struct CelebrationFish {
	float vx;
	float vy;
	float size;
	float spawn_time;
};

CelebrationFish celebration_fish[NUM_CELEBRATION_FISH];
TTexture* celebration_fish_texture = nullptr;
bool fish_burst_active = false;
float fish_burst_time = 0.0f;

TTexture* GetCelebrationFishTexture() {
	for (auto& type : Course.ObjTypes) {
		if (type.textureFile != "herring.png") continue;
		if (type.texture == nullptr) {
			type.texture = new TTexture();
			if (!type.texture->Load(param.obj_dir, type.textureFile, false)) {
				delete type.texture;
				type.texture = nullptr;
			}
		}
		return type.texture;
	}
	return nullptr;
}

void StartFishBurst() {
	const float width = static_cast<float>(Winsys.resolution.width);
	const float height = static_cast<float>(Winsys.resolution.height);
	const float travel = std::max(width, height);
	const float display_scale = height / 720.0f;

	celebration_fish_texture = GetCelebrationFishTexture();
	if (celebration_fish_texture == nullptr) return;

	for (int burst = 0; burst < NUM_FISH_BURSTS; ++burst) {
		for (int i = 0; i < FISH_PER_BURST; ++i) {
			const int index = burst * FISH_PER_BURST + i;
			const float angle = static_cast<float>(2.0 * M_PI * i / FISH_PER_BURST + burst * 0.31);
			const float speed = travel * (0.38f + 0.055f * (i % 9));
			celebration_fish[index].vx = std::cos(angle) * speed;
			celebration_fish[index].vy = std::sin(angle) * speed - height * 0.18f;
			celebration_fish[index].size = (48.0f + 9.0f * (i % 6)) * display_scale;
			celebration_fish[index].spawn_time = burst * FISH_BURST_INTERVAL;
		}
	}

	fish_burst_time = 0.0f;
	fish_burst_active = true;
}

void UpdateAndDrawFishBurst(float time_step) {
	if (!fish_burst_active) return;

	const float dt = std::min(time_step, 0.05f);
	const float gravity = Winsys.resolution.height * 0.28f;
	const float origin_x = Winsys.resolution.width * 0.5f;
	const float origin_y = Winsys.resolution.height * 0.58f;
	fish_burst_time += dt;
	if (fish_burst_time >= FISH_BURST_DURATION) {
		fish_burst_active = false;
		return;
	}

	Setup2dScene();
	ScopedRenderMode rm(TEXFONT);
	for (int i = 0; i < NUM_CELEBRATION_FISH; ++i) {
		const CelebrationFish& fish = celebration_fish[i];
		const float age = fish_burst_time - fish.spawn_time;
		if (age < 0.0f || age > FISH_LIFETIME) continue;

		const float x = origin_x + fish.vx * age - fish.size * 0.5f;
		const float y = origin_y + fish.vy * age + gravity * age * age * 0.5f - fish.size * 0.5f;
		celebration_fish_texture->Draw(static_cast<int>(x), static_cast<int>(y),
		                               static_cast<int>(fish.size), static_cast<int>(fish.size));
	}
}

} // namespace

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

	const Color& backcol = colWhite;
	static const Color framecol(178, 178, 255);

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

	fish_burst_active = false;
	if (!g_game.raceaborted && SubmitCourseHighScore(g_game.time)) {
		StartFishBurst();
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
	UpdateAndDrawFishBurst(time_step);
	Reshape(width, height);
	Winsys.SwapBuffers();
}
