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

#include "hud.h"
#include "ogl.h"
#include "textures.h"
#include "spx.h"
#include "particles.h"
#include "font.h"
#include "course.h"
#include "physics.h"
#include "winsys.h"
#include "game_ctrl.h"
#include "highscore.h"
#include "gui.h"
#include <algorithm>

namespace {

TTexture* highscore_leader_preview = nullptr;
const TCourseHighScore* cached_highscore = nullptr;
std::size_t cached_highscore_revision = 0;
std::string cached_highscore_time;
std::string cached_highscore_hundredths;

void RefreshHighScoreHud(const TCourseHighScore& score) {
	// PNG decoding, texture upload, and time formatting happen only when the
	// course or record changes. Normal frames take this constant-time exit.
	if (cached_highscore == &score &&
	    cached_highscore_revision == score.revision)
		return;

	delete highscore_leader_preview;
	highscore_leader_preview = nullptr;
	cached_highscore = &score;
	cached_highscore_revision = score.revision;

	int min, sec, hundr;
	GetTimeComponents(score.time, &min, &sec, &hundr);
	cached_highscore_time = Int_StrN(min, 2) + ':' + Int_StrN(sec, 2);
	cached_highscore_hundredths = Int_StrN(hundr, 2);

	if (score.character_dir.empty()) return;

	const std::string preview_file =
		MakePathStr(param.char_dir, score.character_dir) + SEP "preview.png";
	highscore_leader_preview = new TTexture();
	if (!highscore_leader_preview->Load(preview_file, false)) {
		delete highscore_leader_preview;
		highscore_leader_preview = nullptr;
		Message("Unable to load high score leader preview", preview_file);
	}
}

} // namespace


#define GAUGE_IMG_SIZE 128
#define ENERGY_GAUGE_BOTTOM 3.0
#define ENERGY_GAUGE_HEIGHT 103.0
#define ENERGY_GAUGE_CENTER_X 71.0
#define ENERGY_GAUGE_CENTER_Y 55.0
#define GAUGE_WIDTH 128.0
#define SPEEDBAR_OUTER_RADIUS  (ENERGY_GAUGE_CENTER_X)
#define SPEEDBAR_BASE_ANGLE 225
#define SPEEDBAR_MAX_ANGLE 45
#define SPEEDBAR_GREEN_MAX_SPEED  (MAX_PADDLING_SPEED * 3.6)
#define SPEEDBAR_YELLOW_MAX_SPEED 100
#define SPEEDBAR_RED_MAX_SPEED 160
#define SPEEDBAR_GREEN_FRACTION 0.5
#define SPEEDBAR_YELLOW_FRACTION 0.25
#define SPEEDBAR_RED_FRACTION 0.25
#define CIRCLE_DIVISIONS 10

static const GLubyte energy_background_color[]   = { 51,  51,  51, 0 };
static const GLubyte energy_foreground_color[]   = { 138, 150, 255, 128 };
static const GLubyte speedbar_background_color[] = { 51,  51,  51, 0 };
static const GLubyte hud_white[]                 = { 255, 255, 255, 255 };

static void draw_time(double time, Color color) {
	Tex.Draw(T_TIME, 10, 10, 1);

	int min, sec, hundr;
	GetTimeComponents(time, &min, &sec, &hundr);
	std::string timestr = Int_StrN(min, 2);
	std::string secstr = Int_StrN(sec, 2);
	std::string hundrstr = Int_StrN(hundr, 2);

	timestr += ':';
	timestr += secstr;


	if (param.use_papercut_font < 2) {
		Tex.DrawNumStr(timestr, 50, 12, 1, color);
		Tex.DrawNumStr(hundrstr, 170, 12, 0.7f, color);
	} else {
		Winsys.begin2D();
		FT.SetColor(color);
		FT.SetSize(30);
		FT.DrawString(138, 3, hundrstr);
		FT.SetSize(42);
		FT.DrawString(53, 3, timestr);
		Winsys.end2D();
	}
}

