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

#include "textures.h"
#include "course_render.h"
#include "course.h"
#include "ogl.h"
#include "quadtree.h"
#include "particles.h"
#include "env.h"
#include "game_ctrl.h"
#include "physics.h"
#include "tux.h"
#include <vector>
#include <cmath>

#define TEX_SCALE 6
static const bool clip_course = true;

void setup_course_tex_gen() {
	static const GLfloat xplane[4] = {1.f / TEX_SCALE, 0.f, 0.f, 0.f };
	static const GLfloat zplane[4] = {0.f, 0.f, 1.f / TEX_SCALE, 0.f };
	glTexGenfv(GL_S, GL_OBJECT_PLANE, xplane);
	glTexGenfv(GL_T, GL_OBJECT_PLANE, zplane);
}

void RenderCourse() {
	ScopedRenderMode rm(COURSE);
	setup_course_tex_gen();
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	set_material(colWhite, colBlack, 1.0);
	const CControl *ctrl = g_game.player->ctrl;
	float detail_level = param.course_detail_level;
#ifdef OS_PS3
	/* Rough late-course sections can exceed the 16.67 ms frame budget at
	 * the desktop default. A slightly coarser distant mesh is hidden well
	 * by the snow texture and fog while preserving nearby terrain shape. */
	if (detail_level > 50.f) detail_level = 50.f;
#endif
	UpdateQuadtree(ctrl->viewpos, detail_level);
	RenderQuadtree();
}

void DrawTrees() {
	const CControl*	ctrl = g_game.player->ctrl;

	ScopedRenderMode rm(TREES);
	double fwd_clip_limit = param.forward_clip_distance;
	double bwd_clip_limit = param.backward_clip_distance;

	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	set_material(colWhite, colBlack, 1.0);

	/* Reusable accumulators (static so capacity persists across frames). */
	static std::vector<GLfloat> vpos;
	static std::vector<GLshort>  vtex;
	static std::vector<GLfloat>  vnrm;

	// ===================== Trees (crossed billboards) =====================
	/* Bake world-space vertex positions and issue one glDrawArrays per
	 * texture run, instead of one flush per tree. The per-tree matrix
	 * stack (translate + 1-degree Y rotation) is folded into the offsets. */
	{
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glNormal3i(0, 0, 1);

		static const GLshort treeTex[] = {
			0,1, 1,1, 1,0, 0,0,
			0,1, 1,1, 1,0, 0,0
		};
		const float ang = (param.perf_level > 1)
			? (1.0f * 3.14159265f / 180.0f) : 0.0f;
		const float c = cosf(ang), s = sinf(ang);

		std::size_t cur_type = (std::size_t)-1;
		for (std::size_t i = 0; i < Course.CollArr.size(); i++) {
			if (clip_course) {
				if (ctrl->viewpos.z - Course.CollArr[i].pt.z > fwd_clip_limit) continue;
				if (Course.CollArr[i].pt.z - ctrl->viewpos.z > bwd_clip_limit) continue;
			}
			std::size_t tt = Course.CollArr[i].tree_type;
			if (tt != cur_type) {
				if (!vpos.empty()) {
					glVertexPointer(3, GL_FLOAT, 0, vpos.data());
					glTexCoordPointer(2, GL_SHORT, 0, vtex.data());
					glDrawArrays(GL_QUADS, 0, (GLsizei)(vpos.size() / 3));
					vpos.clear();
					vtex.clear();
				}
				cur_type = tt;
				Course.ObjTypes[tt].texture->Bind();
			}
			const TCollidable& t = Course.CollArr[i];
			float r = (float)(t.diam / 2.0);
			float h = (float)t.height;
			float px = (float)t.pt.x, py = (float)t.pt.y, pz = (float)t.pt.z;
			float cr = c * r, sr = s * r;
			/* Quad 1: rotated X axis (c,0,-s) */
			GLfloat q1[12] = {
				px - cr, py,     pz + sr,
				px + cr, py,     pz - sr,
				px + cr, py + h, pz - sr,
				px - cr, py + h, pz + sr,
			};
			vpos.insert(vpos.end(), q1, q1 + 12);
			/* A second crossed plane is nearly edge-on to the downhill
			 * camera but doubles alpha-tested overdraw. The
			 * forward-facing plane preserves the tree silhouette. */
			vtex.insert(vtex.end(), treeTex, treeTex + 8);
		}
		if (!vpos.empty()) {
			glVertexPointer(3, GL_FLOAT, 0, vpos.data());
			glTexCoordPointer(2, GL_SHORT, 0, vtex.data());
			glDrawArrays(GL_QUADS, 0, (GLsizei)(vpos.size() / 3));
			vpos.clear();
			vtex.clear();
		}
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);
	}

	// ===================== Items (camera-facing billboards) =====================
	/* Per-item lighting normal supplied via glNormalPointer so a whole
	 * texture run can be drawn in one flush. */
	{
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnableClientState(GL_NORMAL_ARRAY);

		static const GLshort itemTex[] = { 0,1, 1,1, 1,0, 0,0 };

		const TObjectType* item_type = nullptr;
		for (std::size_t i = 0; i < Course.NocollArr.size(); i++) {
			if (Course.NocollArr[i].collectable == 0 || Course.NocollArr[i].type.drawable == false) continue;
			if (clip_course) {
				if (ctrl->viewpos.z - Course.NocollArr[i].pt.z > fwd_clip_limit) continue;
				if (Course.NocollArr[i].pt.z - ctrl->viewpos.z > bwd_clip_limit) continue;
			}
			const TObjectType* it = &Course.NocollArr[i].type;
			if (it != item_type) {
				if (!vpos.empty()) {
					glVertexPointer(3, GL_FLOAT, 0, vpos.data());
					glTexCoordPointer(2, GL_SHORT, 0, vtex.data());
					glNormalPointer(GL_FLOAT, 0, vnrm.data());
					glDrawArrays(GL_QUADS, 0, (GLsizei)(vpos.size() / 3));
					vpos.clear();
					vtex.clear();
					vnrm.clear();
				}
				item_type = it;
				it->texture->Bind();
			}
			const TItem& item = Course.NocollArr[i];
			double itemRadius = item.diam / 2.0;
			double itemHeight = item.height;

			TVector3d normal;
			if (it->use_normal) normal = it->normal;
			else { normal = ctrl->viewpos - item.pt; normal.Norm(); }
			TVector3d drawNormal = normal;   /* full normal (with y) for lighting */
			normal.y = 0.0;
			normal.Norm();                   /* horizontal normal for vert offsets */

			float nx = (float)normal.x, nz = (float)normal.z;
			float px = (float)item.pt.x, py = (float)item.pt.y, pz = (float)item.pt.z;
			float ir = (float)itemRadius, ih = (float)itemHeight;

			GLfloat v[12] = {
				px - ir * nz, py,       pz + ir * nx,
				px + ir * nz, py,       pz - ir * nx,
				px + ir * nz, py + ih,  pz - ir * nx,
				px - ir * nz, py + ih,  pz + ir * nx,
			};
			vpos.insert(vpos.end(), v, v + 12);
			vtex.insert(vtex.end(), itemTex, itemTex + 8);
			for (int k = 0; k < 4; k++) {
				vnrm.push_back((float)drawNormal.x);
				vnrm.push_back((float)drawNormal.y);
				vnrm.push_back((float)drawNormal.z);
			}
		}
		if (!vpos.empty()) {
			glVertexPointer(3, GL_FLOAT, 0, vpos.data());
			glTexCoordPointer(2, GL_SHORT, 0, vtex.data());
			glNormalPointer(GL_FLOAT, 0, vnrm.data());
			glDrawArrays(GL_QUADS, 0, (GLsizei)(vpos.size() / 3));
			vpos.clear();
			vtex.clear();
			vnrm.clear();
		}
		glDisableClientState(GL_NORMAL_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);
	}
}

