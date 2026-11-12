// End-to-end probe: C++ core (as used by the Source mod) <-> Java BridgeServer (as used by the Fabric mod).
// Run tests/run_e2e.sh, which starts FakeWorldServer and then this probe.
#include "mc_collide.h"
#include "mc_net.h"
#include "mc_world.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace mc;

static int g_fail = 0;
#define EXPECT( c, msg ) do { if ( !( c ) ) { ++g_fail; printf( "FAIL: %s\n", msg ); } else printf( "ok:   %s\n", msg ); } while ( 0 )

struct Probe
{
	Connection conn;
	VoxelWorld world;
	bool helloAck = false;
	int spawn[3] = { 0, 0, 0 };
	bool zombieSeen = false, zombieRemoved = false;
	double zombiePos[3] = { 0, 0, 0 };
	int hurtCount = 0, invCount = 0, blockSets = 0, stateDefs = 0, containerSlots = -1, epoch = 0;
	std::string containerType, dimension;

	void Pump()
	{
		std::vector<Packet> pk;
		conn.Poll( pk );
		for ( Packet &p : pk )
		{
			Reader r = p.reader();
			switch ( p.type )
			{
			case S2C_HELLO_ACK:
			{
				r.u16(); int minS = r.i32(), cnt = r.i32();
				spawn[0] = r.i32(); spawn[1] = r.i32(); spawn[2] = r.i32();
				r.str(); dimension = r.str(); epoch = r.i32(); r.u8();
				world.Reset( minS, cnt );
				helloAck = true;
				break;
			}
			case S2C_STATE_DEFS: world.ApplyStateDefsPacket( r ); ++stateDefs; break;
			case S2C_LIGHT: world.ApplyLightPacket( r ); break;
			case S2C_CONTAINER: { r.u16(); r.i32(); std::string type = r.str(); r.str(); containerSlots = r.u16(); containerType = type; break; }
			case S2C_SECTION: world.ApplySectionPacket( r ); break;
			case S2C_BLOCK_SET: world.ApplyBlockSetPacket( r ); ++blockSets; break;
			case S2C_COLUMN_UNLOAD: world.ApplyUnloadPacket( r ); break;
			case S2C_ENTITY_SPAWN: { int id = r.i32(); std::string t = r.str(); if ( id == 1000 && t == "minecraft:zombie" ) zombieSeen = true; break; }
			case S2C_ENTITY_UPDATE: { int id = r.i32(); if ( id == 1000 ) { zombiePos[0] = r.f64(); zombiePos[1] = r.f64(); zombiePos[2] = r.f64(); } break; }
			case S2C_ENTITY_REMOVE: if ( r.i32() == 1000 ) zombieRemoved = true; break;
			case S2C_PLAYER_HURT: ++hurtCount; break;
			case S2C_INVENTORY: ++invCount; break;
			default: break;
			}
		}
	}

	template <class F> bool WaitFor( F cond, int ms )
	{
		for ( int t = 0; t < ms; t += 10 )
		{
			Pump();
			if ( cond() ) return true;
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		}
		return false;
	}
};

