//========= TF2MC =========//
// Minecraft-mode HUD and screens:
//  - item icon atlas
//  - hotbar with icons, hunger / armour / XP, crosshair
//  - inventory / container screen (exact Minecraft slot layout: player inventory, crafting table,
//    furnaces, chests, villagers...) driven by the server's real menus
//  - client commands (mc_steve, mc_hotbar, mc_inventory, mc_drop)
//=========================//
#include "cbase.h"
#include "tf2mc/mc_bridge.h"
#include "c_mc_items.h"
#include "c_tf_player.h"
#include "tf_shareddefs.h"
#include "hudelement.h"
#include "iclientmode.h"
#include "filesystem.h"
#include "tier1/utlbuffer.h"
#include <vgui_controls/Panel.h>
#include <vgui/ISurface.h>
#include <vgui/IScheme.h>
#include <vgui/IInput.h>
#include <vgui/ILocalize.h>

#include "tier0/valve_minmax_off.h"
#include "tier0/memdbgoff.h"
#include <unordered_map>
#include <string>
#include <sstream>
#include "tier0/memdbgon.h"
#include "tier0/valve_minmax_on.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//=============================================================================
// Item icons
//=============================================================================
struct MCItemAtlas
{
	bool loaded = false;
	int tpr = 0, rows = 0;
	std::unordered_map<std::string, int> tiles;

	void Load()
	{
		loaded = true;
		CUtlBuffer buf( 0, 0, CUtlBuffer::TEXT_BUFFER );
		if ( !g_pFullFileSystem || !g_pFullFileSystem->ReadFile( "scripts/mc_items.txt", "MOD", buf ) )
			return;
		std::istringstream in( std::string( (const char *)buf.Base(), buf.TellPut() ) );
		std::string line, kind, id;
		while ( std::getline( in, line ) )
		{
			std::istringstream ls( line );
			if ( !( ls >> kind ) ) continue;
			if ( kind == "atlas" ) ls >> tpr >> rows;
			else if ( kind == "item" ) { int t = -1; ls >> id >> t; if ( t >= 0 ) tiles[id] = t; }
		}
	}
};
static MCItemAtlas g_ItemAtlas;

bool MC_ItemIconUV( const char *itemId, float &s0, float &t0, float &s1, float &t1 )
{
	if ( !g_ItemAtlas.loaded ) g_ItemAtlas.Load();
	if ( !itemId || !itemId[0] || g_ItemAtlas.tpr <= 0 ) return false;
	auto it = g_ItemAtlas.tiles.find( itemId );
	if ( it == g_ItemAtlas.tiles.end() ) return false;
	int col = it->second % g_ItemAtlas.tpr, row = it->second / g_ItemAtlas.tpr;
	const float in = 0.25f / 16.f;
	s0 = ( col + in ) / g_ItemAtlas.tpr; s1 = ( col + 1 - in ) / g_ItemAtlas.tpr;
	t0 = ( row + in ) / g_ItemAtlas.rows; t1 = ( row + 1 - in ) / g_ItemAtlas.rows;
	return true;
}

void MC_ItemColor( const char *itemId, unsigned char rgb[3] )
{
	unsigned int h = 2166136261u;
	for ( const char *p = itemId; p && *p; ++p ) { h ^= (unsigned char)*p; h *= 16777619u; }
	rgb[0] = 80 + ( h & 0x7F ); rgb[1] = 80 + ( ( h >> 8 ) & 0x7F ); rgb[2] = 80 + ( ( h >> 16 ) & 0x7F );
}

static int s_iItemTexture = -1;

