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

#include <iostream>
#include <ctime>
#include <cstring>
#include "racing.h"
#include "audio.h"
#include "course_render.h"
#include "ogl.h"
#include "view.h"
#include "env.h"
#include "track_marks.h"
#include "hud.h"
#include "course.h"
#include "particles.h"
#include "textures.h"
#include "game_ctrl.h"
#include "game_over.h"
#include "paused.h"
#include "race_select.h"
#include "reset.h"
#include "winsys.h"
#include "physics.h"
#include "tux.h"
#include "ps3_log.h"
#if defined(DEMO_MODE) && DEMO_MODE
#include "screenshot.h"
#include "game_config.h"
#include <string>
#endif
#include <algorithm>
#include <cstdlib>
#define MAX_JUMP_AMT 1.0
#define ROLL_DECAY 0.2
#define JUMP_MAX_START_HEIGHT 0.30

CRacing Racing;

static bool stick_turn;
static float stick_turnfact;
static bool key_paddling;
static bool stick_paddling;
static bool key_charging;
static bool stick_charging;
static bool key_braking;
static bool stick_braking;
static double charge_start_time;
static bool trick_modifier;
static bool face_paddling;
static bool trigger_paddling;
static bool face_braking;
static bool trigger_braking;

static bool sky = true;
static bool fog = true;
static bool terr = true;
static bool trees = true;

static int newsound = -1;
static int lastsound = -1;

void CRacing::Jaxis(int axis, float value) {
	if (axis == 0) { 	// left and right
		stick_turn = ((value < -0.2) || (value > 0.2));
		if (stick_turn) stick_turnfact = value;
		else stick_turnfact = 0.0;
	} else if (axis == 1) {	// paddling and braking
		stick_paddling = (value < -0.3);
		stick_braking = (value > 0.3);
	}
}

void CRacing::Jbutt(int button, bool pressed) {
	CControl *ctrl = g_game.player->ctrl;

	switch (button) {
		case 0: // Cross: jump
			key_charging = pressed;
			break;
		case 1: // Circle: brake
			face_braking = pressed;
			key_braking = face_braking || trigger_braking;
			break;
		case 2: // Square: accelerate / flap
			face_paddling = pressed;
			key_paddling = face_paddling || trigger_paddling;
			break;
		case 3: // Triangle: trick modifier
			trick_modifier = pressed;
			break;

		case 4:
			if(pressed){
				ctrl->gear--;
				if(ctrl->gear <= 1){
					ctrl->gear = 1;
				}
			}
			break;
		case 5:
			if(pressed){
				ctrl->gear++;
				if(ctrl->gear >= 9){
					ctrl->gear = 9;
				}
			}
			break;
		case 6: // L2: brake
			trigger_braking = pressed;
			key_braking = face_braking || trigger_braking;
			break;

		case 7: // Start button - exit to map selection
			if (pressed) {
				g_game.raceaborted = true;
				State::manager.RequestEnterState(RaceSelect);
			}
			break;
		case 8: // R2: accelerate / flap
			trigger_paddling = pressed;
			key_paddling = face_paddling || trigger_paddling;
			break;
	}


}

static void CalcJumpEnergy(float time_step) {
	CControl *ctrl = g_game.player->ctrl;

	if (ctrl->jump_charging) {
		ctrl->jump_amt = std::min(MAX_JUMP_AMT, g_game.time - charge_start_time);
	} else if (ctrl->jumping) {
		ctrl->jump_amt *= (1.0 - (g_game.time - ctrl->jump_start_time) /
		                   JUMP_FORCE_DURATION);
	} else {
		ctrl->jump_amt = 0;
	}
}

static int CalcSoundVol(float fact) {
	return std::min(param.sound_volume * fact, 100.f);
}

static void SetSoundVolumes() {
	Sound.SetVolume("pickup1",    CalcSoundVol(0.3f));
	Sound.SetVolume("pickup2",    CalcSoundVol(0.24f));
	Sound.SetVolume("pickup3",    CalcSoundVol(0.24f));
	Sound.SetVolume("snow_sound", CalcSoundVol(1.5f));
	Sound.SetVolume("ice_sound",  CalcSoundVol(0.42f));
	Sound.SetVolume("rock_sound", CalcSoundVol(0.77f));
}

