package dev.tf2mc.bridge.mc;

import com.mojang.authlib.GameProfile;
import dev.tf2mc.bridge.net.Packets;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.UUIDUtil;
import net.minecraft.network.DisconnectionDetails;
import net.minecraft.network.chat.Component;
import net.minecraft.network.protocol.PacketFlow;
import net.minecraft.network.protocol.game.ServerboundPlayerActionPacket;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ClientInformation;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.network.CommonListenerCookie;
import net.minecraft.tags.DamageTypeTags;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageTypes;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.item.component.SwingAnimation;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/**
 * The Minecraft-side stand-in for one TF2 player. Its position mirrors the TF2 player every tick,
 * which keeps chunks loaded and lets mobs see, path to and attack it. Damage it receives is
 * forwarded to TF2 (where health lives) instead of being applied here.
 */
public final class BridgedPlayer extends ServerPlayer {
    public interface HurtSink { void hurt(BridgedPlayer p, float amount, int attackerId, int kind, Vec3 from); }

    public final int sessionId;
    public final int slot;
    public int mode = Packets.MODE_TF_CLASS;
    HurtSink hurtSink;

    // Pose requested by TF2
    private double tx, ty, tz;
    private float tyaw, tpitch;
    private boolean hasTarget;

    // Digging (Steve mode)
    private BlockPos digPos;
    private Direction digFace = Direction.UP;
    private float digProgress;
    private int lastDigRequestTick;
    private int digCooldown;

    // Simple i-frames for forwarded damage, keyed by damage type
    private final Map<String, Integer> lastHurtTick = new HashMap<>();

    int lastInventoryHash = 0;
    int lastStatusHash = 0;
    int lastContainerHash = 0;
    boolean uiOpen;          // TF2 shows a container screen for this player
    boolean uiExternal;      // ...for a block/entity menu (chest, crafting table, villager) rather than the inventory

    private BridgedPlayer(MinecraftServer server, ServerLevel level, GameProfile profile, int sessionId, int slot) {
        super(server, level, profile, ClientInformation.createDefault());
        this.sessionId = sessionId;
        this.slot = slot;
    }

    public static BridgedPlayer spawn(MinecraftServer server, ServerLevel level, int sessionId, int slot, String tfName, Vec3 pos) {
        String name = sanitize("tf" + slot + "_" + tfName);
        GameProfile profile = new GameProfile(UUIDUtil.createOfflinePlayerUUID(name), name);
        BridgedPlayer p = new BridgedPlayer(server, level, profile, sessionId, slot);
        p.snapTo(pos.x, pos.y, pos.z, 0, 0);
        server.getPlayerList().placeNewPlayer(new FakeConnection(), p, new CommonListenerCookie(profile, 0, p.clientInformation(), false));
        p.teleportTo(level, pos.x, pos.y, pos.z, Set.of(), 0, 0, true);
        p.setHealth(p.getMaxHealth());
        p.unsetRemoved();
        p.getAttribute(Attributes.STEP_HEIGHT).setBaseValue(0.6F);
        p.gameMode.changeGameModeForPlayer(GameType.SURVIVAL);
        p.tx = pos.x; p.ty = pos.y; p.tz = pos.z; p.hasTarget = true;
        return p;
    }

    static String sanitize(String n) {
        StringBuilder b = new StringBuilder();
        for (char c : n.toCharArray()) if (Character.isLetterOrDigit(c) || c == '_') b.append(c);
        String s = b.toString();
        return s.length() > 16 ? s.substring(0, 16) : s.isEmpty() ? "tfplayer" : s;
    }

    public void remove() {
        connection.onDisconnect(new DisconnectionDetails(Component.literal("TF2 player left")));
    }

    public void setTargetPose(double x, double y, double z, float yaw, float pitch, int flags) {
        tx = x; ty = y; tz = z; tyaw = yaw; tpitch = pitch; hasTarget = true;
        setShiftKeyDown((flags & 2) != 0);
        setSprinting((flags & 4) != 0);
        applyPose((flags & 1) != 0);
    }

    private void applyPose(boolean onGround) {
        if (!hasTarget) return;
        snapTo(tx, ty, tz, tyaw, tpitch);
        setYHeadRot(tyaw);
        setDeltaMovement(Vec3.ZERO);
        fallDistance = 0;
        if (onGround) setOnGround(true);
    }

    @Override
    public void tick() {
        try {
            super.tick();
            doTick();
        } catch (NullPointerException ignored) {
            // A fake connection occasionally trips vanilla code paths expecting a real client.
        }
        applyPose(onGround());
        if (level().getServer().getTickCount() % 10 == 0) {
            connection.resetPosition();
            level().getChunkSource().move(this);
        }
        tickDigging();
    }

    // ---------------------------------------------------------------- damage

