//========= TF2MC =========//
// Server side of the bridge (server.dll, ROLE_AUTHORITY):
//  - creates the mc_world collision entity and keeps its coordinate mapping in sync
//  - FLOATING ORIGIN: re-centres the Source coordinate frame on the players as they explore,
//    shifting every entity, projectile, building and physics object with it
//  - soft world border when players are too far apart to share one Source map
//  - mirrors every TF2 player into Minecraft (join/leave/move/mode), party dimension travel
//  - spawns mc_mob proxies for Minecraft entities and forwards Minecraft damage to TF2 players
//  - Minecraft-mode interaction (dig, place, use, melee, interact, inventory, drop)
//  - TF2 explosions carve craters, sentries shoot hostile mobs, chat both ways
//=========================//
#include "cbase.h"
#include "tf2mc/mc_bridge.h"
#include "tf2mc/mc_world_entity.h"
#include "tf2mc/mc_mob.h"
#include "tf2mc/mc_physics.h"
#include "mc_server.h"
#include "tf_player.h"
#include "tf_shareddefs.h"
#include "tf_gamerules.h"
#include "igamesystem.h"
#include "GameEventListener.h"
#include "takedamageinfo.h"
#include "in_buttons.h"
#include "world.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar mc_host( "mc_host", "127.0.0.1", FCVAR_ARCHIVE, "Address of the Minecraft server running the tf2bridge mod" );
ConVar mc_port( "mc_port", "25599", FCVAR_ARCHIVE, "TCP port of the tf2bridge mod" );
ConVar mc_public_host( "mc_public_host", "", FCVAR_ARCHIVE, "Bridge address clients should use (empty = same as mc_host). Set this when mc_host is 127.0.0.1 and remote players join." );
ConVar mc_secret( "mc_secret", "", FCVAR_ARCHIVE | FCVAR_PROTECTED, "Shared secret configured in tf2bridge.properties" );
ConVar mc_autoconnect( "mc_autoconnect", "1", FCVAR_ARCHIVE, "Connect to the Minecraft bridge on map load" );
ConVar mc_scale( "mc_scale", "46", FCVAR_ARCHIVE, "Hammer units per Minecraft block (applied on next connect). 46 makes TF2's 82-unit-tall player ~1.8 blocks, like Steve" );
ConVar mc_origin_y( "mc_origin_y", "128", FCVAR_ARCHIVE, "Minecraft Y mapped to Source z=0" );
ConVar mc_reach( "mc_reach", "4.5", FCVAR_NOTIFY, "Minecraft-mode block/entity reach in blocks" );
ConVar mc_max_mobs( "mc_max_mobs", "300", FCVAR_ARCHIVE, "Max Minecraft entities mirrored into Source (edict budget)" );
ConVar mc_spawn_spread( "mc_spawn_spread", "4", FCVAR_NOTIFY, "Random spread (blocks) around the spawn / respawn point" );
ConVar mc_respawn_near_team( "mc_respawn_near_team", "1", FCVAR_NOTIFY, "Respawn next to a living teammate (or anyone) instead of the world spawn" );
ConVar mc_move_rate( "mc_move_rate", "20", FCVAR_NONE, "Player position updates per second sent to Minecraft" );
ConVar mc_physics_ms( "mc_physics_ms", "2", FCVAR_NONE, "Milliseconds per frame spent turning chunk sections into VPhysics collision (pipes, stickies, ammo packs)" );
ConVar mc_physics_radius( "mc_physics_radius", "3", FCVAR_NONE, "VPhysics collision is built for chunks this close to a player (chunks)" );
ConVar mc_rebase_distance( "mc_rebase_distance", "96", FCVAR_NOTIFY, "Re-centre the floating origin when a player is this many blocks from it" );
ConVar mc_border_units( "mc_border_units", "15360", FCVAR_NOTIFY, "Soft border (Source units from the floating origin) when players are too spread out to share one map" );
ConVar mc_tf_explosions( "mc_tf_explosions", "1", FCVAR_NOTIFY, "TF2 explosions break Minecraft blocks (power multiplier, 0 = off)" );
ConVar mc_food_heal( "mc_food_heal", "4", FCVAR_NOTIFY, "TF2 health restored per Minecraft hunger point eaten" );
ConVar mc_sentries_target_mobs( "mc_sentries_target_mobs", "1", FCVAR_NOTIFY, "Engineer sentries shoot hostile Minecraft mobs" );
ConVar mc_chat_bridge( "mc_chat_bridge", "1", FCVAR_NOTIFY, "Relay chat between TF2 and Minecraft" );

class CMCServerSystem : public CAutoGameSystemPerFrame, public CGameEventListener
{
public:
	CMCServerSystem() : CAutoGameSystemPerFrame( "CMCServerSystem" ) {}

	virtual bool Init()
	{
		ListenForGameEvent( "player_spawn" );
		ListenForGameEvent( "player_say" );
		return true;
	}

	virtual void LevelInitPreEntity()
	{
		MC_PrecacheMobModels();
	}

