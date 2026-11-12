//========= TF2MC =========//
#include "cbase.h"
#include "mc_mob.h"
#include "mc_bridge.h"
#include "filesystem.h"
#include "KeyValues.h"

#ifdef CLIENT_DLL
#include "materialsystem/imesh.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/MaterialSystemUtil.h"
#else
#include "tf_player.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

IMPLEMENT_NETWORKCLASS_ALIASED( MCMob, DT_MCMob )

BEGIN_NETWORK_TABLE( CMCMob, DT_MCMob )
#ifdef CLIENT_DLL
	RecvPropString( RECVINFO( m_szMCType ) ),
	RecvPropInt( RECVINFO( m_iMCId ) ),
	RecvPropInt( RECVINFO( m_iMCFlags ) ),
	RecvPropFloat( RECVINFO( m_flMCHealth ) ),
	RecvPropFloat( RECVINFO( m_flMCMaxHealth ) ),
	RecvPropInt( RECVINFO( m_iHurtTicks ) ),
	RecvPropFloat( RECVINFO( m_flHeadPitch ) ),
#else
	SendPropString( SENDINFO( m_szMCType ) ),
	SendPropInt( SENDINFO( m_iMCId ), 32, 0 ),
	SendPropInt( SENDINFO( m_iMCFlags ), 8, SPROP_UNSIGNED ),
	SendPropFloat( SENDINFO( m_flMCHealth ), 0, SPROP_NOSCALE ),
	SendPropFloat( SENDINFO( m_flMCMaxHealth ), 0, SPROP_NOSCALE ),
	SendPropInt( SENDINFO( m_iHurtTicks ), 5, SPROP_UNSIGNED ),
	SendPropFloat( SENDINFO( m_flHeadPitch ), 8, 0, -90.f, 90.f ),
#endif
END_NETWORK_TABLE()

LINK_ENTITY_TO_CLASS( mc_mob, CMCMob );

CMCMob::CMCMob()
{
	m_iMCId = -1;
	m_iMCFlags = 0;
	m_flMCHealth = 0;
	m_flMCMaxHealth = 0;
	m_iHurtTicks = 0;
	m_flHeadPitch = 0;
}

#ifdef GAME_DLL
//=============================================================================
// Server
//=============================================================================
ConVar mc_damage_to_mc( "mc_damage_to_mc", "0.08", FCVAR_NOTIFY, "Multiplier from TF2 damage to Minecraft health points (rocket 90 -> 7.2 = 3.6 hearts)" );
ConVar mc_mob_knockback( "mc_mob_knockback", "0.4", FCVAR_NONE, "Knockback strength applied to mobs hit by TF2 weapons (blast x2)" );

// scripts/mc_entity_models.txt: "minecraft:zombie" "models/bots/..." mapping (optional)
static KeyValues *s_pModelMap = NULL;
static const char *MC_ModelForType( const char *type )
{
	if ( !s_pModelMap )
	{
		s_pModelMap = new KeyValues( "MCEntityModels" );
		if ( !s_pModelMap->LoadFromFile( g_pFullFileSystem, "scripts/mc_entity_models.txt", "MOD" ) )
			DevMsg( "TF2MC: no scripts/mc_entity_models.txt - mobs are drawn as boxes\n" );
	}
	const char *m = s_pModelMap->GetString( type, "" );
	return ( m && m[0] ) ? m : NULL;
}

void MC_PrecacheMobModels()
{
	MC_ModelForType( "" );   // loads the mapping file
	for ( KeyValues *k = s_pModelMap->GetFirstValue(); k; k = k->GetNextValue() )
		if ( k->GetString()[0] ) CBaseEntity::PrecacheModel( k->GetString() );
}

void CMCMob::Precache()
{
	BaseClass::Precache();
}