    @Override
    public boolean hurtServer(ServerLevel level, DamageSource source, float amount) {
        // Our own TF2 explosions breaking blocks must not hurt the TF2 players a second time
        if (source.is(DamageTypeTags.IS_EXPLOSION) && source.getEntity() instanceof BridgedPlayer) return false;
        // TF2 simulates these itself (or they are artefacts of the mirrored position)
        if (source.is(DamageTypeTags.IS_FALL) || source.is(DamageTypes.IN_WALL) || source.is(DamageTypes.CRAMMING)
                || source.is(DamageTypes.FELL_OUT_OF_WORLD) || source.is(DamageTypes.STARVE) || source.is(DamageTypes.GENERIC_KILL)
                || source.is(DamageTypes.FLY_INTO_WALL))
            return false;
        int now = level.getServer().getTickCount();
        String key = source.typeHolder().getRegisteredName();
        Integer last = lastHurtTick.get(key);
        if (last != null && now - last < 10) return false;
        lastHurtTick.put(key, now);

        int kind = Packets.DMG_GENERIC;
        if (source.is(DamageTypeTags.IS_EXPLOSION)) kind = Packets.DMG_EXPLOSION;
        else if (source.is(DamageTypeTags.IS_FIRE)) kind = Packets.DMG_FIRE;
        else if (source.is(DamageTypeTags.IS_PROJECTILE)) kind = Packets.DMG_PROJECTILE;
        else if (source.getDirectEntity() instanceof LivingEntity) kind = Packets.DMG_MELEE;
        // Minecraft-mode players wear Minecraft armour: apply armour, toughness and enchantment protection
        if (mode == Packets.MODE_STEVE) {
            amount = getDamageAfterArmorAbsorb(source, amount);
            amount = getDamageAfterMagicAbsorb(source, amount);
            if (amount <= 0) return false;
        }
        Entity attacker = source.getEntity();
        Vec3 from = source.getSourcePosition();
        if (hurtSink != null) hurtSink.hurt(this, amount, attacker != null ? attacker.getId() : -1, kind, from != null ? from : position());
        return false;
    }

    @Override
    public void die(DamageSource cause) {
        // Health lives in TF2; never die here (that would replace this object on respawn).
        setHealth(getMaxHealth());
    }

    // ---------------------------------------------------------------- digging

    public void requestDig(BlockPos pos, Direction face) {
        lastDigRequestTick = level().getServer().getTickCount();
        if (digPos != null && digPos.equals(pos)) { digFace = face; return; }
        abortDig();
        if (digCooldown > 0) return;
        ServerLevel level = level();
        BlockState state = level.getBlockState(pos);
        if (state.isAir()) return;
        gameMode.handleBlockBreakAction(pos, ServerboundPlayerActionPacket.Action.START_DESTROY_BLOCK, face, level.getMaxY(), -1);
        swing(InteractionHand.MAIN_HAND, SwingAnimation.DEFAULT, true);
        if (gameMode.getGameModeForPlayer().isCreative() || level.getBlockState(pos).isAir()) {
            digCooldown = 5;   // instamined
            return;
        }
        state.attack(level, pos, this);
        digPos = pos;
        digFace = face;
        digProgress = 0;
    }

    public void abortDig() {
        if (digPos == null) return;
        ServerLevel level = level();
        level.destroyBlockProgress(getId(), digPos, -1);
        gameMode.handleBlockBreakAction(digPos, ServerboundPlayerActionPacket.Action.ABORT_DESTROY_BLOCK, digFace, level.getMaxY(), -1);
        digPos = null;
        digProgress = 0;
    }

    /** Returns the current crack stage (-1 none) so the controller can relay it. */
    public int digStage() {
        return digPos == null ? -1 : Math.min(9, (int) (digProgress * 10));
    }

    public BlockPos digPos() { return digPos; }

    private void tickDigging() {
        if (digCooldown > 0) digCooldown--;
        if (digPos == null) return;
        ServerLevel level = level();
        if (level().getServer().getTickCount() - lastDigRequestTick > 20) { abortDig(); return; }   // TF2 stopped asking
        BlockState state = level.getBlockState(digPos);
        if (state.isAir()) { digPos = null; return; }
        digProgress += state.getDestroyProgress(this, level, digPos);
        if (level().getServer().getTickCount() % 4 == 0) swing(InteractionHand.MAIN_HAND, SwingAnimation.DEFAULT, true);
        if (digProgress >= 1.0f) {
            gameMode.handleBlockBreakAction(digPos, ServerboundPlayerActionPacket.Action.STOP_DESTROY_BLOCK, digFace, level.getMaxY(), -1);
            level.destroyBlockProgress(getId(), digPos, -1);
            digPos = null;
            digProgress = 0;
            digCooldown = 5;
        } else {
            level.destroyBlockProgress(getId(), digPos, (int) (digProgress * 10));
        }
    }
}
