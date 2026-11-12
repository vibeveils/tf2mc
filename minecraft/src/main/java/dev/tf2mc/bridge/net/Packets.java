package dev.tf2mc.bridge.net;

/** Packet ids and encoders. Must match source/.../core/mc_protocol.h and docs/PROTOCOL.md. */
public final class Packets {
    private Packets() {}

    public static final int PROTOCOL_VERSION = 2;

    // Minecraft -> Source
    public static final int S2C_HELLO_ACK = 0x01;
    public static final int S2C_SECTION = 0x02;
    public static final int S2C_COLUMN_UNLOAD = 0x03;
    public static final int S2C_BLOCK_SET = 0x04;
    public static final int S2C_ENTITY_SPAWN = 0x05;
    public static final int S2C_ENTITY_UPDATE = 0x06;
    public static final int S2C_ENTITY_REMOVE = 0x07;
    public static final int S2C_PLAYER_HURT = 0x08;
    public static final int S2C_INVENTORY = 0x09;
    public static final int S2C_TIME = 0x0A;
    public static final int S2C_DIG_PROGRESS = 0x0B;
    public static final int S2C_CHAT = 0x0C;
    public static final int S2C_STATE_DEFS = 0x0D;
    public static final int S2C_LIGHT = 0x0E;
    public static final int S2C_PLAYER_STATUS = 0x0F;
    public static final int S2C_CONTAINER = 0x10;
    public static final int S2C_CONTAINER_CLOSE = 0x11;

    // Source -> Minecraft
    public static final int C2S_HELLO = 0x40;
    public static final int C2S_PLAYER_JOIN = 0x41;
    public static final int C2S_PLAYER_LEAVE = 0x42;
    public static final int C2S_PLAYER_MOVE = 0x43;
    public static final int C2S_DIG = 0x44;
    public static final int C2S_USE_ON_BLOCK = 0x45;
    public static final int C2S_USE_ITEM = 0x46;
    public static final int C2S_ATTACK = 0x47;
    public static final int C2S_SELECT_SLOT = 0x48;
    public static final int C2S_SET_MODE = 0x49;
    public static final int C2S_SUBSCRIBE_SLOT = 0x4A;
    public static final int C2S_CHAT = 0x4B;
    public static final int C2S_CONTAINER_CLICK = 0x4C;
    public static final int C2S_CONTAINER_CLOSE = 0x4D;
    public static final int C2S_OPEN_INVENTORY = 0x4E;
    public static final int C2S_RELEASE_USE = 0x4F;
    public static final int C2S_DROP = 0x50;
    public static final int C2S_EXPLOSION = 0x51;
    public static final int C2S_INTERACT_ENTITY = 0x52;

    public static final int STATE_COLLIDES = 1, STATE_OPAQUE_CUBE = 2, STATE_LIQUID = 4, STATE_CLIMBABLE = 8, STATE_WATERLOGGED = 16, STATE_LAVA = 32;

    public static final int ROLE_OBSERVER = 0, ROLE_AUTHORITY = 1;
    public static final int CLICK_PICKUP = 0, CLICK_QUICK_MOVE = 1, CLICK_SWAP = 2, CLICK_CLONE = 3, CLICK_THROW = 4, CLICK_QUICK_CRAFT = 5, CLICK_PICKUP_ALL = 6;
    public static final int MODE_TF_CLASS = 0, MODE_STEVE = 1;
    public static final int DIG_CONTINUE = 0, DIG_ABORT = 1;
    public static final int ATTACK_TF_DAMAGE = 0, ATTACK_MC_MELEE = 1;
    public static final int DMG_GENERIC = 0, DMG_MELEE = 1, DMG_PROJECTILE = 2, DMG_EXPLOSION = 3, DMG_FIRE = 4;
    public static final int ENT_LIVING = 1, ENT_HOSTILE = 2, ENT_PLAYER = 4, ENT_ITEM = 8, ENT_PROJECTILE = 16;

    /** Also re-sent on dimension change ("world reset"); spawn = arrival position, epoch increments. */
    public static byte[] helloAck(int minSectionY, int sectionCount, int spawnX, int spawnY, int spawnZ, String levelName,
                                  String dimension, int epoch, boolean hasSkyLight) {
        return Buf.frame(S2C_HELLO_ACK).u16(PROTOCOL_VERSION).i32(minSectionY).i32(sectionCount)
                .i32(spawnX).i32(spawnY).i32(spawnZ).str(levelName).str(dimension).i32(epoch).u8(hasSkyLight ? 1 : 0).finish();
    }

    /** One block state definition: real collision + outline shapes (block-local AABBs). */
    public record StateDef(int id, String name, String props, int flags, int light, float[][] coll, float[][] shape) {}

    public static byte[] stateDefs(java.util.List<StateDef> defs) {
        Buf b = Buf.frame(S2C_STATE_DEFS).u16(defs.size());
        for (StateDef d : defs) {
            b.i32(d.id()).str(d.name()).str(d.props()).u8(d.flags()).u8(d.light());
            for (float[][] boxes : new float[][][]{d.coll(), d.shape()}) {
                int n = Math.min(16, boxes.length);
                b.u8(n);
                for (int i = 0; i < n; i++) for (int k = 0; k < 6; k++) b.f32(boxes[i][k]);
            }
        }
        return b.finish();
    }

