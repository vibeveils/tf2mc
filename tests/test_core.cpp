// Unit tests for the engine-independent TF2MC core.
// Build: see tests/run_tests.sh
#include "mc_collide.h"
#include "mc_mesher.h"
#include "mc_protocol.h"
#include "mc_world.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace mc;

static int g_fail = 0, g_pass = 0;
#define CHECK( cond ) do { if ( cond ) ++g_pass; else { ++g_fail; printf( "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond ); } } while ( 0 )
#define NEAR( a, b ) ( std::fabs( (a) - (b) ) < 1e-6 )

// Global state ids used by the tests (like Block.getId(state) on the server)
enum { ST_AIR = 0, ST_STONE = 1, ST_GRASS = 9, ST_WATER = 86, ST_SHORT_GRASS = 2000, ST_SLAB = 11000, ST_GLASS = 520, ST_FENCE = 5800, ST_LADDER = 4700, ST_STAIRS_WL = 3001 };

static void PutBox( Writer &w, float a, float b, float c, float d, float e, float f ) { w.f32( a ).f32( b ).f32( c ).f32( d ).f32( e ).f32( f ); }

// v3: break, step, place, hit, fall sound ids after the shapes
static void PutSounds( Writer &w, bool stone )
{
	static const char *kStone[5] = { "minecraft:block.stone.break", "minecraft:block.stone.step", "minecraft:block.stone.place", "minecraft:block.stone.hit", "minecraft:block.stone.fall" };
	for ( int i = 0; i < 5; ++i ) w.str( stone ? kStone[i] : "" );
}

static std::vector<uint8_t> StateDefsPacket()
{
	Writer w( S2C_STATE_DEFS );
	w.u16( 10 );
	w.i32( ST_AIR ).str( "minecraft:air" ).str( "" ).u8( 0 ).u8( 0 ).u8( 0 ).u8( 0 );
	PutSounds( w, false );
	w.i32( ST_STONE ).str( "minecraft:stone" ).str( "" ).u8( STATE_COLLIDES | STATE_OPAQUE_CUBE ).u8( 0 ).u8( 1 ); PutBox( w, 0, 0, 0, 1, 1, 1 ); w.u8( 1 ); PutBox( w, 0, 0, 0, 1, 1, 1 );
	PutSounds( w, true );
	w.i32( ST_GRASS ).str( "minecraft:grass_block" ).str( "snowy=false" ).u8( STATE_COLLIDES | STATE_OPAQUE_CUBE ).u8( 0 ).u8( 1 ); PutBox( w, 0, 0, 0, 1, 1, 1 ); w.u8( 1 ); PutBox( w, 0, 0, 0, 1, 1, 1 );
	PutSounds( w, false );
	w.i32( ST_WATER ).str( "minecraft:water" ).str( "level=0" ).u8( STATE_LIQUID ).u8( 0 ).u8( 0 ).u8( 0 );
	PutSounds( w, false );
	w.i32( ST_SHORT_GRASS ).str( "minecraft:short_grass" ).str( "" ).u8( 0 ).u8( 0 ).u8( 0 ).u8( 1 ); PutBox( w, 0.125f, 0, 0.125f, 0.875f, 0.8125f, 0.875f );
	PutSounds( w, false );
	w.i32( ST_SLAB ).str( "minecraft:oak_slab" ).str( "type=bottom,waterlogged=false" ).u8( STATE_COLLIDES ).u8( 0 ).u8( 1 ); PutBox( w, 0, 0, 0, 1, 0.5f, 1 ); w.u8( 1 ); PutBox( w, 0, 0, 0, 1, 0.5f, 1 );
	PutSounds( w, false );
	w.i32( ST_GLASS ).str( "minecraft:glass" ).str( "" ).u8( STATE_COLLIDES ).u8( 0 ).u8( 1 ); PutBox( w, 0, 0, 0, 1, 1, 1 ); w.u8( 1 ); PutBox( w, 0, 0, 0, 1, 1, 1 );
	PutSounds( w, false );
	w.i32( ST_FENCE ).str( "minecraft:oak_fence" ).str( "east=true" ).u8( STATE_COLLIDES ).u8( 0 ).u8( 2 ); PutBox( w, 0.375f, 0, 0.375f, 0.625f, 1.5f, 0.625f ); PutBox( w, 0.625f, 0, 0.375f, 1, 1.5f, 0.625f );
	w.u8( 2 ); PutBox( w, 0.375f, 0, 0.375f, 0.625f, 1, 0.625f ); PutBox( w, 0.625f, 0.375f, 0.4375f, 1, 0.9375f, 0.5625f );
	PutSounds( w, false );
	w.i32( ST_LADDER ).str( "minecraft:ladder" ).str( "facing=north" ).u8( STATE_COLLIDES | STATE_CLIMBABLE ).u8( 0 ).u8( 1 ); PutBox( w, 0, 0, 0.8125f, 1, 1, 1 ); w.u8( 1 ); PutBox( w, 0, 0, 0.8125f, 1, 1, 1 );
	PutSounds( w, false );
	w.i32( ST_STAIRS_WL ).str( "minecraft:oak_stairs" ).str( "facing=east,half=bottom,waterlogged=true" ).u8( STATE_COLLIDES | STATE_WATERLOGGED ).u8( 0 ).u8( 2 ); PutBox( w, 0, 0, 0, 1, 0.5f, 1 ); PutBox( w, 0.5f, 0.5f, 0, 1, 1, 1 );
	w.u8( 2 ); PutBox( w, 0, 0, 0, 1, 0.5f, 1 ); PutBox( w, 0.5f, 0.5f, 0, 1, 1, 1 );
	PutSounds( w, false );
	return w.finish();
}

