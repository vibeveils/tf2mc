#!/bin/sh
# Meshes a procedural scene with the real core mesher and software-renders it (docs/mesher_preview.png).
set -e
cd "$(dirname "$0")"
C=../source/src/game/shared/tf2mc/core
mkdir -p build
g++ -std=c++14 -O2 -I$C mesh_preview.cpp $C/mc_world.cpp $C/mc_mesher.cpp -o build/mesh_preview
./build/mesh_preview build/preview_quads.txt
python3 render_preview.py build/preview_quads.txt build/preview.png
./build/mesh_preview build/night.txt 0.15 && SKY=12,16,32 python3 render_preview.py build/night.txt build/night.png