// ---------------------------- init ----------------------------------
void CRacing::Enter() {
	CControl *ctrl = g_game.player->ctrl;

	if (param.view_mode < 0 || param.view_mode >= NUM_VIEW_MODES) {
		param.view_mode = ABOVE;
	}
	set_view_mode(ctrl, param.view_mode);

	ctrl->turn_fact = 0.0;
	ctrl->turn_animation = 0.0;
	ctrl->is_braking = false;
	ctrl->is_paddling = false;
	ctrl->jumping = false;
	ctrl->jump_charging = false;

	key_paddling = false;
	key_braking = false;
	key_charging = false;
	trick_modifier = false;
	face_paddling = false;
	trigger_paddling = false;
	face_braking = false;
	trigger_braking = false;
	stick_paddling = false;
	stick_braking = false;
	stick_turn = false;

	lastsound = -1;
	newsound = -1;

	if (State::manager.PreviousState() != &Paused) ctrl->Init();
	g_game.raceaborted = false;

	SetSoundVolumes();
	Music.PlayTheme(g_game.theme_id, MUS_RACING);

	g_game.finish = false;
}

// -------------------- sound -----------------------------------------

// this function is not used yet.
/*static int SlideVolume(CControl *ctrl, double speed, int typ) {
	if (typ == 1) {	// only at paddling or braking
		return (int)(std::min((((std::pow(ctrl->turn_fact, 2) * 128)) +
		                  (ctrl->is_braking ? 128:0) +
		                  (ctrl->jumping ? 128:0) + 20) * (speed / 10), 128.0));
	} else { 	// always
		return (int)(128 * std::pow((speed/2),2));
	}
}*/

static void PlayTerrainSound(CControl *ctrl, bool airborne) {
	if (airborne == false) {
		int terridx = Course.GetTerrainIdx(ctrl->cpos.x, ctrl->cpos.z, 0.5);
		if (terridx >= 0)
			newsound = (int)Course.TerrList[terridx].sound;
		else
			newsound = -1;
	} else
		newsound = -1;

	if ((newsound != lastsound) && (lastsound >= 0))
		Sound.Halt(lastsound);
	if (newsound >= 0)
		Sound.Play(newsound, true);

	lastsound = newsound;
}

// ----------------------- controls -----------------------------------
static void CalcSteeringControls(CControl *ctrl, float time_step) {
	if (stick_turn) {
		ctrl->turn_fact = stick_turnfact;
		ctrl->turn_animation += ctrl->turn_fact * 2 * time_step;
		ctrl->turn_animation = clamp(-1.0, ctrl->turn_animation, 1.0);
	} else {
		ctrl->turn_fact = 0.0;
		if (time_step < ROLL_DECAY) {
			ctrl->turn_animation *= 1.0 - time_step / ROLL_DECAY;
		} else {
			ctrl->turn_animation = 0.0;
		}
	}

	bool paddling = key_paddling || stick_paddling;
	if (paddling && ctrl->is_paddling == false) {
		ctrl->is_paddling = true;
		ctrl->paddle_time = g_game.time;
	}

	bool braking = key_braking || stick_braking;
	ctrl->is_braking = braking;

	bool charge = key_charging || stick_charging;
	bool invcharge = !key_charging && !stick_charging;
	CalcJumpEnergy(time_step);
	if ((charge) && !ctrl->jump_charging && !ctrl->jumping) {
		ctrl->jump_charging = true;
		charge_start_time = g_game.time;
	}
	if ((invcharge) && ctrl->jump_charging) {
		ctrl->jump_charging = false;
		ctrl->begin_jump = true;
	}
}

static void CalcFinishControls(CControl *ctrl, float timestep, bool airborne) {
	double speed = ctrl->cvel.Length();
	double dir_angle = RADIANS_TO_ANGLES(std::atan(ctrl->cvel.x / ctrl->cvel.z));

	if (std::fabs(dir_angle) > 5 && speed > 5) {
		ctrl->turn_fact = dir_angle / 20;
		if (ctrl->turn_fact < -1) ctrl->turn_fact = -1;
		if (ctrl->turn_fact > 1) ctrl->turn_fact = 1;
		ctrl->turn_animation += ctrl->turn_fact * 2 * timestep;
	} else {
		ctrl->turn_fact = 0;
		if (timestep < ROLL_DECAY) {
			ctrl->turn_animation *= 1.0 - timestep / ROLL_DECAY;
		} else ctrl->turn_animation = 0.0;
	}
}

// ----------------------- trick --------------------------------------