void CMCMob::Spawn()
{
	Precache();
	BaseClass::Spawn();
	SetMoveType( MOVETYPE_NONE );
	SetSolid( SOLID_BBOX );
	SetCollisionGroup( COLLISION_GROUP_NPC );
	AddFlag( FL_NPC );
	m_takedamage = DAMAGE_YES;
	SetBlocksLOS( false );
}

void CMCMob::Setup( int id, const char *type, float w, float h, float scale, int flags, float hp, float maxHp )
{
	m_iMCId = id;
	Q_strncpy( m_szMCType.GetForModify(), type, 64 );
	m_iMCFlags = flags;
	m_flMCHealth = hp;
	m_flMCMaxHealth = maxHp;

	const char *model = MC_ModelForType( type );
	if ( model && modelinfo->GetModelIndex( model ) >= 0 )
		SetModel( model );   // precached at level load by MC_PrecacheMobModels()
	float hw = MAX( 0.1f, w ) * 0.5f * scale, hh = MAX( 0.1f, h ) * scale;
	UTIL_SetSize( this, Vector( -hw, -hw, 0 ), Vector( hw, hw, hh ) );

	if ( !( flags & mc::ENT_LIVING ) )
	{
		// Items / projectiles / vehicles: visible and shootable but not blocking players
		SetCollisionGroup( COLLISION_GROUP_DEBRIS );
		m_takedamage = ( flags & mc::ENT_ITEM ) ? DAMAGE_NO : DAMAGE_YES;
	}
}

void CMCMob::UpdateFromMC( const Vector &origin, float yaw, float pitch, float hp, int hurtTicks )
{
	SetAbsOrigin( origin );
	SetAbsAngles( QAngle( 0, yaw, 0 ) );
	m_flHeadPitch = clamp( pitch, -90.f, 90.f );
	m_flMCHealth = hp;
	m_iHurtTicks = MIN( hurtTicks, 31 );
}

int CMCMob::BloodColor()
{
	if ( !( m_iMCFlags & mc::ENT_LIVING ) ) return DONT_BLEED;
	const char *t = m_szMCType.Get();
	if ( Q_strstr( t, "skeleton" ) || Q_strstr( t, "golem" ) || Q_strstr( t, "blaze" ) || Q_strstr( t, "creeper" ) )
		return DONT_BLEED;
	return BLOOD_COLOR_RED;
}

int CMCMob::OnTakeDamage( const CTakeDamageInfo &info )
{
	if ( !MC_IsActive() || m_takedamage == DAMAGE_NO )
		return 0;

	CTFPlayer *pAttacker = ToTFPlayer( info.GetAttacker() );
	if ( !pAttacker )
		return 0;

	int bits = info.GetDamageType();
	uint8 kind = mc::DMG_KIND_GENERIC;
	if ( bits & DMG_BLAST ) kind = mc::DMG_KIND_EXPLOSION;
	else if ( bits & ( DMG_BURN | DMG_IGNITE ) ) kind = mc::DMG_KIND_FIRE;
	else if ( bits & ( DMG_CLUB | DMG_SLASH ) ) kind = mc::DMG_KIND_MELEE;
	else if ( bits & ( DMG_BULLET | DMG_BUCKSHOT ) ) kind = mc::DMG_KIND_PROJECTILE;

	// Push direction in Minecraft space
	Vector force = info.GetDamageForce();
	if ( force.IsZero() ) force = GetAbsOrigin() - pAttacker->GetAbsOrigin();
	mc::Vec3d d = MCBridge().Coords().DirToMC( MC_V( force ) );
	double l = sqrt( d.x * d.x + d.z * d.z );
	float kx = l > 0 ? (float)( d.x / l ) : 0.f, kz = l > 0 ? (float)( d.z / l ) : 0.f;
	float strength = mc_mob_knockback.GetFloat() * ( kind == mc::DMG_KIND_EXPLOSION ? 2.f : 1.f );

	float mcDamage = info.GetDamage() * mc_damage_to_mc.GetFloat();
	mc::Writer w( mc::C2S_ATTACK );
	w.u16( (uint16)pAttacker->entindex() ).i32( m_iMCId ).u8( mc::ATTACK_TF_DAMAGE ).f32( mcDamage ).f32( kx ).f32( kz ).f32( strength ).u8( kind );
	MCBridge().Send( w );

	// Health is owned by Minecraft; report the hit for TF2 effects but don't die here.
	return (int)info.GetDamage();
}