// Draws an item icon (or a coloured tile) plus stack count and durability bar.
static void MC_DrawItem( const char *item, int count, int durability, int x, int y, int size, vgui::HFont font )
{
	if ( !item || !item[0] ) return;
	float s0, t0, s1, t1;
	if ( MC_ItemIconUV( item, s0, t0, s1, t1 ) )
	{
		if ( s_iItemTexture < 0 )
		{
			s_iItemTexture = vgui::surface()->CreateNewTextureID();
			vgui::surface()->DrawSetTextureFile( s_iItemTexture, "mc/items", true, false );
		}
		vgui::surface()->DrawSetColor( 255, 255, 255, 255 );
		vgui::surface()->DrawSetTexture( s_iItemTexture );
		vgui::surface()->DrawTexturedSubRect( x, y, x + size, y + size, s0, t0, s1, t1 );
	}
	else
	{
		unsigned char c[3];
		MC_ItemColor( item, c );
		vgui::surface()->DrawSetColor( c[0], c[1], c[2], 255 );
		vgui::surface()->DrawFilledRect( x + size / 8, y + size / 8, x + size - size / 8, y + size - size / 8 );
		char pretty[64];
		MC_PrettyItemName( item, pretty, sizeof( pretty ) );
		wchar_t w[4] = { (wchar_t)pretty[0], 0 };
		vgui::surface()->DrawSetTextFont( font );
		vgui::surface()->DrawSetTextColor( 0, 0, 0, 255 );
		vgui::surface()->DrawSetTextPos( x + size / 3, y + size / 4 );
		vgui::surface()->DrawUnicodeString( w );
	}
	if ( durability < 255 )
	{
		int bw = size - 4;
		vgui::surface()->DrawSetColor( 0, 0, 0, 255 );
		vgui::surface()->DrawFilledRect( x + 2, y + size - 4, x + 2 + bw, y + size - 2 );
		vgui::surface()->DrawSetColor( 255 - durability, durability, 0, 255 );
		vgui::surface()->DrawFilledRect( x + 2, y + size - 4, x + 2 + bw * durability / 255, y + size - 3 );
	}
	if ( count > 1 )
	{
		wchar_t wc[8];
		V_snwprintf( wc, ARRAYSIZE( wc ), L"%d", count );
		int tw, th;
		vgui::surface()->GetTextSize( font, wc, tw, th );
		vgui::surface()->DrawSetTextFont( font );
		vgui::surface()->DrawSetTextColor( 40, 40, 40, 255 );
		vgui::surface()->DrawSetTextPos( x + size - tw + 1, y + size - th + 1 );
		vgui::surface()->DrawUnicodeString( wc );
		vgui::surface()->DrawSetTextColor( 255, 255, 255, 255 );
		vgui::surface()->DrawSetTextPos( x + size - tw, y + size - th );
		vgui::surface()->DrawUnicodeString( wc );
	}
}

static bool MC_LocalIsSteve()
{
	C_TFPlayer *p = C_TFPlayer::GetLocalTFPlayer();
	return p && p->IsAlive() && p->IsPlayerClass( TF_CLASS_CIVILIAN );
}

//=============================================================================
// Hotbar, hunger, armour, XP, crosshair
//=============================================================================
class CHudMCHotbar : public CHudElement, public vgui::Panel
{
	DECLARE_CLASS_SIMPLE( CHudMCHotbar, vgui::Panel );
public:
	CHudMCHotbar( const char *pElementName ) : CHudElement( pElementName ), BaseClass( NULL, "HudMCHotbar" )
	{
		SetParent( g_pClientMode->GetViewport() );
		SetHiddenBits( HIDEHUD_PLAYERDEAD );
		m_hFont = vgui::INVALID_FONT;
	}

	virtual void ApplySchemeSettings( vgui::IScheme *pScheme )
	{
		BaseClass::ApplySchemeSettings( pScheme );
		m_hFont = pScheme->GetFont( "DefaultSmall", true );
		if ( m_hFont == vgui::INVALID_FONT ) m_hFont = pScheme->GetFont( "Default", true );
		SetPaintBackgroundEnabled( false );
		int sw, sh;
		vgui::surface()->GetScreenSize( sw, sh );
		SetBounds( 0, 0, sw, sh );
		SetMouseInputEnabled( false );
		SetKeyBoardInputEnabled( false );
	}

	virtual bool ShouldDraw()
	{
		return CHudElement::ShouldDraw() && MC_LocalIsSteve();
	}

