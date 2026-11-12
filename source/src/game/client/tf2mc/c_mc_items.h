//========= TF2MC =========//
// Item icon atlas (materials/mc/items.vtf + scripts/mc_items.txt, built by tools/mc_assets.py).
//=========================//
#ifndef C_MC_ITEMS_H
#define C_MC_ITEMS_H
#pragma once

// Texture coordinates of an item's icon. Returns false if unknown (caller draws a coloured square).
bool MC_ItemIconUV( const char *itemId, float &s0, float &t0, float &s1, float &t1 );
// Stable fallback colour for an item without an icon.
void MC_ItemColor( const char *itemId, unsigned char rgb[3] );

#endif