// Section packet with a flat floor (stone at y=0..3, grass at y=4) for column cx,cz, section 0.
// Sky light: 0 inside the ground, 15 above; block light 0.
static std::vector<uint8_t> FloorSectionPacket( int cx, int cz, bool withLight = true )
{
	Writer w( S2C_SECTION );
	w.i32( cx ).i32( 0 ).i32( cz ).u16( 3 ).i32( ST_AIR ).i32( ST_STONE ).i32( ST_GRASS );
	std::vector<uint8_t> idx( 4096, 0 );
	for ( int y = 0; y < 5; ++y )
		for ( int z = 0; z < 16; ++z )
			for ( int x = 0; x < 16; ++x )
				idx[ Section::Index( x, y, z ) ] = y < 4 ? 1 : 2;
	for ( uint8_t b : idx ) w.u8( b );
	if ( withLight )
	{
		w.u8( 1 );
		for ( int i = 0; i < 2048; ++i )
		{
			int y = ( i * 2 ) >> 8;
			w.u8( y < 5 ? 0x00 : 0xFF );
		}
	}
	else w.u8( 0 );
	return w.finish();
}

static void ApplyFrame( VoxelWorld &w, const std::vector<uint8_t> &frame )
{
	uint32_t len; memcpy( &len, frame.data(), 4 );
	CHECK( len == frame.size() - 4 );
	Reader r( frame.data() + 5, len - 1 );
	uint8_t type = frame[4];
	if ( type == S2C_STATE_DEFS ) CHECK( w.ApplyStateDefsPacket( r ) );
	else if ( type == S2C_SECTION ) CHECK( w.ApplySectionPacket( r ) );
	else if ( type == S2C_LIGHT ) CHECK( w.ApplyLightPacket( r ) );
	else if ( type == S2C_BLOCK_SET ) CHECK( w.ApplyBlockSetPacket( r ) );
	else if ( type == S2C_COLUMN_UNLOAD ) CHECK( w.ApplyUnloadPacket( r ) );
}

static void TestProtocol()
{
	Writer w( C2S_PLAYER_MOVE );
	w.u16( 7 ).f64( 1.5 ).f64( -2.25 ).f64( 3e9 ).f32( 90.f ).f32( -10.f ).u8( 3 ).str( "héllo" );
	std::vector<uint8_t> f = w.finish();
	Reader r( f.data() + 5, f.size() - 5 );
	CHECK( f[4] == C2S_PLAYER_MOVE );
	CHECK( r.u16() == 7 );
	CHECK( r.f64() == 1.5 );
	CHECK( r.f64() == -2.25 );
	CHECK( r.f64() == 3e9 );
	CHECK( r.f32() == 90.f );
	CHECK( r.f32() == -10.f );
	CHECK( r.u8() == 3 );
	CHECK( r.str() == "héllo" );
	CHECK( r.ok() && r.remaining() == 0 );
	r.u8();
	CHECK( !r.ok() );   // overrun detected
}

static void TestCoords()
{
	Coords c; c.scale = 64; c.ox = 10; c.oy = 128; c.oz = -5;
	Vec3d m( 12.5, 70.0, -3.0 );
	Vec3d s = c.ToSource( m );
	CHECK( NEAR( s.x, 2.5 * 64 ) );
	CHECK( NEAR( s.y, -2.0 * 64 ) );
	CHECK( NEAR( s.z, -58.0 * 64 ) );
	Vec3d back = c.ToMC( s );
	CHECK( NEAR( back.x, m.x ) && NEAR( back.y, m.y ) && NEAR( back.z, m.z ) );
	// MC yaw 0 faces +Z (south) -> Source -Y -> source yaw -90
	CHECK( NEAR( Coords::YawToSource( 0 ), -90 ) );
	CHECK( NEAR( Coords::YawToMC( -90 ), 0 ) );
	CHECK( NEAR( Coords::YawToMC( Coords::YawToSource( 37 ) ), 37 ) );
	// MC yaw 90 faces -X (west) -> source yaw -180 == 180
	Vec3d dir = c.DirToSource( Vec3d( -1, 0, 0 ) );
	float sy = Coords::YawToSource( 90 );
	CHECK( NEAR( std::cos( sy * M_PI / 180 ), dir.x ) && NEAR( std::sin( sy * M_PI / 180 ), dir.y ) );
}

