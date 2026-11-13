//========= TF2MC =========//
#ifndef MC_SERVER_H
#define MC_SERVER_H
#pragma once

class CTFPlayer;
class CBaseEntity;
class Vector;

// Called from CTFPlayer::PostThink (patch). Minecraft-mode (Civilian) interaction:
// digging, placing/using, interacting, melee on mobs.
void MC_PlayerPostThink( CTFPlayer *pPlayer );
void MC_SetBuildMode( CTFPlayer *pPlayer, bool on );   // TF classes: weapon away, dig/place like Minecraft

// Called from CObjectSentrygun::FindTarget (patch) when no player/bot target was found.
CBaseEntity *MC_SentryFindMob( CBaseEntity *pSentry, const Vector &eye, float range );

// Called from CTFGameRules::RadiusDamage (patch): TF2 explosions break Minecraft blocks.
void MC_OnRadiusDamage( const Vector &src, float radius, CBaseEntity *pAttacker );

#endif
