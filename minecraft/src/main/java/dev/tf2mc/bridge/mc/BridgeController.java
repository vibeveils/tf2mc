package dev.tf2mc.bridge.mc;

import dev.tf2mc.bridge.BridgeConfig;
import dev.tf2mc.bridge.TF2Bridge;
import dev.tf2mc.bridge.net.BridgeServer;
import dev.tf2mc.bridge.net.Buf;
import dev.tf2mc.bridge.net.Packets;
import dev.tf2mc.bridge.net.Session;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.inventory.AbstractContainerMenu;
import net.minecraft.world.inventory.ClickType;
import net.minecraft.world.inventory.Slot;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.SwingAnimation;
import net.minecraft.world.level.Explosion;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.Vec3;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Owns the TCP bridge and translates between protocol packets and the Minecraft world. Game thread only.
 *
 * The TF2 game shows one dimension at a time (the "active level"). When any bridged player is moved to
 * another dimension by a portal, the whole party follows: every session gets a fresh HELLO_ACK with a new
 * epoch, the arrival point as spawn, and the world is re-streamed.
 */
public final class BridgeController {
    private final MinecraftServer server;
    private final BridgeConfig cfg;
    private final BridgeServer net;
    private final Map<Integer, BridgedPlayer> players = new HashMap<>();   // slot -> player (authority session's players)
    private final Map<Session, WorldStreamer> worldStreamers = new HashMap<>();
    private final Map<Session, EntityStreamer> entityStreamers = new HashMap<>();
    private final Map<Integer, int[]> lastDigStage = new HashMap<>();
    private Session authority;
    private long ticks;

    private ServerLevel active;
    private BlockPos arrival;
    private int epoch = 1;

    private static final ExplosionDamageCalculator BLOCKS_ONLY = new ExplosionDamageCalculator() {
        @Override
        public boolean shouldDamageEntity(Explosion explosion, Entity entity) { return false; }   // TF2 already applied the damage
        @Override
        public float getKnockbackMultiplier(Entity entity) { return 0f; }
    };

    private BridgeController(MinecraftServer server, BridgeConfig cfg, BridgeServer net) {
        this.server = server;
        this.cfg = cfg;
        this.net = net;
        this.active = server.overworld();
        this.arrival = active.getLevelData().getRespawnData().pos();
    }

    public static BridgeController start(MinecraftServer server, BridgeConfig cfg) {
        try {
            BridgeServer net = new BridgeServer(cfg.bindHost, cfg.port, cfg.maxSessions);
            TF2Bridge.LOG.info("TF2 bridge listening on {}:{}", cfg.bindHost, net.port());
            return new BridgeController(server, cfg, net);
        } catch (IOException e) {
            TF2Bridge.LOG.error("TF2 bridge could not bind {}:{}", cfg.bindHost, cfg.port, e);
            return null;
        }
    }

    private ServerLevel level() { return active; }

    public void shutdown() {
        for (BridgedPlayer p : new ArrayList<>(players.values())) p.remove();
        players.clear();
        net.close();
    }

    // ------------------------------------------------------------------ tick

    public void tick() {
        ticks++;
        net.drain(this::onOpen, this::onPacket, this::onClose, 20000);

        // Party portal travel: follow the first bridged player that changed dimension
        for (BridgedPlayer p : players.values()) {
            if (p.level() != active) { switchLevel(p.level(), p.blockPosition(), p); break; }
        }

        ServerLevel level = level();
        List<int[]> centres = new ArrayList<>();
        for (BridgedPlayer p : players.values()) centres.add(new int[]{p.getBlockX() >> 4, p.getBlockZ() >> 4});
        if (centres.isEmpty()) centres.add(new int[]{arrival.getX() >> 4, arrival.getZ() >> 4});
        for (Map.Entry<Session, WorldStreamer> e : worldStreamers.entrySet())
            if (e.getKey().helloDone) e.getValue().tick(level, centres);

        List<BridgedPlayer> list = new ArrayList<>(players.values());
        for (EntityStreamer es : entityStreamers.values()) es.tick(level, list);

        for (BridgedPlayer p : list) {
            int stage = p.digStage();
            BlockPos dp = p.digPos();
            int[] last = lastDigStage.get(p.slot);
            int[] now = dp == null ? null : new int[]{dp.getX(), dp.getY(), dp.getZ(), stage};
            if (now != null && (last == null || last[3] != stage || last[0] != now[0] || last[1] != now[1] || last[2] != now[2])) {
                net.broadcast(Packets.digProgress(now[0], now[1], now[2], stage), -1);
                lastDigStage.put(p.slot, now);
            } else if (now == null && last != null) {
                net.broadcast(Packets.digProgress(last[0], last[1], last[2], -1), -1);
                lastDigStage.remove(p.slot);
            }
            if (ticks % 5 == 0) syncInventory(p, false);
            if (ticks % 10 == 0) syncStatus(p, false);
            syncContainer(p);
        }

        if (ticks % 20 == 0) net.broadcast(Packets.time(dayTime(level)), -1);
    }