static void TestWorld()
{
	VoxelWorld w;
	w.Reset( -4, 24 );
	CHECK( w.MinY() == -64 && w.MaxY() == 320 );
	ApplyFrame( w, StateDefsPacket() );
	{
		// v3 state defs carry the block sound type
		const BlockDef &st = w.Registry().Get( w.Registry().ForState( ST_STONE ) );
		CHECK( w.Registry().SoundName( st.sounds[BlockDef::SND_STEP] ) == "minecraft:block.stone.step" );
		CHECK( w.Registry().SoundName( st.sounds[BlockDef::SND_BREAK] ) == "minecraft:block.stone.break" );
		CHECK( w.Registry().Get( w.Registry().ForState( ST_GLASS ) ).sounds[BlockDef::SND_STEP] == 0 );
	}
	ApplyFrame( w, FloorSectionPacket( 0, 0 ) );
	ApplyFrame( w, FloorSectionPacket( -1, 0 ) );
	CHECK( w.IsColumnLoaded( 0, 0 ) && w.IsColumnLoaded( -1, 0 ) && !w.IsColumnLoaded( 1, 0 ) );
	CHECK( w.GetDef( 3, 2, 3 ).name == "minecraft:stone" );
	CHECK( w.GetDef( -3, 4, 3 ).name == "minecraft:grass_block" );
	CHECK( w.Get( 3, 5, 3 ) == BlockRegistry::AIR );
	CHECK( w.TopSolidY( 5, 5 ) == 4 );
	CHECK( w.TopSolidY( 50, 5 ) == w.MinY() - 1 );

	std::vector<SectionKey> dirty;
	w.TakeDirty( dirty );
	CHECK( dirty.size() == 2 );

	Writer bs( S2C_BLOCK_SET );
	bs.i32( 0 ).i32( 4 ).i32( 0 ).i32( ST_AIR );
	ApplyFrame( w, bs.finish() );
	CHECK( w.Get( 0, 4, 0 ) == BlockRegistry::AIR );
	dirty.clear(); w.TakeDirty( dirty );
	CHECK( dirty.size() == 2 );   // own section + west neighbour (x == 0 edge); z==0 neighbour not loaded

	Writer un( S2C_COLUMN_UNLOAD );
	un.i32( -1 ).i32( 0 );
	ApplyFrame( w, un.finish() );
	CHECK( !w.IsColumnLoaded( -1, 0 ) );
	std::vector<SectionKey> removed; w.TakeRemoved( removed );
	CHECK( removed.size() == 24 );
	removed.clear(); w.TakeRemoved( removed, CH_PHYSICS );
	CHECK( removed.size() == 24 );   // every channel sees removals

	// States carry server shapes and flags
	CHECK( w.Registry().Get( w.Registry().ForState( ST_FENCE ) ).shape == SHAPE_BOXES );
	CHECK( w.Registry().Get( w.Registry().ForState( ST_FENCE ) ).coll[0].mx[1] == 1.5f );
	CHECK( w.Registry().Get( w.Registry().ForState( ST_GLASS ) ).shape == SHAPE_CUBE && !w.Registry().Get( w.Registry().ForState( ST_GLASS ) ).opaque );
	CHECK( w.Registry().Get( w.Registry().ForState( ST_STONE ) ).opaque );
	CHECK( w.Registry().Get( w.Registry().ForState( ST_LADDER ) ).climbable );
	CHECK( w.Registry().Get( w.Registry().ForState( ST_WATER ) ).shape == SHAPE_LIQUID );
	CHECK( std::fabs( w.Registry().Get( w.Registry().ForState( ST_WATER ) ).liquidHeight - 8.0 / 9.0 ) < 1e-6 );
	CHECK( w.Registry().Get( w.Registry().ForState( ST_STAIRS_WL ) ).waterlogged );
	CHECK( w.Registry().Get( w.Registry().ForState( 123456 ) ).name == "tf2mc:unknown" );

	// Light
	CHECK( w.SkyLight( 3, 2, 3 ) == 0 && w.SkyLight( 3, 7, 3 ) == 15 );
	CHECK( w.BlockLight( 3, 7, 3 ) == 0 );
	{ std::vector<SectionKey> drain; w.TakeDirty( drain, CH_PHYSICS ); w.TakeDirty( drain, CH_RENDER ); }
	Writer lp( S2C_LIGHT );
	lp.i32( 0 ).i32( 0 ).i32( 0 ).u8( 2 );
	for ( int i = 0; i < 2048; ++i ) lp.u8( 0x77 );
	ApplyFrame( w, lp.finish() );
	CHECK( w.BlockLight( 3, 7, 3 ) == 7 && w.SkyLight( 3, 7, 3 ) == 15 );   // no sky data -> 15
	dirty.clear(); w.TakeDirty( dirty, CH_PHYSICS );
	std::vector<SectionKey> rdirty; w.TakeDirty( rdirty, CH_RENDER );
	CHECK( rdirty.size() >= 1 && dirty.empty() );   // light only re-meshes

	// Heuristics
	BlockRegistry &reg = w.Registry();
	CHECK( reg.Get( reg.Intern( "minecraft:water" ) ).shape == SHAPE_LIQUID );
	CHECK( !reg.Get( reg.Intern( "minecraft:short_grass" ) ).collides );
	CHECK( reg.Get( reg.Intern( "minecraft:oak_slab" ) ).shape == SHAPE_BOX );
	CHECK( reg.Get( reg.Intern( "minecraft:oak_leaves" ) ).cullSame );
	CHECK( reg.Intern( "cave_air" ) == BlockRegistry::AIR );

	// Definitions file overrides heuristics and updates interned blocks
	uint16_t dirt = reg.Intern( "minecraft:dirt" );
	int n = reg.LoadDefinitions( "atlas 64 16\nblock minecraft:dirt cube 5 5 5 5 5 5 # comment\n"
								 "block minecraft:oak_slab box 7 7 8 8 8 8 box=0,0,0,16,8,16\n"
								 "block minecraft:grass_block cube 2 0 1 1 1 1 tint=grass tintmask=2\n" );
	CHECK( n == 3 );
	CHECK( reg.HasAtlas() && reg.TilesPerRow() == 64 );
	CHECK( reg.Get( dirt ).tiles[0] == 5 );
	CHECK( reg.Get( reg.Intern( "minecraft:grass_block" ) ).tint == TINT_GRASS );
}