#else
//=============================================================================
// Client: procedural Minecraft-style models (boxes in Minecraft "pixel" units, animated limbs),
// item sprites, health bars. A model from scripts/mc_entity_models.txt overrides all of this.
//=============================================================================
#include "../../client/tf2mc/c_mc_items.h"
#include "view.h"

static ConVar mc_mob_boxes( "mc_mob_boxes", "1", FCVAR_ARCHIVE, "Draw Minecraft entities without a mapped model procedurally" );
static ConVar mc_mob_healthbars( "mc_mob_healthbars", "1", FCVAR_ARCHIVE, "Show health bars over damaged Minecraft mobs" );

enum PartAnim { A_NONE = 0, A_LEG_L, A_LEG_R, A_ARM_L, A_ARM_R, A_HEAD, A_ARMS_FORWARD_L, A_ARMS_FORWARD_R, A_SPIDER_L, A_SPIDER_R };

struct MobPart
{
	float mn[3], mx[3];      // pixels; +x forward, +y left, +z up; feet at z = 0
	float pivot[3];          // rotation pivot (pixels)
	unsigned char rgb[3];
	unsigned char anim;
	bool face;               // darker front = eyes/face
};

struct MobModel
{
	const char *match;       // substring of the entity type
	float height;            // pixels (model is scaled to the hitbox height)
	MobPart parts[12];
	int count;
};

#define P( x0, y0, z0, x1, y1, z1, px, py, pz, r, g, b, anim, face ) { { x0, y0, z0 }, { x1, y1, z1 }, { px, py, pz }, { r, g, b }, anim, face }

// Humanoid: legs 4x4x12, body 4x8x12, arms 4x4x12, head 8x8x8 -> 32 px tall
#define HUMANOID( name, sr, sg, sb, tr, tg, tb, pr, pg, pb, armsAnimL, armsAnimR ) \
	{ name, 32, { \
		P( -2, 0, 0, 2, 4, 12, 0, 2, 12, pr, pg, pb, A_LEG_L, false ), \
		P( -2, -4, 0, 2, 0, 12, 0, -2, 12, pr, pg, pb, A_LEG_R, false ), \
		P( -2, -4, 12, 2, 4, 24, 0, 0, 12, tr, tg, tb, A_NONE, false ), \
		P( -2, 4, 12, 2, 8, 24, 0, 6, 22, sr, sg, sb, armsAnimL, false ), \
		P( -2, -8, 12, 2, -4, 24, 0, -6, 22, sr, sg, sb, armsAnimR, false ), \
		P( -4, -4, 24, 4, 4, 32, 0, 0, 24, sr, sg, sb, A_HEAD, true ) }, 6 }

