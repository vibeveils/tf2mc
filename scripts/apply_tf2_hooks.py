#!/usr/bin/env python3
"""
Inserts the TF2MC hooks into a source-sdk-2013 checkout. Idempotent: running it twice changes nothing.
Used to generate source/patches/tf2mc.patch, and usable directly if the SDK moved on and the patch
no longer applies:   python3 scripts/apply_tf2_hooks.py ~/source-sdk-2013
"""
import os
import sys

MARK = "TF2MC"


def edit(path, anchor, insert, after=True, replace=None):
    with open(path, newline="") as f:
        s = f.read()
    nl = "\r\n" if "\r\n" in s else "\n"
    new = (replace if replace is not None else insert).replace("\n", nl)
    marked = [l for l in new.splitlines() if MARK in l]
    if (marked[0] if marked else new.strip()) in s:
        return "already"
    a = anchor.replace("\n", nl)
    if a not in s:
        raise SystemExit(f"{path}: anchor not found:\n{anchor}")
    if replace is not None:
        s = s.replace(a, new, 1)
    elif after:
        s = s.replace(a, a + new, 1)
    else:
        s = s.replace(a, new + a, 1)
    with open(path, "w", newline="") as f:
        f.write(s)
    return "ok"


def edit_all(path, old, new):
    """Replace every occurrence of `old` (idempotent: skipped when `new` is already there)."""
    with open(path, newline="") as f:
        s = f.read()
    nl = "\r\n" if "\r\n" in s else "\n"
    o, n = old.replace("\n", nl), new.replace("\n", nl)
    if n in s:
        return "already"
    if o not in s:
        raise SystemExit(f"{path}: anchor not found:\n{old}")
    with open(path, "w", newline="") as f:
        f.write(s.replace(o, n))
    return "ok"