    private static long dayTime(ServerLevel level) {
        // The day-time accessor has moved between versions; look it up reflectively so a rename
        // only disables day/night sync instead of breaking the mod.
        for (String m : new String[]{"getDayTime", "getOverworldClockTime"}) {
            try {
                Object v = level.getClass().getMethod(m).invoke(level);
                if (v instanceof Long l) return l;
            } catch (ReflectiveOperationException | RuntimeException ignored) {}
        }
        return 6000;
    }

    private byte[] helloAck() {
        ServerLevel level = level();
        return Packets.helloAck(level.getMinY() >> 4, level.getHeight() >> 4, arrival.getX(), arrival.getY(), arrival.getZ(),
                server.getWorldData().getLevelName(), level.dimension().identifier().toString(), epoch, level.dimensionType().hasSkyLight());
    }

    private void switchLevel(ServerLevel to, BlockPos at, BridgedPlayer traveller) {
        TF2Bridge.LOG.info("TF2 bridge: party follows {} to {} at {}", traveller.getGameProfile().name(), to.dimension().identifier(), at);
        active = to;
        arrival = at;
        epoch++;
        for (BridgedPlayer p : players.values()) {
            p.abortDig();
            if (p.level() != to)
                p.teleportTo(to, at.getX() + 0.5, at.getY(), at.getZ() + 0.5, java.util.Set.of(), p.getYRot(), p.getXRot(), true);
        }
        for (WorldStreamer ws : worldStreamers.values()) ws.reset();
        for (EntityStreamer es : entityStreamers.values()) es.reset();
        byte[] ack = helloAck();
        net.broadcast(ack, -1);
        net.broadcast(Packets.chat("Entering " + to.dimension().identifier().getPath().replace('_', ' ')), Packets.ROLE_AUTHORITY);
    }

    public void onBlockChanged(ServerLevel level, BlockPos pos, BlockState state) {
        if (level != level()) return;
        for (WorldStreamer ws : worldStreamers.values()) ws.blockChanged(level, pos, state);
    }

    /** Minecraft chat / system messages (deaths, advancements, joins) shown in TF2 chat. */
    public void onChat(String text) {
        if (text == null || text.isEmpty() || text.startsWith("[TF2]")) return;
        if (text.matches("(?s).*\\btf\\d+_\\w* (joined|left) the game.*")) return;   // our own stand-ins
        net.broadcast(Packets.chat(text), Packets.ROLE_AUTHORITY);
    }

    // ------------------------------------------------------------------ sessions

    private void onOpen(Session s) {
        TF2Bridge.LOG.info("TF2 bridge: connection {} from {}", s.id, s.remote());
    }

    private void onClose(Session s) {
        TF2Bridge.LOG.info("TF2 bridge: connection {} ({}) closed", s.id, s.name);
        worldStreamers.remove(s);
        entityStreamers.remove(s);
        if (s == authority) {
            for (BridgedPlayer p : new ArrayList<>(players.values())) p.remove();
            players.clear();
            authority = null;
        }
    }

    private void onPacket(Session.Incoming in) {
        Session s = in.session();
        Buf b = in.buf();
        try {
            if (in.type() == Packets.C2S_HELLO) { handleHello(s, b); return; }
            if (!s.helloDone) return;
            switch (in.type()) {
                case Packets.C2S_SUBSCRIBE_SLOT -> {
                    s.subscribedSlot = b.ru16();
                    BridgedPlayer p = players.get(s.subscribedSlot);
                    if (p != null) { syncInventory(p, true); syncStatus(p, true); p.lastContainerHash = 0; }
                }
                case Packets.C2S_CHAT -> {
                    String msg = b.rstr();
                    server.getPlayerList().broadcastSystemMessage(Component.literal("[TF2] " + msg), false);
                }
                default -> { if (s == authority) handleAuthority(s, in.type(), b); }
            }
        } catch (RuntimeException e) {
            TF2Bridge.LOG.warn("TF2 bridge: bad packet 0x{} from {}", Integer.toHexString(in.type()), s.name, e);
        }
    }