static void CalcTrickControls(CControl *ctrl, float time_step, bool airborne) {
	if (airborne && trick_modifier) {
		if (stick_turnfact < -0.5f) ctrl->roll_left = true;
		if (stick_turnfact >  0.5f) ctrl->roll_right = true;
		if (key_paddling) ctrl->front_flip = true;
		if (ctrl->is_braking) ctrl->back_flip = true;
	}

	if (ctrl->roll_left || ctrl->roll_right) {
		ctrl->roll_factor += (ctrl->roll_left ? -1 : 1) * 0.15 * time_step / 0.05;
		if (ctrl->roll_factor  > 1 || ctrl->roll_factor < -1) {
			ctrl->roll_factor = 0;
			ctrl->roll_left = ctrl->roll_right = false;
		}
	}
	if (ctrl->front_flip || ctrl->back_flip) {
		ctrl->flip_factor += (ctrl->back_flip ? -1 : 1) * 0.15 * time_step / 0.05;
		if (ctrl->flip_factor > 1 || ctrl->flip_factor < -1) {
			ctrl->flip_factor = 0;
			ctrl->front_flip = ctrl->back_flip = false;
		}
	}
}

// ====================================================================
//					loop
// ====================================================================

void CRacing::Loop(float time_step) {
#if defined(DEMO_MODE) && DEMO_MODE
	static int sDemoFrame = 0;
	if (g_game.time >= 10.f) {
#if defined(DEMO_SHOTS) && DEMO_SHOTS
		// Capture right before the demo exits: <=480p, aspect-correct bilinear.
		std::string p = param.save_dir + SEP "demo_close.png";
		demoScreenshot(p.c_str(), 640, 480);
#endif
		State::manager.RequestQuit();
		return;
	}
#endif
	CControl *ctrl = g_game.player->ctrl;
	double ycoord = Course.FindYCoord(ctrl->cpos.x, ctrl->cpos.z);
	bool airborne = (bool)(ctrl->cpos.y > (ycoord + JUMP_MAX_START_HEIGHT));

	ClearRenderContext();
	Env.SetupFog();
	CalcTrickControls(ctrl, time_step, airborne);

	if (!g_game.finish) CalcSteeringControls(ctrl, time_step);
	else CalcFinishControls(ctrl, time_step, airborne);
	PlayTerrainSound(ctrl, airborne);

//  >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
	TIMER_START("RACE_UPDATE");
	ctrl->UpdatePlayerPos(time_step);
	if (g_game.finish) IncCameraDistance(time_step);
	update_view(ctrl, time_step);
	UpdateTrackmarks(ctrl);
	SetupViewFrustum(ctrl);
	TIMER_END("RACE_UPDATE");

	if (sky) Env.DrawSkybox(ctrl->viewpos);
	if (fog) Env.DrawFog();
	Env.SetupLight();

	TIMER_START("RACE_RENDER_COURSE");
	if (terr) RenderCourse();
	DrawTrackmarks();
	TIMER_END("RACE_RENDER_COURSE");

	TIMER_START("RACE_DRAW_TREES");
	if (trees) DrawTrees();
	TIMER_END("RACE_DRAW_TREES");

	if (param.perf_level > 2) {
		TIMER_START("RACE_DRAW_PARTICLES");
		update_particles(time_step);
		draw_particles(ctrl);
		TIMER_END("RACE_DRAW_PARTICLES");
	}

	TIMER_START("RACE_DRAW_TUX");
	g_game.character->shape->Draw();
	TIMER_END("RACE_DRAW_TUX");

	UpdateWind(time_step);
	UpdateSnow(time_step, ctrl);

	TIMER_START("RACE_DRAW_SNOW");
	DrawSnow(ctrl);
	TIMER_END("RACE_DRAW_SNOW");

	TIMER_START("RACE_DRAW_HUD");
	DrawHud(ctrl);
	TIMER_END("RACE_DRAW_HUD");

	Reshape(Winsys.resolution.width, Winsys.resolution.height);
	Winsys.SwapBuffers();
#if defined(DEMO_MODE) && DEMO_MODE
#if defined(DEMO_SHOTS) && DEMO_SHOTS
	if (++sDemoFrame == 150) {
		// Full native-resolution capture mid-race.
		std::string p = param.save_dir + SEP "demo_frame150.png";
		demoScreenshot(p.c_str(), 0, 0);
	}
#endif
#endif
	if (g_game.finish == false) g_game.time += time_step;
}

void CRacing::Exit() {
	Sound.HaltAll();
	break_track_marks();
}