	virtual void Paint()
	{
		int sw, sh;
		GetSize( sw, sh );
		int cell = sh / 16;
		int x0 = sw / 2 - cell * 9 / 2, y0 = sh - cell - sh / 60;

		// crosshair
		vgui::surface()->DrawSetColor( 255, 255, 255, 200 );
		vgui::surface()->DrawFilledRect( sw / 2 - 8, sh / 2 - 1, sw / 2 + 8, sh / 2 + 1 );
		vgui::surface()->DrawFilledRect( sw / 2 - 1, sh / 2 - 8, sw / 2 + 1, sh / 2 + 8 );

		MCHotbar &hb = MCBridge().Hotbar();
		for ( int i = 0; i < 9; ++i )
		{
			int x = x0 + i * cell;
			bool sel = hb.valid && i == hb.selected;
			vgui::surface()->DrawSetColor( 0, 0, 0, 140 );
			vgui::surface()->DrawFilledRect( x + 1, y0 + 1, x + cell - 1, y0 + cell - 1 );
			vgui::surface()->DrawSetColor( sel ? 255 : 110, sel ? 255 : 110, sel ? 255 : 110, 255 );
			vgui::surface()->DrawOutlinedRect( x + 1, y0 + 1, x + cell - 1, y0 + cell - 1 );
			if ( sel ) vgui::surface()->DrawOutlinedRect( x, y0, x + cell, y0 + cell );
			if ( hb.valid ) MC_DrawItem( hb.items[i], hb.counts[i], 255, x + cell / 8, y0 + cell / 8, cell - cell / 4, m_hFont );
		}

		// selected item name
		if ( hb.valid && hb.items[hb.selected][0] )
		{
			char pretty[64];
			wchar_t w[64];
			MC_PrettyItemName( hb.items[hb.selected], pretty, sizeof( pretty ) );
			g_pVGuiLocalize->ConvertANSIToUnicode( pretty, w, sizeof( w ) );
			int tw, th;
			vgui::surface()->GetTextSize( m_hFont, w, tw, th );
			vgui::surface()->DrawSetTextFont( m_hFont );
			vgui::surface()->DrawSetTextColor( 255, 255, 255, 230 );
			vgui::surface()->DrawSetTextPos( sw / 2 - tw / 2, y0 - cell * 3 / 2 - th );
			vgui::surface()->DrawUnicodeString( w );
		}

		MCStatus &st = MCBridge().Status();
		if ( !st.valid ) return;
		// XP bar
		int barY = y0 - cell / 4 - 3, barW = cell * 9;
		vgui::surface()->DrawSetColor( 0, 0, 0, 180 );
		vgui::surface()->DrawFilledRect( x0, barY, x0 + barW, barY + 4 );
		vgui::surface()->DrawSetColor( 128, 255, 32, 255 );
		vgui::surface()->DrawFilledRect( x0, barY, x0 + (int)( barW * clamp( st.xpProgress, 0.f, 1.f ) ), barY + 4 );
		if ( st.xpLevel > 0 )
		{
			wchar_t w[16];
			V_snwprintf( w, ARRAYSIZE( w ), L"%d", st.xpLevel );
			int tw, th;
			vgui::surface()->GetTextSize( m_hFont, w, tw, th );
			vgui::surface()->DrawSetTextFont( m_hFont );
			vgui::surface()->DrawSetTextColor( 128, 255, 32, 255 );
			vgui::surface()->DrawSetTextPos( sw / 2 - tw / 2, barY - th );
			vgui::surface()->DrawUnicodeString( w );
		}
		// hunger (right) and armour (left): 10 pips each, 2 points per pip
		int pip = cell / 3, rowY = barY - pip - 4;
		for ( int i = 0; i < 10; ++i )
		{
			float food = st.food - i * 2;
			int fx = x0 + barW - ( i + 1 ) * ( pip + 2 );
			vgui::surface()->DrawSetColor( 40, 25, 10, 200 );
			vgui::surface()->DrawFilledRect( fx, rowY, fx + pip, rowY + pip );
			if ( food > 0 )
			{
				vgui::surface()->DrawSetColor( 205, 130, 60, 255 );
				vgui::surface()->DrawFilledRect( fx, rowY, food >= 2 ? fx + pip : fx + pip / 2, rowY + pip );
			}
			float armor = st.armor - i * 2;
			if ( st.armor > 0 )
			{
				int ax = x0 + i * ( pip + 2 );
				vgui::surface()->DrawSetColor( 30, 30, 30, 200 );
				vgui::surface()->DrawFilledRect( ax, rowY, ax + pip, rowY + pip );
				if ( armor > 0 )
				{
					vgui::surface()->DrawSetColor( 200, 200, 210, 255 );
					vgui::surface()->DrawFilledRect( ax, rowY, armor >= 2 ? ax + pip : ax + pip / 2, rowY + pip );
				}
			}
		}
	}

private:
	vgui::HFont m_hFont;
};
DECLARE_HUDELEMENT( CHudMCHotbar );

