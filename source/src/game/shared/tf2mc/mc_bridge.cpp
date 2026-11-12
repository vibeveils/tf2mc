//========= TF2MC =========//
#include "cbase.h"
#include "mc_bridge.h"
#include "filesystem.h"
#include "tier1/utlbuffer.h"
#include "vphysics_interface.h"
#include "physics_shared.h"
#include "coordsize.h"
#include "igamemovement.h"
#ifdef CLIENT_DLL
#include "c_baseplayer.h"
#include "c_recipientfilter.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar mc_jump_assist( "mc_jump_assist", "1", FCVAR_REPLICATED | FCVAR_NOTIFY, "Let TF2 classes jump one Minecraft block without crouch-jumping" );
ConVar mc_allow_steve( "mc_allow_steve", "1", FCVAR_REPLICATED | FCVAR_NOTIFY, "Allow the Minecraft-mode player (Civilian slot, 'mc_steve')" );
#ifdef CLIENT_DLL
static ConVar mc_debug( "cl_mc_debug", "0", FCVAR_NONE, "Print client bridge traffic statistics" );
#else
static ConVar mc_debug( "mc_debug", "0", FCVAR_NONE, "Print server bridge traffic statistics" );
#endif

#ifdef CLIENT_DLL
static CMCBridge g_ClientBridge;
CMCBridge &MCBridge() { return g_ClientBridge; }
#else
static CMCBridge g_ServerBridge;
CMCBridge &MCBridge() { return g_ServerBridge; }
#endif

bool MC_IsActive()
{
	return MCBridge().IsReady();
}

static float MC_Gravity()
{
	static ConVarRef sv_gravity( "sv_gravity" );
	return sv_gravity.IsValid() ? sv_gravity.GetFloat() : 800.f;
}

float MC_SteveJumpSpeed()
{
	// Minecraft jump apex is ~1.25 blocks
	return sqrtf( 2.f * MC_Gravity() * 1.25f * (float)MCBridge().Coords().scale );
}

float MC_ClassJumpSpeed( float tfJumpSpeed )
{
	if ( !mc_jump_assist.GetBool() || !MC_IsActive() )
		return tfJumpSpeed;
	float need = sqrtf( 2.f * MC_Gravity() * ( (float)MCBridge().Coords().scale + 4.f ) );
	return MAX( tfJumpSpeed, need );
}

bool MC_AllowCivilian()
{
	return mc_allow_steve.GetBool() && MC_IsActive();
}

//-----------------------------------------------------------------------------

CMCBridge::CMCBridge()
	: m_bHelloAck( false ), m_bDefsLoaded( false ), m_bSkyLight( true ), m_epoch( 0 ), m_dayTime( 6000 ), m_iPort( 0 ), m_role( 0 ),
	  m_digStage( -1 ), m_flLastStatusPrint( 0 ), m_lastState( -1 )
{
	m_spawn[0] = m_spawn[1] = m_spawn[2] = 0;
	m_szLevel[0] = 0;
	m_szDimension[0] = 0;
	m_szHost[0] = 0;
	memset( &m_hotbar, 0, sizeof( m_hotbar ) );
	memset( &m_status, 0, sizeof( m_status ) );
	m_container.open = false;
	m_container.containerId = -1;
	m_container.type[0] = 0;
	m_container.carried[0] = 0;
	m_container.carriedCount = 0;
}

void CMCBridge::LoadBlockDefinitions()
{
	if ( m_bDefsLoaded || !g_pFullFileSystem )
		return;
	m_bDefsLoaded = true;
	CUtlBuffer buf( 0, 0, CUtlBuffer::TEXT_BUFFER );
	if ( g_pFullFileSystem->ReadFile( "scripts/mc_blocks.txt", "MOD", buf ) )
	{
		std::string text( (const char *)buf.Base(), buf.TellPut() );
		int n = m_world.Registry().LoadDefinitions( text );
		DevMsg( "TF2MC: loaded %d block definitions (atlas %dx%d)\n", n, m_world.Registry().TilesPerRow(), m_world.Registry().TileRows() );
	}
	else
	{
		Msg( "TF2MC: scripts/mc_blocks.txt not found - using flat colours (run tools/mc_assets.py to build textures)\n" );
	}
}

