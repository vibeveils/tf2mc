//========= TF2MC =========//
// Client side of the bridge (client.dll, ROLE_OBSERVER):
//  - connects to the bridge address advertised by the server's mc_world entity
//  - mirrors the voxel world (used for rendering AND client-side prediction collision)
//  - builds/draws chunk meshes, the block highlight + crack overlay, and the hotbar HUD
//=========================//
#include "cbase.h"
#include "tf2mc/mc_bridge.h"
#include "tf2mc/mc_world_entity.h"
#include "tf2mc/mc_physics.h"
#include "c_tf_player.h"
#include "tf_shareddefs.h"
#include "igamesystem.h"
#include "materialsystem/imesh.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "vphysics_interface.h"
#include "view.h"
#include "worldsize.h"
#include "hudelement.h"
#include "iclientmode.h"
#include <vgui_controls/Panel.h>
#include <vgui/ISurface.h>
#include <vgui/IScheme.h>
#include <vgui/ILocalize.h>

#include "tier0/valve_minmax_off.h"
#include "tier0/memdbgoff.h"
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "tier0/memdbgon.h"
#include "tier0/valve_minmax_on.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar mc_draw( "mc_draw", "1", FCVAR_NONE, "Draw the Minecraft world" );
static ConVar mc_mesh_budget( "mc_mesh_budget", "24", FCVAR_ARCHIVE, "Chunk sections meshed per frame" );
static ConVar mc_ao( "mc_ao", "1", FCVAR_ARCHIVE, "Ambient occlusion on Minecraft blocks" );
static ConVar mc_flip_winding( "mc_flip_winding", "0", FCVAR_ARCHIVE, "Flip triangle winding if the world renders inside-out" );
static ConVar mc_daynight( "mc_daynight", "1", FCVAR_ARCHIVE, "Darken the world at Minecraft night" );
static ConVar mc_highlight( "mc_highlight", "1", FCVAR_ARCHIVE, "Outline the targeted block in Minecraft mode" );
static ConVar mc_glow( "mc_glow", "1", FCVAR_ARCHIVE, "Block light (torches, lava, glowstone) pass" );
static ConVar cl_mc_physics_budget( "cl_mc_physics_budget", "16", FCVAR_NONE, "Sections turned into client physics (gibs, ragdolls) per frame" );
static ConVar cl_mc_reach( "cl_mc_reach", "4.5", FCVAR_NONE, "Client-side highlight reach (should match server mc_reach)" );

static const int kMaxQuadsPerMesh = 8000;

//=============================================================================
// Mesh cache
//=============================================================================
struct GpuSection
{
	std::vector<IMesh *> opaque;      // std::vector: GpuSection is copied into an unordered_map
	std::vector<IMesh *> glow;        // additive block-light pass (only faces that receive block light)
	std::vector<IMesh *> translucent;
	Vector mins, maxs;
	mc::Coords built;                 // floating origin at build time -> draw-time translation
};

enum MeshPass { PASS_OPAQUE = 0, PASS_GLOW, PASS_TRANSLUCENT };

class CMCWorldMeshes
{
public:
	void Clear();
	void Update();
	void Draw( MeshPass pass );
	void DrawOverlays();
	IMaterial *DebugMaterial() { return Mat( PASS_OPAQUE ); }
	int SectionCount() const { return (int)m_gpu.size(); }

private:
	void Build( const mc::SectionKey &k );
	void Destroy( GpuSection &g );
	void Upload( std::vector<IMesh *> &out, const std::vector<mc::MeshVertex> &verts, IMaterial *mat, int lightChannel );
	IMaterial *Mat( MeshPass pass );

	std::unordered_map<mc::SectionKey, GpuSection, mc::SectionKeyHash> m_gpu;
	std::vector<mc::SectionKey> m_pending;
	mc::Coords m_builtCoords;
	bool m_builtTextured = false;
	bool m_builtFlip = false;
	bool m_builtAO = true;
	CMaterialReference m_atlas, m_atlasTrans, m_atlasGlow, m_flat, m_flatTrans, m_flatGlow;
	bool m_materialsInit = false;
	bool m_textured = false;
};

