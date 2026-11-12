// Builds a small procedural scene with the real core mesher and writes the Source-space quads
// (positions + vertex colours) to a text file for tests/render_preview.py.
#include "mc_mesher.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
using namespace mc;

int main( int argc, char **argv )
{
	const float day = argc > 2 ? (float)atof( argv[2] ) : 1.f;   // 1 = noon, ~0.2 = night
	VoxelWorld w; w.Reset( -4, 24 );
	BlockRegistry &R = w.Registry();
	uint16_t stone = R.Intern( "stone" ), dirt = R.Intern( "dirt" ), grass = R.Intern( "grass_block" ), water = R.Intern( "water" ),
			 log = R.Intern( "oak_log" ), leaves = R.Intern( "oak_leaves" ), sand = R.Intern( "sand" ), flower = R.Intern( "poppy" ),
			 tall = R.Intern( "short_grass" ), slab = R.Intern( "cobblestone_slab" ), glass = R.Intern( "glass" );
	for ( int cx = 0; cx < 2; ++cx ) for ( int cz = 0; cz < 2; ++cz )
	{
		std::vector<uint16_t> pal( 1, 0 ); uint16_t idx[4096] = { 0 };
		w.SetSection( cx, 4, cz, pal, idx );   // loads column (empty section)
	}
	for ( int x = 0; x < 32; ++x ) for ( int z = 0; z < 32; ++z )
	{
		int h = 66 + (int)std::round( 3 * std::sin( x * 0.25 ) * std::cos( z * 0.2 ) );
		for ( int y = 60; y <= h; ++y ) w.SetBlock( x, y, z, y == h ? ( h <= 64 ? sand : grass ) : y > h - 3 ? dirt : stone );
		for ( int y = h + 1; y <= 64; ++y ) w.SetBlock( x, y, z, water );
		if ( h > 66 && ( x * 7 + z * 13 ) % 11 == 0 ) w.SetBlock( x, h + 1, z, ( x + z ) % 2 ? flower : tall );
	}
	int tx = 22, tz = 9, ty = 69;
	for ( int y = 0; y < 5; ++y ) w.SetBlock( tx, ty + y, tz, log );
	for ( int dx = -2; dx <= 2; ++dx ) for ( int dz = -2; dz <= 2; ++dz ) for ( int dy = 3; dy <= 5; ++dy )
		if ( abs( dx ) + abs( dz ) + ( dy - 3 ) < 5 && !( dx == 0 && dz == 0 && dy < 5 ) ) w.SetBlock( tx + dx, ty + dy, tz + dz, leaves );
	for ( int i = 0; i < 6; ++i ) { w.SetBlock( 6 + i, 70, 24, slab ); w.SetBlock( 6 + i, 69, 24, stone ); }
	w.SetBlock( 14, 70, 20, glass ); w.SetBlock( 15, 70, 20, glass ); w.SetBlock( 14, 71, 20, glass );
	// A stone hut with a torch inside (block light) and an open front
	for ( int x = 3; x <= 9; ++x ) for ( int z = 3; z <= 9; ++z ) for ( int y = 68; y <= 72; ++y )
	{
		bool wall = x == 3 || x == 9 || z == 3 || z == 9 || y == 72;
		bool door = z == 9 && x >= 5 && x <= 7 && y <= 70;
		w.SetBlock( x, y, z, wall && !door ? stone : 0 );
	}
	uint16_t torch = R.Intern( "torch" );
	w.SetBlock( 6, 68, 6, torch );

	// Light: sky 15 down to the first opaque block (then 0), torch light 14 - manhattan distance
	for ( int cx = 0; cx < 2; ++cx ) for ( int cz = 0; cz < 2; ++cz ) for ( int sy = 3; sy < 6; ++sy )
	{
		static uint8_t sky[2048], blk[2048];
		memset( sky, 0, sizeof( sky ) ); memset( blk, 0, sizeof( blk ) );
		for ( int ly = 0; ly < 16; ++ly ) for ( int lz = 0; lz < 16; ++lz ) for ( int lx = 0; lx < 16; ++lx )
		{
			int x = cx * 16 + lx, y = sy * 16 + ly, z = cz * 16 + lz;
			int s = 15;
			for ( int yy = y; yy < 96; ++yy ) if ( w.GetDef( x, yy, z ).opaque ) { s = 0; break; }
			if ( s == 0 && !w.GetDef( x, y, z ).opaque )
			{
				// light leaking in through the doorway
				int d = abs( x - 6 ) + abs( z - 10 ) + abs( y - 69 );
				s = std::max( 0, 13 - d );
			}
			int b = std::max( 0, 14 - ( abs( x - 6 ) + abs( y - 68 ) + abs( z - 6 ) ) );
			int i = Section::Index( lx, ly, lz );
			sky[i >> 1] |= (uint8_t)( s << ( ( i & 1 ) * 4 ) );
			blk[i >> 1] |= (uint8_t)( b << ( ( i & 1 ) * 4 ) );
		}
		w.SetLight( cx, sy, cz, sky, blk );
	}

	Coords c; c.ox = 16; c.oz = 16; c.oy = 64;
	MesherOptions opt; opt.textured = false;
	FILE *f = fopen( argc > 1 ? argv[1] : "build/preview_quads.txt", "w" );
	size_t quads = 0;
	for ( int cx = 0; cx < 2; ++cx ) for ( int cz = 0; cz < 2; ++cz ) for ( int sy = 3; sy < 6; ++sy )
	{
		SectionMesh m; BuildSectionMesh( w, c, cx, sy, cz, opt, m );
		for ( int pass = 0; pass < 2; ++pass )
		{
			const std::vector<MeshVertex> &v = pass ? m.translucent : m.opaque;
			for ( size_t i = 0; i < v.size(); i += 4, ++quads )
			{
				fprintf( f, "%d", pass );
				for ( int k = 0; k < 4; ++k )
				{
					// Same as the client: sky pass * daylight + additive block-light pass * (1 - 0.7 day)
					const MeshVertex &mv = v[i + k];
					float l = LightCurve( mv.sky ) * day + std::max( 0.f, LightCurve( mv.blk ) - LightCurve( 0 ) ) * ( 1.f - 0.7f * day ) * ( pass ? 0.f : 1.f );
					l = std::min( 1.f, l );
					fprintf( f, " %.1f %.1f %.1f %d %d %d %d", mv.pos[0], mv.pos[1], mv.pos[2], (int)( mv.rgba[0] * l ), (int)( mv.rgba[1] * l ), (int)( mv.rgba[2] * l ), mv.rgba[3] );
				}
				fprintf( f, "\n" );
			}
		}
	}
	fclose( f );
	printf( "%zu quads\n", quads );
}