static VoxelWorld MakeFloorWorld()
{
	VoxelWorld w;
	w.Reset( -4, 24 );
	ApplyFrame( w, StateDefsPacket() );
	for ( int cx = -1; cx <= 1; ++cx )
		for ( int cz = -1; cz <= 1; ++cz )
			ApplyFrame( w, FloorSectionPacket( cx, cz ) );
	return w;   // floor top surface at y = 5
}

static void TestCollision()
{
	VoxelWorld w = MakeFloorWorld();
	Vec3d half( 0.3, 0.9, 0.3 );

	// Falling onto floor: box centre at y=8, falls 5 blocks; bottom (7.1) reaches 5.0 after 2.1 => fraction 0.42
	SweepResult r = SweepBox( w, Vec3d( 0.5, 8, 0.5 ), Vec3d( 0, -5, 0 ), half );
	CHECK( r.hit && !r.startSolid );
	CHECK( std::fabs( r.fraction - 2.1 / 5.0 ) < 1e-9 );
	CHECK( r.normal.y == 1 && r.normal.x == 0 && r.normal.z == 0 );

	// Standing exactly on the floor, sliding sideways: no hit
	r = SweepBox( w, Vec3d( 0.5, 5.9, 0.5 ), Vec3d( 3, 0, 2 ), half );
	CHECK( !r.hit );

	// Standing on floor, moving down: blocked at 0
	r = SweepBox( w, Vec3d( 0.5, 5.9, 0.5 ), Vec3d( 0, -1, 0 ), half );
	CHECK( r.hit && !r.startSolid && r.fraction == 0.0 );

	// Moving up from the floor: free
	r = SweepBox( w, Vec3d( 0.5, 5.9, 0.5 ), Vec3d( 0, 1, 0 ), half );
	CHECK( !r.hit );

	// Embedded => startsolid
	r = SweepBox( w, Vec3d( 0.5, 5.5, 0.5 ), Vec3d( 0, 0, 0 ), half );
	CHECK( r.startSolid && r.allSolid );

	// Wall: place a pillar at x=3, walk into it along +x
	uint16_t stone = w.Registry().Intern( "minecraft:stone" );
	w.SetBlock( 3, 5, 0, stone ); w.SetBlock( 3, 6, 0, stone );
	r = SweepBox( w, Vec3d( 0.5, 5.9, 0.5 ), Vec3d( 4, 0, 0 ), half );
	CHECK( r.hit && r.normal.x == -1 );
	CHECK( std::fabs( r.fraction - ( 3.0 - 0.3 - 0.5 ) / 4.0 ) < 1e-9 );
	CHECK( r.block == BlockPos( 3, 5, 0 ) || r.block == BlockPos( 3, 6, 0 ) );

	// Long diagonal ray (like a hitscan bullet) hits floor
	r = SweepBox( w, Vec3d( -10, 30, -10 ), Vec3d( 20, -40, 20 ), Vec3d( 0, 0, 0 ) );
	CHECK( r.hit && r.normal.y == 1 );
	CHECK( std::fabs( r.fraction - 25.0 / 40.0 ) < 1e-9 );

	// Slab collision box (half height)
	uint16_t slab = w.Registry().Intern( "minecraft:oak_slab" );
	w.SetBlock( -5, 5, -5, slab );
	r = SweepBox( w, Vec3d( -4.5, 8, -4.5 ), Vec3d( 0, -5, 0 ), half );
	CHECK( r.hit && std::fabs( r.fraction - ( 8 - 0.9 - 5.5 ) / 5.0 ) < 1e-9 );

	// Plants don't collide
	uint16_t grass = w.Registry().Intern( "minecraft:short_grass" );
	w.SetBlock( 6, 5, 6, grass );
	r = SweepBox( w, Vec3d( 6.5, 5.9, 4.5 ), Vec3d( 0, 0, 3 ), half );
	CHECK( !r.hit );

	// Unloaded column is empty (falls through)
	r = SweepBox( w, Vec3d( 100.5, 8, 0.5 ), Vec3d( 0, -50, 0 ), half );
	CHECK( !r.hit );
}