    private static void light(Buf b, byte[] sky, byte[] blk) {
        b.u8((sky != null ? 1 : 0) | (blk != null ? 2 : 0));
        if (sky != null) b.bytes(sky, 0, 2048);
        if (blk != null) b.bytes(blk, 0, 2048);
    }

    public static byte[] light(int cx, int sy, int cz, byte[] sky, byte[] blk) {
        Buf b = Buf.frame(S2C_LIGHT).i32(cx).i32(sy).i32(cz);
        light(b, sky, blk);
        return b.finish();
    }

    /**
     * @param palette global block state ids (STATE_DEFS must have been sent for each)
     * @param indices 4096 palette indices, index = (y*16+z)*16+x
     * @param sky,blk Minecraft DataLayer bytes (2048, nibble per block) or null
     */
    public static byte[] section(int cx, int sy, int cz, int[] palette, int paletteSize, short[] indices, byte[] sky, byte[] blk) {
        Buf b = Buf.frame(S2C_SECTION).i32(cx).i32(sy).i32(cz).u16(paletteSize);
        for (int i = 0; i < paletteSize; i++) b.i32(palette[i]);
        if (paletteSize <= 256) {
            byte[] raw = new byte[4096];
            for (int i = 0; i < 4096; i++) raw[i] = (byte) indices[i];
            b.bytes(raw, 0, 4096);
        } else {
            byte[] raw = new byte[8192];
            for (int i = 0; i < 4096; i++) {
                raw[i * 2] = (byte) indices[i];
                raw[i * 2 + 1] = (byte) (indices[i] >> 8);
            }
            b.bytes(raw, 0, 8192);
        }
        light(b, sky, blk);
        return b.finish();
    }

    public static byte[] columnUnload(int cx, int cz) {
        return Buf.frame(S2C_COLUMN_UNLOAD).i32(cx).i32(cz).finish();
    }

    public static byte[] blockSet(int x, int y, int z, int stateId) {
        return Buf.frame(S2C_BLOCK_SET).i32(x).i32(y).i32(z).i32(stateId).finish();
    }

    public static byte[] entitySpawn(int id, String type, double x, double y, double z, float yaw, float pitch,
                                     float w, float h, float hp, float maxHp, int flags) {
        return Buf.frame(S2C_ENTITY_SPAWN).i32(id).str(type).f64(x).f64(y).f64(z).f32(yaw).f32(pitch)
                .f32(w).f32(h).f32(hp).f32(maxHp).u8(flags).finish();
    }

    public static byte[] entityUpdate(int id, double x, double y, double z, float yaw, float pitch, float hp, int hurtTicks) {
        return Buf.frame(S2C_ENTITY_UPDATE).i32(id).f64(x).f64(y).f64(z).f32(yaw).f32(pitch).f32(hp).u8(hurtTicks).finish();
    }

    public static byte[] entityRemove(int id) {
        return Buf.frame(S2C_ENTITY_REMOVE).i32(id).finish();
    }

    public static byte[] playerHurt(int slot, float amount, int attackerId, int kind, double fromX, double fromY, double fromZ) {
        return Buf.frame(S2C_PLAYER_HURT).u16(slot).f32(amount).i32(attackerId).u8(kind)
                .f64(fromX).f64(fromY).f64(fromZ).finish();
    }

    public static byte[] inventory(int slot, int selected, String[] items, int[] counts) {
        Buf b = Buf.frame(S2C_INVENTORY).u16(slot).u8(selected).u8(items.length);
        for (int i = 0; i < items.length; i++) b.str(items[i]).u8(Math.min(255, counts[i]));
        return b.finish();
    }

    public static byte[] time(long dayTime) {
        return Buf.frame(S2C_TIME).i64(dayTime).finish();
    }

    public static byte[] digProgress(int x, int y, int z, int stage) {
        return Buf.frame(S2C_DIG_PROGRESS).i32(x).i32(y).i32(z).u8(stage & 0xFF).finish();
    }

    public static byte[] playerStatus(int slot, float food, float saturation, int xpLevel, float xpProgress, float armor, int flags) {
        return Buf.frame(S2C_PLAYER_STATUS).u16(slot).f32(food).f32(saturation).i32(xpLevel).f32(xpProgress).f32(armor).u8(flags).finish();
    }

    public record SlotView(int x, int y, String item, int count, int durability) {}

    /** Full container view: slot positions are Minecraft GUI pixels (176x166 inventory layout). */
    public static byte[] container(int slot, int containerId, String menuType, String title, java.util.List<SlotView> slots,
                                   String carriedItem, int carriedCount) {
        Buf b = Buf.frame(S2C_CONTAINER).u16(slot).i32(containerId).str(menuType).str(title).u16(slots.size());
        for (SlotView v : slots) b.u16(v.x() & 0xFFFF).u16(v.y() & 0xFFFF).str(v.item()).u8(Math.min(255, v.count())).u8(v.durability());
        b.str(carriedItem).u8(Math.min(255, carriedCount));
        return b.finish();
    }

    public static byte[] containerClose(int slot) {
        return Buf.frame(S2C_CONTAINER_CLOSE).u16(slot).finish();
    }

    public static byte[] chat(String text) {
        return Buf.frame(S2C_CHAT).str(text).finish();
    }
}
