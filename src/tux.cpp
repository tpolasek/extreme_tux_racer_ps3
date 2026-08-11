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


This module has been completely rewritten. Remember that the way of
defining the character has radically changed though the character is
still shaped with spheres.
---------------------------------------------------------------------*/

#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "tux.h"
#include "ogl.h"
#include "spx.h"
#include "textures.h"
#include "course.h"
#include "physics.h"
#include "ps3_log.h"
#include <GL/glu.h>
#include <algorithm>

#define MAX_ARM_ANGLE2 30.0
#define MAX_PADDLING_ANGLE2 35.0
#define MAX_EXT_PADDLING_ANGLE2 30.0
#define MAX_KICK_PADDLING_ANGLE2 20.0
#define TO_AIR_TIME 0.5
#define TO_TIME 0.14

#define SHADOW_HEIGHT 0.03 // ->0.05

/* Back-side cull margin (in Tux-local length units). A visible node whose
 * centre is more than this far behind Tux's body centre along the view
 * direction is skipped as occluded by the body. Conservative to avoid
 * silhouette popping; lower = more aggressive culling. */
#define TUX_CULL_MARGIN 0.5

#ifdef USE_STENCIL_BUFFER
static const Color shad_col(0, 0, 0, 76);
#else
static const Color shad_col(0, 0, 0, 25);
#endif

static const TCharMaterial TuxDefMat = { Color(128, 128, 128), colBlack, 0.0 };
static const TCharMaterial Highlight = { Color(204, 38, 38), colBlack, 0.0 };
CCharShape TestChar;

CCharShape::CCharShape() {
	for (int i=0; i<MAX_CHAR_NODES; i++) {
		Nodes[i] = nullptr;
		Index[i] = -1;
	}
	numNodes = 0;

	useActions = false;
	newActions = false;
	useMaterials = true;
	useHighlighting = false;
	highlighted = false;
	highlight_node = -1;
	m_lastDrawMat = nullptr;
}

CCharShape::~CCharShape() {
	for (int i=0; i<MAX_CHAR_NODES; i++) {
		if (Nodes[i] != nullptr) {
			delete Nodes[i]->action;
			delete Nodes[i];
		}
	}
}

// --------------------------------------------------------------------
//				nodes
// --------------------------------------------------------------------

std::size_t CCharShape::GetNodeIdx(std::size_t node_name) const {
	if (node_name >= MAX_CHAR_NODES) return -1;
	return Index[node_name];
}

TCharNode *CCharShape::GetNode(std::size_t node_name) const {
	std::size_t idx = GetNodeIdx(node_name);
	if (idx >= numNodes) return nullptr;
	return Nodes[idx];
}

void CCharShape::CreateRootNode() {
	TCharNode *node = new TCharNode;
	node->node_name = 0;
	node->parent = nullptr;
	node->parent_name = 99;
	node->next = nullptr;
	node->next_name = 99;
	node->child = nullptr;
	node->child_name = 99;
	node->mat = nullptr;
	node->joint = "root";
	node->render_shadow = false;
	node->visible = false;
	node->action = nullptr;
	node->trans.SetIdentity();
	node->invtrans.SetIdentity();

	NodeIndex.clear();
	NodeIndex["root"] = 0;
	Index[0] = 0;
	Nodes[0] = node;
	numNodes = 1;
}

bool CCharShape::CreateCharNode(int parent_name, std::size_t node_name, const std::string& joint, const std::string& name, const std::string& order, bool shadow) {
	TCharNode *parent = GetNode(parent_name);
	if (parent == nullptr) {
		Message("wrong parent node");
		return false;
	}
	TCharNode *node = new TCharNode;
	node->node_name = node_name;
	node->parent = parent;
	node->parent_name = parent_name;
	node->next  = nullptr;
	node->next_name = 99;
	node->child = nullptr;
	node->child_name = 99;

	if (useActions) {
		node->action = new TCharAction;
		node->action->num = 0;
		node->action->name = name;
		node->action->order = order;
		node->action->mat = "";
	} else
		node->action = nullptr;

	node->mat   = nullptr;
	node->node_idx = numNodes;
	node->visible = false;
	node->render_shadow = shadow;
	node->joint = joint;

	node->trans.SetIdentity();
	node->invtrans.SetIdentity();

	if (!joint.empty()) NodeIndex[joint] = node_name;
	Nodes[numNodes] = node;
	Index[node_name] = numNodes;

/// -------------------------------------------------------------------
	if (parent->child == nullptr) {
		parent->child = node;
		parent->child_name = node_name;
	} else {
		for (parent = parent->child; parent->next != nullptr; parent = parent->next) {}
		parent->next = node;
		parent->next_name = node_name;
	}
/// -------------------------------------------------------------------

	numNodes++;
	return true;
}

