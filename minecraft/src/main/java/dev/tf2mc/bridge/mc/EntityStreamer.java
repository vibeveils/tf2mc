package dev.tf2mc.bridge.mc;

import dev.tf2mc.bridge.BridgeConfig;
import dev.tf2mc.bridge.net.Packets;
import dev.tf2mc.bridge.net.Session;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.phys.AABB;

import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Mirrors entities near bridged players to the authority (TF2 server) session. */
public final class EntityStreamer {
    private record Known(double x, double y, double z, float yaw, float pitch, float hp, int hurt) {}

    private final Session session;
    private final BridgeConfig cfg;
    private final Map<Integer, Known> known = new HashMap<>();
    private int counter;

    public EntityStreamer(Session session, BridgeConfig cfg) {
        this.session = session;
        this.cfg = cfg;
    }

    public void reset() { known.clear(); counter = 0; }

    public void tick(ServerLevel level, List<BridgedPlayer> players) {
        if (++counter < cfg.entityUpdateInterval) return;
        counter = 0;

        Set<Integer> seen = new HashSet<>();
        double r = cfg.entityRadius;
        for (BridgedPlayer p : players) {
            if (p.level() != level) continue;
            AABB box = p.getBoundingBox().inflate(r, r * 0.5, r);
            for (Entity e : level.getEntities((Entity) null, box, e -> !(e instanceof BridgedPlayer) && e.isAlive())) {
                if (!seen.add(e.getId())) continue;
                float hp = e instanceof LivingEntity le ? le.getHealth() : 0;
                int hurt = e instanceof LivingEntity le ? le.hurtTime : 0;
                Known k = known.get(e.getId());
                if (k == null) {
                    session.send(Packets.entitySpawn(e.getId(), typeName(level, e), e.getX(), e.getY(), e.getZ(), e.getYRot(), e.getXRot(),
                            e.getBbWidth(), e.getBbHeight(), hp, e instanceof LivingEntity le ? le.getMaxHealth() : 0, flags(e)));
                    known.put(e.getId(), new Known(e.getX(), e.getY(), e.getZ(), e.getYRot(), e.getXRot(), hp, hurt));
                } else if (k.x != e.getX() || k.y != e.getY() || k.z != e.getZ() || k.yaw != e.getYRot() || k.pitch != e.getXRot() || k.hp != hp || k.hurt != hurt) {
                    session.send(Packets.entityUpdate(e.getId(), e.getX(), e.getY(), e.getZ(), e.getYRot(), e.getXRot(), hp, hurt));
                    known.put(e.getId(), new Known(e.getX(), e.getY(), e.getZ(), e.getYRot(), e.getXRot(), hp, hurt));
                }
            }
        }
        known.keySet().removeIf(id -> {
            if (seen.contains(id)) return false;
            session.send(Packets.entityRemove(id));
            return true;
        });
    }

    static String typeName(ServerLevel level, Entity e) {
        String t = level.registryAccess().lookupOrThrow(Registries.ENTITY_TYPE).getKey(e.getType()).toString();
        if (e instanceof ItemEntity item) {
            String it = level.registryAccess().lookupOrThrow(Registries.ITEM).getKey(item.getItem().getItem()).toString();
            return t + "/" + it;
        }
        return t;
    }

    static int flags(Entity e) {
        int f = 0;
        if (e instanceof LivingEntity) f |= Packets.ENT_LIVING;
        if (e instanceof Enemy) f |= Packets.ENT_HOSTILE;
        if (e instanceof Player) f |= Packets.ENT_PLAYER;
        if (e instanceof ItemEntity) f |= Packets.ENT_ITEM;
        if (e instanceof Projectile) f |= Packets.ENT_PROJECTILE;
        return f;
    }
}
