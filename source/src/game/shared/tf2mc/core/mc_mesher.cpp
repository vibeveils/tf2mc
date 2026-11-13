//========= TF2MC bridge - engine independent core =========//
#include "mc_mesher.h"

#include <algorithm>

namespace mc
{
	uint32_t TintColor( Tint t )
	{
		switch ( t )   // 0xRRGGBB
		{
		case TINT_GRASS:   return 0x91BD59;
		case TINT_FOLIAGE: return 0x77AB2F;
		case TINT_WATER:   return 0x3F76E4;
		case TINT_SPRUCE:  return 0x619961;
		case TINT_BIRCH:   return 0x80A755;
		case TINT_DRY_FOLIAGE: return 0xA47A4A;   // leaf litter (dry foliage colormap, temperate)
		case TINT_REDSTONE: return 0xB01010;      // mid-power wire
		case TINT_LILY:    return 0x208030;
		case TINT_STEM:    return 0xA0C030;
		default:           return 0xFFFFFF;
		}
	}

	float LightCurve( float level )
	{
		float f = level / 15.f;
		float b = f / ( 4.f - 3.f * f );
		return 0.06f + 0.94f * b;
	}

	namespace
	{
		const float kFaceShade[6] = { 0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f };
		const float kAOLevels[4] = { 0.45f, 0.65f, 0.82f, 1.0f };
		const int kNormal[6][3] = { { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 } };