static void draw_high_score(const TCourseHighScore& score, Color color) {
	const int frame_width = 190;
	const int left = (Winsys.resolution.width - frame_width) / 2;
	RefreshHighScoreHud(score);
	DrawFrameX(left, 6, frame_width, 46, 2, colBlack, colWhite, 0.55f);

	if (param.use_papercut_font < 2) {
		Tex.DrawNumStr(cached_highscore_time, left + 15, 12, 1, color);
		Tex.DrawNumStr(cached_highscore_hundredths, left + 135, 12, 0.7f, color);
	} else {
		Winsys.begin2D();
		FT.SetColor(color);
		FT.SetSize(30);
		FT.DrawString(left + 133, 3, cached_highscore_hundredths);
		FT.SetSize(42);
		FT.DrawString(left + 18, 3, cached_highscore_time);
		Winsys.end2D();
	}

	if (highscore_leader_preview != nullptr)
		highscore_leader_preview->Draw(left + frame_width + 8, 6, 46, 46);
}

static void draw_herring_count(int herring_count, Color color) {
	Tex.Draw(HERRING_ICON, Winsys.resolution.width - 59, 12, 1);

	std::string hcountstr = Int_StrN(herring_count, 3);
	if (param.use_papercut_font < 2) {
		Tex.DrawNumStr(hcountstr, Winsys.resolution.width - 130, 12, 1, color);
	} else {
		Winsys.begin2D();
		FT.SetColor(color);
		FT.DrawString(Winsys.resolution.width - 125, 3, hcountstr);
		Winsys.end2D();
	}
}

TVector2d calc_new_fan_pt(double angle) {
	return TVector2d(
	           ENERGY_GAUGE_CENTER_X + std::cos(ANGLES_TO_RADIANS(angle)) * SPEEDBAR_OUTER_RADIUS,
	           ENERGY_GAUGE_CENTER_Y + std::sin(ANGLES_TO_RADIANS(angle)) * SPEEDBAR_OUTER_RADIUS);
}

void draw_partial_tri_fan(double fraction) {
	double angle = SPEEDBAR_BASE_ANGLE +
	               (SPEEDBAR_MAX_ANGLE - SPEEDBAR_BASE_ANGLE) * fraction;

	int divs = (int)((SPEEDBAR_BASE_ANGLE - angle) * CIRCLE_DIVISIONS / 360.0) + 1;
	double cur_angle = SPEEDBAR_BASE_ANGLE;
	double angle_incr = 360.0 / CIRCLE_DIVISIONS;

	glBegin(GL_TRIANGLE_FAN);
	glVertex2f(ENERGY_GAUGE_CENTER_X,
	           ENERGY_GAUGE_CENTER_Y);

	for (int i=0; i<divs; i++) {
		TVector2d pt = calc_new_fan_pt(cur_angle);
		glVertex2f(pt.x, pt.y);
		cur_angle -= angle_incr;
	}

	if (cur_angle+angle_incr > angle + EPS) {
		cur_angle = angle;
		TVector2d pt = calc_new_fan_pt(cur_angle);
		glVertex2f(pt.x, pt.y);
	}

	glEnd();
}

