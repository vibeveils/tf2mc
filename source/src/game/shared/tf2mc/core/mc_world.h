//========= TF2MC bridge - engine independent core =========//
// Client-side mirror of the Minecraft world: block state registry (with the real
// collision/outline shapes sent by the server), chunk sections with sky/block light,
// coordinate mapping between Minecraft (Y-up) and Source (Z-up) space incl. a
// floating origin.
//==========================================================//
#pragma once

#include "mc_protocol.h"

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mc
{
	struct Vec3d { double x = 0, y = 0, z = 0; Vec3d() {} Vec3d( double a, double b, double c ) : x( a ), y( b ), z( c ) {} };
	struct Vec3f { float x = 0, y = 0, z = 0; Vec3f() {} Vec3f( float a, float b, float c ) : x( a ), y( b ), z( c ) {} };
	struct BlockPos { int x = 0, y = 0, z = 0; BlockPos() {} BlockPos( int a, int b, int c ) : x( a ), y( b ), z( c ) {}
		bool operator==( const BlockPos &o ) const { return x == o.x && y == o.y && z == o.z; }
		bool operator!=( const BlockPos &o ) const { return !( *this == o ); } };

	inline int FloorDiv16( int v ) { return v >> 4; }   // arithmetic shift == floor division for ints
	inline int FloorI( double v ) { return (int)std::floor( v ); }

	//------------------------------------------------------------------
	// Minecraft <-> Source coordinate mapping.
	//   Source.x =  (mc.x - origin.x) * scale
	//   Source.y = -(mc.z - origin.z) * scale
	//   Source.z =  (mc.y - origin.y) * scale
	// A proper rotation (det = +1). The origin floats: the server re-centres it
	// on the players so the whole Minecraft world fits inside Source's +-16384.
	// Yaw: source = -mc - 90 ; Pitch: identical sign convention (down = +).
	//------------------------------------------------------------------
	struct Coords
	{
		double scale = 64.0;
		double ox = 0.0, oy = 128.0, oz = 0.0;

		Vec3d ToSource( const Vec3d &m ) const { return Vec3d( ( m.x - ox ) * scale, -( m.z - oz ) * scale, ( m.y - oy ) * scale ); }
		Vec3d ToMC( const Vec3d &s ) const { return Vec3d( s.x / scale + ox, s.z / scale + oy, -s.y / scale + oz ); }
		Vec3d DirToSource( const Vec3d &m ) const { return Vec3d( m.x, -m.z, m.y ); }
		Vec3d DirToMC( const Vec3d &s ) const { return Vec3d( s.x, s.z, -s.y ); }
		Vec3d ExtentToMC( const Vec3d &s ) const { return Vec3d( s.x / scale, s.z / scale, s.y / scale ); }
		Vec3d DeltaToMC( const Vec3d &s ) const { return Vec3d( s.x / scale, s.z / scale, -s.y / scale ); }
		Vec3d DeltaToSource( const Vec3d &m ) const { return Vec3d( m.x * scale, -m.z * scale, m.y * scale ); }
		// Source-space translation that maps positions computed with `from` onto this mapping.
		Vec3d ShiftFrom( const Coords &from ) const { return Vec3d( ( from.ox - ox ) * scale, -( from.oz - oz ) * scale, ( from.oy - oy ) * scale ); }
		bool SameScale( const Coords &o ) const { return scale == o.scale; }
		bool operator==( const Coords &o ) const { return scale == o.scale && ox == o.ox && oy == o.oy && oz == o.oz; }
		bool operator!=( const Coords &o ) const { return !( *this == o ); }
		static float YawToSource( float mcYaw ) { return -mcYaw - 90.0f; }
		static float YawToMC( float srcYaw ) { float y = -srcYaw - 90.0f; while ( y < -180.f ) y += 360.f; while ( y >= 180.f ) y -= 360.f; return y; }
	};

	//------------------------------------------------------------------
	// Block definitions
	//------------------------------------------------------------------
	enum Shape : uint8_t { SHAPE_AIR = 0, SHAPE_CUBE, SHAPE_BOXES, SHAPE_CROSS, SHAPE_LIQUID };
	static const Shape SHAPE_BOX = SHAPE_BOXES;   // v1 name
	enum Tint : uint8_t { TINT_NONE = 0, TINT_GRASS, TINT_FOLIAGE, TINT_WATER, TINT_SPRUCE, TINT_BIRCH };

	struct Box
	{
		float mn[3], mx[3];   // block-local, MC axes, in blocks (fences reach y = 1.5)
		static Box Unit() { Box b = { { 0, 0, 0 }, { 1, 1, 1 } }; return b; }
		bool IsFull() const { return mn[0] <= 0 && mn[1] <= 0 && mn[2] <= 0 && mx[0] >= 1 && mx[1] >= 1 && mx[2] >= 1; }
	};

	struct BlockDef
	{
		std::string name;
		std::string props;           // "facing=north,half=bottom" (state blocks only)
		Shape shape = SHAPE_CUBE;
		bool collides = true;
		bool opaque = true;          // full opaque cube: hides neighbouring faces, blocks light/AO
		bool cullSame = false;       // glass/leaves: hide faces between identical blocks
		bool translucent = false;    // goes into translucent render pass
		bool emissive = false;
		bool climbable = false;
		bool waterlogged = false;
		bool lava = false;
		uint8_t lightEmission = 0;
		float liquidHeight = 14.f / 16.f;
		int16_t tiles[6] = { -1, -1, -1, -1, -1, -1 };  // atlas tile per Face; -1 = flat colour
		uint8_t tintMask = 0;        // bit per Face (0 = all faces when tinted)
		Tint tint = TINT_NONE;
		uint32_t color = 0xFF808080; // ABGR fallback colour when untextured
		std::vector<Box> coll;       // collision boxes (empty = passable)
		std::vector<Box> boxes;      // render/pick boxes for SHAPE_BOXES (and pick for crosses)

		void SetSingleBox( float x0, float y0, float z0, float x1, float y1, float z1 )
		{
			Box b = { { x0, y0, z0 }, { x1, y1, z1 } };
			boxes.assign( 1, b );
			if ( collides ) coll.assign( 1, b );
			shape = b.IsFull() ? SHAPE_CUBE : SHAPE_BOXES;
		}
	};

	class BlockRegistry
	{
	public:
		BlockRegistry();

		// Loads scripts/mc_blocks.txt style definitions (generated by tools/mc_assets.py).
		int LoadDefinitions( const std::string &text );
		void SetAtlasLayout( int tilesPerRow, int rows ) { m_tilesPerRow = tilesPerRow; m_rows = rows; }
		int TilesPerRow() const { return m_tilesPerRow; }
		int TileRows() const { return m_rows; }
		bool HasAtlas() const { return m_tilesPerRow > 0 && m_rows > 0; }

		// Name based definition (heuristics + definitions file)
		uint16_t Intern( const std::string &name );
		// Block *state* sent by the server (protocol v2): name-based look + real shapes/flags.
		uint16_t DefineState( int32_t stateId, const std::string &name, const std::string &props, uint8_t flags, uint8_t light,
							  const std::vector<Box> &coll, const std::vector<Box> &shape );
		uint16_t ForState( int32_t stateId );   // unknown states map to a grey placeholder cube

		const BlockDef &Get( uint16_t id ) const { return id < m_defs.size() ? m_defs[id] : m_defs[0]; }
		size_t Count() const { return m_defs.size(); }
		int16_t WaterTile() const { auto it = m_loaded.find( "minecraft:water" ); return it != m_loaded.end() ? it->second.tiles[FACE_UP] : -1; }

		static const uint16_t AIR = 0;

	private:
		BlockDef BaseDef( const std::string &name ) const;
		void ApplyHeuristics( BlockDef &d ) const;
		std::vector<BlockDef> m_defs;
		std::unordered_map<std::string, uint16_t> m_ids;
		std::unordered_map<int32_t, uint16_t> m_states;
		std::unordered_map<std::string, BlockDef> m_loaded;   // from definitions file
		uint16_t m_unknown = 0;
		int m_tilesPerRow = 0, m_rows = 0;
	};

	//------------------------------------------------------------------
	// Voxel storage
	//------------------------------------------------------------------
	struct Section
	{
		uint16_t blocks[4096];   // index = (y * 16 + z) * 16 + x
		uint8_t sky[2048];       // nibbles, same index (Minecraft DataLayer layout)
		uint8_t blk[2048];
		int nonAir = 0;
		bool hasSky = false, hasBlk = false;
		Section() { memset( blocks, 0, sizeof( blocks ) ); memset( sky, 0xFF, sizeof( sky ) ); memset( blk, 0, sizeof( blk ) ); }
		static int Index( int x, int y, int z ) { return ( y * 16 + z ) * 16 + x; }
		static uint8_t Nibble( const uint8_t *a, int i ) { return ( a[i >> 1] >> ( ( i & 1 ) * 4 ) ) & 15; }
	};

	struct SectionKey
	{
		int cx, sy, cz;
		bool operator==( const SectionKey &o ) const { return cx == o.cx && sy == o.sy && cz == o.cz; }
	};
	struct SectionKeyHash
	{
		size_t operator()( const SectionKey &k ) const
		{
			uint64_t h = (uint64_t)(uint32_t)k.cx * 0x9E3779B97F4A7C15ull;
			h ^= (uint64_t)(uint32_t)k.cz * 0xC2B2AE3D27D4EB4Full + ( h << 6 ) + ( h >> 2 );
			h ^= (uint64_t)(uint32_t)k.sy * 0x165667B19E3779F9ull + ( h << 6 ) + ( h >> 2 );
			return (size_t)h;
		}
	};

	// Change channels: each consumer drains its own dirty list.
	enum Channel { CH_RENDER = 0, CH_PHYSICS = 1, CH_COUNT = 2 };

	class VoxelWorld
	{
	public:
		VoxelWorld();

		void Reset( int minSectionY, int sectionCount );
		int MinY() const { return m_minSection * 16; }
		int MaxY() const { return ( m_minSection + m_sectionCount ) * 16; }
		int MinSection() const { return m_minSection; }
		int SectionCount() const { return m_sectionCount; }

		BlockRegistry &Registry() { return m_registry; }
		const BlockRegistry &Registry() const { return m_registry; }

		uint16_t Get( int x, int y, int z ) const;
		const BlockDef &GetDef( int x, int y, int z ) const { return m_registry.Get( Get( x, y, z ) ); }
		bool IsColumnLoaded( int cx, int cz ) const;
		bool IsLoadedAt( double x, double z ) const { return IsColumnLoaded( FloorDiv16( FloorI( x ) ), FloorDiv16( FloorI( z ) ) ); }
		const Section *GetSection( int cx, int sy, int cz ) const;

		// Light (0..15). Missing data: sky 15 above loaded terrain, block 0.
		int SkyLight( int x, int y, int z ) const;
		int BlockLight( int x, int y, int z ) const;
		bool IsWaterAt( double x, double y, double z ) const;   // MC space point, incl. waterlogged blocks
		bool IsLavaAt( double x, double y, double z ) const;

		void SetBlock( int x, int y, int z, uint16_t id );
		void SetSection( int cx, int sy, int cz, const std::vector<uint16_t> &palette, const uint16_t *indices );
		void SetLight( int cx, int sy, int cz, const uint8_t *sky, const uint8_t *blk );
		void UnloadColumn( int cx, int cz );

		// Packet decoding (protocol v2)
		bool ApplyStateDefsPacket( Reader &r );
		bool ApplySectionPacket( Reader &r );
		bool ApplyBlockSetPacket( Reader &r, BlockPos *outPos = nullptr, uint16_t *outOld = nullptr, uint16_t *outNew = nullptr );
		bool ApplyLightPacket( Reader &r );
		bool ApplyUnloadPacket( Reader &r );

		void TakeDirty( std::vector<SectionKey> &out, int channel = CH_RENDER );
		void TakeRemoved( std::vector<SectionKey> &out, int channel = CH_RENDER );
		size_t ColumnCount() const { return m_columns.size(); }

		// Highest colliding block at x,z (or MinY-1 if none / unloaded)
		int TopSolidY( int x, int z ) const;

	private:
		struct Column { std::vector<std::unique_ptr<Section>> sections; };
		static int64_t ColKey( int cx, int cz ) { return ( (int64_t)cx << 32 ) ^ (int64_t)(uint32_t)cz; }
		void MarkDirty( int cx, int sy, int cz, bool force = false, unsigned channels = 3 );
		void MarkRemoved( const SectionKey &k );
		Section *EnsureSection( int cx, int sy, int cz );
		bool ReadLight( Reader &r, uint8_t *sky, uint8_t *blk, bool &hasSky, bool &hasBlk );

		BlockRegistry m_registry;
		int m_minSection = -4;
		int m_sectionCount = 24;
		std::unordered_map<int64_t, Column> m_columns;
		std::unordered_set<SectionKey, SectionKeyHash> m_dirty[CH_COUNT];
		std::unordered_set<SectionKey, SectionKeyHash> m_removed[CH_COUNT];
	};
}