void CMCBridge::Connect( const char *host, int port, uint8 role, const char *name )
{
	LoadBlockDefinitions();
	Disconnect();
	Q_strncpy( m_szHost, host, sizeof( m_szHost ) );
	m_iPort = port;
	m_role = role;
	m_conn.Connect( host, port );

	mc::Writer w( mc::C2S_HELLO );
	w.u16( mc::PROTOCOL_VERSION ).u8( role ).str( name ? name : "tf2" );
	m_conn.Send( w );   // queued until the socket connects
	Msg( "TF2MC: connecting to Minecraft bridge %s:%d as %s\n", host, port, role == mc::ROLE_AUTHORITY ? "authority" : "observer" );
}

void CMCBridge::Disconnect()
{
	m_conn.Disconnect();
	m_bHelloAck = false;
	m_digStage = -1;
	m_hotbar.valid = false;
	m_status.valid = false;
	m_container.open = false;
	m_world.Reset( m_world.MinSection(), m_world.SectionCount() );
	m_lastState = -1;
}

float CMCBridge::DaylightFactor() const
{
	if ( !m_bSkyLight ) return 1.f;   // Nether/End: no sky light, block light only
	int t = (int)( m_dayTime % 24000 );
	float f;
	if ( t < 12000 ) f = 1.f;
	else if ( t < 13800 ) f = 1.f - ( t - 12000 ) / 1800.f;
	else if ( t < 22200 ) f = 0.f;
	else f = ( t - 22200 ) / 1800.f;
	return 0.12f + 0.88f * f;
}

void CMCBridge::Pump( std::vector<mc::Packet> &other )
{
	int state = (int)m_conn.GetState();
	if ( state != m_lastState )
	{
		if ( state == mc::Connection::FAILED )
			Warning( "TF2MC: bridge connection to %s:%d failed: %s\n", m_szHost, m_iPort, m_conn.GetError().c_str() );
		else if ( state == mc::Connection::CONNECTED )
			Msg( "TF2MC: connected to Minecraft bridge\n" );
		m_lastState = state;
		if ( state == mc::Connection::FAILED ) m_bHelloAck = false;
	}

	std::vector<mc::Packet> packets;
	m_conn.Poll( packets );
	for ( size_t i = 0; i < packets.size(); ++i )
	{
		mc::Packet &p = packets[i];
		mc::Reader r = p.reader();
		switch ( p.type )
		{
		case mc::S2C_HELLO_ACK:
		{
			int ver = r.u16();
			int minSec = r.i32(), count = r.i32();
			m_spawn[0] = r.i32(); m_spawn[1] = r.i32(); m_spawn[2] = r.i32();
			std::string lvl = r.str();
			Q_strncpy( m_szLevel, lvl.c_str(), sizeof( m_szLevel ) );
			std::string dim = r.str();
			Q_strncpy( m_szDimension, dim.c_str(), sizeof( m_szDimension ) );
			m_epoch = r.i32();
			m_bSkyLight = ( r.u8() & 1 ) != 0;
			m_digStage = -1;
			m_container.open = false;
			if ( ver != mc::PROTOCOL_VERSION )
				Warning( "TF2MC: protocol version mismatch (bridge %d, game %d)\n", ver, mc::PROTOCOL_VERSION );
			m_world.Reset( minSec, count );
			m_bHelloAck = true;
			Msg( "TF2MC: attached to Minecraft world '%s' %s (y %d..%d, arrival %d %d %d, epoch %d)\n", m_szLevel, m_szDimension, minSec * 16, ( minSec + count ) * 16, m_spawn[0], m_spawn[1], m_spawn[2], m_epoch );
			other.push_back( std::move( p ) );   // let the owner react (spawn coords etc.)
			break;
		}
		case mc::S2C_STATE_DEFS: m_world.ApplyStateDefsPacket( r ); break;
		case mc::S2C_SECTION: m_world.ApplySectionPacket( r ); break;
		case mc::S2C_LIGHT: m_world.ApplyLightPacket( r ); break;
		case mc::S2C_BLOCK_SET:
		{
			mc::BlockPos pos; uint16 oldId = 0, newId = 0;
			if ( m_world.ApplyBlockSetPacket( r, &pos, &oldId, &newId ) ) OnBlockChanged( pos, oldId, newId );
			break;
		}
		case mc::S2C_COLUMN_UNLOAD: m_world.ApplyUnloadPacket( r ); break;
		case mc::S2C_TIME: m_dayTime = r.i64(); break;
		case mc::S2C_DIG_PROGRESS:
		{
			int x = r.i32(), y = r.i32(), z = r.i32();
			int stage = r.i8();
			m_digPos = mc::BlockPos( x, y, z );
			m_digStage = stage;
			break;
		}
		case mc::S2C_INVENTORY:
		{
			mc::Reader peek = p.reader();
			int slot = peek.u16();
#ifdef CLIENT_DLL
			C_BasePlayer *pLocal = C_BasePlayer::GetLocalPlayer();
			if ( pLocal && pLocal->entindex() == slot )
			{
				r.u16();
				m_hotbar.selected = r.u8();
				int n = MIN( 9, (int)r.u8() );
				for ( int k = 0; k < n; ++k )
				{
					std::string item = r.str();
					Q_strncpy( m_hotbar.items[k], item.c_str(), sizeof( m_hotbar.items[k] ) );
					m_hotbar.counts[k] = r.u8();
				}
				m_hotbar.valid = r.ok();
			}
#else
			(void)slot;
			other.push_back( std::move( p ) );
#endif
			break;
		}
#ifdef CLIENT_DLL
		case mc::S2C_PLAYER_STATUS:
		{
			C_BasePlayer *pLocal = C_BasePlayer::GetLocalPlayer();
			if ( !pLocal || pLocal->entindex() != r.u16() ) break;
			m_status.food = r.f32(); m_status.saturation = r.f32(); m_status.xpLevel = r.i32();
			m_status.xpProgress = r.f32(); m_status.armor = r.f32(); m_status.usingItem = ( r.u8() & 1 ) != 0;
			m_status.valid = r.ok();
			break;
		}
		case mc::S2C_CONTAINER:
		{
			C_BasePlayer *pLocal = C_BasePlayer::GetLocalPlayer();
			if ( !pLocal || pLocal->entindex() != r.u16() ) break;
			m_container.containerId = r.i32();
			Q_strncpy( m_container.type, r.str().c_str(), sizeof( m_container.type ) );
			r.str();   // title (reserved)
			int n = r.u16();
			m_container.slots.resize( n );
			for ( int k = 0; k < n && r.ok(); ++k )
			{
				MCSlotView &v = m_container.slots[k];
				v.x = (int16)r.u16(); v.y = (int16)r.u16();
				Q_strncpy( v.item, r.str().c_str(), sizeof( v.item ) );
				v.count = r.u8(); v.durability = r.u8();
			}
			Q_strncpy( m_container.carried, r.str().c_str(), sizeof( m_container.carried ) );
			m_container.carriedCount = r.u8();
			m_container.open = r.ok();
			break;
		}
		case mc::S2C_CONTAINER_CLOSE:
		{
			C_BasePlayer *pLocal = C_BasePlayer::GetLocalPlayer();
			if ( pLocal && pLocal->entindex() == r.u16() ) m_container.open = false;
			break;
		}
		case mc::S2C_CHAT:
			Msg( "[Minecraft] %s\n", r.str().c_str() );
			break;
#endif
		default:
			other.push_back( std::move( p ) );
			break;
		}
	}

	if ( mc_debug.GetBool() && gpGlobals->curtime - m_flLastStatusPrint > 2.f )
	{
		m_flLastStatusPrint = gpGlobals->curtime;
		PrintStatus();
	}
}

