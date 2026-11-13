#!/bin/sh
# Builds and runs the engine-independent core unit tests.
set -e
cd "$(dirname "$0")"
CORE=../source/src/game/shared/tf2mc/core
mkdir -p build
python3 test_assets.py
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -I$CORE test_core.cpp $CORE/mc_world.cpp $CORE/mc_collide.cpp $CORE/mc_mesher.cpp -o build/test_core
./build/test_core
