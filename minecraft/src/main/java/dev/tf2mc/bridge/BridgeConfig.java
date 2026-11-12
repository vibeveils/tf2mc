package dev.tf2mc.bridge;

import java.io.IOException;
import java.io.Reader;
import java.io.Writer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

/** config/tf2bridge.properties */
public final class BridgeConfig {
    public String bindHost = "127.0.0.1";
    public int port = 25599;
    public int maxSessions = 34;
    /** Chunk radius streamed around each bridged player. */
    public int chunkRadius = 8;
    /** Max chunk columns sent per tick per session (bandwidth guard). */
    public int columnsPerTick = 6;
    /** Stop streaming new columns while a session has more than this many bytes queued. */
    public long maxQueuedBytes = 8L << 20;
    /** Entities within this many blocks of a bridged player are mirrored to Source. */
    public double entityRadius = 64.0;
    /** Send entity updates every N ticks. */
    public int entityUpdateInterval = 1;
    /** Multiplier applied to Minecraft damage dealt to bridged players (MC hearts -> TF health). */
    public float mcToTfDamage = 8.0f;
    /** TF2 explosions (rockets, stickies...) carve small craters in Minecraft terrain. */
    public boolean tfExplosionsBreakBlocks = true;
    /** Optional shared secret; if non-empty the HELLO name must be "<secret>:<name>". */
    public String secret = "";

    public static BridgeConfig load(Path file) {
        BridgeConfig c = new BridgeConfig();
        Properties p = new Properties();
        if (Files.exists(file)) {
            try (Reader r = Files.newBufferedReader(file)) { p.load(r); } catch (IOException ignored) {}
        }
        c.bindHost = p.getProperty("bind-host", c.bindHost);
        c.port = Integer.parseInt(p.getProperty("port", "" + c.port));
        c.maxSessions = Integer.parseInt(p.getProperty("max-sessions", "" + c.maxSessions));
        c.chunkRadius = Integer.parseInt(p.getProperty("chunk-radius", "" + c.chunkRadius));
        c.columnsPerTick = Integer.parseInt(p.getProperty("columns-per-tick", "" + c.columnsPerTick));
        c.maxQueuedBytes = Long.parseLong(p.getProperty("max-queued-bytes", "" + c.maxQueuedBytes));
        c.entityRadius = Double.parseDouble(p.getProperty("entity-radius", "" + c.entityRadius));
        c.entityUpdateInterval = Integer.parseInt(p.getProperty("entity-update-interval", "" + c.entityUpdateInterval));
        c.mcToTfDamage = Float.parseFloat(p.getProperty("mc-to-tf-damage", "" + c.mcToTfDamage));
        c.secret = p.getProperty("secret", c.secret);
        c.tfExplosionsBreakBlocks = Boolean.parseBoolean(p.getProperty("tf-explosions-break-blocks", "" + c.tfExplosionsBreakBlocks));
        if (!Files.exists(file)) c.save(file);
        return c;
    }

    public void save(Path file) {
        Properties p = new Properties();
        p.setProperty("bind-host", bindHost);
        p.setProperty("port", "" + port);
        p.setProperty("max-sessions", "" + maxSessions);
        p.setProperty("chunk-radius", "" + chunkRadius);
        p.setProperty("columns-per-tick", "" + columnsPerTick);
        p.setProperty("max-queued-bytes", "" + maxQueuedBytes);
        p.setProperty("entity-radius", "" + entityRadius);
        p.setProperty("entity-update-interval", "" + entityUpdateInterval);
        p.setProperty("mc-to-tf-damage", "" + mcToTfDamage);
        p.setProperty("secret", secret);
        p.setProperty("tf-explosions-break-blocks", "" + tfExplosionsBreakBlocks);
        try {
            Files.createDirectories(file.getParent());
            try (Writer w = Files.newBufferedWriter(file)) { p.store(w, "TF2 <-> Minecraft bridge"); }
        } catch (IOException ignored) {}
    }
}
