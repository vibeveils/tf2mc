#!/bin/sh
# Compiles the MC-independent Java bridge + FakeWorldServer and the C++ probe, then runs them together.
set -e
cd "$(dirname "$0")"
CORE=../source/src/game/shared/tf2mc/core
J=../minecraft/src
mkdir -p build/classes
javac -d build/classes $J/main/java/dev/tf2mc/bridge/net/*.java $J/main/java/dev/tf2mc/bridge/BridgeConfig.java $J/test/java/dev/tf2mc/bridge/FakeWorldServer.java
g++ -std=c++17 -O1 -g -Wall -I$CORE e2e_probe.cpp $CORE/mc_net.cpp $CORE/mc_world.cpp $CORE/mc_collide.cpp $CORE/mc_mesher.cpp -lpthread -o build/e2e_probe
PORT=${PORT:-25611}
java -cp build/classes dev.tf2mc.bridge.FakeWorldServer $PORT 30 > build/fake_server.log 2>&1 &
SRV=$!
sleep 1.5
set +e
./build/e2e_probe $PORT
RC=$?
kill $SRV 2>/dev/null
echo "--- fake server log ---"; grep -v "^Picked up" build/fake_server.log
exit $RC
