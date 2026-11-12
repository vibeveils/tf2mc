//========= TF2MC =========//
// Include the engine-independent core (STL based) from Source code. Valve's min/max macros
// and memdbg overrides must be suspended around STL headers.
//=========================//
#ifndef TF2MC_CORE_H
#define TF2MC_CORE_H
#pragma once

#include "tier0/valve_minmax_off.h"
#include "tier0/memdbgoff.h"

#include "tf2mc/core/mc_protocol.h"
#include "tf2mc/core/mc_world.h"
#include "tf2mc/core/mc_collide.h"
#include "tf2mc/core/mc_mesher.h"
#include "tf2mc/core/mc_net.h"

#include "tier0/memdbgon.h"
#include "tier0/valve_minmax_on.h"

// Helpers between Source math types and core types
inline mc::Vec3d MC_V( const Vector &v ) { return mc::Vec3d( v.x, v.y, v.z ); }
inline Vector MC_V( const mc::Vec3d &v ) { return Vector( (float)v.x, (float)v.y, (float)v.z ); }

#endif