		// Corner order (CCW seen from outside, MC space) in local unit coords per face.
		const float kCorners[6][4][3] = {
			{ { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } },   // down
			{ { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },   // up
			{ { 1, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 } },   // north
			{ { 0, 1, 1 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 } },   // south
			{ { 0, 1, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 } },   // west
			{ { 1, 1, 1 }, { 1, 0, 1 }, { 1, 0, 0 }, { 1, 1, 0 } },   // east
		};

		void FaceUV( int face, float lx, float ly, float lz, float &u, float &v )
		{
			switch ( face )
			{
			case FACE_DOWN:  u = lx;     v = lz;     break;
			case FACE_UP:    u = lx;     v = lz;     break;
			case FACE_NORTH: u = 1 - lx; v = 1 - ly; break;
			case FACE_SOUTH: u = lx;     v = 1 - ly; break;
			case FACE_WEST:  u = lz;     v = 1 - ly; break;
			default:         u = 1 - lz; v = 1 - ly; break;
			}
		}

		struct Ctx
		{
			const VoxelWorld &w;
			const Coords &c;
			const MesherOptions &opt;
			const BlockRegistry &reg;
			int16_t waterTile;
		};

		// Full opaque block: hides neighbouring faces, darkens AO. (Models count when their
		// outline is a full cube - opaque is only set then.)
		inline bool IsOccluder( const BlockDef &d ) { return d.opaque && ( d.shape == SHAPE_CUBE || d.shape == SHAPE_MODEL ); }

		inline bool Occludes( const Ctx &ctx, int x, int y, int z )
		{
			return IsOccluder( ctx.w.GetDef( x, y, z ) );
		}

		void TileUV( const Ctx &ctx, int tile, float u, float v, float &ou, float &ov )
		{
			if ( tile < 0 || !ctx.reg.HasAtlas() ) { ou = u; ov = v; return; }
			int tpr = ctx.reg.TilesPerRow(), rows = ctx.reg.TileRows();
			int col = tile % tpr, row = tile / tpr;
			float in = ctx.opt.texelInset;
			ou = ( col + in + u * ( 1.f - 2.f * in ) ) / (float)tpr;
			ov = ( row + in + v * ( 1.f - 2.f * in ) ) / (float)rows;
		}

		uint32_t BaseColor( const Ctx &ctx, const BlockDef &d, int face, bool textured )
		{
			if ( !textured ) return d.color & 0xFFFFFF ? ( ( d.color & 0xFF ) << 16 ) | ( d.color & 0xFF00 ) | ( ( d.color >> 16 ) & 0xFF ) : 0x808080;
			bool tinted = d.tint != TINT_NONE && ( d.tintMask == 0 || ( d.tintMask & ( 1 << face ) ) );
			return tinted ? TintColor( d.tint ) : 0xFFFFFF;
		}

		void PushVertex( std::vector<MeshVertex> &out, const Ctx &ctx, double mx, double my, double mz, float u, float v, uint32_t rgb, float bright, uint8_t a, uint8_t sky, uint8_t blk )
		{
			Vec3d s = ctx.c.ToSource( Vec3d( mx, my, mz ) );
			MeshVertex mv;
			mv.pos[0] = (float)s.x; mv.pos[1] = (float)s.y; mv.pos[2] = (float)s.z;
			mv.uv[0] = u; mv.uv[1] = v;
			bright = std::min( 1.f, std::max( 0.f, bright ) );
			mv.rgba[0] = (uint8_t)( ( ( rgb >> 16 ) & 0xFF ) * bright );
			mv.rgba[1] = (uint8_t)( ( ( rgb >> 8 ) & 0xFF ) * bright );
			mv.rgba[2] = (uint8_t)( ( rgb & 0xFF ) * bright );
			mv.rgba[3] = a;
			mv.sky = sky;
			mv.blk = blk;
			out.push_back( mv );
		}

		// Emits a quad given 4 MC-space corners in CCW-from-outside order.
		void EmitQuad( std::vector<MeshVertex> &out, const Ctx &ctx, const double corners[4][3], const float uvs[4][2], int tile,
					   uint32_t rgb, const float bright[4], uint8_t alpha, const uint8_t sky[4], const uint8_t blk[4], bool forceCCW = false )
		{
			bool cw = ctx.opt.clockwise && !forceCCW;
			for ( int i = 0; i < 4; ++i )
			{
				int k = cw ? ( 4 - i ) % 4 : i;   // 0,3,2,1 reverses winding
				float u, v;
				TileUV( ctx, tile, uvs[k][0], uvs[k][1], u, v );
				PushVertex( out, ctx, corners[k][0], corners[k][1], corners[k][2], u, v, rgb, bright[k], alpha, sky[k], blk[k] );
			}
		}

		// Light of one cell (own light for emissive blocks)
		inline void CellLight( const Ctx &ctx, int x, int y, int z, int &sky, int &blk )
		{
			sky = ctx.w.SkyLight( x, y, z );
			blk = ctx.w.BlockLight( x, y, z );
		}

		void EmitBoxFace( std::vector<MeshVertex> &out, const Ctx &ctx, const BlockDef &d, int bx, int by, int bz, int face,
						  const float mn[3], const float mx[3], bool smooth, float topHeight = -1.f, bool forceTranslucentAlpha = false )
		{
			double corners[4][3];
			float uvs[4][2];
			float bright[4];
			uint8_t sky[4], blk[4];
			int tile = ctx.opt.textured ? d.tiles[face] : -1;
			bool textured = tile >= 0 && ctx.reg.HasAtlas();
			uint32_t rgb = BaseColor( ctx, d, face, textured );
			float shade = d.emissive ? 1.f : kFaceShade[face];

			// Is the face flush with the block boundary? Then light comes from the neighbour cell.
			const int axis = kNormal[face][0] ? 0 : kNormal[face][1] ? 1 : 2;
			const bool positive = kNormal[face][axis] > 0;
			const bool flush = positive ? mx[axis] >= 1.f : mn[axis] <= 0.f;
			const int nx = bx + ( flush ? kNormal[face][0] : 0 ), ny = by + ( flush ? kNormal[face][1] : 0 ), nz = bz + ( flush ? kNormal[face][2] : 0 );
			int baseSky, baseBlk;
			CellLight( ctx, nx, ny, nz, baseSky, baseBlk );
			if ( !flush || d.shape == SHAPE_LIQUID ) { int s2, b2; CellLight( ctx, bx, by, bz, s2, b2 ); baseSky = std::max( baseSky, s2 ); baseBlk = std::max( baseBlk, b2 ); }
			baseBlk = std::max<int>( baseBlk, d.lightEmission );

			for ( int i = 0; i < 4; ++i )
			{
				const float *k = kCorners[face][i];
				float lx = k[0] ? mx[0] : mn[0];
				float ly = k[1] ? mx[1] : mn[1];
				float lz = k[2] ? mx[2] : mn[2];
				if ( topHeight >= 0.f && k[1] ) ly = topHeight;
				corners[i][0] = bx + lx; corners[i][1] = by + ly; corners[i][2] = bz + lz;
				FaceUV( face, lx, ly, lz, uvs[i][0], uvs[i][1] );

				float b = shade;
				int vs = baseSky, vb = baseBlk;
				if ( smooth && flush && !d.emissive )
				{
					// Smooth lighting + AO: the 4 cells in front of the face around this corner
					int off[3] = { k[0] ? 1 : -1, k[1] ? 1 : -1, k[2] ? 1 : -1 };
					int a1 = ( axis + 1 ) % 3, a2 = ( axis + 2 ) % 3;
					int p0[3] = { nx, ny, nz }, p1[3] = { nx, ny, nz }, p2[3] = { nx, ny, nz }, p3[3] = { nx, ny, nz };
					p1[a1] += off[a1];
					p2[a2] += off[a2];
					p3[a1] += off[a1]; p3[a2] += off[a2];
					int s1 = Occludes( ctx, p1[0], p1[1], p1[2] ), s2 = Occludes( ctx, p2[0], p2[1], p2[2] ), cc = Occludes( ctx, p3[0], p3[1], p3[2] );
					if ( ctx.opt.ambientOcclusion )
					{
						int level = ( s1 && s2 ) ? 0 : 3 - ( s1 + s2 + cc );
						b *= kAOLevels[level];
					}
					int sumS = baseSky, sumB = baseBlk, n = 1, ls, lb;
					(void)p0;
					if ( !s1 ) { CellLight( ctx, p1[0], p1[1], p1[2], ls, lb ); sumS += ls; sumB += lb; ++n; }
					if ( !s2 ) { CellLight( ctx, p2[0], p2[1], p2[2], ls, lb ); sumS += ls; sumB += lb; ++n; }
					if ( !cc && !( s1 && s2 ) ) { CellLight( ctx, p3[0], p3[1], p3[2], ls, lb ); sumS += ls; sumB += lb; ++n; }
					vs = ( sumS + n / 2 ) / n;
					vb = ( sumB + n / 2 ) / n;
				}
				bright[i] = b;
				sky[i] = (uint8_t)vs;
				blk[i] = (uint8_t)std::max<int>( vb, d.lightEmission );
			}
			uint8_t alpha = ( d.shape == SHAPE_LIQUID && d.translucent ) || forceTranslucentAlpha ? 180 : 255;
			EmitQuad( out, ctx, corners, uvs, tile, rgb, bright, alpha, sky, blk );
		}

		bool FaceVisible( const Ctx &ctx, const BlockDef &self, uint16_t selfId, int x, int y, int z, int face )
		{
			int nx = x + kNormal[face][0], ny = y + kNormal[face][1], nz = z + kNormal[face][2];
			if ( ny < ctx.w.MinY() ) return false;
			if ( ny >= ctx.w.MaxY() ) return true;
			uint16_t nid = ctx.w.Get( nx, ny, nz );
			const BlockDef &n = ctx.reg.Get( nid );
			if ( IsOccluder( n ) ) return false;
			if ( nid == selfId && self.cullSame ) return false;
			if ( self.shape == SHAPE_LIQUID && n.shape == SHAPE_LIQUID ) return false;
			return true;
		}

		void EmitCross( std::vector<MeshVertex> &out, const Ctx &ctx, const BlockDef &d, int x, int y, int z )
		{
			int tile = ctx.opt.textured ? d.tiles[FACE_NORTH] : -1;
			bool textured = tile >= 0 && ctx.reg.HasAtlas();
			uint32_t rgb = BaseColor( ctx, d, FACE_NORTH, textured );
			const float lo = 0.1464f, hi = 0.8536f;   // MC crosses are inset (45deg, 16px wide)
			const float diag[2][2][2] = { { { lo, lo }, { hi, hi } }, { { lo, hi }, { hi, lo } } };
			float bright[4] = { 0.9f, 0.9f, 0.9f, 0.9f };
			int cs, cb;
			CellLight( ctx, x, y, z, cs, cb );
			cb = std::max<int>( cb, d.lightEmission );
			uint8_t sky[4] = { (uint8_t)cs, (uint8_t)cs, (uint8_t)cs, (uint8_t)cs }, blk[4] = { (uint8_t)cb, (uint8_t)cb, (uint8_t)cb, (uint8_t)cb };
			for ( int p = 0; p < 2; ++p )
			{
				double c[4][3] = {
					{ x + diag[p][0][0], y + 1.0, z + diag[p][0][1] },
					{ x + diag[p][0][0], y + 0.0, z + diag[p][0][1] },
					{ x + diag[p][1][0], y + 0.0, z + diag[p][1][1] },
					{ x + diag[p][1][0], y + 1.0, z + diag[p][1][1] },
				};
				float uv[4][2] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };
				EmitQuad( out, ctx, c, uv, tile, rgb, bright, 255, sky, blk, false );   // world materials are double sided
			}
		}
	}