void CCharShape::AddAction(std::size_t node_name, int type, const TVector3d& vec, double val) {
	std::size_t idx = GetNodeIdx(node_name);
	TCharAction *act = Nodes[idx]->action;
	act->type[act->num] = type;
	act->vec[act->num] = vec;
	act->dval[act->num] = val;
	act->num++;
}

bool CCharShape::TranslateNode(std::size_t node_name, const TVector3d& vec) {
	TCharNode *node = GetNode(node_name);
	if (node == nullptr) return false;

	TMatrix<4, 4> TransMatrix;

	TransMatrix.SetTranslationMatrix(vec.x, vec.y, vec.z);
	node->trans = node->trans * TransMatrix;
	TransMatrix.SetTranslationMatrix(-vec.x, -vec.y, -vec.z);
	node->invtrans = TransMatrix * node->invtrans;

	if (newActions && useActions) AddAction(node_name, 0, vec, 0);
	return true;
}

bool CCharShape::RotateNode(std::size_t node_name, int axis, double angle) {
	TCharNode *node = GetNode(node_name);
	if (node == nullptr) return false;

	if (axis > 3) return false;

	TMatrix<4, 4> rotMatrix;
	char caxis = '0';
	switch (axis) {
		case 1:
			caxis = 'x';
			break;
		case 2:
			caxis = 'y';
			break;
		case 3:
			caxis = 'z';
			break;
	}

	rotMatrix.SetRotationMatrix(angle, caxis);
	node->trans = node->trans * rotMatrix;
	rotMatrix.SetRotationMatrix(-angle, caxis);
	node->invtrans = rotMatrix * node->invtrans;

	if (newActions && useActions) AddAction(node_name, axis, NullVec3, angle);
	return true;
}

bool CCharShape::RotateNode(const std::string& node_trivialname, int axis, double angle) {
	std::unordered_map<std::string, std::size_t>::const_iterator i = NodeIndex.find(node_trivialname);
	if (i == NodeIndex.end()) return false;
	return RotateNode(i->second, axis, angle);
}

void CCharShape::ScaleNode(std::size_t node_name, const TVector3d& vec) {
	TCharNode *node = GetNode(node_name);
	if (node == nullptr) return;

	TMatrix<4, 4> matrix;

	matrix.SetScalingMatrix(vec.x, vec.y, vec.z);
	node->trans = node->trans * matrix;
	matrix.SetScalingMatrix(1.0 / vec.x, 1.0 / vec.y, 1.0 / vec.z);
	node->invtrans = matrix * node->invtrans;

	if (newActions && useActions) AddAction(node_name, 4, vec, 0);
}

bool CCharShape::VisibleNode(std::size_t node_name, float level) {
	TCharNode *node = GetNode(node_name);
	if (node == nullptr) return false;

	node->visible = (level > 0);

	if (node->visible) {
		node->divisions =
		    clamp(MIN_SPHERE_DIV, (int)std::lround(param.tux_sphere_divisions * level / 10), MAX_SPHERE_DIV);
		node->radius = 1.0;
	}
	if (newActions && useActions) AddAction(node_name, 5, NullVec3, level);
	return true;
}

bool CCharShape::MaterialNode(std::size_t node_name, const std::string& mat_name) {
	TCharNode *node = GetNode(node_name);
	if (node == nullptr) return false;
	TCharMaterial *mat = GetMaterial(mat_name);
	if (mat == nullptr) return false;
	node->mat = mat;
	if (newActions && useActions) node->action->mat = mat_name;
	return true;
}

bool CCharShape::ResetNode(std::size_t node_name) {
	TCharNode *node = GetNode(node_name);
	if (node == nullptr) return false;

	node->trans.SetIdentity();
	node->invtrans.SetIdentity();
	return true;
}

bool CCharShape::ResetNode(const std::string& node_trivialname) {
	std::unordered_map<std::string, std::size_t>::const_iterator i = NodeIndex.find(node_trivialname);
	if (i == NodeIndex.end()) return false;
	return ResetNode(i->second);
}