static CMCWorldMeshes g_Meshes;


IMaterial *CMCWorldMeshes::Mat( MeshPass pass )
{
	if ( !m_materialsInit )
	{
		m_materialsInit = true;
		m_atlas.Init( "mc/atlas", TEXTURE_GROUP_WORLD );
		m_atlasTrans.Init( "mc/atlas_translucent", TEXTURE_GROUP_WORLD );
		m_atlasGlow.Init( "mc/atlas_glow", TEXTURE_GROUP_WORLD );
		m_flat.Init( "mc/flat", TEXTURE_GROUP_WORLD );
		m_flatTrans.Init( "mc/flat_translucent", TEXTURE_GROUP_WORLD );
		m_flatGlow.Init( "mc/flat_glow", TEXTURE_GROUP_WORLD );
	}
	m_textured = MCBridge().World().Registry().HasAtlas() && m_atlas.IsValid() && !m_atlas->IsErrorMaterial();
	if ( m_textured ) return pass == PASS_OPAQUE ? m_atlas : pass == PASS_GLOW ? m_atlasGlow : m_atlasTrans;
	return pass == PASS_OPAQUE ? m_flat : pass == PASS_GLOW ? m_flatGlow : m_flatTrans;
}

void CMCWorldMeshes::Destroy( GpuSection &g )
{
	CMatRenderContextPtr ctx( materials );
	for ( size_t i = 0; i < g.opaque.size(); ++i ) ctx->DestroyStaticMesh( g.opaque[i] );
	for ( size_t i = 0; i < g.glow.size(); ++i ) ctx->DestroyStaticMesh( g.glow[i] );
	for ( size_t i = 0; i < g.translucent.size(); ++i ) ctx->DestroyStaticMesh( g.translucent[i] );
	g.opaque.clear();
	g.glow.clear();
	g.translucent.clear();
}

void CMCWorldMeshes::Clear()
{
	for ( auto &it : m_gpu ) Destroy( it.second );
	m_gpu.clear();
	m_pending.clear();
}

// lightChannel: 0 = sky light (scaled by daylight via $color), 1 = block light (additive glow pass)
void CMCWorldMeshes::Upload( std::vector<IMesh *> &out, const std::vector<mc::MeshVertex> &verts, IMaterial *mat, int lightChannel )
{
	const size_t quads = verts.size() / 4;
	CMatRenderContextPtr ctx( materials );
	for ( size_t q0 = 0; q0 < quads; q0 += kMaxQuadsPerMesh )
	{
		int n = (int)MIN( (size_t)kMaxQuadsPerMesh, quads - q0 );
		// Static meshes must carry every field the material's shader wants (DX9 shaders on most
		// cards want a normal too) and must not be compressed, or the engine refuses to draw them.
		VertexFormat_t fmt = ( mat->GetVertexFormat() & ~VERTEX_FORMAT_COMPRESSED ) |
							 VERTEX_POSITION | VERTEX_COLOR | VERTEX_NORMAL | VERTEX_TEXCOORD_SIZE( 0, 2 );
		IMesh *mesh = ctx->CreateStaticMesh( fmt, TEXTURE_GROUP_STATIC_VERTEX_BUFFER_WORLD, mat );
		CMeshBuilder mb;
		mb.Begin( mesh, MATERIAL_TRIANGLES, n * 4, n * 6 );
		for ( int q = 0; q < n; ++q )
		{
			const mc::MeshVertex *v = &verts[ ( q0 + q ) * 4 ];
			for ( int k = 0; k < 4; ++k )
			{
				mb.Position3fv( v[k].pos );
				mb.Normal3f( 0.f, 0.f, 1.f );
				float l = mc::LightCurve( lightChannel == 0 ? v[k].sky : v[k].blk );
				if ( lightChannel == 1 ) l = MAX( 0.f, l - mc::LightCurve( 0 ) );   // glow: block light only
				mb.Color4ub( (unsigned char)( v[k].rgba[0] * l ), (unsigned char)( v[k].rgba[1] * l ), (unsigned char)( v[k].rgba[2] * l ), v[k].rgba[3] );
				mb.TexCoord2f( 0, v[k].uv[0], v[k].uv[1] );
				mb.AdvanceVertex();
			}
			unsigned short b = (unsigned short)( q * 4 );
			mb.Index( b );     mb.AdvanceIndex();
			mb.Index( b + 1 ); mb.AdvanceIndex();
			mb.Index( b + 2 ); mb.AdvanceIndex();
			mb.Index( b );     mb.AdvanceIndex();
			mb.Index( b + 2 ); mb.AdvanceIndex();
			mb.Index( b + 3 ); mb.AdvanceIndex();
		}
		mb.End();
		out.push_back( mesh );
	}
}