	namespace
	{
		// Baked Minecraft model. Full faces flush with the block boundary get the same smooth
		// lighting + AO as cubes; everything else (plants, stairs' inner faces, rotated planes)
		// takes the brighter of the block's own cell and the cell it faces.
		void EmitModel( std::vector<MeshVertex> &out, const Ctx &ctx, const BlockDef &d, uint16_t id, int x, int y, int z )
		{
			for ( uint32_t mi : d.models )
			{
				const BlockModel &m = ctx.reg.Model( mi );
				for ( const ModelQuad &q : m.quads )
				{
					if ( q.cull >= 0 && !FaceVisible( ctx, d, id, x, y, z, q.cull ) ) continue;

					int tile = ctx.opt.textured ? q.tile : -1;
					bool textured = tile >= 0 && ctx.reg.HasAtlas();
					uint32_t rgb;
					if ( !textured ) rgb = BaseColor( ctx, d, q.face, false );
					else rgb = ( q.tint && d.tint != TINT_NONE ) ? TintColor( d.tint ) : 0xFFFFFF;
					float shade = ( d.emissive || !q.shade ) ? 1.f : kFaceShade[q.face];

					// Full face on the block boundary?
					int face = q.cull >= 0 ? q.cull : q.face;
					const int axis = kNormal[face][0] ? 0 : kNormal[face][1] ? 1 : 2;
					const bool positive = kNormal[face][axis] > 0;
					const float plane = positive ? 1.f : 0.f;
					bool full = q.cull >= 0;
					float lo[3] = { 1, 1, 1 }, hi[3] = { 0, 0, 0 };
					for ( int k = 0; k < 4; ++k )
						for ( int a = 0; a < 3; ++a ) { lo[a] = std::min( lo[a], q.pos[k][a] ); hi[a] = std::max( hi[a], q.pos[k][a] ); }
					for ( int a = 0; a < 3 && full; ++a )
					{
						if ( a == axis ) full = std::fabs( lo[a] - plane ) < 1e-3f && std::fabs( hi[a] - plane ) < 1e-3f;
						else full = lo[a] < 1e-3f && hi[a] > 1.f - 1e-3f;
					}

					double corners[4][3];
					float uvs[4][2], bright[4];
					uint8_t sky[4], blk[4];
					const int nx = x + kNormal[face][0], ny = y + kNormal[face][1], nz = z + kNormal[face][2];
					int s1, b1, s2, b2;
					CellLight( ctx, nx, ny, nz, s1, b1 );
					CellLight( ctx, x, y, z, s2, b2 );
					int baseSky = full ? s1 : std::max( s1, s2 ), baseBlk = std::max<int>( full ? b1 : std::max( b1, b2 ), d.lightEmission );
					for ( int k = 0; k < 4; ++k )
					{
						corners[k][0] = x + q.pos[k][0]; corners[k][1] = y + q.pos[k][1]; corners[k][2] = z + q.pos[k][2];
						uvs[k][0] = q.uv[k][0]; uvs[k][1] = q.uv[k][1];
						float b = shade;
						int vs = baseSky, vb = baseBlk;
						if ( full && !d.emissive )
						{
							int off[3] = { q.pos[k][0] > 0.5f ? 1 : -1, q.pos[k][1] > 0.5f ? 1 : -1, q.pos[k][2] > 0.5f ? 1 : -1 };
							int a1 = ( axis + 1 ) % 3, a2 = ( axis + 2 ) % 3;
							int p1[3] = { nx, ny, nz }, p2[3] = { nx, ny, nz }, p3[3] = { nx, ny, nz };
							p1[a1] += off[a1];
							p2[a2] += off[a2];
							p3[a1] += off[a1]; p3[a2] += off[a2];
							int o1 = Occludes( ctx, p1[0], p1[1], p1[2] ), o2 = Occludes( ctx, p2[0], p2[1], p2[2] ), oc = Occludes( ctx, p3[0], p3[1], p3[2] );
							if ( ctx.opt.ambientOcclusion )
							{
								int level = ( o1 && o2 ) ? 0 : 3 - ( o1 + o2 + oc );
								b *= kAOLevels[level];
							}
							int sumS = baseSky, sumB = baseBlk, n = 1, ls, lb;
							if ( !o1 ) { CellLight( ctx, p1[0], p1[1], p1[2], ls, lb ); sumS += ls; sumB += lb; ++n; }
							if ( !o2 ) { CellLight( ctx, p2[0], p2[1], p2[2], ls, lb ); sumS += ls; sumB += lb; ++n; }
							if ( !oc && !( o1 && o2 ) ) { CellLight( ctx, p3[0], p3[1], p3[2], ls, lb ); sumS += ls; sumB += lb; ++n; }
							vs = ( sumS + n / 2 ) / n;
							vb = ( sumB + n / 2 ) / n;
						}
						bright[k] = b;
						sky[k] = (uint8_t)vs;
						blk[k] = (uint8_t)std::max<int>( vb, d.lightEmission );
					}
					EmitQuad( out, ctx, corners, uvs, tile, rgb, bright, d.translucent ? 200 : 255, sky, blk );
				}
			}
		}
	}