	virtual void LevelInitPostEntity()
	{
		m_flNextRetry = 0.f;
		m_flNextRebaseCheck = 0.f;
		ResetPlayers();
		CBaseEntity *pEnt = CreateEntityByName( "mc_world" );
		if ( pEnt )
		{
			DispatchSpawn( pEnt );
			m_hWorld = static_cast<CMCWorld *>( pEnt );
		}
		if ( mc_autoconnect.GetBool() )
			Connect();
	}

	virtual void LevelShutdownPreEntity()
	{
		MCPhysics().Clear();
		MCBridge().Disconnect();
		m_mobs.RemoveAll();
		m_hWorld = NULL;
		ResetPlayers();
	}

	virtual void FireGameEvent( IGameEvent *event )
	{
		if ( !Q_strcmp( event->GetName(), "player_spawn" ) )
		{
			CBasePlayer *p = UTIL_PlayerByUserId( event->GetInt( "userid" ) );
			if ( p && p->entindex() < ARRAYSIZE( m_players ) )
				m_players[ p->entindex() ].spawnPending = true;
		}
		else if ( !Q_strcmp( event->GetName(), "player_say" ) && mc_chat_bridge.GetBool() && MC_IsActive() )
		{
			CBasePlayer *p = UTIL_PlayerByUserId( event->GetInt( "userid" ) );
			char msg[512];
			Q_snprintf( msg, sizeof( msg ), "<%s> %s", p ? p->GetPlayerName() : "console", event->GetString( "text" ) );
			mc::Writer w( mc::C2S_CHAT );
			w.str( msg );
			MCBridge().Send( w );
		}
	}

	void Connect()
	{
		const char *name = "tf2-server";
		char hello[192];
		if ( mc_secret.GetString()[0] ) { Q_snprintf( hello, sizeof( hello ), "%s:%s", mc_secret.GetString(), name ); name = hello; }
		MCBridge().Connect( mc_host.GetString(), mc_port.GetInt(), mc::ROLE_AUTHORITY, name );
		m_flNextRetry = gpGlobals->curtime + 5.f;
		ResetPlayers();
	}

	virtual void FrameUpdatePostEntityThink();

	CMCMob *FindMob( int id )
	{
		int i = m_mobs.Find( id );
		if ( i == m_mobs.InvalidIndex() ) return NULL;
		CMCMob *mob = static_cast<CMCMob *>( m_mobs[i].Get() );
		if ( !mob ) m_mobs.RemoveAt( i );   // proxy was deleted behind our back
		return mob;
	}

	CBaseEntity *FindSentryTarget( CBaseEntity *pSentry, const Vector &eye, float range );
	void Rebase( double newOx, double newOz );
	void PrintOrigin();

private:
	struct PlayerState
	{
		bool joined;
		bool spawnPending;
		bool spawnHold;
		uint8 mode;
		float nextMove;
		float lastFood;
		float nextBorderMsg;
	};

	void ResetPlayers() { memset( m_players, 0, sizeof( m_players ) ); }
	void OnHelloAck();
	void SyncPlayers();
	void HandleSpawn( CTFPlayer *p, PlayerState &st );
	void HandlePacket( const mc::Packet &p );
	void UpdateFloatingOrigin();
	void EnforceBorder();
	void ApplyCoordsToWorldEntity();

	CHandle<CMCWorld> m_hWorld;
	CUtlMap<int, EHANDLE> m_mobs{ DefLessFunc( int ) };
	PlayerState m_players[ MAX_PLAYERS + 1 ];
	float m_flNextRetry;
	float m_flNextRebaseCheck;
	int m_iLastEpoch = 0;
};

static CMCServerSystem g_MCServerSystem;

void CMCServerSystem::ApplyCoordsToWorldEntity()
{
	if ( !m_hWorld ) return;
	const char *pub = mc_public_host.GetString()[0] ? mc_public_host.GetString() : mc_host.GetString();
	m_hWorld->Configure( MCBridge().Coords(), pub, mc_port.GetInt() );
	m_hWorld->SetReady( true );
}

void CMCServerSystem::OnHelloAck()
{
	CMCBridge &b = MCBridge();
	mc::Coords &c = b.Coords();
	bool dimensionChange = m_iLastEpoch != 0 && b.Epoch() != m_iLastEpoch;
	m_iLastEpoch = b.Epoch();

	// The new frame is centred on the arrival point (world spawn, or the portal exit)
	c.scale = MAX( 8.f, mc_scale.GetFloat() );
	c.ox = mc::FloorDiv16( b.SpawnX() ) * 16;
	c.oz = mc::FloorDiv16( b.SpawnZ() ) * 16;
	c.oy = mc_origin_y.GetFloat();
	ApplyCoordsToWorldEntity();
	MCPhysics().Clear();

	// Remove stale proxies from an earlier connection / dimension
	FOR_EACH_MAP_FAST( m_mobs, i ) { if ( m_mobs[i] ) UTIL_Remove( m_mobs[i] ); }
	m_mobs.RemoveAll();

	if ( dimensionChange )
	{
		// Buildings and stickies can't follow through a portal
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CTFPlayer *p = ToTFPlayer( UTIL_PlayerByIndex( i ) );
			if ( p ) p->RemoveAllObjects( false );
		}
		const char *dim = b.Dimension();
		const char *colon = Q_strstr( dim, ":" );
		char pretty[64];
		MC_PrettyItemName( colon ? colon + 1 : dim, pretty, sizeof( pretty ) );
		UTIL_ClientPrintAll( HUD_PRINTCENTER, pretty );
	}

	// Everyone gets (re)placed into the Minecraft world; existing joins stay valid
	for ( int i = 1; i <= gpGlobals->maxClients && i < ARRAYSIZE( m_players ); ++i )
	{
		m_players[i].spawnPending = true;
		if ( !dimensionChange ) m_players[i].joined = false;
	}
}

