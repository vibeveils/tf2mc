package dev.tf2mc.bridge;

import dev.tf2mc.bridge.net.BridgeServer;
import dev.tf2mc.bridge.net.Buf;
import dev.tf2mc.bridge.net.Packets;
import dev.tf2mc.bridge.net.Session;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Standalone stand-in for the Minecraft server, speaking the real bridge protocol through the
 * real {@link BridgeServer}. Lets the Source mod (or tests/e2e_probe) be exercised without Minecraft.
 *
 * World: flat (bedrock y=-64, stone to y=60, dirt 61-62, grass 63), a zombie walking in a circle.
 *
 * Usage: java -cp build/classes dev.tf2mc.bridge.FakeWorldServer [port] [seconds]
 */
public final class FakeWorldServer {
    static final int MIN_SECTION = -4, SECTIONS = 24, SPAWN_Y = 64;

    final Map<Long, String> edits = new HashMap<>();
    final Map<Integer, double[]> players = new HashMap<>();      // slot -> x,y,z
    final Map<Integer, Integer> digTicks = new HashMap<>();
    final Map<Session, Set<Long>> sentColumns = new HashMap<>();
    double zombieHp = 20;
    boolean zombieAlive = true;
    int zombieAttackCooldown;
    long tick;

    static long key(int x, int y, int z) { return ((long) x & 0x3FFFFFF) << 38 | ((long) z & 0x3FFFFFF) << 12 | (y & 0xFFF); }
    static long col(int cx, int cz) { return ((long) cx << 32) ^ (cz & 0xFFFFFFFFL); }

    String blockAt(int x, int y, int z) {
        String e = edits.get(key(x, y, z));
        if (e != null) return e;
        if (y == -64) return "minecraft:bedrock";
        if (y < 61 && y > -64) return "minecraft:stone";
        if (y < 63 && y >= 61) return "minecraft:dirt";
        if (y == 63) return "minecraft:grass_block";
        if (y == 64 && Math.floorMod(x * 7 + z * 13, 11) == 0) return "minecraft:short_grass";
        return "minecraft:air";
    }

    static final String[] STATES = {"minecraft:air", "minecraft:bedrock", "minecraft:stone", "minecraft:dirt", "minecraft:grass_block", "minecraft:short_grass", "minecraft:torch"};
    static int stateId(String name) { for (int i = 0; i < STATES.length; i++) if (STATES[i].equals(name)) return i; return 2; }

    static byte[] stateDefs() {
        java.util.List<Packets.StateDef> defs = new java.util.ArrayList<>();
        float[][] unit = {{0, 0, 0, 1, 1, 1}}, none = {};
        for (int i = 0; i < STATES.length; i++) {
            String n = STATES[i];
            boolean air = n.endsWith(":air"), plant = n.endsWith("short_grass") || n.endsWith("torch");
            int flags = air || plant ? 0 : Packets.STATE_COLLIDES | Packets.STATE_OPAQUE_CUBE;
            float[][] shape = air ? none : plant ? new float[][]{{0.4f, 0, 0.4f, 0.6f, 0.6f, 0.6f}} : unit;
            defs.add(new Packets.StateDef(i, n, "", flags, n.endsWith("torch") ? 14 : 0, air || plant ? none : unit, shape));
        }
        return Packets.stateDefs(defs);
    }

    void sendColumn(Session s, int cx, int cz) {
        for (int sy = MIN_SECTION; sy < MIN_SECTION + SECTIONS; sy++) {
            int[] palette = new int[64];
            Map<String, Integer> index = new HashMap<>();
            short[] idx = new short[4096];
            boolean any = false;
            for (int y = 0; y < 16; y++)
                for (int z = 0; z < 16; z++)
                    for (int x = 0; x < 16; x++) {
                        String b = blockAt(cx * 16 + x, sy * 16 + y, cz * 16 + z);
                        if (!b.equals("minecraft:air")) any = true;
                        Integer i = index.get(b);
                        if (i == null) { i = index.size(); index.put(b, i); palette[i] = stateId(b); }
                        idx[(y * 16 + z) * 16 + x] = (short) (int) i;
                    }
            // sky light: 15 above the surface (y > 63), 0 below
            byte[] sky = new byte[2048];
            for (int k = 0; k < 2048; k++) sky[k] = (byte) (sy * 16 + ((k * 2) >> 8) > 63 ? 0xFF : 0x00);
            if (any) s.send(Packets.section(cx, sy, cz, palette, index.size(), idx, sky, null));
        }
    }

