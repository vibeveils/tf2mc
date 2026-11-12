//========= TF2MC =========//
#include "cbase.h"
#include "mc_physics.h"
#include "physics_shared.h"
#include "vphysics_interface.h"
#ifdef CLIENT_DLL
#include "c_world.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static CMCPhysics g_MCPhysics;
CMCPhysics &MCPhysics() { return g_MCPhysics; }

static void *WorldGameData()
{
#ifdef CLIENT_DLL
	return static_cast<void *>( GetClientWorldEntity() );
#else
	return static_cast<void *>( GetWorldEntity() );
#endif
}

void CMCPhysics::Free( Obj &o )
{
	if ( o.pObj && physenv ) physenv->DestroyObject( o.pObj );
	if ( o.pCollide && physcollision ) physcollision->DestroyCollide( o.pCollide );
	o.pObj = NULL;
	o.pCollide = NULL;
}

void CMCPhysics::Clear()
{
	for ( auto &it : m_objects ) Free( it.second );
	m_objects.clear();
	m_pending.clear();
}

void CMCPhysics::Destroy( const mc::SectionKey &k )
{
	auto it = m_objects.find( k );
	if ( it == m_objects.end() ) return;
	Free( it->second );
	m_objects.erase( it );
}

void CMCPhysics::Update( mc::VoxelWorld &w, const mc::Coords &c, int budget )
{
	if ( !physenv || !physcollision ) return;

	if ( c != m_coords )
	{
		if ( !c.SameScale( m_coords ) )
		{
			for ( auto &it : m_objects ) m_pending.push_back( it.first );   // scale change: rebuild
		}
		else
		{
			// Floating origin moved: slide every static polysoup by the same offset
			for ( auto &it : m_objects )
			{
				mc::Vec3d d = c.ShiftFrom( it.second.built );
				Vector pos( (float)d.x, (float)d.y, (float)d.z );
				it.second.pObj->SetPosition( pos, vec3_angle, true );
			}
		}
		m_coords = c;
	}

	std::vector<mc::SectionKey> removed;
	w.TakeRemoved( removed, mc::CH_PHYSICS );
	for ( size_t i = 0; i < removed.size(); ++i ) Destroy( removed[i] );
	w.TakeDirty( m_pending, mc::CH_PHYSICS );
	int n = 0;
	while ( !m_pending.empty() && n++ < budget )
	{
		mc::SectionKey k = m_pending.back();
		m_pending.pop_back();
		Build( w, k );
	}
}

void CMCPhysics::Build( mc::VoxelWorld &w, const mc::SectionKey &k )
{
	Destroy( k );
	if ( !w.IsColumnLoaded( k.cx, k.cz ) ) return;
	std::vector<mc::Vec3f> tris;
	mc::BuildSectionCollision( w, m_coords, k.cx, k.sy, k.cz, tris );
	if ( tris.size() < 3 ) return;

	int material = physprops ? physprops->GetSurfaceIndex( "concrete" ) : 0;
	CPhysPolysoup *soup = physcollision->PolysoupCreate();
	for ( size_t i = 0; i + 2 < tris.size(); i += 3 )
	{
		Vector a( tris[i].x, tris[i].y, tris[i].z ), b( tris[i + 1].x, tris[i + 1].y, tris[i + 1].z ), cc( tris[i + 2].x, tris[i + 2].y, tris[i + 2].z );
		physcollision->PolysoupAddTriangle( soup, a, b, cc, 0 );
	}
	CPhysCollide *pCollide = physcollision->ConvertPolysoupToCollide( soup, false );
	physcollision->PolysoupDestroy( soup );
	if ( !pCollide ) return;

	objectparams_t params = g_PhysDefaultObjectParams;
	params.pGameData = WorldGameData();
	IPhysicsObject *pObj = physenv->CreatePolyObjectStatic( pCollide, material, vec3_origin, vec3_angle, &params );
	if ( !pObj ) { physcollision->DestroyCollide( pCollide ); return; }
	Obj o = { pObj, pCollide, m_coords };
	m_objects[k] = o;
}