void CMCServerSystem::HandleSpawn( CTFPlayer *p, PlayerState &st )
{
	CMCBridge &b = MCBridge();
	mc::VoxelWorld &w = b.World();
	const mc::Coords &c = b.Coords();
	float spread = mc_spawn_spread.GetFloat();

	// Where: next to a living teammate (or anyone), else the world spawn / arrival point
	double cx = b.SpawnX(), cz = b.SpawnZ();
	if ( mc_respawn_near_team.GetBool() )
	{
		CTFPlayer *pBest = NULL;
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CTFPlayer *o = ToTFPlayer( UTIL_PlayerByIndex( i ) );
			if ( !o || o == p || !o->IsAlive() || o->GetTeamNumber() < FIRST_GAME_TEAM || m_players[i].spawnHold || m_players[i].spawnPending ) continue;
			if ( !pBest || ( o->GetTeamNumber() == p->GetTeamNumber() && pBest->GetTeamNumber() != p->GetTeamNumber() ) ) pBest = o;
		}
		if ( pBest )
		{
			mc::Vec3d m = c.ToMC( MC_V( pBest->GetAbsOrigin() ) );
			cx = m.x; cz = m.z;
		}
	}
	int bx = mc::FloorI( cx ) + RandomInt( (int)-spread, (int)spread );
	int bz = mc::FloorI( cz ) + RandomInt( (int)-spread, (int)spread );

	if ( !w.IsColumnLoaded( mc::FloorDiv16( bx ), mc::FloorDiv16( bz ) ) )
	{
		// Wait (frozen) until the terrain under the spawn arrives
		st.spawnHold = true;
		p->SetMoveType( MOVETYPE_NONE );
		p->SetAbsVelocity( vec3_origin );
		return;
	}
	int top = w.TopSolidY( bx, bz );
	if ( top < w.MinY() ) top = b.SpawnY() - 1;
	while ( top + 2 < w.MaxY() && ( w.GetDef( bx, top + 1, bz ).collides || w.GetDef( bx, top + 2, bz ).collides ) ) ++top;

	Vector pos = MC_V( c.ToSource( mc::Vec3d( bx + 0.5, top + 1.0, bz + 0.5 ) ) );
	pos.z += 1.f;
	QAngle ang = p->EyeAngles();
	p->SetMoveType( MOVETYPE_WALK );
	p->Teleport( &pos, &ang, &vec3_origin );
	st.spawnHold = false;
	st.spawnPending = false;
}

void CMCServerSystem::SyncPlayers()
{
	CMCBridge &b = MCBridge();
	const mc::Coords &c = b.Coords();
	float interval = 1.f / MAX( 1.f, mc_move_rate.GetFloat() );

	for ( int i = 1; i <= gpGlobals->maxClients && i < ARRAYSIZE( m_players ); ++i )
	{
		PlayerState &st = m_players[i];
		CTFPlayer *p = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		bool wants = p && p->IsConnected() && p->GetTeamNumber() >= FIRST_GAME_TEAM;

		if ( !wants )
		{
			if ( st.joined )
			{
				mc::Writer w( mc::C2S_PLAYER_LEAVE );
				w.u16( (uint16)i );
				b.Send( w );
			}
			memset( &st, 0, sizeof( st ) );
			continue;
		}

		uint8 mode = p->IsPlayerClass( TF_CLASS_CIVILIAN ) ? mc::MODE_STEVE : mc::MODE_TF_CLASS;
		if ( !st.joined )
		{
			mc::Writer w( mc::C2S_PLAYER_JOIN );
			w.u16( (uint16)i ).str( p->GetPlayerName() ).u8( mode );
			b.Send( w );
			st.joined = true;
			st.mode = mode;
			st.lastFood = -1.f;
		}
		else if ( st.mode != mode )
		{
			mc::Writer w( mc::C2S_SET_MODE );
			w.u16( (uint16)i ).u8( mode );
			b.Send( w );
			st.mode = mode;
		}

		if ( st.spawnPending && p->IsAlive() )
			HandleSpawn( p, st );

		if ( p->IsAlive() && !st.spawnHold && !st.spawnPending && gpGlobals->curtime >= st.nextMove )
		{
			st.nextMove = gpGlobals->curtime + interval;
			mc::Vec3d feet = c.ToMC( MC_V( p->GetAbsOrigin() ) );
			QAngle eye = p->EyeAngles();
			uint8 flags = ( p->GetFlags() & FL_ONGROUND ? 1 : 0 ) | ( p->GetFlags() & FL_DUCKING ? 2 : 0 );
			mc::Writer w( mc::C2S_PLAYER_MOVE );
			w.u16( (uint16)i ).f64( feet.x ).f64( feet.y ).f64( feet.z ).f32( mc::Coords::YawToMC( eye.y ) ).f32( eye.x ).u8( flags ).i32( b.Epoch() );
			b.Send( w );
		}
	}
}

