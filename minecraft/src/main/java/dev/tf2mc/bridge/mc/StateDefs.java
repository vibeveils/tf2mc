package dev.tf2mc.bridge.mc;

import dev.tf2mc.bridge.net.Packets;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.tags.BlockTags;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.LiquidBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.VoxelShape;

import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;

/** Describes block states for the TF2 side: global id, name, properties, flags and the real shapes. */
public final class StateDefs {
    private static final Map<BlockState, Packets.StateDef> CACHE = new IdentityHashMap<>();
    private static final float[][] UNIT = {{0, 0, 0, 1, 1, 1}};

    private StateDefs() {}

    public static int id(BlockState state) {
        return Block.getId(state);
    }

    public static synchronized Packets.StateDef get(ServerLevel level, BlockState state) {
        Packets.StateDef d = CACHE.get(state);
        if (d == null) {
            d = build(level, state);
            CACHE.put(state, d);
        }
        return d;
    }

    private static Packets.StateDef build(ServerLevel level, BlockState state) {
        String name = level.registryAccess().lookupOrThrow(Registries.BLOCK).getKey(state.getBlock()).toString();
        String s = state.toString();   // "Block{minecraft:oak_stairs}[facing=east,half=bottom,...]"
        int lb = s.indexOf('[');
        String props = lb >= 0 && s.endsWith("]") ? s.substring(lb + 1, s.length() - 1) : "";

        float[][] coll, shape;
        try {
            coll = boxes(state.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO));
        } catch (RuntimeException e) {
            coll = state.isAir() ? new float[0][] : UNIT;
        }
        try {
            shape = boxes(state.getShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO));
        } catch (RuntimeException e) {
            shape = state.isAir() ? new float[0][] : UNIT;
        }

        int flags = 0;
        if (coll.length > 0) flags |= Packets.STATE_COLLIDES;
        boolean fullColl = coll.length == 1 && coll[0][0] <= 0 && coll[0][1] <= 0 && coll[0][2] <= 0 && coll[0][3] >= 1 && coll[0][4] >= 1 && coll[0][5] >= 1;
        if (state.canOcclude() && fullColl) flags |= Packets.STATE_OPAQUE_CUBE;
        FluidState fluid = state.getFluidState();
        if (!fluid.isEmpty()) {
            if (state.getBlock() instanceof LiquidBlock) flags |= Packets.STATE_LIQUID;
            else if (fluid.is(FluidTags.WATER)) flags |= Packets.STATE_WATERLOGGED;
            if (fluid.is(FluidTags.LAVA)) flags |= Packets.STATE_LAVA;
        }
        if (state.is(BlockTags.CLIMBABLE)) flags |= Packets.STATE_CLIMBABLE;
        return new Packets.StateDef(id(state), name, props, flags, state.getLightEmission(), coll, shape);
    }

    private static float[][] boxes(VoxelShape shape) {
        if (shape.isEmpty()) return new float[0][];
        List<AABB> list = shape.toAabbs();
        int n = Math.min(16, list.size());
        float[][] out = new float[n][];
        for (int i = 0; i < n; i++) {
            AABB a = list.get(i);
            out[i] = new float[]{(float) a.minX, (float) a.minY, (float) a.minZ, (float) a.maxX, (float) a.maxY, (float) a.maxZ};
        }
        return out;
    }
}
