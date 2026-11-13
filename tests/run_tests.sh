#!/bin/sh
# Builds and runs the engine-independent core unit tests.
set -e
cd "$(dirname "$0")"
CORE=../source/src/game/shared/tf2mc/core
mkdir -p build
if python3 -c "import PIL" 2>/dev/null; then
	python3 test_assets.py
else
	echo "(Pillow not installed: skipping the asset tool test and baked-model checks - pip install pillow)"
	rm -f build/assets/scripts/mc_models.txt
fi
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -I$CORE test_core.cpp $CORE/mc_world.cpp $CORE/mc_collide.cpp $CORE/mc_mesher.cpp -o build/test_core
./build/test_core
