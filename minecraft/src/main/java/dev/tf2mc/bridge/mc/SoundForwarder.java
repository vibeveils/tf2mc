package dev.tf2mc.bridge.mc;

import dev.tf2mc.bridge.net.BridgeServer;
import dev.tf2mc.bridge.net.Packets;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.protocol.Packet;
import net.minecraft.network.protocol.game.ClientboundSoundEntityPacket;
import net.minecraft.network.protocol.game.ClientboundSoundPacket;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.sounds.SoundEvent;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.entity.Entity;

import java.util.HashSet;
import java.util.Set;

/**
 * Every sound Minecraft sends to a bridged player (mob noises and footsteps, doors, explosions,
 * item pickups...) is forwarded to TF2, once per tick even when several bridged players hear it.
 * Block place/break sounds are played by TF2 itself when the block changes (Minecraft doesn't send
 * them to the player who did it), so those are not forwarded to avoid doubles.
 */
public final class SoundForwarder {
    private static MinecraftServer server;
    private static BridgeServer net;
    private static final Set<String> seen = new HashSet<>();

    private SoundForwarder() {}

    public static synchronized void init(MinecraftServer s, BridgeServer n) {
        server = s;
        net = n;
        seen.clear();
    }

    public static synchronized void tick() {
        seen.clear();
    }

    /** "minecraft:entity.zombie.ambient", or "" */
    public static String soundId(SoundEvent ev) {
        if (ev == null) return "";
        Object key = BuiltInRegistries.SOUND_EVENT.getKey(ev);
        return key != null ? key.toString() : "";
    }

    private static String soundId(Holder<SoundEvent> h) {
        if (h == null) return "";
        var key = h.unwrapKey();
        if (key.isPresent()) return key.get().identifier().toString();
        return soundId(h.value());
    }

    public static void onPacket(Packet<?> packet) {
        if (net == null) return;
        try {
            if (packet instanceof ClientboundSoundPacket p) {
                forward(soundId(p.getSound()), p.getSource(), p.getX(), p.getY(), p.getZ(), p.getVolume(), p.getPitch(), p.getSeed(), -1);
            } else if (packet instanceof ClientboundSoundEntityPacket p) {
                Entity e = find(p.getId());
                if (e != null)
                    forward(soundId(p.getSound()), p.getSource(), e.getX(), e.getY() + e.getBbHeight() * 0.5, e.getZ(), p.getVolume(), p.getPitch(), p.getSeed(), p.getId());
            }
        } catch (RuntimeException ignored) {
            // never let sound forwarding break the game
        }
    }

    private static Entity find(int id) {
        if (server == null) return null;
        for (ServerLevel level : server.getAllLevels()) {
            Entity e = level.getEntity(id);
            if (e != null) return e;
        }
        return null;
    }

    private static synchronized void forward(String id, SoundSource source, double x, double y, double z, float volume, float pitch, long seed, int entityId) {
        if (id.isEmpty() || source == SoundSource.MUSIC || source == SoundSource.RECORDS) return;
        if (id.startsWith("minecraft:block.") && (id.endsWith(".place") || id.endsWith(".break"))) return;
        String key = id + '@' + Math.round(x * 4) + ',' + Math.round(y * 4) + ',' + Math.round(z * 4) + '#' + seed;
        if (!seen.add(key)) return;   // already sent this tick (several bridged players heard it)
        net.broadcast(Packets.sound(id, x, y, z, volume, pitch, source.ordinal(), entityId), -1);
    }
}