bool CCharShape::TransformNode(std::size_t node_name, const TMatrix<4, 4>& mat, const TMatrix<4, 4>& invmat) {
	TCharNode *node = GetNode(node_name);
	if (node == nullptr) return false;

	node->trans = node->trans * mat;
	node->invtrans = invmat * node->invtrans;
	return true;
}

void CCharShape::ResetJoints() {
	ResetNode("left_shldr");
	ResetNode("right_shldr");
	ResetNode("left_hip");
	ResetNode("right_hip");
	ResetNode("left_knee");
	ResetNode("right_knee");
	ResetNode("left_ankle");
	ResetNode("right_ankle");
	ResetNode("tail");
	ResetNode("neck");
	ResetNode("head");
}

void CCharShape::Reset() {
	for (int i=0; i<MAX_CHAR_NODES; i++) {
		if (Nodes[i] != nullptr) {
			delete Nodes[i]->action;
			delete Nodes[i];
			Nodes[i] = nullptr;
		}
		Index[i] = -1;
	}
	Materials.clear();
	NodeIndex.clear();
	MaterialIndex.clear();
	numNodes = 0;

	useActions = true;
	newActions = false;
	useMaterials = true;
	useHighlighting = false;
	highlighted = false;
	highlight_node = -1;
}

// --------------------------------------------------------------------
//				materials
// --------------------------------------------------------------------

TCharMaterial* CCharShape::GetMaterial(const std::string& mat_name) {
	std::unordered_map<std::string, std::size_t>::const_iterator i = MaterialIndex.find(mat_name);
	if (i != MaterialIndex.end() && i->second < Materials.size()) {
		return &Materials[i->second];
	}
	return nullptr;
}

void CCharShape::CreateMaterial(const std::string& line) {
	TVector3d diff = SPVector3d(line, "diff");
	TVector3d spec = SPVector3d(line, "spec");
	float exp = SPFloatN(line, "exp", 50);
	std::string mat = SPStrN(line, "mat");

	Materials.emplace_back();
	Materials.back().diffuse.r = diff.x * 255;
	Materials.back().diffuse.g = diff.y * 255;
	Materials.back().diffuse.b = diff.z * 255;
	Materials.back().diffuse.a = 255;
	Materials.back().specular.r = spec.x * 255;
	Materials.back().specular.g = spec.y * 255;
	Materials.back().specular.b = spec.z * 255;
	Materials.back().specular.a = 255;
	Materials.back().exp = exp;
	if (useActions)
		Materials.back().matline = line;

	MaterialIndex[mat] = Materials.size()-1;
}

// --------------------------------------------------------------------
//				drawing
// --------------------------------------------------------------------

void CCharShape::DrawCharSphere(int num_divisions) const {
#ifdef OS_PS3
	/* Eight divisions retain a smooth silhouette at 720p while avoiding the
	 * disproportionate vertex cost of the desktop character tessellation. */
	if (num_divisions > 8) num_divisions = 8;
#endif
	GLUquadricObj *qobj = gluNewQuadric();
	gluQuadricDrawStyle(qobj, GLU_FILL);
	gluQuadricOrientation(qobj, GLU_OUTSIDE);
	gluQuadricNormals(qobj, GLU_SMOOTH);
	/* Halve the stack count to cut per-sphere vert/flush cost. The shim's
	 * gluSphere floor (stacks>=2) covers the small-divisions edge case. */
	gluSphere(qobj, 1.0, (GLint)2.0 * num_divisions, num_divisions / 2);
	gluDeleteQuadric(qobj);
}

