package dev.tf2mc.bridge.mc;

import dev.tf2mc.bridge.BridgeConfig;
import dev.tf2mc.bridge.net.Packets;
import dev.tf2mc.bridge.net.Session;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.DataLayer;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Streams chunk columns around bridged players to one session as palettised 16^3 sections
 * (block *states* + sky/block light), plus block edits and light updates.
 */
public final class WorldStreamer {
    private final Session session;
    private final BridgeConfig cfg;
    private final Set<Long> sent = new HashSet<>();
    private final Set<Integer> sentStates = new HashSet<>();
    private final List<long[]> pending = new ArrayList<>();   // {key, distSq}
    private record SKey(int cx, int sy, int cz, boolean late) {}
    private final Map<SKey, Integer> lightDue = new HashMap<>();   // section -> tick to resend light
    private int recomputeIn = 0;
    private long tick;

    public WorldStreamer(Session session, BridgeConfig cfg) {
        this.session = session;
        this.cfg = cfg;
    }

    static long key(int cx, int cz) { return ((long) cx << 32) ^ (cz & 0xFFFFFFFFL); }
    static int kx(long k) { return (int) (k >> 32); }
    static int kz(long k) { return (int) k; }

    public boolean hasColumn(int cx, int cz) { return sent.contains(key(cx, cz)); }

    /** Sends STATE_DEFS for any states this session hasn't seen yet. */
    void ensureStates(ServerLevel level, Iterable<BlockState> states) {
        List<Packets.StateDef> defs = null;
        for (BlockState st : states) {
            int id = StateDefs.id(st);
            if (sentStates.add(id)) {
                if (defs == null) defs = new ArrayList<>();
                defs.add(StateDefs.get(level, st));
                if (defs.size() >= 512) { session.send(Packets.stateDefs(defs)); defs = null; }
            }
        }
        if (defs != null) session.send(Packets.stateDefs(defs));
    }