void draw_gauge(double speed, double energy, int x_offset) {
	static const GLfloat xplane[4] = {1.f / GAUGE_IMG_SIZE, 0.f, 0.f, 0.f };
	static const GLfloat yplane[4] = {0.f, 1.f / GAUGE_IMG_SIZE, 0.f, 0.f };

	ScopedRenderMode rm(GAUGE_BARS);

	if (Tex.GetTexture(GAUGE_ENERGY) == nullptr) return;
	if (Tex.GetTexture(GAUGE_SPEED) == nullptr) return;
	if (Tex.GetTexture(GAUGE_OUTLINE) == nullptr) return;

	Tex.BindTex(GAUGE_ENERGY);
	glTexGenfv(GL_S, GL_OBJECT_PLANE, xplane);
	glTexGenfv(GL_T, GL_OBJECT_PLANE, yplane);

	glPushMatrix();
	glTranslatef(Winsys.resolution.width - GAUGE_WIDTH + x_offset, 0, 0);
	Tex.BindTex(GAUGE_ENERGY);
	float y = ENERGY_GAUGE_BOTTOM + energy * ENERGY_GAUGE_HEIGHT;

	const GLfloat vtx1 [] = {
		0.f, y,
		GAUGE_IMG_SIZE, y,
		GAUGE_IMG_SIZE, GAUGE_IMG_SIZE,
		0.f, GAUGE_IMG_SIZE
	};
	const GLfloat vtx2 [] = {
		0.f, 0.f,
		GAUGE_IMG_SIZE, 0.f,
		GAUGE_IMG_SIZE, y,
		0.f, y
	};
	glEnableClientState(GL_VERTEX_ARRAY);

	glColor4ubv(energy_background_color);
	glVertexPointer(2, GL_FLOAT, 0, vtx1);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glColor4ubv(energy_foreground_color);
	glVertexPointer(2, GL_FLOAT, 0, vtx2);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glDisableClientState(GL_VERTEX_ARRAY);

	double speedbar_frac = 0.0;

	if (speed > SPEEDBAR_GREEN_MAX_SPEED) {
		speedbar_frac = SPEEDBAR_GREEN_FRACTION;

		if (speed > SPEEDBAR_YELLOW_MAX_SPEED) {
			speedbar_frac += SPEEDBAR_YELLOW_FRACTION;
			if (speed > SPEEDBAR_RED_MAX_SPEED) {
				speedbar_frac += SPEEDBAR_RED_FRACTION;
			} else {
				speedbar_frac += (speed - SPEEDBAR_YELLOW_MAX_SPEED) /
				                 (SPEEDBAR_RED_MAX_SPEED - SPEEDBAR_YELLOW_MAX_SPEED) * SPEEDBAR_RED_FRACTION;
			}
		} else {
			speedbar_frac += (speed - SPEEDBAR_GREEN_MAX_SPEED) /
			                 (SPEEDBAR_YELLOW_MAX_SPEED - SPEEDBAR_GREEN_MAX_SPEED) * SPEEDBAR_YELLOW_FRACTION;
		}
	} else {
		speedbar_frac +=  speed/SPEEDBAR_GREEN_MAX_SPEED * SPEEDBAR_GREEN_FRACTION;
	}

	glColor4ubv(speedbar_background_color);
	Tex.BindTex(GAUGE_SPEED);
	draw_partial_tri_fan(1.0);
	glColor4ubv(hud_white);
	draw_partial_tri_fan(std::min(1.0, speedbar_frac));

	glColor4ubv(hud_white);
	Tex.BindTex(GAUGE_OUTLINE);
	static const GLshort vtx3 [] = {
		0, 0,
		GAUGE_IMG_SIZE, 0,
		GAUGE_IMG_SIZE, GAUGE_IMG_SIZE,
		0, GAUGE_IMG_SIZE
	};
	glEnableClientState(GL_VERTEX_ARRAY);

	glVertexPointer(2, GL_SHORT, 0, vtx3);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glDisableClientState(GL_VERTEX_ARRAY);
	glPopMatrix();
}

void DrawSpeed(double speed_in, int x_offset) {
	int speed = (int)speed_in*80;
	if (speed > 9000){
		speed = 9000;
	}

	std::string speedstr = Int_StrN(speed, 4);
	if (param.use_papercut_font < 2) {
		Tex.DrawNumStr(speedstr,
		               Winsys.resolution.width - 87 + x_offset, Winsys.resolution.height-73, 1, colWhite);
	} else {
		Winsys.begin2D();
		FT.SetColor(colDDYell);
		FT.DrawString(Winsys.resolution.width - 82 + x_offset, Winsys.resolution.height - 80, speedstr);
		Winsys.end2D();
	}
}