//=============================================================================
// Inventory / container screen
//=============================================================================
class CMCInventoryPanel : public vgui::Panel
{
	DECLARE_CLASS_SIMPLE( CMCInventoryPanel, vgui::Panel );
public:
	CMCInventoryPanel( vgui::Panel *parent ) : BaseClass( parent, "MCInventory" ), m_hFont( vgui::INVALID_FONT ), m_iScale( 3 ), m_bWasOpen( false )
	{
		SetVisible( false );
		MakePopup( false, false );
		SetMouseInputEnabled( true );
		SetKeyBoardInputEnabled( true );
		SetPaintBackgroundEnabled( false );
	}

	virtual void ApplySchemeSettings( vgui::IScheme *pScheme )
	{
		BaseClass::ApplySchemeSettings( pScheme );
		m_hFont = pScheme->GetFont( "Default", true );
		SetPaintBackgroundEnabled( false );
	}

	void Close()
	{
		engine->ServerCmd( "mc_inv_close" );
		MCBridge().Container().open = false;
		SetVisible( false );
	}

	virtual void OnThink()
	{
		bool open = MCBridge().Container().open && MCBridge().IsReady();
		if ( open && !IsVisible() )
		{
			Layout();
			SetVisible( true );
			MoveToFront();
			RequestFocus();
		}
		else if ( !open && IsVisible() )
		{
			SetVisible( false );
		}
		if ( open ) Layout();
	}

	void Layout()
	{
		const MCContainer &c = MCBridge().Container();
		int maxX = 176, maxY = 166;
		for ( size_t i = 0; i < c.slots.size(); ++i ) { maxX = MAX( maxX, c.slots[i].x + 16 + 7 ); maxY = MAX( maxY, c.slots[i].y + 16 + 7 ); }
		int sw, sh;
		vgui::surface()->GetScreenSize( sw, sh );
		m_iScale = MAX( 1, MIN( ( sh - 40 ) / maxY, ( sw - 40 ) / maxX ) );
		m_iScale = MIN( m_iScale, 4 );
		m_iW = maxX * m_iScale; m_iH = maxY * m_iScale;
		SetBounds( 0, 0, sw, sh );
		m_iX = ( sw - m_iW ) / 2; m_iY = ( sh - m_iH ) / 2;
	}

	int SlotAt( int mx, int my ) const
	{
		const MCContainer &c = MCBridge().Container();
		for ( size_t i = 0; i < c.slots.size(); ++i )
		{
			int x = m_iX + ( c.slots[i].x - 1 ) * m_iScale, y = m_iY + ( c.slots[i].y - 1 ) * m_iScale;
			if ( mx >= x && mx < x + 18 * m_iScale && my >= y && my < y + 18 * m_iScale ) return (int)i;
		}
		bool inside = mx >= m_iX && mx < m_iX + m_iW && my >= m_iY && my < m_iY + m_iH;
		return inside ? -1 : -999;   // -999 = outside the window: drop the carried stack
	}

	void Click( int slot, int button, int type )
	{
		char cmd[96];
		Q_snprintf( cmd, sizeof( cmd ), "mc_click %d %d %d %d", MCBridge().Container().containerId, slot, button, type );
		engine->ServerCmd( cmd );
	}