//-----------------------------------------------------------------------------
// Floating origin
//-----------------------------------------------------------------------------
void CMCServerSystem::UpdateFloatingOrigin()
{
	if ( gpGlobals->curtime < m_flNextRebaseCheck ) return;
	m_flNextRebaseCheck = gpGlobals->curtime + 0.5f;

	const mc::Coords &c = MCBridge().Coords();
	double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30, far = 0;
	int n = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *p = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !p || !p->IsAlive() || p->GetTeamNumber() < FIRST_GAME_TEAM || m_players[i].spawnHold || m_players[i].spawnPending ) continue;
		mc::Vec3d m = c.ToMC( MC_V( p->GetAbsOrigin() ) );
		minX = MIN( minX, m.x ); maxX = MAX( maxX, m.x );
		minZ = MIN( minZ, m.z ); maxZ = MAX( maxZ, m.z );
		far = MAX( far, MAX( fabs( m.x - c.ox ), fabs( m.z - c.oz ) ) );
		++n;
	}
	if ( !n || far < mc_rebase_distance.GetFloat() ) return;

	// Centre of everyone, snapped to chunk boundaries
	double nx = floor( ( minX + maxX ) * 0.5 / 16.0 + 0.5 ) * 16.0;
	double nz = floor( ( minZ + maxZ ) * 0.5 / 16.0 + 0.5 ) * 16.0;
	double newFar = MAX( MAX( fabs( minX - nx ), fabs( maxX - nx ) ), MAX( fabs( minZ - nz ), fabs( maxZ - nz ) ) );
	if ( ( nx == c.ox && nz == c.oz ) || newFar >= far - 8.0 ) return;   // no meaningful improvement (players spread out)
	Rebase( nx, nz );
}

void CMCServerSystem::Rebase( double newOx, double newOz )
{
	mc::Coords &c = MCBridge().Coords();
	mc::Coords old = c;
	c.ox = newOx;
	c.oz = newOz;
	mc::Vec3d d = c.ShiftFrom( old );
	Vector delta( (float)d.x, (float)d.y, (float)d.z );

	// Move every root entity (children follow their parents). Teleport() also moves VPhysics
	// objects and breaks interpolation so nothing visibly slides across the map.
	CUtlVector<CBaseEntity *> roots;
	for ( CBaseEntity *e = gEntList.FirstEnt(); e; e = gEntList.NextEnt( e ) )
	{
		if ( e->IsWorld() || e == m_hWorld.Get() || e->GetMoveParent() || e->IsMarkedForDeletion() ) continue;
		roots.AddToTail( e );
	}
	for ( int i = 0; i < roots.Count(); ++i )
	{
		CBaseEntity *e = roots[i];
		Vector pos = e->GetAbsOrigin() + delta;
		e->Teleport( &pos, NULL, NULL );
	}

	ApplyCoordsToWorldEntity();
	MCPhysics().Update( MCBridge().World(), c, std::vector<mc::Vec3d>(), 0, 0.f );   // slide static collision now, before the next physics step
	DevMsg( "TF2MC: floating origin moved to %.0f %.0f (%d entities shifted by %.0f %.0f)\n", newOx, newOz, roots.Count(), delta.x, delta.y );
}

void CMCServerSystem::EnforceBorder()
{
	const float border = mc_border_units.GetFloat();
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *p = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !p || !p->IsAlive() ) continue;
		Vector pos = p->GetAbsOrigin(), vel = p->GetAbsVelocity();
		bool clamped = false;
		for ( int a = 0; a < 2; ++a )
		{
			if ( pos[a] > border ) { pos[a] = border; vel[a] = MIN( vel[a], 0.f ); clamped = true; }
			else if ( pos[a] < -border ) { pos[a] = -border; vel[a] = MAX( vel[a], 0.f ); clamped = true; }
		}
		if ( !clamped ) continue;
		p->Teleport( &pos, NULL, &vel );
		if ( gpGlobals->curtime >= m_players[i].nextBorderMsg )
		{
			m_players[i].nextBorderMsg = gpGlobals->curtime + 3.f;
			ClientPrint( p, HUD_PRINTCENTER, "Too far from the other players - regroup to explore further" );
		}
	}
}

void CMCServerSystem::PrintOrigin()
{
	const mc::Coords &c = MCBridge().Coords();
	Msg( "TF2MC: origin %.0f %.0f %.0f  scale %.0f  dimension %s  epoch %d  physics sections %d  mobs %d\n",
		 c.ox, c.oy, c.oz, c.scale, MCBridge().Dimension(), MCBridge().Epoch(), MCPhysics().Count(), m_mobs.Count() );
}

//-----------------------------------------------------------------------------