void DrawSpeed2(double speed, int x_offset) {
	std::string speedstr = Int_StrN((int)speed, 3);
	if (param.use_papercut_font < 2) {
		Tex.DrawNumStr(speedstr,
		               Winsys.resolution.width - 87 + x_offset, Winsys.resolution.height-73, 1, colWhite);
	} else {
		Winsys.begin2D();
		FT.SetColor(colDDYell);
		FT.DrawString(Winsys.resolution.width - 82 + x_offset, Winsys.resolution.height - 80, speedstr);
		Winsys.end2D();
	}
}


void DrawWind(float dir, float speed, const CControl *ctrl) {
	if (g_game.wind_id < 1) return;

	static const int texHeight = Tex.GetSFTexture(SPEEDMETER).getSize().y;
	static const int texWidth = Tex.GetSFTexture(SPEEDMETER).getSize().x;

	Tex.Draw(SPEEDMETER, 5, Winsys.resolution.height-5-texHeight, 1.0);
	glDisable(GL_TEXTURE_2D);


	float alpha, red, blue;
	if (speed <= 50) {
		alpha = speed / 50;
		red = 0;
	} else {
		alpha = 1.f;
		red = (speed - 50) / 50;
	}
	blue = 1.f - red;

	glPushMatrix();
	glColor4f(red, 0, blue, alpha);
	glTranslatef(5 + texWidth / 2, 5 + texHeight / 2, 0);
	glRotatef(dir, 0, 0, 1);
	glEnableClientState(GL_VERTEX_ARRAY);
	static const int len = 45;
	static const GLshort vtx1 [] = {
		-5, 0,
		    5, 0,
		    5, -len,
		    - 5, -len
	    };
	glVertexPointer(2, GL_SHORT, 0, vtx1);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	// direction indicator
	float dir_angle = RADIANS_TO_ANGLES(std::atan2(ctrl->cvel.x, ctrl->cvel.z));

	glColor4f(0, 0.5, 0, 1.0);
	glRotatef(dir_angle - dir, 0, 0, 1);
	static const GLshort vtx2 [] = {
		-2, 0,
		    2, 0,
		    2, -50,
		    -2, -50
	    };
	glVertexPointer(2, GL_SHORT, 0, vtx2);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState(GL_VERTEX_ARRAY);
	glPopMatrix();

	glEnable(GL_TEXTURE_2D);

	Tex.Draw(SPEED_KNOB, 5 + texWidth / 2 - 8, Winsys.resolution.height - 5 - texWidth / 2 - 8, 1.0);
	std::string windstr = Int_StrN((int)speed, 3);
	if (param.use_papercut_font < 2) {
		Tex.DrawNumStr(windstr, 120, Winsys.resolution.height - 45, 1, colWhite);
	} else {
		Winsys.begin2D();
		FT.SetColor(colDDYell);
		FT.DrawString(120, Winsys.resolution.height - 50, windstr);
		Winsys.end2D();
	}
}

void DrawFps() {
	const  int   maxFrames = 50;
	const  int   warningFps = 58;
	const  int   fpsX = Winsys.resolution.width - 130;
	const  int   fpsY = 52;
	static int   numFrames = 0;
	static float averagefps = 0;
	static float sumTime = 0;

	if (!param.display_fps)
		return;

	if (numFrames >= maxFrames) {
		averagefps = 1 / sumTime * maxFrames;
		numFrames = 0;
		sumTime = 0;
	} else {
		sumTime += g_game.time_step;
		numFrames++;
	}
	if (averagefps < 1) return;

	const int displayedFps = (int)averagefps;
	const Color fpsColor = displayedFps <= warningFps ? colRed : colWhite;
	std::string fpsstr = Int_StrN(displayedFps);
	if (param.use_papercut_font < 2) {
		Tex.DrawNumStr(fpsstr, fpsX, fpsY, 1, fpsColor);
	} else {
		Winsys.begin2D();
		FT.SetColor(fpsColor);
		FT.DrawString(Winsys.resolution.width - 125, 43, fpsstr);
		Winsys.end2D();
	}
}