	virtual void OnMousePressed( vgui::MouseCode code )
	{
		int mx, my;
		vgui::input()->GetCursorPos( mx, my );
		ScreenToLocal( mx, my );
		int slot = SlotAt( mx, my );
		if ( slot == -1 ) return;
		int button = code == MOUSE_RIGHT ? 1 : code == MOUSE_MIDDLE ? 2 : 0;
		bool shift = vgui::input()->IsKeyDown( KEY_LSHIFT ) || vgui::input()->IsKeyDown( KEY_RSHIFT );
		if ( code == MOUSE_MIDDLE ) Click( slot, 2, mc::CLICK_CLONE );
		else Click( slot, button, shift && slot >= 0 ? mc::CLICK_QUICK_MOVE : mc::CLICK_PICKUP );
	}

	virtual void OnMouseDoublePressed( vgui::MouseCode code )
	{
		int mx, my;
		vgui::input()->GetCursorPos( mx, my );
		ScreenToLocal( mx, my );
		int slot = SlotAt( mx, my );
		if ( slot >= 0 && code == MOUSE_LEFT ) Click( slot, 0, mc::CLICK_PICKUP_ALL );
	}

	virtual void OnKeyCodePressed( vgui::KeyCode code )
	{
		if ( code == KEY_ESCAPE || code == KEY_E || code == KEY_TAB ) { Close(); return; }
		int mx, my;
		vgui::input()->GetCursorPos( mx, my );
		ScreenToLocal( mx, my );
		int slot = SlotAt( mx, my );
		if ( slot < 0 ) return;
		if ( code >= KEY_1 && code <= KEY_9 ) Click( slot, code - KEY_1, mc::CLICK_SWAP );   // swap with hotbar slot
		else if ( code == KEY_Q ) Click( slot, vgui::input()->IsKeyDown( KEY_LCONTROL ) ? 1 : 0, mc::CLICK_THROW );
		else if ( code == KEY_F ) Click( slot, 40, mc::CLICK_SWAP );   // offhand
	}

	virtual void Paint()
	{
		const MCContainer &c = MCBridge().Container();
		int k = m_iScale;
		// dim the game
		vgui::surface()->DrawSetColor( 0, 0, 0, 120 );
		vgui::surface()->DrawFilledRect( 0, 0, GetWide(), GetTall() );
		// window
		vgui::surface()->DrawSetColor( 198, 198, 198, 255 );
		vgui::surface()->DrawFilledRect( m_iX, m_iY, m_iX + m_iW, m_iY + m_iH );
		vgui::surface()->DrawSetColor( 255, 255, 255, 255 );
		vgui::surface()->DrawOutlinedRect( m_iX, m_iY, m_iX + m_iW, m_iY + m_iH );

		char title[64];
		const char *t = Q_strstr( c.type, ":" );
		MC_PrettyItemName( t ? t + 1 : c.type, title, sizeof( title ) );
		wchar_t wt[64];
		g_pVGuiLocalize->ConvertANSIToUnicode( title, wt, sizeof( wt ) );
		vgui::surface()->DrawSetTextFont( m_hFont );
		vgui::surface()->DrawSetTextColor( 64, 64, 64, 255 );
		vgui::surface()->DrawSetTextPos( m_iX + 8 * k, m_iY + 4 * k );
		vgui::surface()->DrawUnicodeString( wt );

		int mx, my;
		vgui::input()->GetCursorPos( mx, my );
		ScreenToLocal( mx, my );
		int hover = SlotAt( mx, my );

		for ( size_t i = 0; i < c.slots.size(); ++i )
		{
			const MCSlotView &v = c.slots[i];
			int x = m_iX + ( v.x - 1 ) * k, y = m_iY + ( v.y - 1 ) * k;
			vgui::surface()->DrawSetColor( 139, 139, 139, 255 );
			vgui::surface()->DrawFilledRect( x, y, x + 18 * k, y + 18 * k );
			vgui::surface()->DrawSetColor( 55, 55, 55, 255 );
			vgui::surface()->DrawOutlinedRect( x, y, x + 18 * k, y + 18 * k );
			if ( (int)i == hover )
			{
				vgui::surface()->DrawSetColor( 255, 255, 255, 90 );
				vgui::surface()->DrawFilledRect( x + k, y + k, x + 17 * k, y + 17 * k );
			}
			MC_DrawItem( v.item, v.count, v.durability, x + k, y + k, 16 * k, m_hFont );
		}

		// carried stack follows the cursor
		if ( c.carried[0] )
			MC_DrawItem( c.carried, c.carriedCount, 255, mx - 8 * k, my - 8 * k, 16 * k, m_hFont );
		else if ( hover >= 0 && c.slots[hover].item[0] )
		{
			char pretty[64];
			wchar_t w[64];
			MC_PrettyItemName( c.slots[hover].item, pretty, sizeof( pretty ) );
			g_pVGuiLocalize->ConvertANSIToUnicode( pretty, w, sizeof( w ) );
			int tw, th;
			vgui::surface()->GetTextSize( m_hFont, w, tw, th );
			vgui::surface()->DrawSetColor( 16, 0, 16, 235 );
			vgui::surface()->DrawFilledRect( mx + 12, my - th - 6, mx + 20 + tw, my );
			vgui::surface()->DrawSetTextColor( 255, 255, 255, 255 );
			vgui::surface()->DrawSetTextPos( mx + 16, my - th - 3 );
			vgui::surface()->DrawUnicodeString( w );
		}
	}

private:
	vgui::HFont m_hFont;
	int m_iScale, m_iX = 0, m_iY = 0, m_iW = 0, m_iH = 0;
	bool m_bWasOpen;
};