void CMCWorldMeshes::Build( const mc::SectionKey &k )
{
	auto it = m_gpu.find( k );
	if ( it != m_gpu.end() ) { Destroy( it->second ); m_gpu.erase( it ); }

	CMCBridge &b = MCBridge();
	mc::MesherOptions opt;
	opt.clockwise = !mc_flip_winding.GetBool();
	opt.ambientOcclusion = mc_ao.GetBool();
	opt.textured = m_textured;
	mc::SectionMesh mesh;
	mc::BuildSectionMesh( b.World(), b.Coords(), k.cx, k.sy, k.cz, opt, mesh );
	if ( mesh.QuadCount() == 0 ) return;

	GpuSection g;
	g.built = b.Coords();
	Upload( g.opaque, mesh.opaque, Mat( PASS_OPAQUE ), 0 );
	Upload( g.translucent, mesh.translucent, Mat( PASS_TRANSLUCENT ), 0 );
	// Glow pass: only quads that receive block light
	std::vector<mc::MeshVertex> glow;
	for ( size_t q = 0; q + 3 < mesh.opaque.size(); q += 4 )
		if ( mesh.opaque[q].blk | mesh.opaque[q + 1].blk | mesh.opaque[q + 2].blk | mesh.opaque[q + 3].blk )
			glow.insert( glow.end(), mesh.opaque.begin() + q, mesh.opaque.begin() + q + 4 );
	if ( !glow.empty() ) Upload( g.glow, glow, Mat( PASS_GLOW ), 1 );
	mc::Vec3d a = b.Coords().ToSource( mc::Vec3d( k.cx * 16, k.sy * 16, k.cz * 16 ) );
	mc::Vec3d c = b.Coords().ToSource( mc::Vec3d( k.cx * 16 + 16, k.sy * 16 + 16, k.cz * 16 + 16 ) );
	g.mins = Vector( (float)MIN( a.x, c.x ), (float)MIN( a.y, c.y ), (float)MIN( a.z, c.z ) );
	g.maxs = Vector( (float)MAX( a.x, c.x ), (float)MAX( a.y, c.y ), (float)MAX( a.z, c.z ) );
	m_gpu.insert( std::make_pair( k, g ) );
}