static const MobModel kModels[] = {
	HUMANOID( "zombie_villager", 90, 150, 90, 70, 110, 70, 60, 50, 40, A_ARMS_FORWARD_L, A_ARMS_FORWARD_R ),
	HUMANOID( "husk", 170, 150, 100, 120, 100, 70, 90, 75, 60, A_ARMS_FORWARD_L, A_ARMS_FORWARD_R ),
	HUMANOID( "drowned", 80, 160, 150, 50, 110, 110, 40, 90, 90, A_ARMS_FORWARD_L, A_ARMS_FORWARD_R ),
	HUMANOID( "zombie", 90, 160, 80, 40, 140, 160, 60, 60, 140, A_ARMS_FORWARD_L, A_ARMS_FORWARD_R ),
	HUMANOID( "skeleton", 215, 215, 215, 190, 190, 190, 200, 200, 200, A_ARM_L, A_ARM_R ),
	HUMANOID( "stray", 180, 200, 200, 150, 170, 170, 160, 180, 180, A_ARM_L, A_ARM_R ),
	HUMANOID( "villager", 190, 140, 110, 110, 80, 50, 90, 60, 40, A_NONE, A_NONE ),
	HUMANOID( "pillager", 160, 160, 160, 60, 70, 90, 40, 40, 50, A_ARM_L, A_ARM_R ),
	HUMANOID( "vindicator", 160, 160, 160, 40, 50, 70, 30, 30, 40, A_ARM_L, A_ARM_R ),
	HUMANOID( "witch", 150, 170, 120, 60, 30, 80, 40, 20, 50, A_NONE, A_NONE ),
	HUMANOID( "piglin", 230, 160, 150, 120, 90, 50, 90, 70, 40, A_ARM_L, A_ARM_R ),
	HUMANOID( "player", 210, 160, 120, 40, 170, 190, 50, 50, 140, A_ARM_L, A_ARM_R ),
	{ "enderman", 44, {
		P( -1, 1, 0, 1, 3, 30, 0, 2, 30, 20, 15, 25, A_LEG_L, false ),
		P( -1, -3, 0, 1, -1, 30, 0, -2, 30, 20, 15, 25, A_LEG_R, false ),
		P( -2, -4, 30, 2, 4, 42, 0, 0, 30, 25, 20, 30, A_NONE, false ),
		P( -1, 4, 12, 1, 6, 42, 0, 5, 40, 20, 15, 25, A_ARM_L, false ),
		P( -1, -6, 12, 1, -4, 42, 0, -5, 40, 20, 15, 25, A_ARM_R, false ),
		P( -4, -4, 42, 4, 4, 50, 0, 0, 42, 30, 25, 35, A_HEAD, true ) }, 6 },
	{ "creeper", 26, {
		P( 2, 0, 0, 6, 4, 6, 4, 2, 6, 60, 160, 60, A_LEG_L, false ),
		P( 2, -4, 0, 6, 0, 6, 4, -2, 6, 60, 160, 60, A_LEG_R, false ),
		P( -6, 0, 0, -2, 4, 6, -4, 2, 6, 60, 160, 60, A_LEG_R, false ),
		P( -6, -4, 0, -2, 0, 6, -4, -2, 6, 60, 160, 60, A_LEG_L, false ),
		P( -2, -4, 6, 2, 4, 18, 0, 0, 6, 70, 180, 70, A_NONE, false ),
		P( -4, -4, 18, 4, 4, 26, 0, 0, 18, 80, 190, 80, A_HEAD, true ) }, 6 },
	{ "spider", 9, {
		P( -12, -5, 2, 0, 5, 10, -6, 0, 6, 50, 40, 40, A_NONE, false ),
		P( 0, -3, 3, 6, 3, 9, 3, 0, 6, 60, 50, 50, A_NONE, false ),
		P( 6, -4, 2, 14, 4, 10, 6, 0, 6, 55, 45, 45, A_HEAD, true ),
		P( 3, 3, 4, 5, 18, 6, 4, 3, 5, 40, 30, 30, A_SPIDER_L, false ),
		P( -1, 3, 4, 1, 18, 6, 0, 3, 5, 40, 30, 30, A_SPIDER_R, false ),
		P( 3, -18, 4, 5, -3, 6, 4, -3, 5, 40, 30, 30, A_SPIDER_R, false ),
		P( -1, -18, 4, 1, -3, 6, 0, -3, 5, 40, 30, 30, A_SPIDER_L, false ) }, 7 },
	{ "pig", 16, {
		P( 4, 1, 0, 8, 5, 6, 6, 3, 6, 240, 160, 160, A_LEG_L, false ),
		P( 4, -5, 0, 8, -1, 6, 6, -3, 6, 240, 160, 160, A_LEG_R, false ),
		P( -8, 1, 0, -4, 5, 6, -6, 3, 6, 240, 160, 160, A_LEG_R, false ),
		P( -8, -5, 0, -4, -1, 6, -6, -3, 6, 240, 160, 160, A_LEG_L, false ),
		P( -8, -5, 6, 8, 5, 14, 0, 0, 6, 245, 170, 170, A_NONE, false ),
		P( 8, -4, 6, 16, 4, 14, 8, 0, 10, 245, 175, 175, A_HEAD, true ) }, 6 },
	{ "cow", 22, {
		P( 5, 2, 0, 9, 6, 12, 7, 4, 12, 70, 50, 35, A_LEG_L, false ),
		P( 5, -6, 0, 9, -2, 12, 7, -4, 12, 70, 50, 35, A_LEG_R, false ),
		P( -9, 2, 0, -5, 6, 12, -7, 4, 12, 70, 50, 35, A_LEG_R, false ),
		P( -9, -6, 0, -5, -2, 12, -7, -4, 12, 70, 50, 35, A_LEG_L, false ),
		P( -9, -6, 12, 9, 6, 22, 0, 0, 12, 80, 60, 45, A_NONE, false ),
		P( 9, -4, 14, 15, 4, 22, 9, 0, 18, 90, 70, 55, A_HEAD, true ) }, 6 },
	{ "sheep", 20, {
		P( 5, 2, 0, 9, 6, 12, 7, 4, 12, 220, 200, 180, A_LEG_L, false ),
		P( 5, -6, 0, 9, -2, 12, 7, -4, 12, 220, 200, 180, A_LEG_R, false ),
		P( -9, 2, 0, -5, 6, 12, -7, 4, 12, 220, 200, 180, A_LEG_R, false ),
		P( -9, -6, 0, -5, -2, 12, -7, -4, 12, 220, 200, 180, A_LEG_L, false ),
		P( -9, -6, 10, 9, 6, 20, 0, 0, 10, 235, 235, 235, A_NONE, false ),
		P( 9, -3, 12, 15, 3, 20, 9, 0, 16, 200, 180, 165, A_HEAD, true ) }, 6 },
	{ "chicken", 12, {
		P( -1, 1, 0, 1, 2, 5, 0, 1, 5, 230, 180, 40, A_LEG_L, false ),
		P( -1, -2, 0, 1, -1, 5, 0, -1, 5, 230, 180, 40, A_LEG_R, false ),
		P( -4, -3, 5, 4, 3, 11, 0, 0, 5, 250, 250, 250, A_NONE, false ),
		P( 3, -2, 9, 6, 2, 15, 4, 0, 11, 250, 250, 250, A_HEAD, true ) }, 4 },
	{ "wolf", 14, {
		P( 4, 1, 0, 6, 3, 8, 5, 2, 8, 200, 200, 200, A_LEG_L, false ),
		P( 4, -3, 0, 6, -1, 8, 5, -2, 8, 200, 200, 200, A_LEG_R, false ),
		P( -6, 1, 0, -4, 3, 8, -5, 2, 8, 200, 200, 200, A_LEG_R, false ),
		P( -6, -3, 0, -4, -1, 8, -5, -2, 8, 200, 200, 200, A_LEG_L, false ),
		P( -7, -3, 8, 6, 3, 14, 0, 0, 8, 215, 215, 215, A_NONE, false ),
		P( 6, -3, 9, 12, 3, 15, 6, 0, 12, 220, 220, 220, A_HEAD, true ) }, 6 },
	{ "slime", 8, { P( -4, -4, 0, 4, 4, 8, 0, 0, 0, 110, 200, 90, A_NONE, true ) }, 1 },
	{ "magma_cube", 8, { P( -4, -4, 0, 4, 4, 8, 0, 0, 0, 160, 40, 20, A_NONE, true ) }, 1 },
};