    void broadcast(BridgeServer srv, byte[] f) { srv.broadcast(f, -1); }

    void onPacket(BridgeServer srv, Session.Incoming in) {
        Buf b = in.buf();
        Session s = in.session();
        switch (in.type()) {
            case Packets.C2S_HELLO -> {
                int ver = b.ru16();
                s.role = b.ru8();
                s.name = b.rstr();
                s.helloDone = true;
                System.out.println("[fake] HELLO v" + ver + " role=" + s.role + " name=" + s.name);
                s.send(Packets.helloAck(MIN_SECTION, SECTIONS, 0, SPAWN_Y, 0, "fake_flat", "minecraft:overworld", 1, true));
                s.send(stateDefs());
                sentColumns.put(s, new HashSet<>());
                if (zombieAlive)
                    s.send(Packets.entitySpawn(1000, "minecraft:zombie", 4, SPAWN_Y, 4, 0, 0, 0.6f, 1.95f, (float) zombieHp, 20, Packets.ENT_LIVING | Packets.ENT_HOSTILE));
            }
            case Packets.C2S_PLAYER_JOIN -> {
                int slot = b.ru16(); String name = b.rstr(); int mode = b.ru8();
                System.out.println("[fake] JOIN slot=" + slot + " name=" + name + " mode=" + mode);
                players.put(slot, new double[]{0.5, SPAWN_Y, 0.5});
                s.send(Packets.inventory(slot, 0, new String[]{"minecraft:stone", "minecraft:oak_planks", "minecraft:diamond_pickaxe", "", "", "", "", "", ""},
                        new int[]{64, 32, 1, 0, 0, 0, 0, 0, 0}));
            }
            case Packets.C2S_PLAYER_LEAVE -> players.remove(b.ru16());
            case Packets.C2S_PLAYER_MOVE -> {
                int slot = b.ru16();
                double[] p = players.computeIfAbsent(slot, k -> new double[3]);
                p[0] = b.rf64(); p[1] = b.rf64(); p[2] = b.rf64();
            }
            case Packets.C2S_DIG -> {
                int slot = b.ru16(); int action = b.ru8(); int x = b.ri32(), y = b.ri32(), z = b.ri32();
                if (action == Packets.DIG_ABORT) { digTicks.remove(slot); return; }
                int t = digTicks.merge(slot, 1, Integer::sum);
                broadcast(srv, Packets.digProgress(x, y, z, Math.min(9, t * 2)));
                if (t >= 5) {
                    digTicks.remove(slot);
                    edits.put(key(x, y, z), "minecraft:air");
                    broadcast(srv, Packets.blockSet(x, y, z, 0));
                    broadcast(srv, Packets.digProgress(x, y, z, -1));
                    System.out.println("[fake] broke " + x + "," + y + "," + z);
                }
            }
            case Packets.C2S_USE_ON_BLOCK -> {
                int slot = b.ru16(); int x = b.ri32(), y = b.ri32(), z = b.ri32(); int face = b.ru8();
                int[][] off = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                int px = x + off[face][0], py = y + off[face][1], pz = z + off[face][2];
                edits.put(key(px, py, pz), "minecraft:stone");
                broadcast(srv, Packets.blockSet(px, py, pz, stateId("minecraft:stone")));
                System.out.println("[fake] placed " + px + "," + py + "," + pz + " by " + slot);
            }
            case Packets.C2S_ATTACK -> {
                int slot = b.ru16(); int id = b.ri32(); int mode = b.ru8(); float dmg = b.rf32();
                if (id == 1000 && zombieAlive) {
                    zombieHp -= (mode == Packets.ATTACK_MC_MELEE ? 1 : dmg);
                    System.out.println("[fake] zombie hit by " + slot + " for " + dmg + " hp=" + zombieHp);
                    if (zombieHp <= 0) { zombieAlive = false; broadcast(srv, Packets.entityRemove(1000)); }
                }
            }
            case Packets.C2S_SUBSCRIBE_SLOT -> s.subscribedSlot = b.ru16();
            case Packets.C2S_OPEN_INVENTORY -> {
                int slot = b.ru16();
                java.util.List<Packets.SlotView> v = new java.util.ArrayList<>();
                for (int i = 0; i < 9; i++) v.add(new Packets.SlotView(8 + i * 18, 142, i == 0 ? "minecraft:stone" : "", i == 0 ? 64 : 0, 255));
                srv.broadcast(Packets.container(slot, 0, "inventory", "", v, "", 0), -1);
            }
            case Packets.C2S_EXPLOSION -> {
                b.ru16(); double x = b.rf64(), y = b.rf64(), z = b.rf64(); float power = b.rf32();
                int bx = (int) Math.floor(x), by = (int) Math.floor(y), bz = (int) Math.floor(z);
                int r = Math.max(1, Math.round(power));
                for (int dx = -r; dx <= r; dx++) for (int dy = -r; dy <= r; dy++) for (int dz = -r; dz <= r; dz++)
                    if (dx * dx + dy * dy + dz * dz <= r * r && by + dy > -64) {
                        edits.put(key(bx + dx, by + dy, bz + dz), "minecraft:air");
                        broadcast(srv, Packets.blockSet(bx + dx, by + dy, bz + dz, 0));
                    }
                System.out.println("[fake] explosion power " + power + " at " + bx + "," + by + "," + bz);
            }
            default -> { }
        }
    }