void CMCWorldMeshes::Update()
{
	CMCBridge &b = MCBridge();
	if ( !b.IsReady() )
	{
		if ( !m_gpu.empty() ) Clear();
		return;
	}

	Mat( PASS_OPAQUE );   // refresh m_textured
	const mc::Coords &c = b.Coords();
	// Floating-origin moves don't rebuild: sections are translated at draw time.
	bool rebuildAll = c.scale != m_builtCoords.scale ||
					  m_textured != m_builtTextured || mc_flip_winding.GetBool() != m_builtFlip || mc_ao.GetBool() != m_builtAO;
	if ( rebuildAll )
	{
		m_builtCoords = c;
		m_builtTextured = m_textured;
		m_builtFlip = mc_flip_winding.GetBool();
		m_builtAO = mc_ao.GetBool();
		for ( auto &it : m_gpu ) m_pending.push_back( it.first );
	}

	std::vector<mc::SectionKey> removed;
	b.World().TakeRemoved( removed );
	for ( size_t i = 0; i < removed.size(); ++i )
	{
		auto it = m_gpu.find( removed[i] );
		if ( it != m_gpu.end() ) { Destroy( it->second ); m_gpu.erase( it ); }
	}
	b.World().TakeDirty( m_pending, mc::CH_RENDER );
	MCPhysics().Update( b.World(), c, cl_mc_physics_budget.GetInt() );   // gibs & ragdolls collide with voxels
	if ( m_pending.empty() ) return;

	// Nearest sections first
	Vector eye = MainViewOrigin();
	mc::Vec3d e = c.ToMC( MC_V( eye ) );
	int ecx = mc::FloorDiv16( mc::FloorI( e.x ) ), ecy = mc::FloorDiv16( mc::FloorI( e.y ) ), ecz = mc::FloorDiv16( mc::FloorI( e.z ) );
	// de-duplicate + sort by distance
	std::sort( m_pending.begin(), m_pending.end(), [&]( const mc::SectionKey &a, const mc::SectionKey &b2 ) {
		int da = ( a.cx - ecx ) * ( a.cx - ecx ) + ( a.sy - ecy ) * ( a.sy - ecy ) + ( a.cz - ecz ) * ( a.cz - ecz );
		int db = ( b2.cx - ecx ) * ( b2.cx - ecx ) + ( b2.sy - ecy ) * ( b2.sy - ecy ) + ( b2.cz - ecz ) * ( b2.cz - ecz );
		if ( da != db ) return da < db;
		if ( a.cx != b2.cx ) return a.cx < b2.cx;
		if ( a.sy != b2.sy ) return a.sy < b2.sy;
		return a.cz < b2.cz;
	} );
	m_pending.erase( std::unique( m_pending.begin(), m_pending.end() ), m_pending.end() );

	int budget = MAX( 1, mc_mesh_budget.GetInt() );
	int done = 0;
	for ( ; done < (int)m_pending.size() && done < budget; ++done )
	{
		const mc::SectionKey &k = m_pending[done];
		if ( b.World().IsColumnLoaded( k.cx, k.cz ) ) Build( k );
	}
	m_pending.erase( m_pending.begin(), m_pending.begin() + done );
}

static int s_nDrawCalls[3], s_nDrawnSections[3], s_nCulledSections[3], s_nDrawFrame = -1;

void CMCWorldMeshes::Draw( MeshPass pass )
{
	if ( s_nDrawFrame != gpGlobals->framecount )
	{
		s_nDrawFrame = gpGlobals->framecount;
		for ( int i = 0; i < 3; ++i ) s_nDrawCalls[i] = s_nDrawnSections[i] = s_nCulledSections[i] = 0;
	}
	s_nDrawCalls[pass]++;
	if ( !mc_draw.GetBool() || m_gpu.empty() ) return;
	if ( pass == PASS_GLOW && !mc_glow.GetBool() ) return;
	IMaterial *mat = Mat( pass );
	if ( !mat ) return;

	float day = mc_daynight.GetBool() ? MCBridge().DaylightFactor() : 1.f;
	// Sky-lit passes follow the sun; block light is added on top, strongest at night
	float k = pass == PASS_GLOW ? ( 1.f - 0.7f * day ) : day;
	bool found = false;
	IMaterialVar *color = mat->FindVar( "$color", &found, false );
	if ( found && color )
	{
		if ( pass == PASS_GLOW ) color->SetVecValue( k * 1.25f, k * 1.1f, k * 0.85f );   // warm torch light
		else color->SetVecValue( k, k, k );
	}

	const mc::Coords &now = MCBridge().Coords();
	CMatRenderContextPtr ctx( materials );
	ctx->Bind( mat );
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PushMatrix();
	bool haveOffset = false;
	Vector current( 0, 0, 0 );
	for ( auto &it : m_gpu )
	{
		GpuSection &g = it.second;
		std::vector<IMesh *> &list = pass == PASS_OPAQUE ? g.opaque : pass == PASS_GLOW ? g.glow : g.translucent;
		if ( list.empty() ) continue;
		mc::Vec3d d = now.ShiftFrom( g.built );
		Vector off( (float)d.x, (float)d.y, (float)d.z );
		if ( engine->CullBox( g.mins + off, g.maxs + off ) ) { s_nCulledSections[pass]++; continue; }
		s_nDrawnSections[pass]++;
		if ( !haveOffset || off != current )
		{
			ctx->LoadIdentity();
			ctx->Translate( off.x, off.y, off.z );
			current = off;
			haveOffset = true;
		}
		for ( size_t i = 0; i < list.size(); ++i ) list[i]->Draw();
	}
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PopMatrix();
}