static void TestPick()
{
	VoxelWorld w = MakeFloorWorld();
	PickResult p = PickBlock( w, Vec3d( 0.5, 6.62, 0.5 ), Vec3d( 0, -1, 0 ), 4.5 );
	CHECK( p.hit && p.block == BlockPos( 0, 4, 0 ) && p.face == FACE_UP );
	CHECK( std::fabs( p.distance - 1.62 ) < 1e-9 );

	p = PickBlock( w, Vec3d( 0.5, 6.62, 0.5 ), Vec3d( 1, -0.2, 0.3 ), 4.5 );
	CHECK( !p.hit );   // too far to reach the floor

	uint16_t stone = w.Registry().Intern( "minecraft:stone" );
	w.SetBlock( 2, 6, 0, stone );
	p = PickBlock( w, Vec3d( 0.5, 6.5, 0.5 ), Vec3d( 1, 0, 0 ), 4.5 );
	CHECK( p.hit && p.block == BlockPos( 2, 6, 0 ) && p.face == FACE_WEST );
	CHECK( Offset( p.block, p.face ) == BlockPos( 1, 6, 0 ) );

	// Negative coordinates
	w.SetBlock( -3, 6, -1, stone );
	p = PickBlock( w, Vec3d( -0.5, 6.5, -0.5 ), Vec3d( -1, 0, 0 ), 4.5 );
	CHECK( p.hit && p.block == BlockPos( -3, 6, -1 ) && p.face == FACE_EAST );
}

static double Cross( const MeshVertex &a, const MeshVertex &b, const MeshVertex &c, int comp )
{
	double u[3] = { b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2] };
	double v[3] = { c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2] };
	double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
	return n[comp];
}

static void TestMesher()
{
	VoxelWorld w;
	w.Reset( -4, 24 );
	// One isolated stone block in an otherwise empty, loaded section
	ApplyFrame( w, StateDefsPacket() );
	Writer pk( S2C_SECTION );
	pk.i32( 0 ).i32( 0 ).i32( 0 ).u16( 2 ).i32( ST_AIR ).i32( ST_STONE );
	for ( int i = 0; i < 4096; ++i ) pk.u8( i == Section::Index( 5, 5, 5 ) ? 1 : 0 );
	pk.u8( 0 );
	ApplyFrame( w, pk.finish() );

	Coords c;
	MesherOptions opt; opt.textured = false;
	SectionMesh m;
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	CHECK( m.opaque.size() == 24 && m.translucent.empty() );

	// Top face must be at source z = (6 - oy) * scale and wound clockwise seen from above (normal -Z by cross product)
	bool foundTop = false;
	for ( size_t q = 0; q < m.opaque.size(); q += 4 )
	{
		const MeshVertex *v = &m.opaque[q];
		if ( NEAR( v[0].pos[2], ( 6 - 128 ) * 64.0 ) && NEAR( v[1].pos[2], v[0].pos[2] ) && NEAR( v[2].pos[2], v[0].pos[2] ) )
		{
			foundTop = true;
			CHECK( Cross( v[0], v[1], v[2], 2 ) < 0 );   // clockwise from outside
		}
	}
	CHECK( foundTop );

	opt.clockwise = false;
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	for ( size_t q = 0; q < m.opaque.size(); q += 4 )
	{
		const MeshVertex *v = &m.opaque[q];
		if ( NEAR( v[0].pos[2], ( 6 - 128 ) * 64.0 ) && NEAR( v[1].pos[2], v[0].pos[2] ) && NEAR( v[2].pos[2], v[0].pos[2] ) )
			CHECK( Cross( v[0], v[1], v[2], 2 ) > 0 );
	}

	// Two adjacent blocks share a hidden face
	uint16_t stone = w.Registry().Intern( "minecraft:stone" );
	w.SetBlock( 6, 5, 5, stone );
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	CHECK( m.opaque.size() == 10 * 4 );

	// Water goes to translucent pass; glass culls glass
	w.SetBlock( 0, 0, 0, w.Registry().Intern( "minecraft:water" ) );
	uint16_t glass = w.Registry().Intern( "minecraft:glass" );
	w.SetBlock( 10, 10, 10, glass ); w.SetBlock( 11, 10, 10, glass );
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	CHECK( m.translucent.size() == 6 * 4 );   // isolated water: all 6 faces, top lowered
	CHECK( m.opaque.size() == ( 10 + 10 ) * 4 );  // 2 stones (10 faces) + 2 glass (10 faces)

	// Collision: flat 16x16 floor merges into one quad per exposed face direction
	VoxelWorld f = MakeFloorWorld();
	std::vector<Vec3f> tris;
	BuildSectionCollision( f, c, 0, 0, 0, tris );
	// top: 1 quad; bottom: 1 quad (section below unloaded -> air); 4 sides facing neighbours are covered by loaded neighbours
	CHECK( tris.size() == 2 * 2 * 3 );
	BuildSectionCollision( f, c, 1, 0, 1, tris );   // corner column: two outer sides exposed
	CHECK( tris.size() == 4 * 2 * 3 );
}

