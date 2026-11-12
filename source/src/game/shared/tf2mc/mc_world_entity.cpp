//========= TF2MC =========//
#include "cbase.h"
#include "mc_world_entity.h"
#include "worldsize.h"
#include "mc_bridge.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Collision bounds cover the whole legal Source coordinate range.
// Keep well inside the engine's world limits: boxes touching +-MAX_COORD_FLOAT fall off the edge of
// the spatial partition (no traces reach TestCollision) and the client leaf system (never drawn).
#define MC_WORLD_EXTENT 15872.f
static const Vector s_vecWorldMins( -MC_WORLD_EXTENT, -MC_WORLD_EXTENT, -MC_WORLD_EXTENT );
static const Vector s_vecWorldMaxs( MC_WORLD_EXTENT, MC_WORLD_EXTENT, MC_WORLD_EXTENT );
const Vector &MC_WorldMins() { return s_vecWorldMins; }
const Vector &MC_WorldMaxs() { return s_vecWorldMaxs; }

static CHandle<CMCWorld> s_hWorld;

IMPLEMENT_NETWORKCLASS_ALIASED( MCWorld, DT_MCWorld )

BEGIN_NETWORK_TABLE( CMCWorld, DT_MCWorld )
#ifdef CLIENT_DLL
	RecvPropFloat( RECVINFO( m_flScale ) ),
	RecvPropFloat( RECVINFO( m_flOriginX ) ),
	RecvPropFloat( RECVINFO( m_flOriginY ) ),
	RecvPropFloat( RECVINFO( m_flOriginZ ) ),
	RecvPropInt( RECVINFO( m_iBridgePort ) ),
	RecvPropBool( RECVINFO( m_bReady ) ),
	RecvPropString( RECVINFO( m_szBridgeHost ) ),
#else
	SendPropFloat( SENDINFO( m_flScale ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flOriginX ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flOriginY ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flOriginZ ), 0, SPROP_NOSCALE ),
	SendPropInt( SENDINFO( m_iBridgePort ), 17, SPROP_UNSIGNED ),
	SendPropBool( SENDINFO( m_bReady ) ),
	SendPropString( SENDINFO( m_szBridgeHost ) ),
#endif
END_NETWORK_TABLE()

LINK_ENTITY_TO_CLASS( mc_world, CMCWorld );

CMCWorld::CMCWorld()
{
	m_flScale = 64.f;
	m_flOriginX = 0.f;
	m_flOriginY = 128.f;
	m_flOriginZ = 0.f;
	m_iBridgePort = 25599;
	m_bReady = false;
#ifdef CLIENT_DLL
	m_szConnectedHost[0] = 0;
	m_iConnectedPort = 0;
	m_flNextConnectTry = 0.f;
#endif
}

CMCWorld::~CMCWorld()
{
}

CMCWorld *CMCWorld::Get()
{
	return s_hWorld.Get();
}

void CMCWorld::ApplyCoords( mc::Coords &c ) const
{
	c.scale = m_flScale > 1.f ? m_flScale : 64.f;
	c.ox = m_flOriginX;
	c.oy = m_flOriginY;
	c.oz = m_flOriginZ;
}

void CMCWorld::Spawn()
{
	BaseClass::Spawn();
	s_hWorld = this;
	SetMoveType( MOVETYPE_NONE );
	SetAbsOrigin( vec3_origin );
	SetAbsAngles( vec3_angle );
	SetSolid( SOLID_CUSTOM );
	AddSolidFlags( FSOLID_CUSTOMRAYTEST | FSOLID_CUSTOMBOXTEST );
	SetCollisionGroup( COLLISION_GROUP_NONE );
	SetCollisionBounds( s_vecWorldMins, s_vecWorldMaxs );
#ifndef CLIENT_DLL
	SetTransmitState( FL_EDICT_ALWAYS );
	AddEFlags( EFL_FORCE_CHECK_TRANSMIT );
#endif
}

bool CMCWorld::TestCollision( const Ray_t &ray, unsigned int mask, trace_t &tr )
{
	return MCBridge().TraceRay( ray, mask, tr, this );
}

#ifdef GAME_DLL
void CMCWorld::Configure( const mc::Coords &c, const char *publicHost, int port )
{
	m_flScale = (float)c.scale;
	m_flOriginX = (float)c.ox;
	m_flOriginY = (float)c.oy;
	m_flOriginZ = (float)c.oz;
	m_iBridgePort = port;
	Q_strncpy( m_szBridgeHost.GetForModify(), publicHost, 64 );
}
#endif