// Block outline + crack overlay for the local Minecraft-mode player
void CMCWorldMeshes::DrawOverlays()
{
	CMCBridge &b = MCBridge();
	C_TFPlayer *pLocal = C_TFPlayer::GetLocalTFPlayer();
	if ( !pLocal || !pLocal->IsAlive() || !pLocal->IsPlayerClass( TF_CLASS_CIVILIAN ) || !mc_highlight.GetBool() ) return;

	const mc::Coords &c = b.Coords();
	Vector fwd;
	AngleVectors( MainViewAngles(), &fwd );
	mc::PickResult pick = mc::PickBlock( b.World(), c.ToMC( MC_V( MainViewOrigin() ) ), c.DeltaToMC( MC_V( fwd ) ), cl_mc_reach.GetFloat() );
	if ( !pick.hit ) return;

	const mc::BlockDef &d = b.World().GetDef( pick.block.x, pick.block.y, pick.block.z );
	float bx0 = 0, by0 = 0, bz0 = 0, bx1 = 1, by1 = 1, bz1 = 1;
	if ( !d.boxes.empty() )
	{
		// outline = union of the outline boxes
		bx0 = by0 = bz0 = 1; bx1 = by1 = bz1 = 0;
		for ( size_t i = 0; i < d.boxes.size(); ++i )
		{
			bx0 = MIN( bx0, d.boxes[i].mn[0] ); by0 = MIN( by0, d.boxes[i].mn[1] ); bz0 = MIN( bz0, d.boxes[i].mn[2] );
			bx1 = MAX( bx1, d.boxes[i].mx[0] ); by1 = MAX( by1, d.boxes[i].mx[1] ); bz1 = MAX( bz1, d.boxes[i].mx[2] );
		}
	}
	const float e = 0.002f;
	Vector corner[8];
	for ( int i = 0; i < 8; ++i )
	{
		mc::Vec3d m( pick.block.x + ( ( i & 1 ) ? bx1 + e : bx0 - e ), pick.block.y + ( ( i & 2 ) ? by1 + e : by0 - e ), pick.block.z + ( ( i & 4 ) ? bz1 + e : bz0 - e ) );
		corner[i] = MC_V( c.ToSource( m ) );
	}

	CMatRenderContextPtr ctx( materials );
	IMaterial *flat = Mat( PASS_OPAQUE ) ? (IMaterial *)m_flat : NULL;
	if ( !flat ) return;
	ctx->Bind( flat );
	IMesh *pMesh = ctx->GetDynamicMesh();
	CMeshBuilder mb;
	static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
	mb.Begin( pMesh, MATERIAL_LINES, 12 );
	for ( int i = 0; i < 12; ++i )
		for ( int k = 0; k < 2; ++k )
		{
			mb.Position3fv( corner[ edges[i][k] ].Base() );
			mb.Color4ub( 0, 0, 0, 255 );
			mb.TexCoord2f( 0, 0, 0 );
			mb.AdvanceVertex();
		}
	mb.End();
	pMesh->Draw();

	// Crack overlay: darken the block proportionally to Minecraft's destroy stage
	mc::BlockPos digPos;
	int stage;
	if ( b.GetDig( digPos, stage ) && digPos == pick.block )
	{
		ctx->Bind( m_flatTrans );
		IMesh *pm = ctx->GetDynamicMesh();
		CMeshBuilder qb;
		static const int faces[6][4] = { { 0, 4, 6, 2 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 2, 3, 1 }, { 4, 5, 7, 6 } };
		qb.Begin( pm, MATERIAL_QUADS, 6 );
		unsigned char a = (unsigned char)MIN( 220, 30 + stage * 20 );
		for ( int f = 0; f < 6; ++f )
			for ( int k = 0; k < 4; ++k )
			{
				qb.Position3fv( corner[ faces[f][k] ].Base() );
				qb.Color4ub( 0, 0, 0, a );
				qb.TexCoord2f( 0, 0, 0 );
				qb.AdvanceVertex();
			}
		qb.End();
		pm->Draw();
	}
}