    void tick(BridgeServer srv) {
        tick++;
        // Stream columns around players (radius 3) to every session
        for (Session s : srv.sessions()) {
            Set<Long> sent = sentColumns.get(s);
            if (sent == null) continue;
            int budget = 4;
            Iterable<double[]> centres = players.isEmpty() ? java.util.List.of(new double[]{0, 0, 0}) : players.values();
            outer:
            for (double[] p : centres) {
                int pcx = (int) Math.floor(p[0]) >> 4, pcz = (int) Math.floor(p[2]) >> 4;
                for (int dx = -3; dx <= 3; dx++)
                    for (int dz = -3; dz <= 3; dz++) {
                        long k = col(pcx + dx, pcz + dz);
                        if (sent.add(k)) {
                            sendColumn(s, pcx + dx, pcz + dz);
                            if (--budget == 0) break outer;
                        }
                    }
            }
        }
        if (zombieAlive) {
            double a = tick * 0.05;
            double zx = 4 + Math.cos(a) * 3, zz = 4 + Math.sin(a) * 3;
            float yaw = (float) Math.toDegrees(Math.atan2(-Math.sin(a + Math.PI / 2), Math.cos(a + Math.PI / 2)));
            broadcast(srv, Packets.entityUpdate(1000, zx, SPAWN_Y, zz, yaw, 0, (float) zombieHp, 0));
            if (zombieAttackCooldown > 0) zombieAttackCooldown--;
            for (Map.Entry<Integer, double[]> e : players.entrySet()) {
                double[] p = e.getValue();
                double d2 = (p[0] - zx) * (p[0] - zx) + (p[2] - zz) * (p[2] - zz);
                if (d2 < 2.0 && Math.abs(p[1] - SPAWN_Y) < 2 && zombieAttackCooldown == 0) {
                    zombieAttackCooldown = 20;
                    srv.broadcast(Packets.playerHurt(e.getKey(), 3, 1000, Packets.DMG_MELEE, zx, SPAWN_Y, zz), Packets.ROLE_AUTHORITY);
                }
            }
        }
        if (tick % 20 == 0) broadcast(srv, Packets.time(6000 + tick));
    }

    public static void main(String[] args) throws Exception {
        int port = args.length > 0 ? Integer.parseInt(args[0]) : 25599;
        long seconds = args.length > 1 ? Long.parseLong(args[1]) : Long.MAX_VALUE / 2000;
        BridgeServer srv = new BridgeServer("127.0.0.1", port, 34);
        FakeWorldServer w = new FakeWorldServer();
        System.out.println("[fake] listening on " + srv.port());
        long end = System.currentTimeMillis() + seconds * 1000;
        while (System.currentTimeMillis() < end) {
            long t0 = System.nanoTime();
            srv.drain(s -> System.out.println("[fake] session " + s.id + " from " + s.remote()),
                    in -> w.onPacket(srv, in),
                    s -> { w.sentColumns.remove(s); System.out.println("[fake] session " + s.id + " closed"); },
                    10000);
            w.tick(srv);
            long sleep = 50 - (System.nanoTime() - t0) / 1_000_000;
            if (sleep > 0) Thread.sleep(sleep);
        }
        srv.close();
    }
}