void CCharShape::DrawNodes(const TCharNode *node, const TMatrix<4, 4>& accum,
                           const TVector3d& rootEye, const TVector3d& viewDir) {
	/* Eye-space transform for this node (mirrors the GL push/mult below). */
	TMatrix<4, 4> nodeAccum = accum * node->trans;

	glPushMatrix();
	glMultMatrix(node->trans);

	if (node->node_name == highlight_node) highlighted = true;
	const TCharMaterial *mat;
	if (highlighted && useHighlighting) {
		mat = &Highlight;
	} else {
		if (node->mat != nullptr && useMaterials) mat = node->mat;
		else mat = &TuxDefMat;
	}

	if (node->visible == true) {
		/* Back-side cull: skip nodes whose centre is clearly behind Tux's
		 * body centre along the view direction (occluded by the body).
		 * Conservative margin avoids popping at the silhouette. */
		TVector3d nodeEye(nodeAccum[3][0], nodeAccum[3][1], nodeAccum[3][2]);
		double s = DotProduct(nodeEye - rootEye, viewDir);
		if (s > -TUX_CULL_MARGIN) {
			if (mat != m_lastDrawMat) {
				set_material(mat->diffuse, mat->specular, mat->exp);
				m_lastDrawMat = mat;
			}
			DrawCharSphere(node->divisions);
		}
	}
// -------------- recursive loop -------------------------------------
	TCharNode *child = node->child;
	while (child != nullptr) {
		DrawNodes(child, nodeAccum, rootEye, viewDir);
		if (child->node_name == highlight_node) highlighted = false;
		child = child->next;
	}
// -------------------------------------------------------------------
	glPopMatrix();
}

void CCharShape::Draw() {
	static const float dummy_color[] = {0.0, 0.0, 0.0, 1.0};

	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, dummy_color);
	ScopedRenderMode rm(TUX);
	glEnable(GL_NORMALIZE);

	const TCharNode *node = GetNode(0);
	if (node == nullptr) return;

	/* Capture the eye-space base (view × tuxWorld) for back-side culling.
	 * The modelview at this point is set up by the caller before Draw(). */
	float mv[16];
	glGetFloatv(GL_MODELVIEW_MATRIX, mv);
	TMatrix<4, 4> base;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			base[c][r] = (double)mv[c * 4 + r];

	TMatrix<4, 4> rootAccum = base * node->trans;
	TVector3d rootEye(rootAccum[3][0], rootAccum[3][1], rootAccum[3][2]);
	TVector3d toCam = -rootEye;             /* root centre -> camera at origin */
	double camLen = toCam.Length();
	TVector3d viewDir = (camLen > 1e-6) ? (1.0 / camLen) * toCam : TVector3d(0.0, 0.0, 1.0);

	m_lastDrawMat = nullptr;
	DrawNodes(node, base, rootEye, viewDir);
	glDisable(GL_NORMALIZE);
	if (param.perf_level > 2) {
		TIMER_START("RACE_TUX_SHADOW");
		DrawShadow();
		TIMER_END("RACE_TUX_SHADOW");
	}
	highlighted = false;
}

// --------------------------------------------------------------------

bool CCharShape::Load(const std::string& dir, const std::string& filename, bool with_actions) {
	CSPList list;

	useActions = with_actions;
	CreateRootNode();
	newActions = true;

	if (!list.Load(dir, filename)) {
		Message("could not load character", filename);
		return false;
	}

	for (CSPList::const_iterator line = list.cbegin(); line != list.cend(); ++line) {
		int node_name = SPIntN(*line, "node", -1);
		int parent_name = SPIntN(*line, "par", -1);
		std::string mat_name = SPStrN(*line, "mat");
		std::string name = SPStrN(*line, "joint");
		std::string fullname = SPStrN(*line, "name");

		if (SPIntN(*line, "material", 0) > 0) {
			CreateMaterial(*line);
		} else {
			float visible = SPFloatN(*line, "vis", -1.f);
			bool shadow = SPBoolN(*line, "shad", false);
			std::string order = SPStrN(*line, "order");
			CreateCharNode(parent_name, node_name, name, fullname, order, shadow);
			TVector3d rot = SPVector3d(*line, "rot");
			MaterialNode(node_name, mat_name);
			for (std::size_t ii = 0; ii < order.size(); ii++) {
				int act = order[ii]-48;
				switch (act) {
					case 0: {
						TVector3d trans = SPVector3d(*line, "trans");
						TranslateNode(node_name, trans);
						break;
					}
					case 1:
						RotateNode(node_name, 1, rot.x);
						break;
					case 2:
						RotateNode(node_name, 2, rot.y);
						break;
					case 3:
						RotateNode(node_name, 3, rot.z);
						break;
					case 4: {
						TVector3d scale = SPVector3(*line, "scale", TVector3d(1, 1, 1));
						ScaleNode(node_name, scale);
						break;
					}
					case 5:
						VisibleNode(node_name, visible);
						break;
					case 9:
						RotateNode(node_name, 2, rot.z);
						break;
					default:
						break;
				}
			}
		}
	}
	newActions = false;
	return true;
}