static const MobModel *FindModel( const char *type )
{
	for ( int i = 0; i < ARRAYSIZE( kModels ); ++i )
		if ( Q_strstr( type, kModels[i].match ) ) return &kModels[i];
	return NULL;
}

// Rotate p (pixels) around pivot about the local Y axis (pitch / leg swing), then optionally Z (head yaw)
static Vector RotatePart( const Vector &p, const float pivot[3], float pitchDeg )
{
	float r = DEG2RAD( pitchDeg ), c = cosf( r ), s = sinf( r );
	Vector d( p.x - pivot[0], p.y - pivot[1], p.z - pivot[2] );
	Vector o( d.x * c + d.z * s, d.y, -d.x * s + d.z * c );
	return Vector( o.x + pivot[0], o.y + pivot[1], o.z + pivot[2] );
}

static void DrawPart( CMeshBuilder &mb, const MobPart &part, float pitchDeg, float pxToUnits, const matrix3x4_t &xf,
					  const unsigned char rgb[3], float light, bool flash )
{
	static const int f[6][4] = { { 0, 4, 6, 2 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 2, 3, 1 }, { 4, 5, 7, 6 } };
	static const float shade[6] = { 0.65f, 0.9f, 0.8f, 0.8f, 0.5f, 1.0f };   // -x +x -y +y -z +z
	Vector c[8];
	for ( int i = 0; i < 8; ++i )
	{
		Vector p( ( i & 1 ) ? part.mx[0] : part.mn[0], ( i & 2 ) ? part.mx[1] : part.mn[1], ( i & 4 ) ? part.mx[2] : part.mn[2] );
		if ( pitchDeg != 0.f ) p = RotatePart( p, part.pivot, pitchDeg );
		VectorTransform( p * pxToUnits, xf, c[i] );
	}
	for ( int fi = 0; fi < 6; ++fi )
	{
		float s = shade[fi] * light * ( part.face && fi == 1 ? 0.55f : 1.f );
		unsigned char r = (unsigned char)MIN( 255.f, rgb[0] * s ), g = (unsigned char)MIN( 255.f, rgb[1] * s ), b = (unsigned char)MIN( 255.f, rgb[2] * s );
		if ( flash ) { r = (unsigned char)MIN( 255, r + 120 ); g = g / 2; b = b / 2; }
		for ( int k = 0; k < 4; ++k )
		{
			mb.Position3fv( c[ f[fi][k] ].Base() );
			mb.Color4ub( r, g, b, 255 );
			mb.TexCoord2f( 0, 0, 0 );
			mb.AdvanceVertex();
		}
	}
}