// --------------------------------------------------------------------
//                   collision debug overlay
// --------------------------------------------------------------------
#ifdef DRAW_COLLISION_DEBUG

void DrawCollisionDebug() {
	const CControl* ctrl = g_game.player->ctrl;

	glPushAttrib(GL_ENABLE_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_LIGHTING);
	glDisable(GL_DEPTH_TEST);   // draw over everything
	glColor4f(0.5f, 1.0f, 0.2f, 1.0f);   // neon green

	// Tree silhouettes — stroke each contour polygon in the billboard plane.
	const double fwd_clip_limit = param.forward_clip_distance;
	const double bwd_clip_limit = param.backward_clip_distance;
	for (std::size_t i = 0; i < Course.CollArr.size(); i++) {
		if (clip_course) {
			if (ctrl->viewpos.z - Course.CollArr[i].pt.z > fwd_clip_limit) continue;
			if (Course.CollArr[i].pt.z - ctrl->viewpos.z > bwd_clip_limit) continue;
		}
		const TCollidable& t = Course.CollArr[i];
		const TreeSilhouette& sil = Course.ObjTypes[t.tree_type].silhouette;
		if (sil.contourUV.empty()) continue;

		const double r = t.diam * 0.5;
		const double h = t.height;
		const double px = t.pt.x, py = t.pt.y, pz = t.pt.z;

		// GL_LINE_STRIP (not LOOP) — the shim doesn't map GL_LINE_LOOP to
		// an RSX primitive. Close the loop by re-emitting the first vertex.
		glBegin(GL_LINE_STRIP);
		for (const TVector2d& uv : sil.contourUV)
			glVertex3d(px + (uv.x - 0.5) * 2.0 * r,
			           py + uv.y * h,
			           pz);
		glVertex3d(px + (sil.contourUV.front().x - 0.5) * 2.0 * r,
		           py + sil.contourUV.front().y * h,
		           pz);
		glEnd();
	}

	// Player collision sphere: three perpendicular rings matching the
	// largest rendered sphere of the character (same proxy the tree
	// collision uses), so what you see is what collides.
	const CCharShape *shape = g_game.character->shape;
	const TVector3d center = shape->CollisionCenterWorld(ctrl->cpos);
	const double radius = shape->CollisionRadius();
	constexpr int kSegments = 24;
	constexpr double kTwoPi = 6.2831853;

	for (int axis = 0; axis < 3; ++axis) {
		glBegin(GL_LINE_STRIP);
		for (int i = 0; i <= kSegments; ++i) {
			const double a = (double)i * kTwoPi / kSegments;
			const double ca = cos(a), sa = sin(a);
			double x = center.x, y = center.y, z = center.z;
			if (axis == 0) { x += ca * radius; z += sa * radius; }
			else if (axis == 1) { x += ca * radius; y += sa * radius; }
			else { y += sa * radius; z += ca * radius; }
			glVertex3d(x, y, z);
		}
		glEnd();
	}

	glPopAttrib();
}

#endif // DRAW_COLLISION_DEBUG