TVector3d CCharShape::AdjustRollvector(const CControl *ctrl, const TVector3d& vel, const TVector3d& zvec) {
	TMatrix<4, 4> rot_mat;
	TVector3d v = ProjectToPlane(zvec, vel);
	v.Norm();
	if (ctrl->is_braking) {
		rot_mat = RotateAboutVectorMatrix(v, ctrl->turn_fact * BRAKING_ROLL_ANGLE);
	} else {
		rot_mat = RotateAboutVectorMatrix(v, ctrl->turn_fact * MAX_ROLL_ANGLE);
	}
	return TransformVector(rot_mat, zvec);
}

void CCharShape::AdjustOrientation(CControl *ctrl, double dtime,
                                   double dist_from_surface, const TVector3d& surf_nml) {
	TVector3d new_y, new_z;
	static const TVector3d minus_z_vec(0, 0, -1);
	static const TVector3d y_vec(0, 1, 0);

	if (dist_from_surface > 0) {
		new_y = ctrl->cvel;
		new_y.Norm();
		new_z = ProjectToPlane(new_y, TVector3d(0, -1, 0));
		new_z.Norm();
		new_z = AdjustRollvector(ctrl, ctrl->cvel, new_z);
	} else {
		new_z = -surf_nml;
		new_z = AdjustRollvector(ctrl, ctrl->cvel, new_z);
		new_y = ProjectToPlane(surf_nml, ctrl->cvel);
		new_y.Norm();
	}

	TVector3d new_x = CrossProduct(new_y, new_z);
	TMatrix<4, 4> cob_mat(new_x, new_y, new_z);
	TQuaternion new_orient = MakeQuaternionFromMatrix(cob_mat);

	if (!ctrl->orientation_initialized) {
		ctrl->orientation_initialized = true;
		ctrl->corientation = new_orient;
	}

	double time_constant = dist_from_surface > 0 ? TO_AIR_TIME : TO_TIME;

	ctrl->corientation = InterpolateQuaternions(
	                         ctrl->corientation, new_orient,
	                         std::min(dtime / time_constant, 1.0));

	ctrl->plane_nml = RotateVector(ctrl->corientation, minus_z_vec);
	ctrl->cdirection = RotateVector(ctrl->corientation, y_vec);
	cob_mat = MakeMatrixFromQuaternion(ctrl->corientation);

	// Trick rotations
	new_y = TVector3d(cob_mat[1][0], cob_mat[1][1], cob_mat[1][2]);
	TMatrix<4, 4> rot_mat = RotateAboutVectorMatrix(new_y, (ctrl->roll_factor * 360));
	cob_mat = rot_mat * cob_mat;
	new_x = TVector3d(cob_mat[0][0], cob_mat[0][1], cob_mat[0][2]);
	rot_mat = RotateAboutVectorMatrix(new_x, ctrl->flip_factor * 360);
	cob_mat = rot_mat * cob_mat;

	TransformNode(0, cob_mat, cob_mat.GetTransposed());
}

void CCharShape::AdjustJoints(double turnFact, bool isBraking,
                              double paddling_factor, double speed,
                              const TVector3d& net_force, double flap_factor) {
	double turning_angle[2];
	double paddling_angle = 0;
	double ext_paddling_angle = 0;
	double kick_paddling_angle = 0;
	double braking_angle = 0;
	double force_angle = 0;
	double turn_leg_angle = 0;
	double flap_angle = 0;

	if (isBraking) braking_angle = MAX_ARM_ANGLE2;

	paddling_angle = MAX_PADDLING_ANGLE2 * std::sin(paddling_factor * M_PI);
	ext_paddling_angle = MAX_EXT_PADDLING_ANGLE2 * std::sin(paddling_factor * M_PI);
	kick_paddling_angle = MAX_KICK_PADDLING_ANGLE2 * std::sin(paddling_factor * M_PI * 2.0);

	turning_angle[0] = std::max(-turnFact,0.0) * MAX_ARM_ANGLE2;
	turning_angle[1] = std::max(turnFact,0.0) * MAX_ARM_ANGLE2;
	flap_angle = MAX_ARM_ANGLE2 * (0.5 + 0.5 * std::sin(M_PI * flap_factor * 6 - M_PI / 2));
	force_angle = clamp(-20.0, -net_force.z / 300.0, 20.0);
	turn_leg_angle = turnFact * 10;

	ResetJoints();

	RotateNode("left_shldr", 3,
	           std::min(braking_angle + paddling_angle + turning_angle[0], MAX_ARM_ANGLE2) + flap_angle);
	RotateNode("right_shldr", 3,
	           std::min(braking_angle + paddling_angle + turning_angle[1], MAX_ARM_ANGLE2) + flap_angle);

	RotateNode("left_shldr", 2, -ext_paddling_angle);
	RotateNode("right_shldr", 2, ext_paddling_angle);
	RotateNode("left_hip", 3, -20 + turn_leg_angle + force_angle);
	RotateNode("right_hip", 3, -20 - turn_leg_angle + force_angle);

	RotateNode("left_knee", 3,
	           -10 + turn_leg_angle - std::min(35.0, speed) + kick_paddling_angle + force_angle);
	RotateNode("right_knee", 3,
	           -10 - turn_leg_angle - std::min(35.0, speed) - kick_paddling_angle + force_angle);
	RotateNode("left_ankle", 3, -20 + std::min(50.0, speed));
	RotateNode("right_ankle", 3, -20 + std::min(50.0, speed));
	RotateNode("tail", 3, turnFact * 20);
	RotateNode("neck", 3, -50);
	RotateNode("head", 3, -30);
	RotateNode("head", 2, -turnFact * 70);
}

