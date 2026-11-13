//========= TF2MC =========//
// VPhysics mirror of the voxel world. Engine traces already hit voxels through mc_world's
// TestCollision; VPhysics objects (pipes, stickies, ammo packs, gibs, ragdolls) need real
// collision geometry, so chunk sections near players get a static polysoup owned by the world
// entity - TF2 code that checks IsWorld() (stickybombs sticking) behaves as on a real map.
// Polysoup conversion is expensive, so only sections within a few chunks of a focus point
// (the players) are built, nearest first, under a per-frame time budget; far ones are freed.
// Floating-origin shifts move existing objects instead of rebuilding them.
//=========================//
#ifndef MC_PHYSICS_H
#define MC_PHYSICS_H
#pragma once

#include "tf2mc_core.h"

class IPhysicsObject;
class CPhysCollide;

class CMCPhysics
{
public:
	// focus: Minecraft-space points (players) to build around; radius in chunks; msBudget per call.
	// An empty focus list just applies origin shifts and removals (no building).
	void Update( mc::VoxelWorld &w, const mc::Coords &c, const std::vector<mc::Vec3d> &focus, int radiusChunks, float msBudget );
	void Clear();
	int Count() const { return (int)m_objects.size(); }

private:
	struct Obj { IPhysicsObject *pObj; CPhysCollide *pCollide; mc::Coords built; };
	void Free( Obj &o );
	void Destroy( const mc::SectionKey &k );
	void Build( mc::VoxelWorld &w, const mc::SectionKey &k );

	std::unordered_map<mc::SectionKey, Obj, mc::SectionKeyHash> m_objects;
	std::unordered_set<mc::SectionKey, mc::SectionKeyHash> m_pending;   // loaded, not built (or stale)
	mc::Coords m_coords;
	float m_flNextEvict = 0.f;
};

CMCPhysics &MCPhysics();

#endif