void CMCBridge::PrintStatus()
{
	static const char *states[] = { "disconnected", "connecting", "connected", "failed" };
	Msg( "TF2MC[%s]: %s %s:%d  ready=%d columns=%d blocks=%d in=%llu KB out=%llu KB\n",
#ifdef CLIENT_DLL
		 "client",
#else
		 "server",
#endif
		 states[ m_conn.GetState() ], m_szHost, m_iPort, (int)m_bHelloAck, (int)m_world.ColumnCount(),
		 (int)m_world.Registry().Count(), (unsigned long long)( m_conn.BytesIn() / 1024 ), (unsigned long long)( m_conn.BytesOut() / 1024 ) );
}

//-----------------------------------------------------------------------------
// Voxel trace -> Source trace
//-----------------------------------------------------------------------------
int MC_SurfacePropForName( const std::string &n );
int MC_SurfacePropForName( const std::string &n )
{
	if ( !physprops ) return 0;
	const char *prop = "concrete";
	if ( n.find( "grass" ) != std::string::npos || n.find( "dirt" ) != std::string::npos || n.find( "podzol" ) != std::string::npos ||
		 n.find( "mud" ) != std::string::npos || n.find( "farmland" ) != std::string::npos || n.find( "mycelium" ) != std::string::npos ) prop = "dirt";
	else if ( n.find( "sand" ) != std::string::npos && n.find( "stone" ) == std::string::npos ) prop = "sand";
	else if ( n.find( "gravel" ) != std::string::npos ) prop = "gravel";
	else if ( n.find( "log" ) != std::string::npos || n.find( "planks" ) != std::string::npos || n.find( "wood" ) != std::string::npos ||
			  n.find( "stem" ) != std::string::npos || n.find( "fence" ) != std::string::npos || n.find( "door" ) != std::string::npos ) prop = "wood";
	else if ( n.find( "glass" ) != std::string::npos || n.find( "ice" ) != std::string::npos ) prop = "glass";
	else if ( n.find( "snow" ) != std::string::npos ) prop = "snow";
	else if ( n.find( "leaves" ) != std::string::npos || n.find( "wool" ) != std::string::npos || n.find( "carpet" ) != std::string::npos ) prop = "carpet";
	else if ( n.find( "iron" ) != std::string::npos || n.find( "copper" ) != std::string::npos || n.find( "gold_block" ) != std::string::npos ) prop = "metal";
	return physprops->GetSurfaceIndex( prop );
}

