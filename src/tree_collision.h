/* --------------------------------------------------------------------
EXTREME TUXRACER — tree silhouette collision

Per-texel collision test against a collidable object's rendered billboard
silhouette. Replaces the legacy single-diamond-polyhedron narrow phase in
CheckTreeCollisions (physics.cpp) for the CollArr path.

"What collides" == "what you see": the silhouette mask is built from the
texture's alpha channel using the same 0.5 threshold the renderer's
glAlphaFunc(GL_GEQUAL, 0.5) uses (ogl.cpp), so any texel that draws also
blocks.
---------------------------------------------------------------------*/
#ifndef TREE_COLLISION_H
#define TREE_COLLISION_H

#include "course.h"
#include "vectors.h"  // TVector3d (typedef of TVector3<double>)

// Decode type.textureFile, decimate to a 64x128 binary alpha mask, pack into
// type.silhouette. Safe no-op (leaves silhouette empty) if the PNG can't be
// decoded or is empty. Called once per collidable TObjectType at course load.
void BuildTreeSilhouette(TObjectType& type);

// Narrow-phase test: does a Tux collision sphere of radius `tuxRadius`
// centred at `pos` overlap the rendered silhouette of a tree at `treePos`
// with billboard dims `diam` x `height`?  `sil` is the precomputed mask.
// Returns false for an empty mask (W == 0).
bool TestTreeSilhouette(const TreeSilhouette& sil,
                        const TVector3d& pos,
                        const TVector3d& treePos,
                        double diam, double height,
                        double tuxRadius);

#endif // TREE_COLLISION_H
