//========= TF2MC bridge - engine independent core =========//
#include "mc_collide.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mc
{
	static const double kOverlapEps = 1e-7;
	static const int kMaxCandidates = 200000;   // guards absurdly large query boxes

	BlockPos Offset( const BlockPos &p, Face f )
	{
		switch ( f )
		{
		case FACE_DOWN:  return BlockPos( p.x, p.y - 1, p.z );
		case FACE_UP:    return BlockPos( p.x, p.y + 1, p.z );
		case FACE_NORTH: return BlockPos( p.x, p.y, p.z - 1 );
		case FACE_SOUTH: return BlockPos( p.x, p.y, p.z + 1 );
		case FACE_WEST:  return BlockPos( p.x - 1, p.y, p.z );
		default:         return BlockPos( p.x + 1, p.y, p.z );
		}
	}

	static const int kMaxBoxes = 16;

	int BlockCollisionBoxes( const VoxelWorld &w, int x, int y, int z, Vec3d *mins, Vec3d *maxs, int maxBoxes )
	{
		const BlockDef &d = w.GetDef( x, y, z );
		if ( !d.collides ) return 0;
		int n = 0;
		for ( size_t i = 0; i < d.coll.size() && n < maxBoxes; ++i, ++n )
		{
			const Box &b = d.coll[i];
			mins[n] = Vec3d( x + b.mn[0], y + b.mn[1], z + b.mn[2] );
			maxs[n] = Vec3d( x + b.mx[0], y + b.mx[1], z + b.mx[2] );
		}
		return n;
	}

	bool BoxTouchesClimbable( const VoxelWorld &w, const Vec3d &mn, const Vec3d &mx )
	{
		for ( int y = FloorI( mn.y ); y <= FloorI( mx.y ); ++y )
			for ( int z = FloorI( mn.z ); z <= FloorI( mx.z ); ++z )
				for ( int x = FloorI( mn.x ); x <= FloorI( mx.x ); ++x )
					if ( w.GetDef( x, y, z ).climbable ) return true;
		return false;
	}

	bool BoxOverlaps( const VoxelWorld &w, const Vec3d &mn, const Vec3d &mx, BlockPos *outBlock )
	{
		int x0 = FloorI( mn.x + kOverlapEps ), x1 = FloorI( mx.x - kOverlapEps );
		int y0 = FloorI( mn.y + kOverlapEps ), y1 = FloorI( mx.y - kOverlapEps );
		int z0 = FloorI( mn.z + kOverlapEps ), z1 = FloorI( mx.z - kOverlapEps );
		y0 = std::max( y0 - 1, w.MinY() ); y1 = std::min( y1, w.MaxY() - 1 );   // -1: tall boxes (fences) reach up
		if ( (int64_t)( x1 - x0 + 1 ) * ( y1 - y0 + 1 ) * ( z1 - z0 + 1 ) > kMaxCandidates ) return false;
		Vec3d bmn[kMaxBoxes], bmx[kMaxBoxes];
		for ( int y = y0; y <= y1; ++y )
			for ( int z = z0; z <= z1; ++z )
				for ( int x = x0; x <= x1; ++x )
				{
					int n = BlockCollisionBoxes( w, x, y, z, bmn, bmx, kMaxBoxes );
					for ( int i = 0; i < n; ++i )
						if ( mn.x < bmx[i].x - kOverlapEps && mx.x > bmn[i].x + kOverlapEps &&
							 mn.y < bmx[i].y - kOverlapEps && mx.y > bmn[i].y + kOverlapEps &&
							 mn.z < bmx[i].z - kOverlapEps && mx.z > bmn[i].z + kOverlapEps )
						{
							if ( outBlock ) *outBlock = BlockPos( x, y, z );
							return true;
						}
				}
		return false;
	}

	// Ray (origin s, delta d) vs box [mn,mx]. Touching counts as outside.
	static bool RayBox( const Vec3d &s, const Vec3d &d, const Vec3d &mn, const Vec3d &mx, double &tEnter, int &axis, int &sign )
	{
		const double so[3] = { s.x, s.y, s.z }, dd[3] = { d.x, d.y, d.z };
		const double lo[3] = { mn.x, mn.y, mn.z }, hi[3] = { mx.x, mx.y, mx.z };
		double t0 = -std::numeric_limits<double>::infinity(), t1 = std::numeric_limits<double>::infinity();
		int ax = -1, sg = 0;
		for ( int i = 0; i < 3; ++i )
		{
			if ( dd[i] == 0.0 )
			{
				if ( so[i] <= lo[i] || so[i] >= hi[i] ) return false;
				continue;
			}
			double inv = 1.0 / dd[i];
			double a = ( lo[i] - so[i] ) * inv, b = ( hi[i] - so[i] ) * inv;
			int s_ = dd[i] > 0 ? -1 : 1;   // normal points against motion
			if ( a > b ) std::swap( a, b );
			if ( a > t0 ) { t0 = a; ax = i; sg = s_; }
			if ( b < t1 ) t1 = b;
		}
		if ( ax < 0 ) return false;          // started inside on all axes (handled as startsolid)
		if ( !( t0 < t1 ) || t1 <= 0.0 || t0 > 1.0 ) return false;
		if ( t0 < 0.0 ) return false;        // overlapping at start on the entry axis
		tEnter = t0; axis = ax; sign = sg;
		return true;
	}

	SweepResult SweepBox( const VoxelWorld &w, const Vec3d &start, const Vec3d &delta, const Vec3d &half )
	{
		SweepResult res;
		Vec3d smn( start.x - half.x, start.y - half.y, start.z - half.z );
		Vec3d smx( start.x + half.x, start.y + half.y, start.z + half.z );

		bool isRay = half.x <= 0 && half.y <= 0 && half.z <= 0;
		if ( !isRay && BoxOverlaps( w, smn, smx, &res.block ) )
		{
			res.hit = true; res.startSolid = true; res.fraction = 0.0;
			Vec3d end( start.x + delta.x, start.y + delta.y, start.z + delta.z );
			Vec3d emn( end.x - half.x, end.y - half.y, end.z - half.z ), emx( end.x + half.x, end.y + half.y, end.z + half.z );
			res.allSolid = BoxOverlaps( w, emn, emx );
			return res;
		}
		if ( isRay )
		{
			// A ray that starts inside a solid block is startsolid.
			Vec3d bmn[kMaxBoxes], bmx[kMaxBoxes];
			int bx = FloorI( start.x ), by = FloorI( start.y ), bz = FloorI( start.z );
			for ( int dy = 0; dy >= -1; --dy )
			{
				int n = BlockCollisionBoxes( w, bx, by + dy, bz, bmn, bmx, kMaxBoxes );
				for ( int i = 0; i < n; ++i )
					if ( start.x > bmn[i].x && start.x < bmx[i].x && start.y > bmn[i].y && start.y < bmx[i].y && start.z > bmn[i].z && start.z < bmx[i].z )
					{
						res.hit = true; res.startSolid = true; res.allSolid = true; res.fraction = 0.0; res.block = BlockPos( bx, by + dy, bz );
						return res;
					}
			}
		}

		double len = std::sqrt( delta.x * delta.x + delta.y * delta.y + delta.z * delta.z );
		if ( len <= 0.0 ) return res;

		int segments = std::max( 1, (int)std::ceil( len / 1.0 ) );
		double best = 2.0;
		for ( int seg = 0; seg < segments; ++seg )
		{
			double ta = (double)seg / segments, tb = (double)( seg + 1 ) / segments;
			Vec3d a( start.x + delta.x * ta, start.y + delta.y * ta, start.z + delta.z * ta );
			Vec3d b( start.x + delta.x * tb, start.y + delta.y * tb, start.z + delta.z * tb );
			int x0 = FloorI( std::min( a.x, b.x ) - half.x - kOverlapEps ), x1 = FloorI( std::max( a.x, b.x ) + half.x + kOverlapEps );
			int y0 = FloorI( std::min( a.y, b.y ) - half.y - kOverlapEps ), y1 = FloorI( std::max( a.y, b.y ) + half.y + kOverlapEps );
			int z0 = FloorI( std::min( a.z, b.z ) - half.z - kOverlapEps ), z1 = FloorI( std::max( a.z, b.z ) + half.z + kOverlapEps );
			y0 = std::max( y0 - 1, w.MinY() ); y1 = std::min( y1, w.MaxY() - 1 );
			if ( (int64_t)( x1 - x0 + 1 ) * std::max( 0, y1 - y0 + 1 ) * ( z1 - z0 + 1 ) > kMaxCandidates ) break;

			Vec3d bmn[kMaxBoxes], bmx[kMaxBoxes];
			for ( int y = y0; y <= y1; ++y )
				for ( int z = z0; z <= z1; ++z )
					for ( int x = x0; x <= x1; ++x )
					{
						int n = BlockCollisionBoxes( w, x, y, z, bmn, bmx, kMaxBoxes );
						for ( int i = 0; i < n; ++i )
						{
							// Minkowski expansion
							Vec3d emn( bmn[i].x - half.x, bmn[i].y - half.y, bmn[i].z - half.z );
							Vec3d emx( bmx[i].x + half.x, bmx[i].y + half.y, bmx[i].z + half.z );
							double t; int axis, sign;
							if ( RayBox( start, delta, emn, emx, t, axis, sign ) && t < best )
							{
								best = t;
								res.hit = true;
								res.fraction = t;
								res.normal = Vec3d( axis == 0 ? sign : 0, axis == 1 ? sign : 0, axis == 2 ? sign : 0 );
								res.block = BlockPos( x, y, z );
							}
						}
					}
			if ( res.hit && best <= tb ) break;
		}
		return res;
	}

	PickResult PickBlock( const VoxelWorld &w, const Vec3d &o, const Vec3d &dirIn, double maxDist, bool solidOnly )
	{
		PickResult pr;
		double l = std::sqrt( dirIn.x * dirIn.x + dirIn.y * dirIn.y + dirIn.z * dirIn.z );
		if ( l <= 0 ) return pr;
		Vec3d d( dirIn.x / l, dirIn.y / l, dirIn.z / l );

		int x = FloorI( o.x ), y = FloorI( o.y ), z = FloorI( o.z );
		int stepX = d.x > 0 ? 1 : -1, stepY = d.y > 0 ? 1 : -1, stepZ = d.z > 0 ? 1 : -1;
		const double inf = std::numeric_limits<double>::infinity();
		double tdx = d.x != 0 ? std::fabs( 1.0 / d.x ) : inf;
		double tdy = d.y != 0 ? std::fabs( 1.0 / d.y ) : inf;
		double tdz = d.z != 0 ? std::fabs( 1.0 / d.z ) : inf;
		double tmx = d.x != 0 ? ( ( d.x > 0 ? ( x + 1 - o.x ) : ( o.x - x ) ) * tdx ) : inf;
		double tmy = d.y != 0 ? ( ( d.y > 0 ? ( y + 1 - o.y ) : ( o.y - y ) ) * tdy ) : inf;
		double tmz = d.z != 0 ? ( ( d.z > 0 ? ( z + 1 - o.z ) : ( o.z - z ) ) * tdz ) : inf;

		double t = 0.0;
		Face face = FACE_UP;
		for ( int iter = 0; iter < 4096 && t <= maxDist; ++iter )
		{
			const BlockDef &def = w.GetDef( x, y, z );
			bool candidate = def.shape != SHAPE_AIR && def.shape != SHAPE_LIQUID && ( !solidOnly || def.collides );
			if ( candidate )
			{
				// Outline boxes (stairs, slabs, fences, plants...) or the unit cube
				std::vector<Box> unit( 1, Box::Unit() );
				const std::vector<Box> &boxes = ( def.shape == SHAPE_BOXES || def.shape == SHAPE_CROSS ) && !def.boxes.empty() ? def.boxes : unit;
				bool exact = def.shape == SHAPE_BOXES || ( def.shape == SHAPE_CROSS && !def.boxes.empty() );
				double bestT = 1e30; Face bestFace = face;
				Vec3d dl( d.x * maxDist, d.y * maxDist, d.z * maxDist );
				for ( size_t bi = 0; bi < boxes.size(); ++bi )
				{
					Vec3d mn( x + boxes[bi].mn[0], y + boxes[bi].mn[1], z + boxes[bi].mn[2] ), mx( x + boxes[bi].mx[0], y + boxes[bi].mx[1], z + boxes[bi].mx[2] );
					bool inside = o.x > mn.x && o.x < mx.x && o.y > mn.y && o.y < mx.y && o.z > mn.z && o.z < mx.z;
					if ( !exact ) { if ( t < bestT ) { bestT = t; bestFace = face; } continue; }
					double te; int axis, sign;
					if ( inside ) { bestT = 0; bestFace = face; continue; }
					if ( RayBox( o, dl, mn, mx, te, axis, sign ) && te * maxDist < bestT )
					{
						static const Face fx[2] = { FACE_WEST, FACE_EAST }, fy[2] = { FACE_DOWN, FACE_UP }, fz[2] = { FACE_NORTH, FACE_SOUTH };
						int k = sign > 0 ? 1 : 0;
						bestT = te * maxDist;
						bestFace = axis == 0 ? fx[k] : axis == 1 ? fy[k] : fz[k];
					}
				}
				if ( bestT <= maxDist )
				{
					pr.hit = true; pr.block = BlockPos( x, y, z ); pr.face = bestFace; pr.distance = bestT;
					pr.point = Vec3d( o.x + d.x * bestT, o.y + d.y * bestT, o.z + d.z * bestT );
					return pr;
				}
			}
			if ( tmx < tmy && tmx < tmz ) { x += stepX; t = tmx; tmx += tdx; face = stepX > 0 ? FACE_WEST : FACE_EAST; }
			else if ( tmy < tmz )         { y += stepY; t = tmy; tmy += tdy; face = stepY > 0 ? FACE_DOWN : FACE_UP; }
			else                          { z += stepZ; t = tmz; tmz += tdz; face = stepZ > 0 ? FACE_NORTH : FACE_SOUTH; }
		}
		return pr;
	}
}
