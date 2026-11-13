//========= TF2MC =========//
#include "cbase.h"
#include "mc_physics.h"
#include "physics_shared.h"
#include "vphysics_interface.h"
#ifdef CLIENT_DLL
#include "c_world.h"
#else
#include "world.h"
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

void CMCPhysics::Update( mc::VoxelWorld &w, const mc::Coords &c, const std::vector<mc::Vec3d> &focus, int radiusChunks, float msBudget )
{
	if ( !physenv || !physcollision ) return;

	if ( c != m_coords )
	{
		if ( !c.SameScale( m_coords ) )
		{
			// scale change: everything is stale
			for ( auto &it : m_objects ) { m_pending.insert( it.first ); Free( it.second ); }
			m_objects.clear();
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

	std::vector<mc::SectionKey> keys;
	w.TakeRemoved( keys, mc::CH_PHYSICS );
	for ( size_t i = 0; i < keys.size(); ++i ) { Destroy( keys[i] ); m_pending.erase( keys[i] ); }
	keys.clear();
	w.TakeDirty( keys, mc::CH_PHYSICS );
	for ( size_t i = 0; i < keys.size(); ++i ) m_pending.insert( keys[i] );
	if ( focus.empty() ) return;

	struct F { int cx, sy, cz; };
	std::vector<F> f( focus.size() );
	for ( size_t i = 0; i < focus.size(); ++i )
		f[i] = { mc::FloorDiv16( mc::FloorI( focus[i].x ) ), mc::FloorDiv16( mc::FloorI( focus[i].y ) ), mc::FloorDiv16( mc::FloorI( focus[i].z ) ) };
	// Distance (in sections) to the nearest focus; -1 = out of range
	auto rank = [&]( const mc::SectionKey &k, int radius ) -> int {
		int best = -1;
		for ( const F &p : f )
		{
			int dx = abs( k.cx - p.cx ), dz = abs( k.cz - p.cz ), dy = abs( k.sy - p.sy );
			if ( dx > radius || dz > radius || dy > 2 ) continue;
			int d = dx * dx + dz * dz + dy * dy;
			if ( best < 0 || d < best ) best = d;
		}
		return best;
	};

	// Free sections well outside every focus (hysteresis: radius + 2), at most twice a second
	float now = (float)Plat_FloatTime();
	if ( now >= m_flNextEvict )
	{
		m_flNextEvict = now + 0.5f;
		std::vector<mc::SectionKey> far;
		for ( auto &it : m_objects )
			if ( rank( it.first, radiusChunks + 2 ) < 0 ) far.push_back( it.first );
		for ( size_t i = 0; i < far.size(); ++i ) { Destroy( far[i] ); m_pending.insert( far[i] ); }
	}

	// Build the nearest pending sections until the time budget runs out
	std::vector<std::pair<int, mc::SectionKey>> todo;
	for ( const mc::SectionKey &k : m_pending )
	{
		int r = rank( k, radiusChunks );
		if ( r >= 0 ) todo.push_back( std::make_pair( r, k ) );
	}
	if ( todo.empty() ) return;
	std::sort( todo.begin(), todo.end(), []( const std::pair<int, mc::SectionKey> &a, const std::pair<int, mc::SectionKey> &b ) { return a.first < b.first; } );
	double start = Plat_FloatTime();
	for ( size_t i = 0; i < todo.size(); ++i )
	{
		Build( w, todo[i].second );
		m_pending.erase( todo[i].second );
		if ( ( Plat_FloatTime() - start ) * 1000.0 >= msBudget ) break;
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