    private void handleHello(Session s, Buf b) {
        int version = b.ru16();
        int role = b.ru8();
        String name = b.rstr();
        if (version != Packets.PROTOCOL_VERSION) {
            s.send(Packets.chat("protocol mismatch: server " + Packets.PROTOCOL_VERSION + ", client " + version));
            s.close();
            return;
        }
        if (!cfg.secret.isEmpty()) {
            if (!name.startsWith(cfg.secret + ":")) { s.close(); return; }
            name = name.substring(cfg.secret.length() + 1);
        }
        if (role == Packets.ROLE_AUTHORITY) {
            if (authority != null && authority.isOpen()) {
                s.send(Packets.chat("another TF2 server is already attached"));
                s.close();
                return;
            }
            authority = s;
            entityStreamers.put(s, new EntityStreamer(s, cfg));
        }
        s.role = role;
        s.name = name;
        s.helloDone = true;
        worldStreamers.put(s, new WorldStreamer(s, cfg));
        s.send(helloAck());
        TF2Bridge.LOG.info("TF2 bridge: {} attached as {}", name, role == Packets.ROLE_AUTHORITY ? "authority" : "observer");
    }

    private void handleAuthority(Session s, int type, Buf b) {
        ServerLevel level = level();
        switch (type) {
            case Packets.C2S_PLAYER_JOIN -> {
                int slot = b.ru16();
                String name = b.rstr();
                int mode = b.ru8();
                BridgedPlayer old = players.remove(slot);
                if (old != null) old.remove();
                BridgedPlayer p = BridgedPlayer.spawn(server, level, s.id, slot, name, Vec3.atBottomCenterOf(arrival));
                p.mode = mode;
                p.hurtSink = (pl, amount, attacker, kind, from) ->
                        s.send(Packets.playerHurt(pl.slot, amount * cfg.mcToTfDamage, attacker, kind, from.x, from.y, from.z));
                players.put(slot, p);
                syncInventory(p, true);
                syncStatus(p, true);
            }
            case Packets.C2S_PLAYER_LEAVE -> {
                BridgedPlayer p = players.remove(b.ru16());
                if (p != null) p.remove();
            }
            case Packets.C2S_PLAYER_MOVE -> {
                BridgedPlayer p = players.get(b.ru16());
                double x = b.rf64(), y = b.rf64(), z = b.rf64();
                float yaw = b.rf32(), pitch = b.rf32();
                int flags = b.ru8();
                int moveEpoch = b.remaining() >= 4 ? b.ri32() : epoch;
                // Moves computed before a dimension switch would drag players to the wrong place
                if (p != null && moveEpoch == epoch && p.level() == level) p.setTargetPose(x, y, z, yaw, pitch, flags);
            }
            case Packets.C2S_SET_MODE -> {
                BridgedPlayer p = players.get(b.ru16());
                int mode = b.ru8();
                if (p != null) { p.mode = mode; if (mode != Packets.MODE_STEVE) p.abortDig(); }
            }
            case Packets.C2S_DIG -> {
                BridgedPlayer p = players.get(b.ru16());
                int action = b.ru8();
                BlockPos pos = new BlockPos(b.ri32(), b.ri32(), b.ri32());
                Direction face = Direction.from3DDataValue(b.ru8());
                if (p == null) return;
                if (action == Packets.DIG_ABORT) p.abortDig();
                else p.requestDig(pos, face);
            }
            case Packets.C2S_USE_ON_BLOCK -> {
                BridgedPlayer p = players.get(b.ru16());
                BlockPos pos = new BlockPos(b.ri32(), b.ri32(), b.ri32());
                Direction face = Direction.from3DDataValue(b.ru8());
                Vec3 hit = new Vec3(pos.getX() + b.rf32(), pos.getY() + b.rf32(), pos.getZ() + b.rf32());
                if (p == null || !level.mayInteract(p, pos)) return;
                for (InteractionHand hand : InteractionHand.values()) {
                    ItemStack stack = p.getItemInHand(hand);
                    var result = p.gameMode.useItemOn(p, level, stack, hand, new BlockHitResult(hit, face, pos, false));
                    if (result.consumesAction()) { p.swing(hand, SwingAnimation.DEFAULT, true); break; }
                }
                syncInventory(p, false);
            }
            case Packets.C2S_USE_ITEM -> {
                BridgedPlayer p = players.get(b.ru16());
                if (p == null) return;
                for (InteractionHand hand : InteractionHand.values()) {
                    if (p.gameMode.useItem(p, level, p.getItemInHand(hand), hand).consumesAction()) break;
                }
            }
            case Packets.C2S_RELEASE_USE -> {
                BridgedPlayer p = players.get(b.ru16());
                if (p != null && p.isUsingItem()) p.releaseUsingItem();   // bows, tridents, shields
            }
            case Packets.C2S_INTERACT_ENTITY -> {
                BridgedPlayer p = players.get(b.ru16());
                Entity target = level.getEntity(b.ri32());
                if (p == null || target == null || target instanceof BridgedPlayer) return;
                for (InteractionHand hand : InteractionHand.values()) {
                    if (p.interactOn(target, hand, Vec3.ZERO).consumesAction()) { p.swing(hand, SwingAnimation.DEFAULT, true); break; }
                }
            }
            case Packets.C2S_DROP -> {
                BridgedPlayer p = players.get(b.ru16());
                boolean all = b.ru8() != 0;
                if (p != null) { p.drop(all); syncInventory(p, false); }
            }
            case Packets.C2S_SELECT_SLOT -> {
                BridgedPlayer p = players.get(b.ru16());
                int hotbar = b.ru8();
                if (p != null && hotbar < 9) { p.getInventory().setSelectedSlot(hotbar); syncInventory(p, true); }
            }
            case Packets.C2S_OPEN_INVENTORY -> {
                BridgedPlayer p = players.get(b.ru16());
                if (p != null) { p.uiOpen = true; p.uiExternal = false; p.lastContainerHash = 0; }
            }
            case Packets.C2S_CONTAINER_CLOSE -> {
                BridgedPlayer p = players.get(b.ru16());
                if (p != null) {
                    p.uiOpen = false;
                    p.uiExternal = false;
                    if (p.containerMenu != p.inventoryMenu) p.closeContainer();
                    else p.inventoryMenu.removed(p);   // returns crafting-grid items / carried stack
                }
            }
            case Packets.C2S_CONTAINER_CLICK -> {
                BridgedPlayer p = players.get(b.ru16());
                int containerId = b.ri32();
                int slotIndex = (short) b.ru16();
                int button = b.ru8();
                int clickType = b.ru8();
                if (p == null) return;
                AbstractContainerMenu menu = p.containerMenu;
                if (menu.containerId != containerId) return;
                ClickType[] types = ClickType.values();
                if (clickType >= types.length) return;
                if (slotIndex != -999 && (slotIndex < 0 || slotIndex >= menu.slots.size())) return;
                menu.clicked(slotIndex, button, types[clickType], p);
                menu.broadcastChanges();
                syncInventory(p, false);
            }
            case Packets.C2S_EXPLOSION -> {
                BridgedPlayer p = players.get(b.ru16());
                double x = b.rf64(), y = b.rf64(), z = b.rf64();
                float power = b.rf32();
                if (!cfg.tfExplosionsBreakBlocks || power <= 0) return;
                level.explode(p, null, BLOCKS_ONLY, x, y, z, Math.min(power, 6f), false, Level.ExplosionInteraction.TNT);
            }
            case Packets.C2S_ATTACK -> handleAttack(level, b);
            default -> { }
        }
    }

