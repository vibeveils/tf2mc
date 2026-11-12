//========= TF2MC =========//
// mc_mob: Source-side proxy for one Minecraft entity (mob, item, projectile, other player).
// Server: hit box for TF2 weapons; damage is forwarded to Minecraft, which stays authoritative
// for health/AI. Client: draws the entity (mapped model or a coloured box).
//=========================//
#ifndef MC_MOB_H
#define MC_MOB_H
#pragma once

#ifdef CLIENT_DLL
#include "c_baseanimating.h"
#else
#include "baseanimating.h"
#endif

#ifdef CLIENT_DLL
#define CMCMob C_MCMob
#endif

// Precache every model referenced by scripts/mc_entity_models.txt (call during level load)
void MC_PrecacheMobModels();

class CMCMob : public CBaseAnimating
{
public:
	DECLARE_CLASS( CMCMob, CBaseAnimating );
	DECLARE_NETWORKCLASS();

	CMCMob();

	const char *GetMCType() const { return m_szMCType.Get(); }
	int GetMCId() const { return m_iMCId; }
	int GetMCFlags() const { return m_iMCFlags; }
	float GetMCHealth() const { return m_flMCHealth; }
	float GetMCMaxHealth() const { return m_flMCMaxHealth; }

#ifdef CLIENT_DLL
	virtual bool ShouldDraw() { return true; }
	virtual int DrawModel( int flags );
	virtual void GetRenderBounds( Vector &mins, Vector &maxs );
	virtual int GetHealth() const { return (int)m_flMCHealth; }
	virtual int GetMaxHealth() const { return (int)m_flMCMaxHealth; }
private:
	Vector m_vecLastOrigin = Vector( 0, 0, 0 );
	float m_flWalkSpeed = 0.f;
	float m_flWalkPhase = 0.f;
public:
#else
	virtual void Spawn();
	virtual void Precache();
	virtual int UpdateTransmitState() { return SetTransmitState( FL_EDICT_PVSCHECK ); }
	virtual int OnTakeDamage( const CTakeDamageInfo &info );
	virtual int BloodColor();
	virtual bool IsNPC() const { return false; }

	void Setup( int id, const char *type, float widthBlocks, float heightBlocks, float scale, int flags, float hp, float maxHp );
	void UpdateFromMC( const Vector &origin, float yaw, float pitch, float hp, int hurtTicks );
#endif

	CNetworkString( m_szMCType, 64 );
	CNetworkVar( int, m_iMCId );
	CNetworkVar( int, m_iMCFlags );
	CNetworkVar( float, m_flMCHealth );
	CNetworkVar( float, m_flMCMaxHealth );
	CNetworkVar( int, m_iHurtTicks );
	CNetworkVar( float, m_flHeadPitch );
};

#endif