//=============================================================================
// Translucent pass renderable (water, stained glass, ice, overlays)
//=============================================================================
class C_MCWorldTranslucent : public C_BaseEntity
{
public:
	DECLARE_CLASS( C_MCWorldTranslucent, C_BaseEntity );
	virtual bool ShouldDraw() { return true; }
	virtual bool IsTransparent() { return true; }
	virtual void GetRenderBounds( Vector &mins, Vector &maxs ) { mins = MC_WorldMins(); maxs = MC_WorldMaxs(); }
	virtual int DrawModel( int flags )
	{
		g_Meshes.Draw( PASS_TRANSLUCENT );
		g_Meshes.DrawOverlays();
		return 1;
	}
};
static C_MCWorldTranslucent *s_pTranslucent = NULL;

//=============================================================================
// C_MCWorld client implementation
//=============================================================================
// The server moved the floating origin: networked entities arrive already moved, but client-only
// things (gibs, shells, client physics props) must be shifted here. Ragdolls are hidden - their
// many physics bones aren't worth re-posing for a frame-perfect result.
static void MC_ShiftClientOnlyEntities( const Vector &delta )
{
	for ( C_BaseEntity *e = ClientEntityList().FirstBaseEntity(); e; e = ClientEntityList().NextBaseEntity( e ) )
	{
		if ( e->index != -1 || e->GetMoveParent() || e == (C_BaseEntity *)s_pTranslucent ) continue;
		C_BaseAnimating *pAnim = e->GetBaseAnimating();
		if ( pAnim && pAnim->IsRagdoll() ) { e->AddEffects( EF_NODRAW ); continue; }
		Vector pos = e->GetAbsOrigin() + delta;
		if ( IPhysicsObject *pPhys = e->VPhysicsGetObject() )
		{
			QAngle ang;
			Vector cur;
			pPhys->GetPosition( &cur, &ang );
			cur += delta;
			pPhys->SetPosition( cur, ang, true );
		}
		e->SetAbsOrigin( pos );
	}
}

void C_MCWorld::OnDataChanged( DataUpdateType_t type )
{
	BaseClass::OnDataChanged( type );
	mc::Coords before = MCBridge().Coords();
	ApplyCoords( MCBridge().Coords() );
	const mc::Coords &after = MCBridge().Coords();
	if ( type != DATA_UPDATE_CREATED && before != after && before.SameScale( after ) && MCBridge().IsReady() )
	{
		mc::Vec3d d = after.ShiftFrom( before );
		MC_ShiftClientOnlyEntities( Vector( (float)d.x, (float)d.y, (float)d.z ) );
	}

	if ( type == DATA_UPDATE_CREATED )
	{
		SetNextClientThink( CLIENT_THINK_ALWAYS );
		if ( !s_pTranslucent )
		{
			s_pTranslucent = new C_MCWorldTranslucent;
			if ( !s_pTranslucent->InitializeAsClientEntity( NULL, RENDER_GROUP_TRANSLUCENT_ENTITY ) )
			{
				s_pTranslucent->Release();
				s_pTranslucent = NULL;
			}
		}
	}
}

void C_MCWorld::ClientThink()
{
	// (Re)connect when the server advertises a bridge we're not attached to.
	const char *host = m_szBridgeHost.Get();
	if ( !m_bReady || !host[0] ) return;
	CMCBridge &b = MCBridge();
	bool sameTarget = ( !Q_strcmp( m_szConnectedHost, host ) && m_iConnectedPort == m_iBridgePort ) ||
					  ( b.IsConnected() && !Q_strcmp( b.Host(), host ) && b.Port() == m_iBridgePort );
	if ( ( !sameTarget || ( !b.IsConnected() && gpGlobals->curtime > m_flNextConnectTry ) ) )
	{
		Q_strncpy( m_szConnectedHost, host, sizeof( m_szConnectedHost ) );
		m_iConnectedPort = m_iBridgePort;
		m_flNextConnectTry = gpGlobals->curtime + 5.f;
		C_BasePlayer *pLocal = C_BasePlayer::GetLocalPlayer();
		b.Connect( host, m_iBridgePort, mc::ROLE_OBSERVER, pLocal ? pLocal->GetPlayerName() : "tf2-client" );
	}
}