    public void blockChanged(ServerLevel level, BlockPos pos, BlockState state) {
        if (!hasColumn(pos.getX() >> 4, pos.getZ() >> 4)) return;
        ensureStates(level, List.of(state));
        session.send(Packets.blockSet(pos.getX(), pos.getY(), pos.getZ(), StateDefs.id(state)));
        // Light settles over the next ticks: resend the 3x3x3 sections around the edit, twice.
        int cx = pos.getX() >> 4, sy = pos.getY() >> 4, cz = pos.getZ() >> 4;
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++) {
                    lightDue.put(new SKey(cx + dx, sy + dy, cz + dz, false), (int) (tick + 3));
                    lightDue.put(new SKey(cx + dx, sy + dy, cz + dz, true), (int) (tick + 25));   // late pass
                }
    }

    /** Drops everything already sent (used after a dimension switch). */
    public void reset() {
        sent.clear();
        pending.clear();
        lightDue.clear();
        recomputeIn = 0;
    }

    /** @param centres chunk centres (cx, cz) of interest */
    public void tick(ServerLevel level, List<int[]> centres) {
        tick++;
        int r = cfg.chunkRadius;
        if (--recomputeIn <= 0) {
            recomputeIn = 10;
            Set<Long> wanted = new HashSet<>();
            pending.clear();
            for (int[] c : centres)
                for (int dx = -r; dx <= r; dx++)
                    for (int dz = -r; dz <= r; dz++) {
                        if (dx * dx + dz * dz > r * r + 1) continue;
                        long k = key(c[0] + dx, c[1] + dz);
                        if (!wanted.add(k)) continue;
                        if (!sent.contains(k)) pending.add(new long[]{k, (long) dx * dx + (long) dz * dz});
                    }
            pending.sort((a, b) -> Long.compare(a[1], b[1]));
            int ur = r + 2;
            sent.removeIf(k -> {
                for (int[] c : centres) {
                    int dx = kx(k) - c[0], dz = kz(k) - c[1];
                    if (dx * dx + dz * dz <= ur * ur) return false;
                }
                session.send(Packets.columnUnload(kx(k), kz(k)));
                return true;
            });
        }

        int budget = cfg.columnsPerTick;
        for (int i = 0; i < pending.size() && budget > 0; ) {
            if (session.queuedBytes() > cfg.maxQueuedBytes) break;
            long k = pending.get(i)[0];
            LevelChunk chunk = level.getChunkSource().getChunk(kx(k), kz(k), false);
            if (chunk == null) { i++; continue; }   // not generated / loaded yet; retry on recompute
            sendColumn(level, chunk, kx(k), kz(k));
            sent.add(k);
            pending.remove(i);
            budget--;
        }

        // Scheduled light refreshes
        if (!lightDue.isEmpty()) {
            Iterator<Map.Entry<SKey, Integer>> it = lightDue.entrySet().iterator();
            while (it.hasNext()) {
                Map.Entry<SKey, Integer> e = it.next();
                if (e.getValue() > tick) continue;
                it.remove();
                int cx = e.getKey().cx(), sy = e.getKey().sy(), cz = e.getKey().cz();
                if (!hasColumn(cx, cz)) continue;
                byte[][] l = light(level, cx, sy, cz);
                if (l[0] != null || l[1] != null) session.send(Packets.light(cx, sy, cz, l[0], l[1]));
            }
        }
    }

    static byte[][] light(ServerLevel level, int cx, int sy, int cz) {
        byte[] sky = null, blk = null;
        try {
            SectionPos sp = SectionPos.of(cx, sy, cz);
            if (level.dimensionType().hasSkyLight()) {
                DataLayer d = level.getLightEngine().getLayerListener(LightLayer.SKY).getDataLayerData(sp);
                if (d != null) sky = d.getData().clone();
            }
            DataLayer b = level.getLightEngine().getLayerListener(LightLayer.BLOCK).getDataLayerData(sp);
            if (b != null) blk = b.getData().clone();
        } catch (RuntimeException ignored) {
            // light engine not ready for this section
        }
        if (sky != null && sky.length != 2048) sky = null;
        if (blk != null && blk.length != 2048) blk = null;
        return new byte[][]{sky, blk};
    }

    static boolean trivialLight(byte[] sky, byte[] blk) {
        if (blk != null) for (byte v : blk) if (v != 0) return false;
        if (sky != null) for (byte v : sky) if (v != (byte) 0xFF) return false;
        return true;
    }

    private void sendColumn(ServerLevel level, LevelChunk chunk, int cx, int cz) {
        LevelChunkSection[] sections = chunk.getSections();
        int minSection = level.getMinY() >> 4;
        int[] palette = new int[64];
        Map<BlockState, Integer> index = new IdentityHashMap<>();
        short[] idx = new short[4096];
        for (int i = 0; i < sections.length; i++) {
            LevelChunkSection s = sections[i];
            int sy = minSection + i;
            byte[][] l = light(level, cx, sy, cz);
            if (s == null || s.hasOnlyAir()) {
                // Air sections still matter for lighting (dark caves, torch light)
                if (!trivialLight(l[0], l[1])) session.send(Packets.light(cx, sy, cz, l[0], l[1]));
                continue;
            }
            index.clear();
            int n = 0;
            for (int y = 0; y < 16; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        BlockState st = s.getBlockState(x, y, z);
                        Integer pi = index.get(st);
                        if (pi == null) {
                            pi = n++;
                            index.put(st, pi);
                            if (pi >= palette.length) palette = java.util.Arrays.copyOf(palette, palette.length * 2);
                            palette[pi] = StateDefs.id(st);
                        }
                        idx[(y * 16 + z) * 16 + x] = (short) (int) pi;
                    }
            ensureStates(level, index.keySet());
            session.send(Packets.section(cx, sy, cz, palette, n, idx, l[0], l[1]));
        }
    }
}
