package dev.tf2mc.bridge.mixin;

import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Entity#invulnerableTime is private in 26.x; TF2 hits bypass vanilla i-frames. */
@Mixin(Entity.class)
public interface EntityAccessor {
    @Accessor("invulnerableTime")
    void tf2bridge$setInvulnerableTime(int ticks);
}