void C_MCWorld::UpdateOnRemove()
{
	// Keep the bridge connection (and the mirrored world): if this entity is recreated, the new one
	// picks the same connection up. Level shutdown (CMCClientSystem) disconnects.
	if ( s_pTranslucent ) { s_pTranslucent->Release(); s_pTranslucent = NULL; }
	BaseClass::UpdateOnRemove();
}

int C_MCWorld::DrawModel( int flags )
{
	g_Meshes.Draw( PASS_OPAQUE );
	g_Meshes.Draw( PASS_GLOW );
	return 1;
}

void C_MCWorld::GetRenderBounds( Vector &mins, Vector &maxs )
{
	mins = MC_WorldMins();
	maxs = MC_WorldMaxs();
}

//=============================================================================
// Per-frame client system: pump the bridge, mesh, subscribe to own hotbar
//=============================================================================
class CMCClientSystem : public CAutoGameSystemPerFrame
{
public:
	CMCClientSystem() : CAutoGameSystemPerFrame( "CMCClientSystem" ), m_iSubscribed( -1 ) {}

	virtual void Update( float frametime )
	{
		CMCBridge &b = MCBridge();
		std::vector<mc::Packet> other;
		b.Pump( other );
		for ( size_t i = 0; i < other.size(); ++i )
			if ( other[i].type == mc::S2C_HELLO_ACK ) m_iSubscribed = -1;

		if ( b.IsReady() )
		{
			C_BasePlayer *pLocal = C_BasePlayer::GetLocalPlayer();
			if ( pLocal && pLocal->entindex() != m_iSubscribed )
			{
				m_iSubscribed = pLocal->entindex();
				mc::Writer w( mc::C2S_SUBSCRIBE_SLOT );
				w.u16( (uint16)m_iSubscribed );
				b.Send( w );
			}
		}
		g_Meshes.Update();
	}

	virtual void LevelShutdownPostEntity()
	{
		MCPhysics().Clear();
		MCBridge().Disconnect();
		g_Meshes.Clear();
		m_iSubscribed = -1;
	}

private:
	int m_iSubscribed;
};
static CMCClientSystem g_MCClientSystem;

CON_COMMAND( cl_mc_status, "Show Minecraft bridge status (client)" )
{
	MCBridge().PrintStatus();
	Msg( "TF2MC[client]: %d meshed sections, %d physics sections\n", g_Meshes.SectionCount(), MCPhysics().Count() );
	bool fresh = gpGlobals->framecount - s_nDrawFrame <= 2;
	Msg( "TF2MC[client]: last drawn frame %d (now %d): opaque calls %d drawn %d culled %d | translucent calls %d drawn %d\n",
		 s_nDrawFrame, gpGlobals->framecount, fresh ? s_nDrawCalls[PASS_OPAQUE] : 0, fresh ? s_nDrawnSections[PASS_OPAQUE] : 0,
		 fresh ? s_nCulledSections[PASS_OPAQUE] : 0, fresh ? s_nDrawCalls[PASS_TRANSLUCENT] : 0, fresh ? s_nDrawnSections[PASS_TRANSLUCENT] : 0 );
	IMaterial *m = g_Meshes.DebugMaterial();
	Msg( "TF2MC[client]: material %s%s  mc_draw %d\n", m ? m->GetName() : "<none>", m && m->IsErrorMaterial() ? " (ERROR - missing vmt)" : "", mc_draw.GetInt() );
	C_MCWorld *w = C_MCWorld::Get();
	Msg( "TF2MC[client]: mc_world entity %s  render handle %s  dormant %d\n", w ? "present" : "MISSING",
		 w && w->GetRenderHandle() != INVALID_CLIENT_RENDER_HANDLE ? "ok" : "none", w ? (int)w->IsDormant() : -1 );
	MC_DebugProbe( C_BasePlayer::GetLocalPlayer(), "client" );
}