static int DamageBitsForKind( int kind )
{
	switch ( kind )
	{
	case mc::DMG_KIND_EXPLOSION:  return DMG_BLAST;
	case mc::DMG_KIND_FIRE:       return DMG_BURN;
	case mc::DMG_KIND_MELEE:      return DMG_CLUB;
	case mc::DMG_KIND_PROJECTILE: return DMG_BULLET;
	default:                      return DMG_GENERIC;
	}
}

void CMCServerSystem::HandlePacket( const mc::Packet &pk )
{
	mc::Reader r = pk.reader();
	const mc::Coords &c = MCBridge().Coords();
	switch ( pk.type )
	{
	case mc::S2C_HELLO_ACK:
		OnHelloAck();
		break;

	case mc::S2C_ENTITY_SPAWN:
	{
		int id = r.i32();
		std::string type = r.str();
		double x = r.f64(), y = r.f64(), z = r.f64();
		float yaw = r.f32(), pitch = r.f32(), w = r.f32(), h = r.f32(), hp = r.f32(), maxHp = r.f32();
		int flags = r.u8();
		if ( !r.ok() || FindMob( id ) || m_mobs.Count() >= mc_max_mobs.GetInt() ) break;
		CMCMob *mob = static_cast<CMCMob *>( CreateEntityByName( "mc_mob" ) );
		if ( !mob ) break;
		mob->Setup( id, type.c_str(), w, h, (float)c.scale, flags, hp, maxHp );
		mob->UpdateFromMC( MC_V( c.ToSource( mc::Vec3d( x, y, z ) ) ), mc::Coords::YawToSource( yaw ), pitch, hp, 0 );
		DispatchSpawn( mob );
		mob->Setup( id, type.c_str(), w, h, (float)c.scale, flags, hp, maxHp );   // re-apply size after Spawn()
		m_mobs.Insert( id, mob );
		break;
	}
	case mc::S2C_ENTITY_UPDATE:
	{
		int id = r.i32();
		double x = r.f64(), y = r.f64(), z = r.f64();
		float yaw = r.f32(), pitch = r.f32(), hp = r.f32();
		int hurt = r.u8();
		CMCMob *mob = FindMob( id );
		if ( mob && r.ok() )
			mob->UpdateFromMC( MC_V( c.ToSource( mc::Vec3d( x, y, z ) ) ), mc::Coords::YawToSource( yaw ), pitch, hp, hurt );
		break;
	}
	case mc::S2C_ENTITY_REMOVE:
	{
		int id = r.i32();
		int i = m_mobs.Find( id );
		if ( i != m_mobs.InvalidIndex() )
		{
			if ( m_mobs[i] ) UTIL_Remove( m_mobs[i] );
			m_mobs.RemoveAt( i );
		}
		break;
	}
	case mc::S2C_PLAYER_HURT:
	{
		int slot = r.u16();
		float amount = r.f32();
		int attackerId = r.i32();
		int kind = r.u8();
		mc::Vec3d from( r.f64(), r.f64(), r.f64() );
		CTFPlayer *p = ToTFPlayer( UTIL_PlayerByIndex( slot ) );
		if ( !p || !p->IsAlive() || !r.ok() ) break;
		CBaseEntity *pAttacker = attackerId >= 0 ? FindMob( attackerId ) : NULL;
		if ( !pAttacker ) pAttacker = GetWorldEntity();
		Vector src = MC_V( c.ToSource( from ) );
		Vector dir = p->WorldSpaceCenter() - src;
		VectorNormalize( dir );
		CTakeDamageInfo info( pAttacker, pAttacker, amount, DamageBitsForKind( kind ) );
		info.SetDamagePosition( src );
		info.SetDamageForce( dir * amount * 80.f );
		p->TakeDamage( info );
		break;
	}
	case mc::S2C_PLAYER_STATUS:
	{
		// Eating in Minecraft heals in TF2
		int slot = r.u16();
		float food = r.f32();
		if ( !r.ok() || slot <= 0 || slot >= ARRAYSIZE( m_players ) ) break;
		PlayerState &st = m_players[slot];
		CTFPlayer *p = ToTFPlayer( UTIL_PlayerByIndex( slot ) );
		if ( p && p->IsAlive() && st.lastFood >= 0.f && food > st.lastFood )
			p->TakeHealth( ( food - st.lastFood ) * mc_food_heal.GetFloat(), DMG_GENERIC );
		st.lastFood = food;
		break;
	}
	case mc::S2C_CHAT:
	{
		if ( !mc_chat_bridge.GetBool() ) break;
		std::string text = r.str();
		char line[512];
		Q_snprintf( line, sizeof( line ), "\x07" "55CC55[MC]\x01 %s\n", text.c_str() );
		UTIL_ClientPrintAll( HUD_PRINTTALK, line );
		break;
	}
	default:
		break;
	}
}