// --------------------------------------------------------------------
//				collision
// --------------------------------------------------------------------

bool CCharShape::CheckPolyhedronCollision(const TCharNode *node, const TMatrix<4, 4>& modelMatrix,
        const TMatrix<4, 4>& invModelMatrix, const TPolyhedron& ph) {
	bool hit = false;

	TMatrix<4, 4> newModelMatrix = modelMatrix * node->trans;
	TMatrix<4, 4> newInvModelMatrix = node->invtrans * invModelMatrix;

	if (node->visible) {
		TPolyhedron newph = ph;
		TransPolyhedron(newInvModelMatrix, newph);
		hit = IntersectPolyhedron(newph);
	}

	if (hit == true) return hit;
	const TCharNode *child = node->child;
	while (child != nullptr) {
		hit = CheckPolyhedronCollision(child, newModelMatrix, newInvModelMatrix, ph);
		if (hit == true) return hit;
		child = child->next;
	}
	return false;
}

bool CCharShape::CheckCollision(const TPolyhedron& ph) {
	const TCharNode *node = GetNode(0);
	if (node == nullptr) return false;
	const TMatrix<4, 4>& identity = TMatrix<4, 4>::getIdentity();
	return CheckPolyhedronCollision(node, identity, identity, ph);
}

bool CCharShape::Collision(const TVector3d& pos, const TPolyhedron& ph) {
	ResetNode(0);
	TranslateNode(0, TVector3d(pos.x, pos.y, pos.z));
	return CheckCollision(ph);
}

// --------------------------------------------------------------------
//				shadow
// --------------------------------------------------------------------

void CCharShape::DrawShadowVertex(double x, double y, double z, const TMatrix<4, 4>& mat) const {
	TVector3d pt(x, y, z);
	pt = TransformPoint(mat, pt);
	double old_y = pt.y;
	// TUX_SHADOW mode disables GL_LIGHTING, so the per-vertex normal is dead
	// work -- skip the cross+normalize in FindCourseNormalAndY and just sample
	// the course Y (FindYCoord has a 1-entry cache too).
	double course_y = Course.FindYCoord(pt.x, pt.z);
	pt.y = course_y + SHADOW_HEIGHT;
	if (pt.y > old_y) pt.y = old_y;
	glVertex3(pt);
}