	void BuildSectionMesh( const VoxelWorld &w, const Coords &c, int cx, int sy, int cz, const MesherOptions &opt, SectionMesh &out )
	{
		out.opaque.clear();
		out.translucent.clear();
		const Section *s = w.GetSection( cx, sy, cz );
		if ( !s || s->nonAir == 0 ) return;
		Ctx ctx{ w, c, opt, w.Registry(), w.Registry().WaterTile() };
		const int bx0 = cx * 16, by0 = sy * 16, bz0 = cz * 16;

		for ( int ly = 0; ly < 16; ++ly )
			for ( int lz = 0; lz < 16; ++lz )
				for ( int lx = 0; lx < 16; ++lx )
				{
					uint16_t id = s->blocks[ Section::Index( lx, ly, lz ) ];
					if ( id == BlockRegistry::AIR ) continue;
					const BlockDef &d = ctx.reg.Get( id );
					int x = bx0 + lx, y = by0 + ly, z = bz0 + lz;
					std::vector<MeshVertex> &bucket = d.translucent ? out.translucent : out.opaque;

					switch ( d.shape )
					{
					case SHAPE_AIR: break;
					case SHAPE_MODEL: EmitModel( bucket, ctx, d, id, x, y, z ); break;
					case SHAPE_CROSS: EmitCross( bucket, ctx, d, x, y, z ); break;
					case SHAPE_CUBE:
					{
						static const float mn[3] = { 0, 0, 0 }, mx[3] = { 1, 1, 1 };
						for ( int f = 0; f < 6; ++f )
							if ( FaceVisible( ctx, d, id, x, y, z, f ) )
								EmitBoxFace( bucket, ctx, d, x, y, z, f, mn, mx, true );
						break;
					}
					case SHAPE_BOXES:
					{
						for ( size_t bi = 0; bi < d.boxes.size(); ++bi )
						{
							const Box &bx = d.boxes[bi];
							float mn[3] = { std::max( 0.f, bx.mn[0] ), std::max( 0.f, bx.mn[1] ), std::max( 0.f, bx.mn[2] ) };
							float mx[3] = { std::min( 1.f, bx.mx[0] ), std::min( 1.f, bx.mx[1] ), std::min( 1.f, bx.mx[2] ) };
							if ( mx[0] <= mn[0] || mx[1] <= mn[1] || mx[2] <= mn[2] ) continue;
							for ( int f = 0; f < 6; ++f )
							{
								// Only cull faces flush with the block boundary
								bool flush = ( f == FACE_DOWN && mn[1] <= 0 ) || ( f == FACE_UP && mx[1] >= 1 ) ||
											 ( f == FACE_NORTH && mn[2] <= 0 ) || ( f == FACE_SOUTH && mx[2] >= 1 ) ||
											 ( f == FACE_WEST && mn[0] <= 0 ) || ( f == FACE_EAST && mx[0] >= 1 );
								if ( flush && !FaceVisible( ctx, d, id, x, y, z, f ) ) continue;
								EmitBoxFace( bucket, ctx, d, x, y, z, f, mn, mx, false );
							}
						}
						break;
					}
					case SHAPE_LIQUID:
					{
						// (waterlogged blocks are handled after the switch)
						static const float mn[3] = { 0, 0, 0 }, mx[3] = { 1, 1, 1 };
						bool covered = ctx.reg.Get( w.Get( x, y + 1, z ) ).shape == SHAPE_LIQUID;
						float top = covered ? 1.f : d.liquidHeight;
						for ( int f = 0; f < 6; ++f )
						{
							if ( f == FACE_UP && !covered ) { EmitBoxFace( bucket, ctx, d, x, y, z, f, mn, mx, false, top ); continue; }
							if ( FaceVisible( ctx, d, id, x, y, z, f ) ) EmitBoxFace( bucket, ctx, d, x, y, z, f, mn, mx, false, top );
						}
						break;
					}
					}
					if ( d.waterlogged && d.shape != SHAPE_LIQUID )
					{
						// Water surface inside stairs/slabs/fences etc.: top face only (+ sides next to air)
						static const float wmn[3] = { 0, 0, 0 }, wmx[3] = { 1, 8.f / 9.f, 1 };
						BlockDef water = d;
						water.shape = SHAPE_LIQUID; water.translucent = true; water.tint = TINT_WATER; water.tintMask = 0; water.emissive = false; water.lightEmission = 0;
						for ( int f = 0; f < 6; ++f ) water.tiles[f] = ctx.waterTile;
						const BlockDef &above = ctx.reg.Get( w.Get( x, y + 1, z ) );
						if ( above.shape != SHAPE_LIQUID && !above.waterlogged )
							EmitBoxFace( out.translucent, ctx, water, x, y, z, FACE_UP, wmn, wmx, false, -1.f, true );
					}
				}
	}

