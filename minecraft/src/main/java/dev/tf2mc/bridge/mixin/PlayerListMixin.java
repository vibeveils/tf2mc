package dev.tf2mc.bridge.mixin;

import dev.tf2mc.bridge.mc.BridgedPlayer;
import dev.tf2mc.bridge.mc.BridgedPacketListener;
import net.minecraft.network.Connection;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.network.CommonListenerCookie;
import net.minecraft.server.network.ServerGamePacketListenerImpl;
import net.minecraft.server.players.PlayerList;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import dev.tf2mc.bridge.TF2Bridge;
import net.minecraft.network.chat.ChatType;
import net.minecraft.network.chat.Component;
import net.minecraft.network.chat.PlayerChatMessage;

/** Gives bridged players a packet listener that never times out / gets kicked; relays chat to TF2. */
@Mixin(PlayerList.class)
public abstract class PlayerListMixin {
    @Redirect(method = "placeNewPlayer", at = @At(value = "NEW", target = "(Lnet/minecraft/server/MinecraftServer;Lnet/minecraft/network/Connection;Lnet/minecraft/server/level/ServerPlayer;Lnet/minecraft/server/network/CommonListenerCookie;)Lnet/minecraft/server/network/ServerGamePacketListenerImpl;"))
    private ServerGamePacketListenerImpl tf2bridge$listener(MinecraftServer server, Connection connection, ServerPlayer player, CommonListenerCookie cookie) {
        if (player instanceof BridgedPlayer) return new BridgedPacketListener(server, connection, player, cookie);
        return new ServerGamePacketListenerImpl(server, connection, player, cookie);
    }

    @Inject(method = "broadcastChatMessage(Lnet/minecraft/network/chat/PlayerChatMessage;Lnet/minecraft/server/level/ServerPlayer;Lnet/minecraft/network/chat/ChatType$Bound;)V", at = @At("HEAD"))
    private void tf2bridge$chat(PlayerChatMessage message, ServerPlayer player, ChatType.Bound params, CallbackInfo ci) {
        TF2Bridge.onChat("<" + player.getGameProfile().name() + "> " + message.signedContent());
    }

    @Inject(method = "broadcastSystemMessage(Lnet/minecraft/network/chat/Component;Z)V", at = @At("HEAD"))
    private void tf2bridge$system(Component message, boolean overlay, CallbackInfo ci) {
        if (!overlay) TF2Bridge.onChat(message.getString());
    }
}