static int SurfacePropForBlock( const mc::BlockDef &d )
{
	return MC_SurfacePropForName( d.name );
}

bool CMCBridge::TraceRay( const Ray_t &ray, unsigned int mask, trace_t &tr, CBaseEntity *pOwner )
{
	if ( !IsReady() || !( mask & CONTENTS_SOLID ) )
		return false;

	const mc::Vec3d start = m_coords.ToMC( MC_V( (Vector)ray.m_Start ) );
	const mc::Vec3d delta = m_coords.DeltaToMC( MC_V( (Vector)ray.m_Delta ) );
	const mc::Vec3d half = ray.m_IsRay ? mc::Vec3d( 0, 0, 0 ) : m_coords.ExtentToMC( MC_V( (Vector)ray.m_Extents ) );

	mc::SweepResult r = mc::SweepBox( m_world, start, delta, half );
	if ( !r.hit )
		return false;

	float len = ray.m_Delta.Length();
	float frac = (float)r.fraction;
	if ( r.startSolid ) frac = 0.f;
	else if ( len > 0.f ) frac = MAX( 0.f, ( frac * len - DIST_EPSILON ) / len );

	Vector actualStart = (Vector)ray.m_Start + (Vector)ray.m_StartOffset;
	tr.fraction = frac;
	tr.startsolid = r.startSolid;
	tr.allsolid = r.allSolid;
	tr.fractionleftsolid = 0.f;
	tr.startpos = actualStart;
	tr.endpos = actualStart + (Vector)ray.m_Delta * frac;

	Vector n = r.startSolid ? Vector( 0, 0, 1 ) : MC_V( m_coords.DirToSource( r.normal ) );
	Vector centreEnd = (Vector)ray.m_Start + (Vector)ray.m_Delta * frac;
	tr.plane.normal = n;
	tr.plane.dist = DotProduct( centreEnd, n ) - fabsf( DotProduct( (Vector)ray.m_Extents, n ) );
	tr.plane.type = ( n.x != 0.f ) ? 0 : ( n.y != 0.f ) ? 1 : 2;
	tr.plane.signbits = ( n.x < 0 ? 1 : 0 ) | ( n.y < 0 ? 2 : 0 ) | ( n.z < 0 ? 4 : 0 );
	tr.contents = CONTENTS_SOLID;

	// Surface property (footsteps, impact effects) cached per block id
	static std::vector<int> s_surfCache;
	uint16 id = m_world.Get( r.block.x, r.block.y, r.block.z );
	if ( id >= s_surfCache.size() ) s_surfCache.resize( id + 1, -1 );
	if ( s_surfCache[id] < 0 ) s_surfCache[id] = SurfacePropForBlock( m_world.Registry().Get( id ) );
	tr.surface.name = "mc_block";
	tr.surface.flags = 0;
	tr.surface.surfaceProps = (unsigned short)s_surfCache[id];
	tr.hitgroup = 0;
	tr.hitbox = 0;
	tr.physicsbone = 0;
	tr.m_pEnt = pOwner;
	return true;
}