    private void handleAttack(ServerLevel level, Buf b) {
        BridgedPlayer p = players.get(b.ru16());
        int entityId = b.ri32();
        int mode = b.ru8();
        float damage = b.rf32();
        float kx = b.rf32(), kz = b.rf32(), strength = b.rf32();
        int kind = b.ru8();
        if (p == null) return;
        Entity target = level.getEntity(entityId);
        if (target == null || target instanceof BridgedPlayer || !target.isAlive()) return;

        if (mode == Packets.ATTACK_MC_MELEE) {
            p.attack(target);   // vanilla melee: held item, enchantments, crits, attack cooldown
            p.swing(InteractionHand.MAIN_HAND, SwingAnimation.DEFAULT, true);
            return;
        }
        DamageSource src = kind == Packets.DMG_EXPLOSION ? p.damageSources().explosion(p, p) : p.damageSources().playerAttack(p);
        if (target instanceof LivingEntity le) {
            le.invulnerableTime = 0;   // TF2 weapons have their own fire rates; skip vanilla i-frames
            boolean hurt = le.hurtServer(level, src, damage);
            if (hurt && strength > 0) le.knockback(strength, -kx, -kz);
            if (kind == Packets.DMG_FIRE) le.igniteForSeconds(4);
        } else {
            target.hurtServer(level, src, damage);
        }
    }

