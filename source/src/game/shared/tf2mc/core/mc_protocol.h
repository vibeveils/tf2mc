//========= TF2MC bridge - engine independent core =========//
// Wire protocol shared with the Minecraft side (see docs/PROTOCOL.md).
// Frame: u32 length (LE, counts type byte + payload), u8 type, payload.
// All integers little endian, doubles/floats IEEE754 LE, strings u16 len + UTF-8.
//==========================================================//
#pragma once

#include "mc_platform.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mc
{
	static const uint16_t PROTOCOL_VERSION = 3;   // v3: block sound types in STATE_DEFS, S2C_SOUND
	static const uint32_t MAX_FRAME = 4u * 1024u * 1024u;

	enum PacketId : uint8_t
	{
		// Minecraft -> Source
		S2C_HELLO_ACK       = 0x01,
		S2C_SECTION         = 0x02,
		S2C_COLUMN_UNLOAD   = 0x03,
		S2C_BLOCK_SET       = 0x04,
		S2C_ENTITY_SPAWN    = 0x05,
		S2C_ENTITY_UPDATE   = 0x06,
		S2C_ENTITY_REMOVE   = 0x07,
		S2C_PLAYER_HURT     = 0x08,
		S2C_INVENTORY       = 0x09,
		S2C_TIME            = 0x0A,
		S2C_DIG_PROGRESS    = 0x0B,
		S2C_CHAT            = 0x0C,
		S2C_STATE_DEFS      = 0x0D,
		S2C_LIGHT           = 0x0E,
		S2C_PLAYER_STATUS   = 0x0F,
		S2C_CONTAINER       = 0x10,
		S2C_CONTAINER_CLOSE = 0x11,
		S2C_SOUND           = 0x12,   // str event, f64 x y z (MC), f32 volume, f32 pitch, u8 category, i32 entity (-1)

		// Source -> Minecraft
		C2S_HELLO           = 0x40,
		C2S_PLAYER_JOIN     = 0x41,
		C2S_PLAYER_LEAVE    = 0x42,
		C2S_PLAYER_MOVE     = 0x43,
		C2S_DIG             = 0x44,
		C2S_USE_ON_BLOCK    = 0x45,
		C2S_USE_ITEM        = 0x46,
		C2S_ATTACK          = 0x47,
		C2S_SELECT_SLOT     = 0x48,
		C2S_SET_MODE        = 0x49,
		C2S_SUBSCRIBE_SLOT  = 0x4A,
		C2S_CHAT            = 0x4B,
		C2S_CONTAINER_CLICK = 0x4C,
		C2S_CONTAINER_CLOSE = 0x4D,
		C2S_OPEN_INVENTORY  = 0x4E,
		C2S_RELEASE_USE     = 0x4F,
		C2S_DROP            = 0x50,
		C2S_EXPLOSION       = 0x51,
		C2S_INTERACT_ENTITY = 0x52,
	};

	enum Role : uint8_t { ROLE_OBSERVER = 0, ROLE_AUTHORITY = 1 };
	enum PlayerMode : uint8_t { MODE_TF_CLASS = 0, MODE_STEVE = 1 };
	enum DigAction : uint8_t { DIG_CONTINUE = 0, DIG_ABORT = 1 };
	enum AttackMode : uint8_t { ATTACK_TF_DAMAGE = 0, ATTACK_MC_MELEE = 1 };
	enum DamageKind : uint8_t { DMG_KIND_GENERIC = 0, DMG_KIND_MELEE = 1, DMG_KIND_PROJECTILE = 2, DMG_KIND_EXPLOSION = 3, DMG_KIND_FIRE = 4 };
	// Minecraft Direction ordinal order
	enum Face : uint8_t { FACE_DOWN = 0, FACE_UP = 1, FACE_NORTH = 2, FACE_SOUTH = 3, FACE_WEST = 4, FACE_EAST = 5 };

	// S2C_STATE_DEFS flags
	enum StateFlags : uint8_t { STATE_COLLIDES = 1, STATE_OPAQUE_CUBE = 2, STATE_LIQUID = 4, STATE_CLIMBABLE = 8, STATE_WATERLOGGED = 16, STATE_LAVA = 32 };
	// Minecraft ClickType ordinals
	enum ClickType : uint8_t { CLICK_PICKUP = 0, CLICK_QUICK_MOVE = 1, CLICK_SWAP = 2, CLICK_CLONE = 3, CLICK_THROW = 4, CLICK_QUICK_CRAFT = 5, CLICK_PICKUP_ALL = 6 };

	enum EntityFlags : uint8_t { ENT_LIVING = 1, ENT_HOSTILE = 2, ENT_PLAYER = 4, ENT_ITEM = 8, ENT_PROJECTILE = 16 };

	class Writer
	{
	public:
		explicit Writer( uint8_t type ) { m_buf.resize( 4 ); u8( type ); }
		Writer &u8( uint8_t v ) { m_buf.push_back( v ); return *this; }
		Writer &i8( int8_t v ) { return u8( (uint8_t)v ); }
		Writer &u16( uint16_t v ) { raw( &v, 2 ); return *this; }
		Writer &i32( int32_t v ) { raw( &v, 4 ); return *this; }
		Writer &i64( int64_t v ) { raw( &v, 8 ); return *this; }
		Writer &f32( float v ) { raw( &v, 4 ); return *this; }
		Writer &f64( double v ) { raw( &v, 8 ); return *this; }
		Writer &str( const std::string &s )
		{
			size_t n = s.size() > 65535 ? 65535 : s.size();
			u16( (uint16_t)n );
			m_buf.insert( m_buf.end(), s.begin(), s.begin() + n );
			return *this;
		}
		// Finalise length prefix and return the frame.
		std::vector<uint8_t> &finish()
		{
			uint32_t len = (uint32_t)( m_buf.size() - 4 );
			memcpy( m_buf.data(), &len, 4 );   // all supported targets are little endian
			return m_buf;
		}
	private:
		void raw( const void *p, size_t n ) { const uint8_t *b = (const uint8_t *)p; m_buf.insert( m_buf.end(), b, b + n ); }
		std::vector<uint8_t> m_buf;
	};

	class Reader
	{
	public:
		Reader( const uint8_t *data, size_t len ) : m_p( data ), m_end( data + len ) {}
		bool ok() const { return !m_bad; }
		size_t remaining() const { return (size_t)( m_end - m_p ); }
		uint8_t  u8()  { uint8_t v = 0; raw( &v, 1 ); return v; }
		int8_t   i8()  { return (int8_t)u8(); }
		uint16_t u16() { uint16_t v = 0; raw( &v, 2 ); return v; }
		int32_t  i32() { int32_t v = 0; raw( &v, 4 ); return v; }
		int64_t  i64() { int64_t v = 0; raw( &v, 8 ); return v; }
		float    f32() { float v = 0; raw( &v, 4 ); return v; }
		double   f64() { double v = 0; raw( &v, 8 ); return v; }
		std::string str()
		{
			uint16_t n = u16();
			if ( m_bad || remaining() < n ) { m_bad = true; return std::string(); }
			std::string s( (const char *)m_p, n );
			m_p += n;
			return s;
		}
		const uint8_t *bytes( size_t n )
		{
			if ( m_bad || remaining() < n ) { m_bad = true; return nullptr; }
			const uint8_t *p = m_p; m_p += n; return p;
		}
	private:
		void raw( void *out, size_t n )
		{
			if ( m_bad || remaining() < n ) { m_bad = true; return; }
			memcpy( out, m_p, n ); m_p += n;
		}
		const uint8_t *m_p;
		const uint8_t *m_end;
		bool m_bad = false;
	};

	struct Packet
	{
		uint8_t type = 0;
		std::vector<uint8_t> data;   // payload without type byte
		Reader reader() const { return Reader( data.data(), data.size() ); }
	};
}