void DrawPercentBar(float fact, float x, float y) {
	Tex.BindTex(T_ENERGY_MASK);
	glColor4f(1.0, 1.0, 1.0, 1.0);

	const GLfloat tex[] = {
		0, 1,
		1, 1,
		1, 1 - fact,
		0, 1 - fact
	};
	const GLfloat vtx[] = {
		x, y,
		x + 32, y,
		x + 32, y + fact * 128,
		x, y + fact * 128
	};

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glVertexPointer(2, GL_FLOAT, 0, vtx);
	glTexCoordPointer(2, GL_FLOAT, 0, tex);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
}

void DrawCoursePosition(const CControl *ctrl) {
	double fact = ctrl->cpos.z / Course.GetPlayDimensions().y;
	if (fact > 1.0) fact = 1.0;
	glEnable(GL_TEXTURE_2D);
	DrawPercentBar(-fact, Winsys.resolution.width - 48, 280-128);
	Tex.Draw(T_MASK_OUTLINE, Winsys.resolution.width - 48, Winsys.resolution.height - 280, 1.0);
}

// -------------------------------------------------------
void DrawHud(const CControl *ctrl) {
	if (!param.show_hud)
		return;

	double speed = ctrl->cvel.Length();
	Setup2dScene();
	ScopedRenderMode rm(TEXFONT);


	draw_time(g_game.time, param.use_papercut_font < 2 ? colWhite : colDYell);
	draw_high_score(GetCourseHighScoreRecord(), param.use_papercut_font < 2 ? colWhite : colDYell);
	draw_herring_count(g_game.herring, param.use_papercut_font < 2 ? colWhite : colDYell);

	// rpm
	double rpm_mult = 1.0;
	if(ctrl->gear == 2){
		rpm_mult = 0.9;
	}
	if(ctrl->gear == 3){
		rpm_mult = 0.8;
	}
	if(ctrl->gear == 4){
		rpm_mult = 0.7;
	}
	if(ctrl->gear == 5){
		rpm_mult = 0.6;
	}
	if(ctrl->gear == 6){
		rpm_mult = 0.5;
	}
	if(ctrl->gear == 7){
		rpm_mult = 0.45;
	}
	if(ctrl->gear == 8){
		rpm_mult = 0.4;
	}
	if(ctrl->gear == 9){
		rpm_mult = 0.35;
	}

	draw_gauge(speed * 9.0* rpm_mult, ctrl->jump_amt, 0);
	DrawSpeed(speed * 6.0, 0);


	// speed
	draw_gauge(speed * 5.0, ctrl->jump_amt, -300);
	DrawSpeed2(speed * 7.0, -300);

	// gear
	std::string gear_str = Int_StrN((int)ctrl->gear, 1);
	if(ctrl->gear == 0){
		Tex.DrawNumChr('N',Winsys.resolution.width - 87 - 160, Winsys.resolution.height-73, 32,22);
	}
	else{
	Tex.DrawNumStr(gear_str,Winsys.resolution.width - 87 - 160, Winsys.resolution.height-73, 1, colWhite);
	}

	DrawFps();
	DrawCoursePosition(ctrl);
	DrawWind(Wind.Angle(), Wind.Speed(), ctrl);



}

void PrepareHudResources() {
	RefreshHighScoreHud(GetCourseHighScoreRecord());
}

void FreeHudResources() {
	delete highscore_leader_preview;
	highscore_leader_preview = nullptr;
	cached_highscore = nullptr;
	cached_highscore_revision = 0;
	cached_highscore_time.clear();
	cached_highscore_hundredths.clear();
}