void CMCServerSystem::FrameUpdatePostEntityThink()
{
	CMCBridge &b = MCBridge();

	// Something (a round restart without the cleanup hook, a plugin) removed the world entity: recreate it.
	if ( !m_hWorld.Get() )
	{
		CBaseEntity *pEnt = CreateEntityByName( "mc_world" );
		if ( pEnt )
		{
			DispatchSpawn( pEnt );
			m_hWorld = static_cast<CMCWorld *>( pEnt );
			if ( b.IsReady() ) ApplyCoordsToWorldEntity();
			DevMsg( "TF2MC: recreated mc_world\n" );
		}
	}
	if ( mc_autoconnect.GetBool() && b.Port() && !b.IsConnected() && gpGlobals->curtime > m_flNextRetry && m_hWorld )
	{
		m_hWorld->SetReady( false );
		Connect();
	}

	std::vector<mc::Packet> packets;
	b.Pump( packets );
	for ( size_t i = 0; i < packets.size(); ++i )
		HandlePacket( packets[i] );

	if ( !b.IsReady() )
	{
		MCPhysics().Clear();
		return;
	}

	SyncPlayers();
	UpdateFloatingOrigin();
	EnforceBorder();
	{
		// VPhysics collision only around players (pipes and stickies don't fly further than that)
		std::vector<mc::Vec3d> focus;
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CBasePlayer *pl = UTIL_PlayerByIndex( i );
			if ( pl && pl->IsAlive() ) focus.push_back( b.Coords().ToMC( MC_V( pl->GetAbsOrigin() ) ) );
		}
		MCPhysics().Update( b.World(), b.Coords(), focus, mc_physics_radius.GetInt(), mc_physics_ms.GetFloat() );
	}

	// Release players waiting for terrain
	for ( int i = 1; i <= gpGlobals->maxClients && i < ARRAYSIZE( m_players ); ++i )
	{
		if ( !m_players[i].spawnHold ) continue;
		CTFPlayer *p = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( p && p->IsAlive() ) HandleSpawn( p, m_players[i] );
		else m_players[i].spawnHold = false;
	}
}

CBaseEntity *CMCServerSystem::FindSentryTarget( CBaseEntity *pSentry, const Vector &eye, float range )
{
	if ( !mc_sentries_target_mobs.GetBool() || !MC_IsActive() ) return NULL;
	CBaseEntity *pBest = NULL;
	float best = range * range;
	FOR_EACH_MAP_FAST( m_mobs, i )
	{
		CMCMob *mob = static_cast<CMCMob *>( m_mobs[i].Get() );
		if ( !mob || !( mob->GetMCFlags() & mc::ENT_HOSTILE ) || mob->GetMCHealth() <= 0 ) continue;
		float d2 = ( mob->WorldSpaceCenter() - eye ).LengthSqr();
		if ( d2 >= best ) continue;
		if ( !pSentry->FVisible( mob, MASK_SHOT | CONTENTS_GRATE ) ) continue;
		best = d2;
		pBest = mob;
	}
	return pBest;
}

//-----------------------------------------------------------------------------
// Hooks called from TF2 code (patch)
//-----------------------------------------------------------------------------
CBaseEntity *MC_SentryFindMob( CBaseEntity *pSentry, const Vector &eye, float range )
{
	return g_MCServerSystem.FindSentryTarget( pSentry, eye, range );
}

void MC_OnRadiusDamage( const Vector &src, float radius, CBaseEntity *pAttacker )
{
	if ( !MC_IsActive() || mc_tf_explosions.GetFloat() <= 0.f || radius <= 0.f ) return;
	const mc::Coords &c = MCBridge().Coords();
	CTFPlayer *p = ToTFPlayer( pAttacker );
	mc::Vec3d m = c.ToMC( MC_V( src ) );
	// Rocket radius ~146u = 2.3 blocks -> power ~1.4 (a small crater). TNT is 4.
	float power = (float)( radius / c.scale ) * 0.6f * mc_tf_explosions.GetFloat();
	mc::Writer w( mc::C2S_EXPLOSION );
	w.u16( p ? (uint16)p->entindex() : 0 ).f64( m.x ).f64( m.y ).f64( m.z ).f32( power );
	MCBridge().Send( w );
}

//-----------------------------------------------------------------------------
// Minecraft mode interaction
//-----------------------------------------------------------------------------
struct SteveState
{
	bool digging;
	mc::BlockPos digPos;
	float nextDigSend;
	float nextUse;
	bool build;                              // TF class in build mode (weapon holstered, mouse digs/places)
	CHandle<CBaseCombatWeapon> lastWeapon;   // re-deployed when build mode ends
};
static SteveState s_steve[ MAX_PLAYERS + 1 ];

// Build mode: any TF class can put its weapon away and use the mouse like Minecraft
// (LMB dig/attack, RMB place/use, 1-9 hotbar). Civilian is always in this mode.
void MC_SetBuildMode( CTFPlayer *p, bool on )
{
	int idx = p ? p->entindex() : 0;
	if ( idx <= 0 || idx > MAX_PLAYERS ) return;
	SteveState &st = s_steve[idx];
	if ( on == st.build ) return;
	if ( on )
	{
		if ( !MC_IsActive() || !p->IsAlive() || p->IsPlayerClass( TF_CLASS_CIVILIAN ) ) return;
		st.build = true;
		st.lastWeapon = p->GetActiveWeapon();
		if ( p->GetActiveWeapon() ) { p->GetActiveWeapon()->Holster(); p->ClearActiveWeapon(); }
	}
	else
	{
		st.build = false;
		if ( p->IsAlive() && !p->GetActiveWeapon() )
		{
			CBaseCombatWeapon *w = st.lastWeapon.Get();
			if ( w && w->GetOwner() == p ) p->Weapon_Switch( w );
			else p->SwitchToNextBestWeapon( NULL );
		}
		st.lastWeapon = NULL;
	}
}

