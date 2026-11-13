//========= TF2MC bridge - engine independent core =========//
// Turns chunk sections into render quads (Source space) and physics triangles.
//==========================================================//
#pragma once

#include "mc_world.h"

namespace mc
{
	struct MeshVertex
	{
		float pos[3];
		float uv[2];
		uint8_t rgba[4];   // base colour: tint * face shade * ambient occlusion (no light)
		uint8_t sky;       // smooth sky light 0..15 (scaled by daylight at draw time)
		uint8_t blk;       // smooth block light 0..15 (torches, lava...)
	};

	// Minecraft's light curve (0..15 -> brightness), with a small ambient floor.
	float LightCurve( float level );

	struct SectionMesh
	{
		std::vector<MeshVertex> opaque;       // 4 vertices per quad (includes alpha-tested cutouts)
		std::vector<MeshVertex> translucent;  // 4 vertices per quad
		size_t QuadCount() const { return ( opaque.size() + translucent.size() ) / 4; }
	};

	struct MesherOptions
	{
		bool clockwise = true;        // Source/D3D front faces are clockwise
		bool ambientOcclusion = true;
		bool textured = true;         // false -> flat per-block colours (no atlas available)
		// Fraction of a tile kept away from tile edges. The atlas is point sampled without mips, so
		// this only guards against float rounding picking the neighbouring tile; 0.5 texel (the old
		// value) squeezed every 16px texture into 15px and shifted it by half a pixel.
		float texelInset = 0.002f;
	};

	void BuildSectionMesh( const VoxelWorld &w, const Coords &c, int cx, int sy, int cz, const MesherOptions &opt, SectionMesh &out );

	// Collision triangles (Source space) for a section: exposed faces of colliding blocks,
	// greedily merged for full cubes. 3 vertices per triangle.
	void BuildSectionCollision( const VoxelWorld &w, const Coords &c, int cx, int sy, int cz, std::vector<Vec3f> &tris );

	uint32_t TintColor( Tint t );
}