static void TestV2()
{
	VoxelWorld w = MakeFloorWorld();
	BlockRegistry &R = w.Registry();
	Vec3d half( 0.3, 0.9, 0.3 );

	// Fence collision is 1.5 blocks tall: can't be walked over or stepped onto at 1.2
	w.SetBlock( 3, 5, 0, R.ForState( ST_FENCE ) );
	SweepResult r = SweepBox( w, Vec3d( 0.5, 5.9 + 1.2, 0.5 ), Vec3d( 4, 0, 0 ), half );
	CHECK( r.hit && r.normal.x == -1 );
	// ...but its outline (render box) is only 1 tall and the post is thin: walking past at z=2 is free
	r = SweepBox( w, Vec3d( 0.5, 5.9, 2.5 ), Vec3d( 6, 0, 0 ), half );
	CHECK( !r.hit );
	// The fence's east arm blocks movement along z at x = 3.8
	r = SweepBox( w, Vec3d( 3.85, 5.9, 2.5 ), Vec3d( 0, 0, -3 ), Vec3d( 0.1, 0.9, 0.1 ) );
	CHECK( r.hit && r.normal.z == 1 );

	// Picking uses the outline boxes: a ray over the post top (y=6.2) misses, at 5.5 hits
	PickResult p = PickBlock( w, Vec3d( 0.5, 6.2, 0.5 ), Vec3d( 1, 0, 0 ), 4.5 );
	CHECK( !p.hit );
	p = PickBlock( w, Vec3d( 0.5, 5.5, 0.5 ), Vec3d( 1, 0, 0 ), 4.5 );
	CHECK( p.hit && p.block == BlockPos( 3, 5, 0 ) && p.face == FACE_WEST );
	CHECK( std::fabs( p.point.x - 3.375 ) < 1e-9 );

	// Water & climbing
	w.SetBlock( -4, 5, -4, R.ForState( ST_WATER ) );
	CHECK( w.IsWaterAt( -3.5, 5.5, -3.5 ) && !w.IsWaterAt( -3.5, 5.95, -3.5 ) && !w.IsWaterAt( -2.5, 5.5, -3.5 ) );
	w.SetBlock( 4, 5, 4, R.ForState( ST_STAIRS_WL ) );
	CHECK( w.IsWaterAt( 4.5, 5.2, 4.5 ) );
	w.SetBlock( 6, 5, 6, R.ForState( ST_LADDER ) );
	CHECK( BoxTouchesClimbable( w, Vec3d( 6.3, 5.0, 6.6 ), Vec3d( 6.9, 6.8, 7.2 ) ) );
	CHECK( !BoxTouchesClimbable( w, Vec3d( 7.1, 5.0, 6.6 ), Vec3d( 7.9, 6.8, 7.2 ) ) );

	// Stairs: two collision boxes -> step shape
	r = SweepBox( w, Vec3d( 4.25, 8, 4.5 ), Vec3d( 0, -5, 0 ), Vec3d( 0.2, 0.9, 0.2 ) );
	CHECK( r.hit && std::fabs( r.fraction - ( 8 - 0.9 - 5.5 ) / 5.0 ) < 1e-9 );
	r = SweepBox( w, Vec3d( 4.75, 8, 4.5 ), Vec3d( 0, -5, 0 ), Vec3d( 0.2, 0.9, 0.2 ) );
	CHECK( r.hit && std::fabs( r.fraction - ( 8 - 0.9 - 6.0 ) / 5.0 ) < 1e-9 );

	// Lit mesh: top faces of the floor get full sky light, waterlogged stairs add a translucent water top
	Coords c;
	MesherOptions opt; opt.textured = false;
	SectionMesh m;
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	bool topLit = false;
	for ( size_t i = 0; i < m.opaque.size(); ++i )
		if ( NEAR( m.opaque[i].pos[2], ( 5 - 128 ) * 64.0 ) && m.opaque[i].sky == 15 ) topLit = true;
	CHECK( topLit );
	bool waterTop = false;
	for ( size_t i = 0; i + 3 < m.translucent.size(); i += 4 )
		if ( std::fabs( m.translucent[i].pos[2] - ( 5 + 8.0 / 9.0 - 128 ) * 64.0 ) < 0.01 ) waterTop = true;
	CHECK( waterTop );
	CHECK( LightCurve( 15 ) > 0.99f && LightCurve( 0 ) < 0.1f && LightCurve( 8 ) > LightCurve( 7 ) );

	// Floating origin shift
	Coords a; a.ox = 100; a.oz = -40; a.oy = 128;
	Coords b2 = a; b2.ox = 116; b2.oz = -56;
	Vec3d pa = a.ToSource( Vec3d( 120.5, 70, -50 ) ), pb = b2.ToSource( Vec3d( 120.5, 70, -50 ) );
	Vec3d sh = b2.ShiftFrom( a );
	CHECK( NEAR( pa.x + sh.x, pb.x ) && NEAR( pa.y + sh.y, pb.y ) && NEAR( pa.z + sh.z, pb.z ) );

	// Background meshing: a section meshed from a 3x3x3 neighbourhood copy (with a registry
	// snapshot) must be identical to meshing it from the live world.
	{
		BlockRegistry snap( w.Registry() );
		VoxelWorld copy;
		copy.CopyNeighborhood( w, 0, 0, 0, &snap );
		MesherOptions o2; o2.textured = false;
		SectionMesh live, fromCopy;
		BuildSectionMesh( w, c, 0, 0, 0, o2, live );
		BuildSectionMesh( copy, c, 0, 0, 0, o2, fromCopy );
		CHECK( live.QuadCount() > 0 );
		CHECK( live.opaque.size() == fromCopy.opaque.size() && live.translucent.size() == fromCopy.translucent.size() );
		bool same = live.opaque.size() == fromCopy.opaque.size() &&
					( live.opaque.empty() || memcmp( live.opaque.data(), fromCopy.opaque.data(), live.opaque.size() * sizeof( MeshVertex ) ) == 0 );
		CHECK( same );
		CHECK( &static_cast<const VoxelWorld &>( copy ).Registry() == &snap );
		uint32_t gen = w.Registry().Generation();
		w.Registry().Intern( "minecraft:test_generation_bump" );
		CHECK( w.Registry().Generation() != gen );
	}
}


