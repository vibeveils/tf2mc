//========= TF2MC bridge - engine independent core =========//
// Swept AABB / ray queries against the voxel world (in Minecraft space).
//==========================================================//
#pragma once

#include "mc_world.h"

namespace mc
{
	struct SweepResult
	{
		bool hit = false;
		bool startSolid = false;
		bool allSolid = false;
		double fraction = 1.0;     // 0..1 along delta, *without* any epsilon backoff
		Vec3d normal;              // MC space, unit axis
		BlockPos block;            // block that was hit
	};

	// Sweep a box (centre `start`, half extents `half`, all MC space) by `delta`.
	// Zero half extents == a ray. Zero delta == a position test.
	SweepResult SweepBox( const VoxelWorld &w, const Vec3d &start, const Vec3d &delta, const Vec3d &half );

	// True if the box overlaps any colliding block.
	bool BoxOverlaps( const VoxelWorld &w, const Vec3d &mins, const Vec3d &maxs, BlockPos *outBlock = nullptr );

	// Voxel DDA ray cast (Amanatides & Woo) for block picking. Hits any non-air block
	// (including non-colliding plants) unless solidOnly. Liquids are skipped.
	struct PickResult
	{
		bool hit = false;
		BlockPos block;
		Face face = FACE_UP;
		Vec3d point;       // MC space hit point
		double distance = 0;
	};
	PickResult PickBlock( const VoxelWorld &w, const Vec3d &origin, const Vec3d &dir, double maxDist, bool solidOnly = false );

	// Neighbour position across a face
	BlockPos Offset( const BlockPos &p, Face f );

	// Collision boxes of a block in absolute MC coords (fences etc. may extend to y+1.5).
	// Returns the number written (<= maxBoxes).
	int BlockCollisionBoxes( const VoxelWorld &w, int x, int y, int z, Vec3d *mins, Vec3d *maxs, int maxBoxes );

	// True if any climbable block (ladder, vines, scaffolding) overlaps the box.
	bool BoxTouchesClimbable( const VoxelWorld &w, const Vec3d &mins, const Vec3d &maxs );
}
