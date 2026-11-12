package dev.tf2mc.bridge;

import dev.tf2mc.bridge.mc.BridgeController;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.block.state.BlockState;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * Entry point. Runs inside a headless (dedicated, "nogui") Minecraft 26.3 server and exposes the
 * world to the TF2 Source mod over TCP. Hooks come from mixins (see dev.tf2mc.bridge.mixin) so the
 * mod only depends on Fabric Loader, not Fabric API.
 */
public final class TF2Bridge implements ModInitializer {
    public static final Logger LOG = LoggerFactory.getLogger("tf2bridge");
    private static BridgeController controller;
    private static BridgeConfig config;

    @Override
    public void onInitialize() {
        config = BridgeConfig.load(FabricLoader.getInstance().getConfigDir().resolve("tf2bridge.properties"));
        LOG.info("TF2 bridge loaded; will listen on {}:{} once the server ticks", config.bindHost, config.port);
    }

    /** Called at the end of every server tick (MinecraftServerMixin). */
    public static void onServerTick(MinecraftServer server) {
        if (controller == null) {
            if (config == null) config = new BridgeConfig();
            controller = BridgeController.start(server, config);
            if (controller == null) return;
        }
        controller.tick();
    }

    public static void onServerStopping(MinecraftServer server) {
        if (controller != null) {
            controller.shutdown();
            controller = null;
        }
    }

    /** PlayerListMixin: chat and system messages. */
    public static void onChat(String text) {
        if (controller != null) controller.onChat(text);
    }

    /** ServerLevelMixin: every block change that would be sent to vanilla clients. */
    public static void onBlockChanged(ServerLevel level, BlockPos pos, BlockState newState) {
        if (controller != null) controller.onBlockChanged(level, pos, newState);
    }
}
