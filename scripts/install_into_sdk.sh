#!/bin/sh
# Copies TF2MC into a clone of https://github.com/ValveSoftware/source-sdk-2013 and applies the TF2 patch.
#   ./install_into_sdk.sh ~/source-sdk-2013
set -e
SDK=${1:?usage: install_into_sdk.sh <path to source-sdk-2013>}
HERE=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$SDK/src/game/shared/tf/tf_gamemovement.cpp" ] || { echo "$SDK does not look like source-sdk-2013"; exit 1; }
cp -R "$HERE/source/src/." "$SDK/src/"
cp -R "$HERE/game/mod_tf/." "$SDK/game/mod_tf/" 2>/dev/null || { mkdir -p "$SDK/game/mod_tf"; cp -R "$HERE/game/mod_tf/." "$SDK/game/mod_tf/"; }
cd "$SDK"
if git apply --check -p1 "$HERE/source/patches/tf2mc.patch" 2>/dev/null; then
  git apply -p1 "$HERE/source/patches/tf2mc.patch"
  echo "patch applied"
else
  echo "patch does not apply as-is (already applied, line endings or SDK changes) - inserting hooks with apply_tf2_hooks.py"
  python3 "$HERE/scripts/apply_tf2_hooks.py" "$SDK"
fi
echo "Now build: cd $SDK/src && ./buildallprojects   (Windows: createallprojects.bat, then build everything.sln)"