    // ------------------------------------------------------------------ sync to TF2

    private void sendToSlot(BridgedPlayer p, byte[] frame, boolean includeAuthority) {
        for (Session s : net.sessions())
            if (s.helloDone && ((includeAuthority && s == authority) || s.subscribedSlot == p.slot)) s.send(frame);
    }

    private String itemId(BridgedPlayer p, ItemStack st) {
        return st.isEmpty() ? "" : p.level().registryAccess().lookupOrThrow(Registries.ITEM).getKey(st.getItem()).toString();
    }

    private void syncInventory(BridgedPlayer p, boolean force) {
        String[] items = new String[9];
        int[] counts = new int[9];
        int hash = p.getInventory().getSelectedSlot();
        for (int i = 0; i < 9; i++) {
            ItemStack st = p.getInventory().getItem(i);
            items[i] = itemId(p, st);
            counts[i] = st.isEmpty() ? 0 : st.getCount();
            hash = hash * 31 + items[i].hashCode() * 17 + counts[i];
        }
        if (!force && hash == p.lastInventoryHash) return;
        p.lastInventoryHash = hash;
        sendToSlot(p, Packets.inventory(p.slot, p.getInventory().getSelectedSlot(), items, counts), true);
    }

    private void syncStatus(BridgedPlayer p, boolean force) {
        float food = p.getFoodData().getFoodLevel();
        float sat = p.getFoodData().getSaturationLevel();
        int xp = p.experienceLevel;
        float prog = p.experienceProgress;
        float armor = p.getArmorValue();
        int flags = p.isUsingItem() ? 1 : 0;
        int hash = Float.hashCode(food) * 31 + Float.hashCode(sat) * 7 + xp * 131 + Float.hashCode(prog) + Float.hashCode(armor) * 3 + flags;
        if (!force && hash == p.lastStatusHash) return;
        p.lastStatusHash = hash;
        sendToSlot(p, Packets.playerStatus(p.slot, food, sat, xp, prog, armor, flags), true);
    }

    private void syncContainer(BridgedPlayer p) {
        AbstractContainerMenu menu = p.containerMenu;
        boolean external = menu != p.inventoryMenu;
        if (external && !p.uiExternal) { p.uiOpen = true; p.uiExternal = true; p.lastContainerHash = 0; }
        if (!external && p.uiExternal) {
            // Minecraft closed the menu (walked away, block broken...)
            p.uiExternal = false;
            p.uiOpen = false;
            sendToSlot(p, Packets.containerClose(p.slot), false);
            return;
        }
        if (!p.uiOpen) return;

        int hash = menu.containerId * 7919;
        List<Packets.SlotView> views = new ArrayList<>(menu.slots.size());
        for (Slot slot : menu.slots) {
            ItemStack st = slot.getItem();
            int dur = st.isDamageableItem() && st.getMaxDamage() > 0 ? 255 - (int) (255L * st.getDamageValue() / st.getMaxDamage()) : 255;
            String id = itemId(p, st);
            views.add(new Packets.SlotView(slot.x, slot.y, id, st.getCount(), dur));
            hash = hash * 31 + id.hashCode() + st.getCount() * 131 + dur;
        }
        ItemStack carried = menu.getCarried();
        hash = hash * 31 + itemId(p, carried).hashCode() + carried.getCount();
        if (hash == p.lastContainerHash) return;
        p.lastContainerHash = hash;

        String type = "inventory";
        if (external) {
            try { type = BuiltInRegistries.MENU.getKey(menu.getType()).toString(); } catch (RuntimeException ignored) { type = "container"; }
        }
        sendToSlot(p, Packets.container(p.slot, menu.containerId, type, "", views, itemId(p, carried), carried.getCount()), false);
    }
}