void CCharShape::DrawShadowSphere(const TMatrix<4, 4>& mat) const {
	int div = param.tux_shadow_sphere_divisions;
	if (div < 2) div = 2;

	const int slices = div * 2;
	const int stacks = div;
	const double d_theta = 2.0 * M_PI / slices;
	const double d_phi = M_PI / stacks;

	/* Appends into the enclosing glBegin(GL_TRIANGLE_STRIP) opened by
	 * DrawShadow -- one draw for the whole silhouette rather than one per
	 * shadow-casting body node. Two leading north-pole duplicates form two
	 * degenerate triangles that bridge cleanly to the previous sphere's last
	 * vert (and are harmless on the first sphere). */
	DrawShadowVertex(0.0, 0.0, 1.0, mat);
	DrawShadowVertex(0.0, 0.0, 1.0, mat);
	double previous_x = 0.0, previous_y = 0.0, previous_z = 1.0;
	for (int i = 0; i < stacks; ++i) {
		const double phi0 = i * d_phi;
		const double phi1 = (i + 1) * d_phi;
		const double sin_phi0 = std::sin(phi0);
		const double cos_phi0 = std::cos(phi0);
		const double sin_phi1 = std::sin(phi1);
		const double cos_phi1 = std::cos(phi1);

		if (i > 0) {
			DrawShadowVertex(previous_x, previous_y, previous_z, mat);
			DrawShadowVertex(sin_phi0, 0.0, cos_phi0, mat);
		}

		for (int j = 0; j <= slices; ++j) {
			const double theta = j * d_theta;
			const double cos_theta = std::cos(theta);
			const double sin_theta = std::sin(theta);

			DrawShadowVertex(cos_theta * sin_phi0,
			                 sin_theta * sin_phi0,
			                 cos_phi0, mat);

			previous_x = cos_theta * sin_phi1;
			previous_y = sin_theta * sin_phi1;
			previous_z = cos_phi1;
			DrawShadowVertex(previous_x, previous_y, previous_z, mat);
		}
	}
}

void CCharShape::TraverseDagForShadow(const TCharNode *node, const TMatrix<4, 4>& mat) const {
	TMatrix<4, 4> new_matrix = mat * node->trans;
	if (node->visible && node->render_shadow)
		DrawShadowSphere(new_matrix);

	TCharNode* child = node->child;
	while (child != nullptr) {
		TraverseDagForShadow(child, new_matrix);
		child = child->next;
	}
}

void CCharShape::DrawShadow() const {
	if (g_game.light_id == 1 || g_game.light_id == 3) return;

	ScopedRenderMode rm(TUX_SHADOW);
	glColor(shad_col);

	const TCharNode *node = GetNode(0);
	if (node == nullptr) {
		Message("couldn't find tux's root node");
		return;
	}
	/* One strip for the whole silhouette: each shadow sphere's two leading
	 * north-pole duplicates form degenerate bridges to the previous sphere,
	 * so all body nodes flush in a single RSX draw. */
	glBegin(GL_TRIANGLE_STRIP);
	TraverseDagForShadow(node, TMatrix<4, 4>::getIdentity());
	glEnd();
}

// --------------------------------------------------------------------
//				testing and tools
// --------------------------------------------------------------------

std::string CCharShape::GetNodeJoint(std::size_t idx) const {
	if (idx >= numNodes) return "";
	TCharNode *node = Nodes[idx];
	if (node == nullptr) return "";
	if (!node->joint.empty()) return node->joint;
	else return Int_StrN((int)node->node_name);
}

std::size_t CCharShape::GetNodeName(std::size_t idx) const {
	if (idx >= numNodes) return -1;
	return Nodes[idx]->node_name;
}

std::size_t CCharShape::GetNodeName(const std::string& node_trivialname) const {
	return NodeIndex.at(node_trivialname);
}


void CCharShape::RefreshNode(std::size_t idx) {
	if (idx >= numNodes) return;
	TMatrix<4, 4> TempMatrix;
	char caxis;
	double angle;

	TCharNode *node = Nodes[idx];
	TCharAction *act = node->action;
	if (act == nullptr) return;
	if (act->num < 1) return;

	node->trans.SetIdentity();
	node->invtrans.SetIdentity();

	for (std::size_t i=0; i<act->num; i++) {
		int type = act->type[i];
		const TVector3d& vec = act->vec[i];
		double dval = act->dval[i];

		switch (type) {
			case 0:
				TempMatrix.SetTranslationMatrix(vec.x, vec.y, vec.z);
				node->trans = node->trans * TempMatrix;
				TempMatrix.SetTranslationMatrix(-vec.x, -vec.y, -vec.z);
				node->invtrans = TempMatrix * node->invtrans;
				break;
			case 1:
				caxis = 'x';
				angle = dval;
				TempMatrix.SetRotationMatrix(angle, caxis);
				node->trans = node->trans * TempMatrix;
				TempMatrix.SetRotationMatrix(-angle, caxis);
				node->invtrans = TempMatrix * node->invtrans;
				break;
			case 2:
				caxis = 'y';
				angle = dval;
				TempMatrix.SetRotationMatrix(angle, caxis);
				node->trans = node->trans * TempMatrix;
				TempMatrix.SetRotationMatrix(-angle, caxis);
				node->invtrans = TempMatrix * node->invtrans;
				break;
			case 3:
				caxis = 'z';
				angle = dval;
				TempMatrix.SetRotationMatrix(angle, caxis);
				node->trans = node->trans * TempMatrix;
				TempMatrix.SetRotationMatrix(-angle, caxis);
				node->invtrans = TempMatrix * node->invtrans;
				break;
			case 4:
				TempMatrix.SetScalingMatrix(vec.x, vec.y, vec.z);
				node->trans = node->trans * TempMatrix;
				TempMatrix.SetScalingMatrix(1.0 / vec.x, 1.0 / vec.y, 1.0 / vec.z);
				node->invtrans = TempMatrix * node->invtrans;
				break;
			case 5:
				VisibleNode(node->node_name, dval);
				break;
			default:
				break;
		}
	}
}

