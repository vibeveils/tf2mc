//========= TF2MC =========//
// VPhysics mirror of the voxel world. Engine traces already hit voxels through mc_world's
// TestCollision; VPhysics objects (pipes, stickies, ammo packs, gibs, ragdolls) need real
// collision geometry, so every chunk section gets a static polysoup owned by the world entity
// - TF2 code that checks IsWorld() (stickybombs sticking) behaves exactly as on a real map.
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
	void Update( mc::VoxelWorld &w, const mc::Coords &c, int budget );
	void Clear();
	int Count() const { return (int)m_objects.size(); }

private:
	struct Obj { IPhysicsObject *pObj; CPhysCollide *pCollide; mc::Coords built; };
	void Free( Obj &o );
	void Destroy( const mc::SectionKey &k );
	void Build( mc::VoxelWorld &w, const mc::SectionKey &k );

	std::unordered_map<mc::SectionKey, Obj, mc::SectionKeyHash> m_objects;
	std::vector<mc::SectionKey> m_pending;
	mc::Coords m_coords;
};

CMCPhysics &MCPhysics();

#endif