	//------------------------------------------------------------------
	// Collision
	//------------------------------------------------------------------
	namespace
	{
		inline bool FullCollider( const VoxelWorld &w, int x, int y, int z )
		{
			const BlockDef &d = w.GetDef( x, y, z );
			return d.collides && d.coll.size() == 1 && d.coll[0].IsFull();
		}

		void PushTriQuad( std::vector<Vec3f> &tris, const Coords &c, const Vec3d q[4] )
		{
			Vec3f s[4];
			for ( int i = 0; i < 4; ++i ) { Vec3d v = c.ToSource( q[i] ); s[i] = Vec3f( (float)v.x, (float)v.y, (float)v.z ); }
			tris.push_back( s[0] ); tris.push_back( s[1] ); tris.push_back( s[2] );
			tris.push_back( s[0] ); tris.push_back( s[2] ); tris.push_back( s[3] );
		}
	}

	void BuildSectionCollision( const VoxelWorld &w, const Coords &c, int cx, int sy, int cz, std::vector<Vec3f> &tris )
	{
		tris.clear();
		const Section *s = w.GetSection( cx, sy, cz );
		if ( !s || s->nonAir == 0 ) return;
		const int bx0 = cx * 16, by0 = sy * 16, bz0 = cz * 16;

		// Greedy merge of exposed full-cube faces: for each face direction and slice, build a 16x16 mask.
		for ( int face = 0; face < 6; ++face )
		{
			int axis = kNormal[face][0] ? 0 : kNormal[face][1] ? 1 : 2;
			int ua = ( axis + 1 ) % 3, va = ( axis + 2 ) % 3;
			int dir = kNormal[face][axis];
			for ( int slice = 0; slice < 16; ++slice )
			{
				bool mask[16][16];
				for ( int u = 0; u < 16; ++u )
					for ( int v = 0; v < 16; ++v )
					{
						int l[3]; l[axis] = slice; l[ua] = u; l[va] = v;
						int x = bx0 + l[0], y = by0 + l[1], z = bz0 + l[2];
						bool m = FullCollider( w, x, y, z );
						if ( m )
						{
							int n[3] = { x, y, z }; n[axis] += dir;
							m = !FullCollider( w, n[0], n[1], n[2] );
						}
						mask[u][v] = m;
					}
				for ( int u = 0; u < 16; ++u )
					for ( int v = 0; v < 16; )
					{
						if ( !mask[u][v] ) { ++v; continue; }
						int h = 1;
						while ( v + h < 16 && mask[u][v + h] ) ++h;
						int wdt = 1;
						bool ok = true;
						while ( u + wdt < 16 && ok )
						{
							for ( int k = 0; k < h; ++k ) if ( !mask[u + wdt][v + k] ) { ok = false; break; }
							if ( ok ) ++wdt;
						}
						for ( int i = 0; i < wdt; ++i ) for ( int k = 0; k < h; ++k ) mask[u + i][v + k] = false;

						double plane = slice + ( dir > 0 ? 1 : 0 );
						double base[3] = { (double)bx0, (double)by0, (double)bz0 };
						Vec3d q[4];
						double cu[4] = { (double)u, (double)( u + wdt ), (double)( u + wdt ), (double)u };
						double cv[4] = { (double)v, (double)v, (double)( v + h ), (double)( v + h ) };
						for ( int i = 0; i < 4; ++i )
						{
							double p[3]; p[axis] = plane; p[ua] = cu[i]; p[va] = cv[i];
							q[i] = Vec3d( base[0] + p[0], base[1] + p[1], base[2] + p[2] );
						}
						// Winding is irrelevant for polysoup collision.
						PushTriQuad( tris, c, q );
						v += h;
					}
			}
		}

		// Partial boxes: all faces.
		for ( int ly = 0; ly < 16; ++ly )
			for ( int lz = 0; lz < 16; ++lz )
				for ( int lx = 0; lx < 16; ++lx )
				{
					const BlockDef &d = w.Registry().Get( s->blocks[ Section::Index( lx, ly, lz ) ] );
					if ( !d.collides || ( d.coll.size() == 1 && d.coll[0].IsFull() ) ) continue;
					int x = bx0 + lx, y = by0 + ly, z = bz0 + lz;
					for ( size_t bi = 0; bi < d.coll.size(); ++bi )
					{
						const Box &b = d.coll[bi];
						for ( int f = 0; f < 6; ++f )
						{
							Vec3d q[4];
							for ( int i = 0; i < 4; ++i )
							{
								const float *k = kCorners[f][i];
								q[i] = Vec3d( x + ( k[0] ? b.mx[0] : b.mn[0] ), y + ( k[1] ? b.mx[1] : b.mn[1] ), z + ( k[2] ? b.mx[2] : b.mn[2] ) );
							}
							PushTriQuad( tris, c, q );
						}
					}
				}
	}
}