static void SendAbortDig( int idx, SteveState &st )
{
	if ( !st.digging ) return;
	mc::Writer w( mc::C2S_DIG );
	w.u16( (uint16)idx ).u8( mc::DIG_ABORT ).i32( st.digPos.x ).i32( st.digPos.y ).i32( st.digPos.z ).u8( 0 );
	MCBridge().Send( w );
	st.digging = false;
}

void MC_PlayerPostThink( CTFPlayer *p )
{
	int idx = p->entindex();
	if ( idx <= 0 || idx > MAX_PLAYERS ) return;
	SteveState &st = s_steve[idx];

	// Minecraft step height (networked through m_Local, so prediction follows)
	static ConVarRef sv_stepsize( "sv_stepsize" );
	float step = MC_StepSize( sv_stepsize.IsValid() ? sv_stepsize.GetFloat() : 18.f );
	if ( p->m_Local.m_flStepSize != step ) p->m_Local.m_flStepSize = step;

	if ( st.build && ( !MC_IsActive() || !p->IsAlive() || p->IsPlayerClass( TF_CLASS_CIVILIAN ) ) )
		MC_SetBuildMode( p, false );
	if ( st.build && p->GetActiveWeapon() )
	{
		// something (resupply, a pickup, a taunt) re-deployed a weapon: holster it again
		st.lastWeapon = p->GetActiveWeapon();
		p->GetActiveWeapon()->Holster();
		p->ClearActiveWeapon();
	}
	if ( !MC_IsActive() || !p->IsAlive() || !( p->IsPlayerClass( TF_CLASS_CIVILIAN ) || st.build ) )
	{
		SendAbortDig( idx, st );
		return;
	}

	const mc::Coords &c = MCBridge().Coords();
	const float reach = mc_reach.GetFloat() * (float)c.scale;
	Vector eye = p->EyePosition(), fwd;
	AngleVectors( p->EyeAngles(), &fwd );

	// Entities first (the voxel world blocks the trace, so you can't hit through walls)
	trace_t tr;
	UTIL_TraceLine( eye, eye + fwd * reach, MASK_SHOT, p, COLLISION_GROUP_NONE, &tr );
	CMCMob *pMob = ( tr.m_pEnt && FClassnameIs( tr.m_pEnt, "mc_mob" ) ) ? static_cast<CMCMob *>( tr.m_pEnt ) : NULL;

	mc::PickResult pick = mc::PickBlock( MCBridge().World(), c.ToMC( MC_V( eye ) ), c.DeltaToMC( MC_V( fwd ) ), mc_reach.GetFloat() );
	if ( pMob && pick.hit && pick.distance * c.scale < tr.fraction * reach ) pMob = NULL;   // block is in front of the mob

	const int buttons = p->m_nButtons, pressed = p->m_afButtonPressed, released = p->m_afButtonReleased;

	// --- Left mouse: attack mob (click) or dig block (hold)
	if ( ( pressed & IN_ATTACK ) && pMob )
	{
		SendAbortDig( idx, st );
		mc::Writer w( mc::C2S_ATTACK );
		w.u16( (uint16)idx ).i32( pMob->GetMCId() ).u8( mc::ATTACK_MC_MELEE ).f32( 0 ).f32( 0 ).f32( 0 ).f32( 0 ).u8( mc::DMG_KIND_MELEE );
		MCBridge().Send( w );
		p->DoAnimationEvent( PLAYERANIMEVENT_ATTACK_PRIMARY );
	}
	else if ( ( buttons & IN_ATTACK ) && !pMob && pick.hit )
	{
		bool changed = !st.digging || st.digPos != pick.block;
		if ( changed || gpGlobals->curtime >= st.nextDigSend )
		{
			mc::Writer w( mc::C2S_DIG );
			w.u16( (uint16)idx ).u8( mc::DIG_CONTINUE ).i32( pick.block.x ).i32( pick.block.y ).i32( pick.block.z ).u8( pick.face );
			MCBridge().Send( w );
			st.digging = true;
			st.digPos = pick.block;
			st.nextDigSend = gpGlobals->curtime + 0.25f;
			if ( changed ) p->DoAnimationEvent( PLAYERANIMEVENT_ATTACK_PRIMARY );
		}
	}
	else
	{
		SendAbortDig( idx, st );
	}

	// --- Right mouse: interact with mob / place / use (repeats every 0.2s while held, like Minecraft)
	if ( ( buttons & IN_ATTACK2 ) && ( ( pressed & IN_ATTACK2 ) || gpGlobals->curtime >= st.nextUse ) )
	{
		st.nextUse = gpGlobals->curtime + 0.2f;
		if ( pMob )
		{
			if ( pressed & IN_ATTACK2 )
			{
				mc::Writer w( mc::C2S_INTERACT_ENTITY );
				w.u16( (uint16)idx ).i32( pMob->GetMCId() );
				MCBridge().Send( w );
			}
		}
		else if ( pick.hit )
		{
			float hx = (float)( pick.point.x - pick.block.x ), hy = (float)( pick.point.y - pick.block.y ), hz = (float)( pick.point.z - pick.block.z );
			mc::Writer w( mc::C2S_USE_ON_BLOCK );
			w.u16( (uint16)idx ).i32( pick.block.x ).i32( pick.block.y ).i32( pick.block.z ).u8( pick.face ).f32( hx ).f32( hy ).f32( hz );
			MCBridge().Send( w );
		}
		else if ( pressed & IN_ATTACK2 )
		{
			mc::Writer w( mc::C2S_USE_ITEM );
			w.u16( (uint16)idx );
			MCBridge().Send( w );
		}
		p->DoAnimationEvent( PLAYERANIMEVENT_ATTACK_SECONDARY );
	}
	if ( released & IN_ATTACK2 )
	{
		// Bows, tridents, shields, eating: finish / release the use
		mc::Writer w( mc::C2S_RELEASE_USE );
		w.u16( (uint16)idx );
		MCBridge().Send( w );
	}
}