//-----------------------------------------------------------------------------
// Block change feedback (client: break/place sounds from the block's surface properties)
//-----------------------------------------------------------------------------
void CMCBridge::OnBlockChanged( const mc::BlockPos &pos, uint16 oldId, uint16 newId )
{
#ifdef CLIENT_DLL
	if ( !physprops || oldId == newId ) return;
	const mc::BlockDef &oldDef = m_world.Registry().Get( oldId );
	const mc::BlockDef &newDef = m_world.Registry().Get( newId );
	bool broke = newId == mc::BlockRegistry::AIR && oldDef.shape != mc::SHAPE_AIR && oldDef.shape != mc::SHAPE_LIQUID;
	bool placed = oldId == mc::BlockRegistry::AIR && newDef.shape != mc::SHAPE_AIR && newDef.shape != mc::SHAPE_LIQUID;
	if ( !broke && !placed ) return;
	Vector origin = MC_V( m_coords.ToSource( mc::Vec3d( pos.x + 0.5, pos.y + 0.5, pos.z + 0.5 ) ) );
	C_BasePlayer *pLocal = C_BasePlayer::GetLocalPlayer();
	if ( pLocal && ( pLocal->GetAbsOrigin() - origin ).LengthSqr() > 2048.f * 2048.f ) return;

	const mc::BlockDef &probe = broke ? oldDef : newDef;
	surfacedata_t *pData = physprops->GetSurfaceData( MC_SurfacePropForName( probe.name ) );
	if ( !pData ) return;
	unsigned short snd = broke ? ( pData->sounds.breakSound ? pData->sounds.breakSound : pData->sounds.impactHard ) : pData->sounds.impactSoft;
	const char *pszSound = physprops->GetString( snd );
	if ( !pszSound || !pszSound[0] ) return;
	CLocalPlayerFilter filter;
	C_BaseEntity::EmitSound( filter, SOUND_FROM_WORLD, pszSound, &origin );
#else
	(void)pos; (void)oldId; (void)newId;
#endif
}

//-----------------------------------------------------------------------------
// Swimming: Minecraft water/lava as CONTENTS_WATER / CONTENTS_SLIME for TF2's water code
//-----------------------------------------------------------------------------
int MC_PointContents( const Vector &pt, int engineContents )
{
	if ( !MC_IsActive() ) return engineContents;
	CMCBridge &b = MCBridge();
	mc::Vec3d m = b.Coords().ToMC( MC_V( pt ) );
	if ( b.World().IsWaterAt( m.x, m.y, m.z ) ) return engineContents | CONTENTS_WATER;
	if ( b.World().IsLavaAt( m.x, m.y, m.z ) ) return engineContents | CONTENTS_SLIME;
	return engineContents;
}

//-----------------------------------------------------------------------------
// Ladders, vines, scaffolding: Minecraft-style climbing for every class
//-----------------------------------------------------------------------------
static ConVar mc_climb_speed( "mc_climb_speed", "2.35", FCVAR_REPLICATED, "Climb speed on ladders/vines in blocks per second" );

void MC_ClimbMove( CBaseEntity *pPlayer, CMoveData *mv, const Vector &mins, const Vector &maxs )
{
	if ( !MC_IsActive() || !pPlayer ) return;
	CMCBridge &b = MCBridge();
	const mc::Coords &c = b.Coords();
	// Slightly inflated hull: you climb when touching the ladder
	Vector lo = mv->GetAbsOrigin() + mins - Vector( 2, 2, 0 ), hi = mv->GetAbsOrigin() + maxs + Vector( 2, 2, 0 );
	mc::Vec3d a = c.ToMC( MC_V( lo ) ), z = c.ToMC( MC_V( hi ) );
	mc::Vec3d mn( MIN( a.x, z.x ), MIN( a.y, z.y ), MIN( a.z, z.z ) ), mx( MAX( a.x, z.x ), MAX( a.y, z.y ), MAX( a.z, z.z ) );
	if ( !mc::BoxTouchesClimbable( b.World(), mn, mx ) ) return;

	const float speed = mc_climb_speed.GetFloat() * (float)c.scale;
	bool up = ( mv->m_nButtons & IN_JUMP ) || mv->m_flForwardMove > 0.f;
	bool hold = ( mv->m_nButtons & IN_DUCK ) != 0;
	if ( up ) mv->m_vecVelocity.z = speed;
	else if ( hold ) mv->m_vecVelocity.z = MAX( mv->m_vecVelocity.z, 0.f );
	else mv->m_vecVelocity.z = MAX( mv->m_vecVelocity.z, -0.15f * 20.f * (float)c.scale );   // Minecraft's slow slide
}

const char *MC_PrettyItemName( const char *id, char *out, int outLen )
{
	const char *n = Q_strstr( id, ":" );
	n = n ? n + 1 : id;
	Q_strncpy( out, n, outLen );
	bool up = true;
	for ( char *p = out; *p; ++p )
	{
		if ( *p == '_' ) { *p = ' '; up = true; continue; }
		if ( up && *p >= 'a' && *p <= 'z' ) *p = (char)( *p - 'a' + 'A' );
		up = false;
	}
	return out;
}