int main( int argc, char **argv )
{
	int port = argc > 1 ? atoi( argv[1] ) : 25599;
	Probe p;
	p.conn.Connect( "127.0.0.1", port );
	bool connected = p.WaitFor( [&] { return p.conn.GetState() == Connection::CONNECTED || p.conn.GetState() == Connection::FAILED; }, 5000 );
	EXPECT( connected && p.conn.IsConnected(), "TCP connect to Java BridgeServer" );
	if ( !p.conn.IsConnected() ) { printf( "error: %s\n", p.conn.GetError().c_str() ); return 1; }

	{ Writer w( C2S_HELLO ); w.u16( PROTOCOL_VERSION ).u8( ROLE_AUTHORITY ).str( "e2e-probe" ); p.conn.Send( w ); }
	EXPECT( p.WaitFor( [&] { return p.helloAck; }, 3000 ), "HELLO -> HELLO_ACK" );
	EXPECT( p.world.MinY() == -64 && p.world.MaxY() == 320, "world height from HELLO_ACK" );
	EXPECT( p.dimension == "minecraft:overworld" && p.epoch == 1, "dimension + epoch in HELLO_ACK (v2)" );

	{ Writer w( C2S_PLAYER_JOIN ); w.u16( 1 ).str( "Heavy" ).u8( MODE_STEVE ); p.conn.Send( w ); }
	{ Writer w( C2S_PLAYER_MOVE ); w.u16( 1 ).f64( 0.5 ).f64( 64 ).f64( 0.5 ).f32( 0 ).f32( 0 ).u8( 1 ).i32( 1 ); p.conn.Send( w ); }

	EXPECT( p.WaitFor( [&] { return p.world.ColumnCount() >= 49; }, 10000 ), "received 7x7 chunk columns" );
	EXPECT( p.invCount > 0, "inventory for joined player" );
	EXPECT( p.world.TopSolidY( 0, 0 ) == 63, "top solid block at spawn is grass y=63" );
	EXPECT( p.world.GetDef( 0, 63, 0 ).name == "minecraft:grass_block", "block state decoded via STATE_DEFS" );
	EXPECT( p.stateDefs > 0 && p.world.GetDef( 0, 63, 0 ).opaque && !p.world.GetDef( 0, 64, 0 ).collides, "state flags (opaque cube, passable plant)" );
	EXPECT( p.world.SkyLight( 5, 70, 5 ) == 15 && p.world.SkyLight( 5, 50, 5 ) == 0, "sky light streamed with sections" );

	// Player physics against the streamed world (MC space; player hull 0.6 x 1.8)
	SweepResult fall = SweepBox( p.world, Vec3d( 0.5, 70.9, 0.5 ), Vec3d( 0, -10, 0 ), Vec3d( 0.3, 0.9, 0.3 ) );
	EXPECT( fall.hit && std::fabs( fall.fraction - 0.6 ) < 1e-9, "falling hull lands on y=64 surface" );

	// Source-space round trip: a TF2 hull (49x49x83) at scale 64
	Coords c; c.ox = 0; c.oz = 0; c.oy = 128;
	Vec3d srcStart = c.ToSource( Vec3d( 0.5, 70, 0.5 ) );
	Vec3d mcStart = c.ToMC( Vec3d( srcStart.x, srcStart.y, srcStart.z + 41.5 ) );   // hull centre
	SweepResult tfFall = SweepBox( p.world, mcStart, c.DeltaToMC( Vec3d( 0, 0, -64 * 10 ) ), c.ExtentToMC( Vec3d( 24.5, 24.5, 41.5 ) ) );
	printf( "tf frac %.9f hit %d ss %d\n", tfFall.fraction, tfFall.hit, tfFall.startSolid ); EXPECT( tfFall.hit && std::fabs( tfFall.fraction - 0.6 ) < 1e-6, "TF2 hull in Source units lands on the same surface" );

	// Look down, dig the grass block under the player
	PickResult pick = PickBlock( p.world, Vec3d( 1.5, 65.62, 0.5 ), Vec3d( 0, -1, 0 ), 4.5 );
	EXPECT( pick.hit && pick.block == BlockPos( 1, 63, 0 ), "pick the block under the crosshair" );
	for ( int i = 0; i < 8 && p.world.Get( 0, 63, 0 ) != BlockRegistry::AIR; ++i )
	{
		Writer w( C2S_DIG ); w.u16( 1 ).u8( DIG_CONTINUE ).i32( 0 ).i32( 63 ).i32( 0 ).u8( FACE_UP ); p.conn.Send( w );
		p.WaitFor( [&] { return false; }, 60 );
	}
	EXPECT( p.WaitFor( [&] { return p.world.Get( 0, 63, 0 ) == BlockRegistry::AIR; }, 2000 ), "digging removes block via BLOCK_SET" );
	std::vector<SectionKey> dirty; p.world.TakeDirty( dirty );
	EXPECT( !dirty.empty(), "edit marks section for remesh" );

	// Place a block on top of the dirt below
	{ Writer w( C2S_USE_ON_BLOCK ); w.u16( 1 ).i32( 2 ).i32( 63 ).i32( 2 ).u8( FACE_UP ).f32( 0.5f ).f32( 1.f ).f32( 0.5f ); p.conn.Send( w ); }
	EXPECT( p.WaitFor( [&] { return p.world.GetDef( 2, 64, 2 ).name == "minecraft:stone"; }, 2000 ), "placing a block via USE_ON_BLOCK" );

	// Mob mirrored and attackable
	EXPECT( p.WaitFor( [&] { return p.zombieSeen && p.zombiePos[1] > 0; }, 2000 ), "zombie spawn + movement updates" );
	for ( int i = 0; i < 5 && !p.zombieRemoved; ++i )
	{
		Writer w( C2S_ATTACK ); w.u16( 1 ).i32( 1000 ).u8( ATTACK_TF_DAMAGE ).f32( 6.5f ).f32( 1 ).f32( 0 ).f32( 0.4f ).u8( DMG_KIND_PROJECTILE ); p.conn.Send( w );
		p.WaitFor( [&] { return false; }, 60 );
	}
	EXPECT( p.WaitFor( [&] { return p.zombieRemoved; }, 2000 ), "TF damage kills the mob (ENTITY_REMOVE)" );

	// TF2 rocket explosion carves a crater
	{ Writer w( C2S_EXPLOSION ); w.u16( 1 ).f64( 8.5 ).f64( 63.5 ).f64( 8.5 ).f32( 1.4f ); p.conn.Send( w ); }
	EXPECT( p.WaitFor( [&] { return p.world.Get( 8, 63, 8 ) == BlockRegistry::AIR && p.world.Get( 9, 63, 8 ) == BlockRegistry::AIR; }, 2000 ), "explosion crater via C2S_EXPLOSION" );

	// Inventory screen
	{ Writer w( C2S_OPEN_INVENTORY ); w.u16( 1 ); p.conn.Send( w ); }
	EXPECT( p.WaitFor( [&] { return p.containerSlots == 9 && p.containerType == "inventory"; }, 2000 ), "container view after OPEN_INVENTORY" );

	printf( "bytes in %llu, out %llu\n", (unsigned long long)p.conn.BytesIn(), (unsigned long long)p.conn.BytesOut() );
	p.conn.Disconnect();
	printf( g_fail ? "E2E FAILED (%d)\n" : "E2E PASSED\n", g_fail );
	return g_fail ? 1 : 0;
}