//-----------------------------------------------------------------------------
// Commands
//-----------------------------------------------------------------------------
CON_COMMAND( mc_connect, "Connect the server to the Minecraft bridge: mc_connect [host] [port]" )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() ) return;
	if ( args.ArgC() > 1 ) mc_host.SetValue( args[1] );
	if ( args.ArgC() > 2 ) mc_port.SetValue( args[2] );
	g_MCServerSystem.Connect();
}

CON_COMMAND( mc_disconnect, "Disconnect the server from the Minecraft bridge" )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() ) return;
	mc_autoconnect.SetValue( 0 );
	MCBridge().Disconnect();
}

CON_COMMAND( mc_status, "Show Minecraft bridge status (server)" )
{
	MCBridge().PrintStatus();
	g_MCServerSystem.PrintOrigin();
	CBaseEntity *w = CMCWorld::Get();
	Msg( "TF2MC[server]: mc_world entity %s\n", w ? "present" : "MISSING" );
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBasePlayer *p = UTIL_PlayerByIndex( i );
		if ( p && p->IsConnected() ) MC_DebugProbe( p, "server" );
	}
}

CON_COMMAND( mc_rebase, "Force the floating origin to Minecraft x z: mc_rebase <x> <z>" )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() || args.ArgC() < 3 || !MC_IsActive() ) return;
	g_MCServerSystem.Rebase( floor( atof( args[1] ) / 16.0 ) * 16.0, floor( atof( args[2] ) / 16.0 ) * 16.0 );
}

// --- Issued by clients (client.dll binds / inventory screen) ---
static CTFPlayer *CommandPlayer() { return ToTFPlayer( UTIL_GetCommandClient() ); }

CON_COMMAND( mc_build, "mc_build <0|1>: TF classes put their weapon away and dig/place like Minecraft" )
{
	CTFPlayer *p = CommandPlayer();
	if ( !p || args.ArgC() < 2 ) return;
	MC_SetBuildMode( p, atoi( args[1] ) != 0 );
}

CON_COMMAND( mc_select, "mc_select <0-8>: select Minecraft hotbar slot" )
{
	CTFPlayer *p = CommandPlayer();
	if ( !p || args.ArgC() < 2 ) return;
	mc::Writer w( mc::C2S_SELECT_SLOT );
	w.u16( (uint16)p->entindex() ).u8( (uint8)clamp( atoi( args[1] ), 0, 8 ) );
	MCBridge().Send( w );
}

CON_COMMAND( mc_inv, "Open the Minecraft inventory" )
{
	CTFPlayer *p = CommandPlayer();
	if ( !p ) return;
	mc::Writer w( mc::C2S_OPEN_INVENTORY );
	w.u16( (uint16)p->entindex() );
	MCBridge().Send( w );
}

CON_COMMAND( mc_inv_close, "Close the Minecraft container screen" )
{
	CTFPlayer *p = CommandPlayer();
	if ( !p ) return;
	mc::Writer w( mc::C2S_CONTAINER_CLOSE );
	w.u16( (uint16)p->entindex() );
	MCBridge().Send( w );
}

CON_COMMAND( mc_click, "mc_click <containerId> <slot> <button> <clickType>" )
{
	CTFPlayer *p = CommandPlayer();
	if ( !p || args.ArgC() < 5 ) return;
	mc::Writer w( mc::C2S_CONTAINER_CLICK );
	w.u16( (uint16)p->entindex() ).i32( atoi( args[1] ) ).u16( (uint16)(int16)atoi( args[2] ) ).u8( (uint8)atoi( args[3] ) ).u8( (uint8)atoi( args[4] ) );
	MCBridge().Send( w );
}

CON_COMMAND( mc_drop_item, "Drop the held Minecraft item (mc_drop_item 1 = whole stack)" )
{
	CTFPlayer *p = CommandPlayer();
	if ( !p ) return;
	mc::Writer w( mc::C2S_DROP );
	w.u16( (uint16)p->entindex() ).u8( args.ArgC() > 1 && atoi( args[1] ) ? 1 : 0 );
	MCBridge().Send( w );
}
