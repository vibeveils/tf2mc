# TF2MC bridge protocol (v2)

TCP, one connection per Source DLL. `server.dll` connects as **authority** (one per Minecraft server),
each `client.dll` connects as **observer**.

**Frame:** `u32 length` (little endian; counts the type byte + payload) · `u8 type` · payload.
Integers/floats are little endian; `str` = `u16 byte length` + UTF-8. Max frame 4 MiB.
Coordinates are Minecraft world coordinates (Y up, blocks); conversion to Source happens in the game
(`mc::Coords`: `src = ((x-ox)*s, -(z-oz)*s, (y-oy)*s)`, `yaw_src = -yaw_mc - 90`).

Sources of truth: `source/src/game/shared/tf2mc/core/mc_protocol.h` and
`minecraft/src/main/java/dev/tf2mc/bridge/net/Packets.java`.

## Minecraft → Source

| id | name | payload | sent to |
|----|------|---------|---------|
| 0x01 | HELLO_ACK | u16 version, i32 minSectionY, i32 sectionCount, i32 spawnX, i32 spawnY, i32 spawnZ, str levelName, str dimension, i32 epoch, u8 flags (1 = has sky light). **Re-sent to everyone on a dimension change** (spawn = arrival point, epoch+1): receivers reset their world | requester / all |
| 0x02 | SECTION | i32 cx, i32 sy, i32 cz, u16 paletteN, i32[paletteN] block-state ids, 4096 indices (u8 if N≤256 else u16), index=(y·16+z)·16+x, then *light*: u8 flags (1 sky, 2 block), [2048 sky nibbles], [2048 block nibbles] (Minecraft DataLayer layout) | all, by interest |
| 0x0D | STATE_DEFS | u16 n, n × (i32 stateId, str block, str properties, u8 flags (1 collides, 2 opaque full cube, 4 liquid, 8 climbable, 16 waterlogged, 32 lava), u8 lightEmission, u8 nColl, nColl × 6 f32 box, u8 nShape, nShape × 6 f32 box). Boxes are block-local AABBs from `getCollisionShape` / `getShape`. Sent before any SECTION/BLOCK_SET that uses a new state | per session, lazily |
| 0x0E | LIGHT | i32 cx, i32 sy, i32 cz, light (as in SECTION). Sent for all-air sections with non-trivial light, and 3 + 25 ticks after edits around the edited section | all |
| 0x03 | COLUMN_UNLOAD | i32 cx, i32 cz | all |
| 0x04 | BLOCK_SET | i32 x, i32 y, i32 z, i32 stateId | sessions that have the column |
| 0x05 | ENTITY_SPAWN | i32 id, str type (`minecraft:zombie`, items: `minecraft:item/<item>`), f64 x y z (feet), f32 yaw, f32 pitch, f32 width, f32 height, f32 hp, f32 maxHp, u8 flags (1 living, 2 hostile, 4 player, 8 item, 16 projectile) | authority |
| 0x06 | ENTITY_UPDATE | i32 id, f64 x y z, f32 yaw, f32 pitch, f32 hp, u8 hurtTicks | authority |
| 0x07 | ENTITY_REMOVE | i32 id | authority |
| 0x08 | PLAYER_HURT | u16 slot, f32 amount (already ×`mc-to-tf-damage`), i32 attackerEntityId (−1 none), u8 kind (0 generic,1 melee,2 projectile,3 explosion,4 fire), f64 fromX fromY fromZ | authority |
| 0x09 | INVENTORY | u16 slot, u8 selected, u8 n, n × (str item, u8 count) — hotbar | authority + observer subscribed to slot |
| 0x0A | TIME | i64 dayTime | all (1 Hz) |
| 0x0B | DIG_PROGRESS | i32 x y z, i8 stage (−1 = clear, 0..9) | all |
| 0x0C | CHAT | str — Minecraft chat, deaths, advancements | authority |
| 0x0F | PLAYER_STATUS | u16 slot, f32 food, f32 saturation, i32 xpLevel, f32 xpProgress, f32 armor, u8 flags (1 using item) | authority + subscribed observer |
| 0x10 | CONTAINER | u16 slot, i32 containerId, str menuType (`inventory`, `minecraft:crafting`, `minecraft:generic_9x3`…), str title, u16 n, n × (u16 x, u16 y (Minecraft GUI pixels), str item, u8 count, u8 durability 0-255), str carriedItem, u8 carriedCount | subscribed observer |
| 0x11 | CONTAINER_CLOSE | u16 slot (Minecraft closed the menu) | subscribed observer |

## Source → Minecraft

| id | name | payload | from |
|----|------|---------|------|
| 0x40 | HELLO | u16 version, u8 role (0 observer, 1 authority), str name (`secret:name` if a secret is set) | any |
| 0x41 | PLAYER_JOIN | u16 slot (Source entindex), str name, u8 mode (0 TF class, 1 Minecraft) | authority |
| 0x42 | PLAYER_LEAVE | u16 slot | authority |
| 0x43 | PLAYER_MOVE | u16 slot, f64 x y z (feet), f32 yaw, f32 pitch, u8 flags (1 onGround, 2 crouch, 4 sprint), i32 epoch (moves from an older epoch are ignored) | authority (20 Hz) |
| 0x44 | DIG | u16 slot, u8 action (0 dig/continue, 1 abort), i32 x y z, u8 face (MC `Direction` 3D value) | authority |
| 0x45 | USE_ON_BLOCK | u16 slot, i32 x y z, u8 face, f32 hitX hitY hitZ (0..1 within block) | authority |
| 0x46 | USE_ITEM | u16 slot | authority |
| 0x47 | ATTACK | u16 slot, i32 entityId, u8 mode (0 TF damage, 1 vanilla melee), f32 damage (MC hp), f32 pushX, f32 pushZ, f32 knockback, u8 kind | authority |
| 0x48 | SELECT_SLOT | u16 slot, u8 hotbar (0..8) | authority |
| 0x49 | SET_MODE | u16 slot, u8 mode | authority |
| 0x4A | SUBSCRIBE_SLOT | u16 slot (receive this player's INVENTORY) | observer |
| 0x4B | CHAT | str | any |
| 0x4C | CONTAINER_CLICK | u16 slot, i32 containerId, i16 slotIndex (−999 = outside), u8 button, u8 clickType (Minecraft `ClickType` ordinal) | authority |
| 0x4D | CONTAINER_CLOSE | u16 slot | authority |
| 0x4E | OPEN_INVENTORY | u16 slot | authority |
| 0x4F | RELEASE_USE | u16 slot (bow release, stop eating/blocking) | authority |
| 0x50 | DROP | u16 slot, u8 wholeStack | authority |
| 0x51 | EXPLOSION | u16 slot, f64 x y z, f32 power — breaks blocks only (no entity damage/knockback) | authority |
| 0x52 | INTERACT_ENTITY | u16 slot, i32 entityId (trade, shear, milk, ride…) | authority |

Digging: the authority repeats `DIG` (≥4 Hz) while the button is held; Minecraft advances real destroy
progress each tick (tool/enchantment/haste aware) and aborts if no `DIG` arrives for 1 s.

## Floating origin
Not part of the wire format. The TF2 server picks the Minecraft point mapped to Source (0, 0, `mc_origin_y`),
re-centres it on the players (snapped to 16 blocks), and publishes it to clients through the `mc_world`
entity's network vars. All protocol coordinates stay absolute Minecraft coordinates.
