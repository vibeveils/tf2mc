package dev.tf2mc.bridge.mixin;

import dev.tf2mc.bridge.TF2Bridge;
import net.minecraft.server.MinecraftServer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import java.util.function.BooleanSupplier;

@Mixin(MinecraftServer.class)
public abstract class MinecraftServerMixin {
    @Inject(method = "tickServer", at = @At("TAIL"))
    private void tf2bridge$tick(BooleanSupplier hasTimeLeft, CallbackInfo ci) {
        TF2Bridge.onServerTick((MinecraftServer) (Object) this);
    }

    @Inject(method = "stopServer", at = @At("HEAD"))
    private void tf2bridge$stop(CallbackInfo ci) {
        TF2Bridge.onServerStopping((MinecraftServer) (Object) this);
    }
}
