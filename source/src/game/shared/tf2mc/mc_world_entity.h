//========= TF2MC =========//
// mc_world: a single networked entity that *is* the Minecraft world inside Source.
//  - SOLID_CUSTOM: every engine trace (player movement, bullets, projectiles, melee)
//    calls TestCollision(), which sweeps against the mirrored voxels.
//  - On the client it also renders the chunk meshes (see client/tf2mc/c_mc_world.cpp).
//  - Its network vars carry the coordinate mapping and bridge address to clients.
//=========================//
#ifndef MC_WORLD_ENTITY_H
#define MC_WORLD_ENTITY_H
#pragma once

#include "tf2mc_core.h"

#ifdef CLIENT_DLL
#define CMCWorld C_MCWorld
class CMCWorldRenderer;
#endif

const Vector &MC_WorldMins();
const Vector &MC_WorldMaxs();

class CMCWorld : public CBaseEntity
{
public:
	DECLARE_CLASS( CMCWorld, CBaseEntity );
	DECLARE_NETWORKCLASS();

	CMCWorld();
	virtual ~CMCWorld();

	virtual void Spawn();
	virtual bool TestCollision( const Ray_t &ray, unsigned int mask, trace_t &tr );
	virtual bool ShouldCollide( int collisionGroup, int contentsMask ) const { return true; }

	void ApplyCoords( mc::Coords &c ) const;

#ifdef CLIENT_DLL
	virtual void OnDataChanged( DataUpdateType_t type );
	virtual bool ShouldDraw() { return true; }
	virtual int DrawModel( int flags );
	virtual void GetRenderBounds( Vector &mins, Vector &maxs );
	virtual bool IsTransparent() { return false; }
	virtual void ClientThink();
	virtual void UpdateOnRemove();
#else
	virtual int UpdateTransmitState() { return SetTransmitState( FL_EDICT_ALWAYS ); }
	void Configure( const mc::Coords &c, const char *publicHost, int port );
	void SetReady( bool b ) { m_bReady = b; }
#endif

	static CMCWorld *Get();

	CNetworkVar( float, m_flScale );
	CNetworkVar( float, m_flOriginX );
	CNetworkVar( float, m_flOriginY );
	CNetworkVar( float, m_flOriginZ );
	CNetworkVar( int, m_iBridgePort );
	CNetworkVar( bool, m_bReady );
	CNetworkString( m_szBridgeHost, 64 );

private:
#ifdef CLIENT_DLL
	char m_szConnectedHost[64];
	int m_iConnectedPort;
	float m_flNextConnectTry;
#endif
};

#endif
