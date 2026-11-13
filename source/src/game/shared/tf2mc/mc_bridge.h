//========= TF2MC =========//
// One bridge connection per DLL:
//   server.dll -> ROLE_AUTHORITY (players, actions, entities, collision, floating origin)
//   client.dll -> ROLE_OBSERVER  (world for rendering + prediction collision, HUD/inventory data)
//=========================//
#ifndef MC_BRIDGE_H
#define MC_BRIDGE_H
#pragma once

#include "tf2mc_core.h"
#include "convar.h"

class CMoveData;

extern ConVar mc_jump_assist;
extern ConVar mc_allow_steve;

struct MCHotbar
{
	char items[9][64];
	int counts[9];
	int selected;
	bool valid;
};

struct MCStatus
{
	float food, saturation, xpProgress, armor;
	int xpLevel;
	bool usingItem;
	bool valid;
};

struct MCSlotView
{
	int x, y;          // Minecraft GUI pixels
	char item[64];
	int count;
	int durability;    // 0..255 (255 = undamaged / not damageable)
};

struct MCContainer
{
	bool open;
	int containerId;
	char type[64];
	std::vector<MCSlotView> slots;
	char carried[64];
	int carriedCount;
};

class CMCBridge
{
public:
	CMCBridge();

	void Connect( const char *host, int port, uint8 role, const char *name );
	void Disconnect();
	bool IsConnected() const { return m_conn.IsConnected(); }
	bool IsReady() const { return m_bHelloAck && m_conn.IsConnected(); }
	const char *Host() const { return m_szHost; }
	int Port() const { return m_iPort; }

	mc::VoxelWorld &World() { return m_world; }
	mc::Coords &Coords() { return m_coords; }
	const mc::Coords &Coords() const { return m_coords; }

	void Send( mc::Writer &w ) { if ( m_conn.IsConnected() ) m_conn.Send( w ); }

	// Applies world packets; everything else is appended to `other` for the caller.
	void Pump( std::vector<mc::Packet> &other );

	// HELLO_ACK data (re-sent on dimension change)
	int SpawnX() const { return m_spawn[0]; }
	int SpawnY() const { return m_spawn[1]; }
	int SpawnZ() const { return m_spawn[2]; }
	int Epoch() const { return m_epoch; }
	const char *LevelName() const { return m_szLevel; }
	const char *Dimension() const { return m_szDimension; }
	bool HasSkyLight() const { return m_bSkyLight; }

	int64 DayTime() const { return m_dayTime; }
	float DaylightFactor() const;   // 0..1 sky brightness multiplier (always 1 without sky light)

	MCHotbar &Hotbar() { return m_hotbar; }
	MCStatus &Status() { return m_status; }
	MCContainer &Container() { return m_container; }
	bool GetDig( mc::BlockPos &pos, int &stage ) const { pos = m_digPos; stage = m_digStage; return m_digStage >= 0; }

	void PrintStatus();

	// Converts a Source ray/box trace into a voxel sweep and fills a Source trace.
	bool TraceRay( const Ray_t &ray, unsigned int mask, trace_t &tr, CBaseEntity *pOwner );

	// Minecraft sounds (scripts/mc_sounds.txt from tools/mc_assets.py). Client: plays at a MC-space
	// position; returns false when the event has no converted files.
	bool PlaySound( const char *event, const mc::Vec3d &mcPos, float volume, float pitch );
	bool HasSound( const char *event ) const { return m_sounds.find( event ) != m_sounds.end(); }

private:
	void LoadBlockDefinitions();
	void LoadSounds( const std::string &text );
	struct SoundFile { std::string path; float volume, pitch; int weight; };
	std::unordered_map<std::string, std::vector<SoundFile>> m_sounds;
	std::unordered_set<std::string> m_precached;
	void OnBlockChanged( const mc::BlockPos &pos, uint16 oldId, uint16 newId );

	mc::Connection m_conn;
	std::deque<mc::Packet> m_backlog;   // received, not decoded yet (time budget)
	mc::VoxelWorld m_world;
	mc::Coords m_coords;
	bool m_bHelloAck;
	bool m_bDefsLoaded;
	bool m_bSkyLight;
	int m_spawn[3];
	int m_epoch;
	int64 m_dayTime;
	char m_szLevel[64];
	char m_szDimension[64];
	char m_szHost[128];
	int m_iPort;
	uint8 m_role;
	MCHotbar m_hotbar;
	MCStatus m_status;
	MCContainer m_container;
	mc::BlockPos m_digPos;
	int m_digStage;
	float m_flLastStatusPrint;
	int m_lastState;
};

CMCBridge &MCBridge();
float MC_StepSize( float tfStepSize );
// Defined in baseplayer_shared.cpp (TF2MC hook), set by mc_bridge.cpp
typedef bool ( *MC_PlayStepSoundFn )( CBasePlayer *pPlayer, const Vector &vecOrigin, float fvol );
extern MC_PlayStepSoundFn g_pfnMCPlayStepSound;   // 0.6 blocks while the Minecraft world is live
// Prints where the player is in both worlds and what a downward trace hits (engine vs voxels).
void MC_DebugProbe( CBaseEntity *pPlayer, const char *tag );

// True when the bridge is up and the world is mirrored (used by TF2 patches).
bool MC_IsActive();
// Hooks for the TF2 code patches (shared code)
float MC_SteveJumpSpeed();                     // vertical jump impulse for the Minecraft-mode (Civilian) player
float MC_ClassJumpSpeed( float tfJumpSpeed );  // optional assist so TF classes can hop one block
bool MC_AllowCivilian();
int MC_PointContents( const Vector &pt, int engineContents );   // adds CONTENTS_WATER/SLIME for MC water/lava
void MC_ClimbMove( CBaseEntity *pPlayer, CMoveData *mv, const Vector &mins, const Vector &maxs );   // ladders/vines

// Shared helpers
const char *MC_PrettyItemName( const char *id, char *out, int outLen );   // "minecraft:oak_planks" -> "Oak Planks"

#endif