// Baked Minecraft models (tests/test_assets.py writes build/assets/scripts/mc_models.txt first)
static std::string ReadText( const char *path )
{
	std::ifstream f( path );
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static void TestModels()
{
	std::string models = ReadText( "build/assets/scripts/mc_models.txt" );
	std::string blocks = ReadText( "build/assets/scripts/mc_blocks.txt" );
	if ( models.empty() ) { printf( "(no baked models: asset tool test skipped, model checks skipped)\n" ); return; }

	VoxelWorld w;
	w.Reset( 0, 2 );
	BlockRegistry &reg = w.Registry();
	CHECK( reg.LoadDefinitions( blocks ) > 0 );
	CHECK( reg.LoadModels( models ) > 0 );
	std::vector<Box> unit( 1, Box::Unit() ), none;
	Box sg = { { 0.125f, 0, 0.125f }, { 0.875f, 0.75f, 0.875f } };
	Box slab = { { 0, 0, 0 }, { 1, 0.5f, 1 } }, step = { { 0, 0.5f, 0 }, { 1, 1, 0.5f } };
	std::vector<Box> stairShape = { slab, step };

	uint16_t stone = reg.DefineState( 1, "minecraft:stone", "", STATE_COLLIDES | STATE_OPAQUE_CUBE, 0, unit, unit );
	uint16_t seagrass = reg.DefineState( 2, "minecraft:seagrass", "", STATE_WATERLOGGED, 0, none, std::vector<Box>( 1, sg ) );
	uint16_t stairsN = reg.DefineState( 3, "minecraft:oak_stairs", "facing=north,half=bottom,shape=straight,waterlogged=false", STATE_COLLIDES, 0, stairShape, stairShape );
	uint16_t fence = reg.DefineState( 4, "minecraft:oak_fence", "east=false,north=true,south=false,waterlogged=false,west=true", STATE_COLLIDES, 0, unit, unit );
	uint16_t litter = reg.DefineState( 5, "minecraft:leaf_litter", "facing=north,segment_amount=1", 0, 0, none, std::vector<Box>( 1, Box{ { 0, 0, 0 }, { 1, 1 / 16.f, 1 } } ) );
	uint16_t chest = reg.DefineState( 6, "minecraft:chest", "facing=north,type=single,waterlogged=false", STATE_COLLIDES, 0, unit, std::vector<Box>( 1, Box{ { 1 / 16.f, 0, 1 / 16.f }, { 15 / 16.f, 14 / 16.f, 15 / 16.f } } ) );

	CHECK( reg.Get( stone ).shape == SHAPE_MODEL && reg.Get( stone ).opaque );
	CHECK( reg.Get( seagrass ).shape == SHAPE_MODEL && reg.Get( seagrass ).models.size() == 1 && !reg.Get( seagrass ).opaque && reg.Get( seagrass ).waterlogged );
	CHECK( reg.Get( stairsN ).shape == SHAPE_MODEL && reg.Get( stairsN ).models.size() == 1 && !reg.Get( stairsN ).opaque );
	CHECK( reg.Get( fence ).models.size() == 3 );           // post + north side + (east|west) side
	CHECK( reg.Get( litter ).shape == SHAPE_MODEL && reg.Get( litter ).tint == TINT_DRY_FOLIAGE );
	CHECK( reg.Get( chest ).shape == SHAPE_BOXES );          // no elements: simple-shape fallback
	CHECK( reg.Get( stairsN ).boxes.size() == 2 );           // picking keeps the real outline
	// variants: first match wins, unknown state -> no model
	CHECK( reg.ModelsFor( "minecraft:oak_stairs", "facing=south,half=top,shape=outer_left,waterlogged=false" ).empty() );
	CHECK( reg.ModelsFor( "minecraft:oak_fence", "east=false,north=false,south=false,waterlogged=false,west=false" ).size() == 1 );

	{
		// load column (0,0) with an empty section so edits apply
		std::vector<uint16_t> pal( 1, BlockRegistry::AIR );
		static uint16_t idx[4096] = {};
		w.SetSection( 0, 0, 0, pal, idx );
	}
	Coords c;
	MesherOptions opt;   // textured
	auto quads = [&]( std::vector<MeshVertex> &v ) { return (int)v.size() / 4; };

	// 1. two stones side by side: the touching faces are culled -> 10 quads
	w.SetBlock( 2, 2, 2, stone );
	w.SetBlock( 3, 2, 2, stone );
	SectionMesh m;
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	CHECK( quads( m.opaque ) == 10 );
	w.SetBlock( 3, 2, 2, BlockRegistry::AIR );

	// 2. seagrass: 4 double-sided planes, plus a water surface (it is waterlogged)
	w.SetBlock( 8, 2, 8, seagrass );
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	CHECK( quads( m.opaque ) == 6 + 4 );
	CHECK( quads( m.translucent ) == 1 );
	w.SetBlock( 8, 2, 8, BlockRegistry::AIR );

	// 3. north stairs on top of stone: the slab's bottom is culled by the stone; the step top is on the north half
	w.SetBlock( 2, 3, 2, stairsN );
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	bool stepNorth = false;
	for ( size_t i = 0; i + 3 < m.opaque.size(); i += 4 )
	{
		Vec3d a = c.ToMC( Vec3d( m.opaque[i].pos[0], m.opaque[i].pos[1], m.opaque[i].pos[2] ) );
		Vec3d b = c.ToMC( Vec3d( m.opaque[i + 2].pos[0], m.opaque[i + 2].pos[1], m.opaque[i + 2].pos[2] ) );
		if ( std::fabs( a.y - 4.0 ) < 1e-3 && std::fabs( b.y - 4.0 ) < 1e-3 && std::max( a.z, b.z ) <= 2.5 + 1e-3 && std::min( a.z, b.z ) >= 2.0 - 1e-3 ) stepNorth = true;
	}
	CHECK( stepNorth );
	CHECK( quads( m.opaque ) == 6 + 10 );   // stone (all 6: stairs are not a full block) + stairs (11 faces, bottom culled by the stone)
	w.SetBlock( 2, 3, 2, BlockRegistry::AIR );

	// 4. UVs land inside the face's own tile (no half-texel squeeze)
	bool inTile = true;
	BuildSectionMesh( w, c, 0, 0, 0, opt, m );
	int tpr = reg.TilesPerRow(), rows = reg.TileRows();
	for ( size_t i = 0; i < m.opaque.size(); ++i )
	{
		float u = m.opaque[i].uv[0] * tpr, v = m.opaque[i].uv[1] * rows;
		float fu = u - std::floor( u + 1e-4f ), fv = v - std::floor( v + 1e-4f );
		if ( !( fu < 0.01f || fu > 0.99f ) || !( fv < 0.01f || fv > 0.99f ) ) inTile = false;   // cube corners sit on tile corners
	}
	CHECK( inTile );
}

int main()
{
	TestProtocol();
	TestCoords();
	TestWorld();
	TestCollision();
	TestPick();
	TestMesher();
	TestV2();
	TestModels();
	printf( "%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