void CMCMob::GetRenderBounds( Vector &mins, Vector &maxs )
{
	if ( GetModel() ) { BaseClass::GetRenderBounds( mins, maxs ); return; }
	float r = MAX( CollisionProp()->OBBMaxs().x, CollisionProp()->OBBMaxs().z ) * 1.6f + 16.f;
	mins = Vector( -r, -r, -8 );
	maxs = Vector( r, r, CollisionProp()->OBBMaxs().z + 24.f );
}

// Light at the mob's position (sky light follows the day, block light from torches)
static float MobLight( const Vector &pos )
{
	CMCBridge &b = MCBridge();
	if ( !b.IsReady() ) return 1.f;
	mc::Vec3d m = b.Coords().ToMC( MC_V( pos + Vector( 0, 0, 8 ) ) );
	int x = mc::FloorI( m.x ), y = mc::FloorI( m.y ), z = mc::FloorI( m.z );
	float sky = mc::LightCurve( (float)b.World().SkyLight( x, y, z ) ) * b.DaylightFactor();
	float blk = mc::LightCurve( (float)b.World().BlockLight( x, y, z ) );
	return MAX( 0.15f, MAX( sky, blk ) );
}

int CMCMob::DrawModel( int flags )
{
	if ( GetModel() )
		return BaseClass::DrawModel( flags );
	if ( !mc_mob_boxes.GetBool() )
		return 0;

	static CMaterialReference s_mat, s_items;
	if ( !s_mat.IsValid() ) s_mat.Init( "mc/flat", TEXTURE_GROUP_OTHER );

	const char *type = m_szMCType.Get();
	const float light = MobLight( GetRenderOrigin() );
	const bool flash = m_iHurtTicks > 0;
	Vector mins = CollisionProp()->OBBMins(), maxs = CollisionProp()->OBBMaxs();
	CMatRenderContextPtr pRenderContext( materials );

	// ---- dropped items: spinning, bobbing icon sprite
	if ( m_iMCFlags & mc::ENT_ITEM )
	{
		const char *slash = Q_strstr( type, "/" );
		const char *item = slash ? slash + 1 : type;
		float s0 = 0, t0 = 0, s1 = 1, t1 = 1;
		bool icon = MC_ItemIconUV( item, s0, t0, s1, t1 );
		if ( icon && !s_items.IsValid() ) s_items.Init( "mc/items_world", TEXTURE_GROUP_OTHER );
		float spin = gpGlobals->curtime * 120.f + m_iMCId * 37.f;
		float bob = sinf( gpGlobals->curtime * 2.f + m_iMCId ) * 4.f + 8.f;
		float half = MAX( maxs.x, 8.f ) * 1.4f;
		Vector right;
		AngleVectors( QAngle( 0, spin, 0 ), NULL, &right, NULL );
		Vector base = GetRenderOrigin() + Vector( 0, 0, bob );
		unsigned char rgb[3] = { 255, 255, 255 };
		if ( !icon ) MC_ItemColor( item, rgb );
		unsigned char l = (unsigned char)( 255 * light );
		pRenderContext->Bind( icon ? (IMaterial *)s_items : (IMaterial *)s_mat );
		IMesh *pMesh = pRenderContext->GetDynamicMesh();
		CMeshBuilder mb;
		mb.Begin( pMesh, MATERIAL_QUADS, 1 );
		Vector p[4] = { base - right * half + Vector( 0, 0, 2 * half ), base + right * half + Vector( 0, 0, 2 * half ), base + right * half, base - right * half };
		float uv[4][2] = { { s0, t0 }, { s1, t0 }, { s1, t1 }, { s0, t1 } };
		for ( int k = 0; k < 4; ++k )
		{
			mb.Position3fv( p[k].Base() );
			mb.Color4ub( (unsigned char)( rgb[0] * l / 255 ), (unsigned char)( rgb[1] * l / 255 ), (unsigned char)( rgb[2] * l / 255 ), 255 );
			mb.TexCoord2f( 0, uv[k][0], uv[k][1] );
			mb.AdvanceVertex();
		}
		mb.End();
		pMesh->Draw();
		return 1;
	}

	pRenderContext->Bind( s_mat );
	matrix3x4_t xf;
	AngleMatrix( GetRenderAngles(), GetRenderOrigin(), xf );

	// Walk cycle from how fast the proxy moves
	Vector vel = ( GetRenderOrigin() - m_vecLastOrigin ) / MAX( gpGlobals->frametime, 0.001f );
	m_vecLastOrigin = GetRenderOrigin();
	float speed = vel.Length2D();
	m_flWalkSpeed = Approach( MIN( speed, 400.f ), m_flWalkSpeed, gpGlobals->frametime * 800.f );
	m_flWalkPhase += gpGlobals->frametime * ( 4.f + m_flWalkSpeed * 0.04f );
	float swing = MIN( 45.f, m_flWalkSpeed * 0.35f ) * sinf( m_flWalkPhase );

	const MobModel *model = FindModel( type );
	IMesh *pMesh = pRenderContext->GetDynamicMesh();
	CMeshBuilder mb;
	if ( model )
	{
		float pxToUnits = ( maxs.z - mins.z ) / model->height;
		mb.Begin( pMesh, MATERIAL_QUADS, model->count * 6 );
		for ( int i = 0; i < model->count; ++i )
		{
			const MobPart &part = model->parts[i];
			float pitch = 0.f;
			switch ( part.anim )
			{
			case A_LEG_L: case A_ARM_R: case A_SPIDER_L: pitch = swing; break;
			case A_LEG_R: case A_ARM_L: case A_SPIDER_R: pitch = -swing; break;
			case A_ARMS_FORWARD_L: case A_ARMS_FORWARD_R: pitch = -90.f + swing * 0.2f; break;
			case A_HEAD: pitch = m_flHeadPitch; break;
			}
			if ( part.anim == A_SPIDER_L || part.anim == A_SPIDER_R ) pitch *= 0.5f;
			DrawPart( mb, part, pitch, pxToUnits, xf, part.rgb, light, flash );
		}
		mb.End();
	}
	else
	{
		// Unknown entity: body + head box sized to the hitbox
		unsigned char rgb[3];
		MC_ItemColor( type, rgb );
		MobPart body = { { mins.x, mins.y, 0 }, { maxs.x, maxs.y, maxs.z * 0.75f }, { 0, 0, 0 }, { rgb[0], rgb[1], rgb[2] }, A_NONE, false };
		MobPart head = { { mins.x * 0.8f, mins.y * 0.8f, maxs.z * 0.75f }, { maxs.x * 0.8f, maxs.y * 0.8f, maxs.z }, { 0, 0, maxs.z * 0.75f }, { rgb[0], rgb[1], rgb[2] }, A_HEAD, true };
		mb.Begin( pMesh, MATERIAL_QUADS, 12 );
		DrawPart( mb, body, 0.f, 1.f, xf, body.rgb, light, flash );
		DrawPart( mb, head, m_flHeadPitch, 1.f, xf, head.rgb, light, flash );
		mb.End();
	}
	pMesh->Draw();

	// Health bar over damaged mobs
	if ( mc_mob_healthbars.GetBool() && ( m_iMCFlags & mc::ENT_LIVING ) && m_flMCMaxHealth > 0 && m_flMCHealth < m_flMCMaxHealth )
	{
		Vector right, up;
		AngleVectors( MainViewAngles(), NULL, &right, &up );
		Vector top = GetRenderOrigin() + Vector( 0, 0, maxs.z + 12.f );
		float w = 24.f, h = 3.f, frac = clamp( m_flMCHealth / m_flMCMaxHealth, 0.f, 1.f );
		IMesh *pBar = pRenderContext->GetDynamicMesh();
		CMeshBuilder bb;
		bb.Begin( pBar, MATERIAL_QUADS, 2 );
		for ( int q = 0; q < 2; ++q )
		{
			float x0 = -w, x1 = q == 0 ? w : -w + 2 * w * frac;
			unsigned char r = q == 0 ? 20 : (unsigned char)( 255 * ( 1 - frac ) ), g = q == 0 ? 20 : (unsigned char)( 255 * frac ), b = 20;
			Vector p[4] = { top + right * x0 + up * h, top + right * x1 + up * h, top + right * x1 - up * h, top + right * x0 - up * h };
			for ( int k = 0; k < 4; ++k )
			{
				Vector pk = p[k] + ( q ? Vector( 0, 0, 0.1f ) : vec3_origin );
				bb.Position3fv( pk.Base() );
				bb.Color4ub( r, g, b, 255 );
				bb.TexCoord2f( 0, 0, 0 );
				bb.AdvanceVertex();
			}
		}
		bb.End();
		pBar->Draw();
	}
	return 1;
}
#endif
