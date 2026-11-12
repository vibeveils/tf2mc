#!/bin/sh
# Starts the Minecraft 26.3 Fabric dedicated server with no GUI. The tf2bridge mod must be in $MC_DIR/mods.
#   MC_DIR=~/mc-tf2 ./start_minecraft_headless.sh
set -e
MC_DIR=${MC_DIR:-$HOME/mc-tf2}
cd "$MC_DIR"
[ -f eula.txt ] || { echo "Accept the Minecraft EULA first: echo eula=true > $MC_DIR/eula.txt"; exit 1; }
exec java -Xms2G -Xmx${MC_RAM:-4G} -jar fabric-server-launch.jar --nogui