static vgui::DHANDLE<CMCInventoryPanel> s_hInventory;

class CMCHudSystem : public CAutoGameSystemPerFrame
{
public:
	CMCHudSystem() : CAutoGameSystemPerFrame( "CMCHudSystem" ) {}
	virtual void Update( float frametime )
	{
		if ( !s_hInventory.Get() && g_pClientMode && g_pClientMode->GetViewport() )
			s_hInventory = new CMCInventoryPanel( g_pClientMode->GetViewport() );
	}
	virtual void LevelShutdownPostEntity()
	{
		if ( s_hInventory.Get() ) s_hInventory->SetVisible( false );
	}
};
static CMCHudSystem g_MCHudSystem;

//=============================================================================
// Client commands
//=============================================================================
static void MC_SelectHotbar( int slot )
{
	slot = ( slot % 9 + 9 ) % 9;
	MCBridge().Hotbar().selected = slot;   // predict; server confirms via INVENTORY
	char cmd[32];
	Q_snprintf( cmd, sizeof( cmd ), "mc_select %d", slot );
	engine->ServerCmd( cmd );
}

CON_COMMAND( mc_hotbar, "mc_hotbar <1-9>: select a Minecraft hotbar slot" )
{
	if ( args.ArgC() > 1 ) MC_SelectHotbar( atoi( args[1] ) - 1 );
}
CON_COMMAND( mc_hotbar_next, "Next Minecraft hotbar slot" ) { MC_SelectHotbar( MCBridge().Hotbar().selected + 1 ); }
CON_COMMAND( mc_hotbar_prev, "Previous Minecraft hotbar slot" ) { MC_SelectHotbar( MCBridge().Hotbar().selected - 1 ); }

CON_COMMAND( mc_inventory, "Open/close the Minecraft inventory (works for every class)" )
{
	if ( MCBridge().Container().open && s_hInventory.Get() ) { s_hInventory->Close(); return; }
	engine->ServerCmd( "mc_inv" );
}

CON_COMMAND( mc_drop, "Drop the held Minecraft item (mc_drop 1 = whole stack)" )
{
	engine->ServerCmd( args.ArgC() > 1 && atoi( args[1] ) ? "mc_drop_item 1" : "mc_drop_item 0" );
}

CON_COMMAND( mc_steve, "Play as a regular Minecraft player (Civilian slot)" )
{
	engine->ClientCmd_Unrestricted( "joinclass civilian" );
	engine->ClientCmd_Unrestricted( "exec tf2mc_steve_binds" );
}

CON_COMMAND( mc_tfbinds, "Restore normal TF2 weapon binds after Minecraft mode" )
{
	engine->ClientCmd_Unrestricted( "exec tf2mc_tf_binds" );
}