def main(sdk):
    src = os.path.join(sdk, "src")
    gm = os.path.join(src, "game/shared/tf/tf_gamemovement.cpp")
    pl = os.path.join(src, "game/server/tf/tf_player.cpp")
    sg = os.path.join(src, "game/server/tf/tf_obj_sentrygun.cpp")
    gr = os.path.join(src, "game/shared/tf/tf_gamerules.cpp")
    fl = os.path.join(src, "game/shared/tf/tf_flame.cpp")
    cv = os.path.join(src, "game/client/client_tf.vpc")
    sv = os.path.join(src, "game/server/server_tf.vpc")
    r = []

    # --- movement: jump height, swimming in Minecraft water, ladders/vines
    r.append(edit(gm, '#include "tf_logic_player_destruction.h"', '\n#include "tf2mc/mc_bridge.h"\t// TF2MC'))
    r.append(edit(gm, "\tfloat flMul = ( 289.0f * flJumpMod ) * flGroundFactor;\n",
                  "\n\t// TF2MC: Minecraft-mode players jump like Steve; TF classes optionally get enough lift for one block\n"
                  "\tif ( MC_IsActive() )\n\t{\n"
                  "\t\tif ( m_pTFPlayer->IsPlayerClass( TF_CLASS_CIVILIAN ) )\n"
                  "\t\t\tflMul = MC_SteveJumpSpeed() * flGroundFactor;\n"
                  "\t\telse\n"
                  "\t\t\tflMul = MC_ClassJumpSpeed( 289.0f * flJumpMod ) * flGroundFactor;\n\t}\n"))
    for n in ("0", "1", "2"):
        call = f"GetPointContentsCached( vecPoint, {n} )"
        r.append(edit(gm, call, None, replace=f"MC_PointContents( vecPoint, {call} ) /* TF2MC: Minecraft water */"))
    r.append(edit(gm, "void CTFGameMovement::FullWalkMove()\n{\n",
                  "\tMC_ClimbMove( player, mv, GetPlayerMins(), GetPlayerMaxs() );\t// TF2MC: ladders, vines, scaffolding\n\n"))

    # --- collision: every game-side trace also sweeps the Minecraft voxels. The engine's SOLID_CUSTOM
    #     path doesn't reliably reach mc_world, so movement, bullets, melee and projectiles ask directly.
    us_h = os.path.join(src, "game/shared/util_shared.h")
    us_c = os.path.join(src, "game/shared/util_shared.cpp")
    r.append(edit(us_h, "extern ConVar r_visualizetraces;\n",
                  "\n// TF2MC: merge the Minecraft voxel world into a finished trace. Function pointers (set by tf2mc/mc_bridge.cpp)\n"
                  "// so projects without TF2MC (hl2mp etc.) still link: there they stay NULL and the hooks do nothing.\n"
                  "typedef void ( *MC_ClipTraceFn )( const Ray_t &ray, unsigned int mask, ITraceFilter *pFilter, trace_t *ptr );\t// TF2MC\n"
                  "extern MC_ClipTraceFn g_pfnMCClipTrace;\t// TF2MC\n"
                  "inline void MC_ClipTrace( const Ray_t &ray, unsigned int mask, ITraceFilter *pFilter, trace_t *ptr ) { if ( g_pfnMCClipTrace ) g_pfnMCClipTrace( ray, mask, pFilter, ptr ); }\t// TF2MC\n"
                  "void MC_ClipEntityTrace( ICollideable *pCollision, const Vector &vecAbsStart, const Vector &vecAbsEnd, unsigned int mask, ITraceFilter *pFilter, trace_t *ptr );\t// TF2MC\n"))
    r.append(edit(us_c, "void UTIL_TraceEntity( CBaseEntity *pEntity, const Vector &vecAbsStart, const Vector &vecAbsEnd, unsigned int mask, trace_t *ptr )\n",
                  "// TF2MC: set by tf2mc/mc_bridge.cpp when the Minecraft bridge is compiled in\n"
                  "MC_ClipTraceFn g_pfnMCClipTrace = NULL;\t// TF2MC\n"
                  "void MC_ClipEntityTrace( ICollideable *pCollision, const Vector &vecAbsStart, const Vector &vecAbsEnd, unsigned int mask, ITraceFilter *pFilter, trace_t *ptr )\t// TF2MC\n"
                  "{\n\tif ( !g_pfnMCClipTrace || !pCollision ) return;\n"
                  "\tRay_t ray;\n\tray.Init( vecAbsStart, vecAbsEnd, pCollision->OBBMins(), pCollision->OBBMaxs() );\n"
                  "\tg_pfnMCClipTrace( ray, mask, pFilter, ptr );\n}\n\n", after=False))
    r.append(edit_all(us_h, "\tenginetrace->TraceRay( ray, mask, &traceFilter, ptr );\n",
                      "\tenginetrace->TraceRay( ray, mask, &traceFilter, ptr );\n\tMC_ClipTrace( ray, mask, &traceFilter, ptr );\t// TF2MC\n"))
    r.append(edit_all(us_h, "\tenginetrace->TraceRay( ray, mask, pFilter, ptr );\n",
                      "\tenginetrace->TraceRay( ray, mask, pFilter, ptr );\n\tMC_ClipTrace( ray, mask, pFilter, ptr );\t// TF2MC\n"))
    r.append(edit_all(us_c, "pCollision->GetCollisionAngles(), mask, &traceFilter, ptr );\n",
                      "pCollision->GetCollisionAngles(), mask, &traceFilter, ptr );\n\tMC_ClipEntityTrace( pCollision, vecAbsStart, vecAbsEnd, mask, &traceFilter, ptr );\t// TF2MC\n"))
    r.append(edit_all(us_c, "pCollision->GetCollisionAngles(), mask, pFilter, ptr );\n",
                      "pCollision->GetCollisionAngles(), mask, pFilter, ptr );\n\tMC_ClipEntityTrace( pCollision, vecAbsStart, vecAbsEnd, mask, pFilter, ptr );\t// TF2MC\n"))
    r.append(edit_all(gm, "\tenginetrace->TraceRay( ray, PlayerSolidMask(), &traceFilter, &pm );\n",
                      "\tenginetrace->TraceRay( ray, PlayerSolidMask(), &traceFilter, &pm );\n\tMC_ClipTrace( ray, PlayerSolidMask(), &traceFilter, &pm );\t// TF2MC\n"))
    r.append(edit_all(gm, "\tenginetrace->TraceRay( ray, fMask, &traceFilter, &pm );\n",
                      "\tenginetrace->TraceRay( ray, fMask, &traceFilter, &pm );\n\tMC_ClipTrace( ray, fMask, &traceFilter, &pm );\t// TF2MC\n"))

    # --- footsteps on Minecraft blocks use the block's Minecraft step sound (function pointer:
    #     baseplayer_shared.cpp is also built into projects without TF2MC)
    bp = os.path.join(src, "game/shared/baseplayer_shared.cpp")
    r.append(edit(bp, "void CBasePlayer::PlayStepSound( Vector &vecOrigin, surfacedata_t *psurface, float fvol, bool force )\n",
                  "// TF2MC: set by tf2mc/mc_bridge.cpp; plays Minecraft block step sounds\n"
                  "typedef bool ( *MC_PlayStepSoundFn )( CBasePlayer *pPlayer, const Vector &vecOrigin, float fvol );\t// TF2MC\n"
                  "MC_PlayStepSoundFn g_pfnMCPlayStepSound = NULL;\t// TF2MC\n\n", after=False))
    r.append(edit(bp, "\tif ( !psurface )\n\t\treturn;\n\n\tint nSide = m_Local.m_nStepside;\n",
                  "\tif ( g_pfnMCPlayStepSound && g_pfnMCPlayStepSound( this, vecOrigin, fvol ) )\t// TF2MC: Minecraft block step sound\n\t\treturn;\n\n",
                  after=False))

    # --- player: Civilian = Minecraft mode, PostThink interaction, no TF weapons for it
    r.append(edit(pl, '#include "gc_clientsystem.h"', '\n#include "tf2mc/mc_bridge.h"\t\t// TF2MC\n#include "tf2mc/mc_server.h"\t\t// TF2MC'))
    r.append(edit(pl, "\t\tbool bCivilianOkay = false;", None,
                  replace="\t\tbool bCivilianOkay = MC_AllowCivilian() && ( i == TF_CLASS_CIVILIAN );\t// TF2MC: Civilian = Minecraft mode"))
    r.append(edit(pl, "void CTFPlayer::PostThink()\n{\n\tBaseClass::PostThink();\n",
                  "\n\tMC_PlayerPostThink( this );\t// TF2MC: Minecraft-mode digging / placing / melee\n"))
    r.append(edit(pl, "\tif ( GetTeamNumber() == TEAM_SPECTATOR )\n\t{\n\t\tRemoveAllWeapons();\n\t\treturn;\n\t}\n",
                  "\n\t// TF2MC: the Minecraft-mode player uses Minecraft items (bridged), not TF2 weapons\n"
                  "\tif ( IsPlayerClass( TF_CLASS_CIVILIAN ) )\n\t{\n\t\tRemoveAllWeapons();\n\t\treturn;\n\t}\n"))

    # --- sentries shoot hostile Minecraft mobs
    r.append(edit(sg, '#include "tf_target_dummy.h"', '\n#include "tf2mc/mc_server.h"\t// TF2MC'))
    r.append(edit(sg, "\t\tif ( ( pTargetCurrent == NULL ) && !bTruceActive )\n\t\t{\n\t\t\t// target objects",
                  "\t\t// TF2MC: hostile Minecraft mobs\n\t\tif ( pTargetCurrent == NULL )\n"
                  "\t\t\tpTargetCurrent = MC_SentryFindMob( this, vecSentryOrigin, m_flSentryRange );\n\n", after=False))

    # --- TF2 explosions carve Minecraft terrain
    r.append(edit(gr, '#include "tier3/tier3.h"', '\n#ifdef GAME_DLL\n#include "tf2mc/mc_server.h"\t// TF2MC\n#endif'))
    r.append(edit(gr, "void CTFGameRules::RadiusDamage( CTFRadiusDamageInfo &info )\n{\n",
                  "\tMC_OnRadiusDamage( info.vecSrc, info.flRadius, info.dmgInfo ? info.dmgInfo->GetAttacker() : NULL );\t// TF2MC\n\n"))

    # --- Minecraft-width player hull while the bridge is live (1-block doorways at ~46 units/block)
    r.append(edit(gr, "const CViewVectors *CTFGameRules::GetViewVectors() const\n{\n",
                  "\tif ( const CViewVectors *pMC = MC_GetViewVectors() )\t// TF2MC: Minecraft-width hull\n\t\treturn pMC;\n"))
    r.append(edit(gr, "const CViewVectors *CTFGameRules::GetViewVectors() const\n",
                  "const CViewVectors *MC_GetViewVectors();\t// TF2MC (tf2mc/mc_bridge.cpp)\n", after=False))

    # --- round restarts ("waiting for players", every new round) must not delete the Minecraft world/mobs
    r.append(edit(gr, "bool CTFGameRules::RoundCleanupShouldIgnore( CBaseEntity *pEnt )\n{\n",
                  "\tif ( FClassnameIs( pEnt, \"mc_world\" ) || FClassnameIs( pEnt, \"mc_mob\" ) )\t// TF2MC: keep the Minecraft world across round restarts\n\t\treturn true;\n\n"))

    # --- flamethrowers burn Minecraft mobs
    for var in ("pEnt", "pEntity"):
        old = f'FClassnameIs( {var}, "func_breakable" )'
        r.append(edit(fl, old, None, replace=f'FClassnameIs( {var}, "mc_mob" ) /* TF2MC */ || {old}'))

    # --- build files
    r.append(edit(cv, '$Include "$SRCDIR\\game\\client\\client_econ_base.vpc"', '\n$Include "$SRCDIR\\game\\client\\tf2mc\\tf2mc_client.vpc"\t// TF2MC'))
    r.append(edit(sv, '$Include "$SRCDIR\\game\\server\\server_econ_base.vpc"', '\n$Include "$SRCDIR\\game\\server\\tf2mc\\tf2mc_server.vpc"\t// TF2MC'))
    print(f"TF2MC hooks: {r.count('ok')} inserted, {r.count('already')} already present")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1])