const std::string& CCharShape::GetNodeFullname(std::size_t idx) const {
	if (idx >= numNodes) return emptyString;
	return Nodes[idx]->action->name;
}

std::size_t CCharShape::GetNumActs(std::size_t idx) const {
	if (idx >= numNodes) return -1;
	return Nodes[idx]->action->num;
}

TCharAction *CCharShape::GetAction(std::size_t idx) const {
	if (idx >= numNodes) return nullptr;
	return Nodes[idx]->action;
}

void CCharShape::PrintAction(std::size_t idx) const {
	if (idx >= numNodes) return;
	TCharAction *act = Nodes[idx]->action;
	PrintInt((int)act->num);
	for (std::size_t i=0; i<act->num; i++) {
		PrintInt(act->type[i]);
		PrintDouble(act->dval[i]);
		PrintVector(act->vec[i]);
	}
}

void CCharShape::PrintNode(std::size_t idx) const {
	TCharNode *node = Nodes[idx];
	PrintInt("node: ", (int)node->node_name);
	PrintInt("parent: ", (int)node->parent_name);
	PrintInt("child: ", (int)node->child_name);
	PrintInt("next: ", (int)node->next_name);
}

void CCharShape::SaveCharNodes(const std::string& dir, const std::string& filename) {
	CSPList list;

	list.Add("# Generated by Tuxracer tools");
	list.Add();
	if (!Materials.empty()) {
		list.Add("# Materials:");
		for (std::size_t i=0; i<Materials.size(); i++)
			if (!Materials[i].matline.empty())
				list.Add(Materials[i].matline);
		list.Add();
	}

	list.Add("# Nodes:");
	for (std::size_t i=1; i<numNodes; i++) {
		TCharNode* node = Nodes[i];
		TCharAction* act = node->action;
		if (node->parent_name >= node->node_name) Message("wrong parent index");
		std::string line = "*[node] " + Int_StrN((int)node->node_name);
		line += " [par] " + Int_StrN((int)node->parent_name);

		if (!act->order.empty()) {
			bool rotflag = false;
			TVector3d rotation;
			line += " [order] " + act->order;
			for (std::size_t ii=0; ii<act->order.size(); ii++) {
				int aa = act->order[ii]-48;
				switch (aa) {
					case 0:
						line += " [trans] " + Vector_StrN(act->vec[ii], 2);
						break;
					case 4:
						line += " [scale] " + Vector_StrN(act->vec[ii], 2);
						break;
					case 1:
						rotation.x = act->dval[ii];
						rotflag = true;
						break;
					case 2:
						rotation.y = act->dval[ii];
						rotflag = true;
						break;
					case 3:
						rotation.z = act->dval[ii];
						rotflag = true;
						break;
					case 5:
						line += " [vis] " + Int_StrN((int)act->dval[ii]);
						break;
					case 9:
						rotation.z = act->dval[ii];
						rotflag = true;
						break;
				}
			}
			if (rotflag) line += " [rot] " + Vector_StrN(rotation, 2);
		}
		if (!act->mat.empty()) line += " [mat] " + act->mat;
		if (!node->joint.empty()) line += " [joint] " + node->joint;
		if (!act->name.empty()) line += " [name] " + act->name;
		if (node->render_shadow) line += " [shad] 1";

		list.Add(line);
		if (i<numNodes-3) {
			if (node->visible && !Nodes[i+1]->visible) list.Add();
			const std::string& joint = Nodes[i+2]->joint;
			if (joint.empty()) list.Add("# " + joint);
		}
	}
	list.Save(dir, filename);
}
