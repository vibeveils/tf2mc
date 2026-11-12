#!/usr/bin/env bash
# One-shot: headless Minecraft in the background, wait for the bridge port, then launch the TF2 mod on mc_world.
#   MC_DIR=~/mc-tf2 SDK_DIR=~/source-sdk-2013 ./run_all.sh
MC_DIR=${MC_DIR:-$HOME/mc-tf2}
SDK_DIR=${SDK_DIR:-$HOME/source-sdk-2013}
PORT=${MC_BRIDGE_PORT:-25599}
HERE=$(cd "$(dirname "$0")" && pwd)

MC_DIR="$MC_DIR" "$HERE/start_minecraft_headless.sh" > "$MC_DIR/tf2bridge-console.log" 2>&1 &
MC_PID=$!
echo "Minecraft server pid $MC_PID (log: $MC_DIR/tf2bridge-console.log); waiting for bridge on :$PORT ..."
i=0
while ! (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; do
  i=$((i+1)); [ $i -gt 180 ] && { echo "bridge did not come up"; kill $MC_PID; exit 1; }
  sleep 1
done
echo "bridge is up - launching TF2"
cd "$SDK_DIR/game" && ./mod_tf -novid -insecure +exec tf2mc_server.cfg +map mc_world
kill $MC_PID 2>/dev/null
