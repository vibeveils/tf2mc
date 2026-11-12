package dev.tf2mc.bridge.mc;

import dev.tf2mc.bridge.mixin.ConnectionAccessor;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.embedded.EmbeddedChannel;
import net.minecraft.network.Connection;
import net.minecraft.network.PacketListener;
import net.minecraft.network.ProtocolInfo;
import net.minecraft.network.protocol.Packet;
import net.minecraft.network.protocol.PacketFlow;
import org.jspecify.annotations.Nullable;

/** A network connection that goes nowhere: bridged players are driven by the TF2 side. */
public final class FakeConnection extends Connection {
    public FakeConnection() {
        super(PacketFlow.SERVERBOUND);
        ((ConnectionAccessor) this).tf2bridge$setChannel(new EmbeddedChannel());
    }

    @Override public void setReadOnly() {}
    @Override public void send(Packet<?> packet, @Nullable ChannelFutureListener listener, boolean flush) {}
    @Override public void handleDisconnection() {}
    @Override public void setListenerForServerboundHandshake(PacketListener listener) {}
    @Override public <T extends PacketListener> void setupInboundProtocol(ProtocolInfo<T> info, T listener) {}
}
