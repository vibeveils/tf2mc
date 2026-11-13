#!/usr/bin/env python3
"""Runs tools/mc_assets.py on a fake jar and checks the baked block models.
Leaves build/assets/scripts/mc_models.txt for the C++ core test."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fake_jar  # noqa: E402

passed = failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", what)


def near(a, b, eps=1e-3):
    return abs(a - b) <= eps


build = os.path.join(HERE, "build")
out = os.path.join(build, "assets")
os.makedirs(out, exist_ok=True)

# A fake launcher layout: .minecraft/versions/26.3/26.3.jar + asset index + hashed objects
import hashlib
import json
import shutil
mc = os.path.join(build, "dotminecraft")
shutil.rmtree(mc, ignore_errors=True)
vdir = os.path.join(mc, "versions", "26.3")
os.makedirs(vdir)
jar = os.path.join(vdir, "26.3.jar")
fake_jar.build(jar, extra={
    "assets/minecraft/textures/entity/zombie/zombie.png": fake_jar.png((90, 160, 80, 255), (64, 64)),
    "assets/minecraft/textures/entity/sheep/sheep.png": fake_jar.png((220, 200, 180, 255), (64, 32)),
    "assets/minecraft/textures/entity/sheep/sheep_wool.png": fake_jar.png((240, 240, 240, 255), (64, 32)),
    "assets/minecraft/textures/entity/cow/cow.png": fake_jar.png((70, 50, 35, 255), (128, 64)),   # old name, HD
})
json.dump({"assetIndex": {"id": "30"}}, open(os.path.join(vdir, "26.3.json"), "w"))
objects = {}


def put_object(key, data):
    h = hashlib.sha1(data).hexdigest()
    d = os.path.join(mc, "assets", "objects", h[:2])
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, h), "wb").write(data)
    objects[key] = {"hash": h, "size": len(data)}


have_ffmpeg = shutil.which("ffmpeg") is not None
if have_ffmpeg:
    for i, (name, freq, rate, ch) in enumerate([("mob/zombie/say1", 300, 44100, 1), ("mob/zombie/say2", 320, 48000, 2), ("step/stone1", 900, 44100, 1)]):
        ogg = os.path.join(build, f"tmp{i}.ogg")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", f"sine=frequency={freq}:duration=0.5:sample_rate={rate}",
                        "-ac", str(ch), "-c:a", "libvorbis", ogg], check=True)
        put_object(f"minecraft/sounds/{name}.ogg", open(ogg, "rb").read())
sounds_json = {
    "entity.zombie.ambient": {"sounds": ["mob/zombie/say1", {"name": "mob/zombie/say2", "volume": 0.8, "pitch": 1.2, "weight": 3}]},
    "block.stone.step": {"sounds": ["step/stone1"]},
    "entity.zombie.step_alias": {"sounds": [{"name": "block.stone.step", "type": "event"}]},
    "music.game": {"sounds": [{"name": "mob/zombie/say1", "stream": True}]},
    "entity.missing": {"sounds": ["does/not/exist"]},
}
put_object("minecraft/sounds.json", json.dumps(sounds_json).encode())
os.makedirs(os.path.join(mc, "assets", "indexes"))
json.dump({"objects": objects}, open(os.path.join(mc, "assets", "indexes", "30.json"), "w"))

r = subprocess.run([sys.executable, os.path.join(HERE, "..", "tools", "mc_assets.py"), jar, out], capture_output=True, text=True)
check(r.returncode == 0, "mc_assets.py ran: " + r.stderr[-400:])
print(r.stdout.strip())

text = open(os.path.join(out, "scripts", "mc_models.txt")).read().splitlines()
models, cur, rules = {}, None, {}
for line in text:
    p = line.split()
    if not p or p[0].startswith("#"):
        continue
    if p[0] == "m":
        cur = models.setdefault(int(p[1]), [])
    elif p[0] == "q":
        nums = list(map(float, p[6:]))
        pts = [tuple(nums[i * 5:i * 5 + 3]) for i in range(4)]
        uvs = [tuple(nums[i * 5 + 3:i * 5 + 5]) for i in range(4)]
        cur.append({"tile": int(p[1]), "cull": int(p[2]), "face": int(p[3]), "tint": int(p[4]), "shade": int(p[5]), "pts": pts, "uvs": uvs})
    elif p[0] in ("v", "p"):
        rules.setdefault(p[1], []).append((p[0], p[2], int(p[3])))


def model_of(block, sel=None):
    for kind, cond, mid in rules.get(block, []):
        if sel is None or cond == sel:
            return models[mid]
    return None


# seagrass: 8 upright quads at x/z = 0.25 / 0.75 (not a cube)
sg = model_of("minecraft:seagrass")
check(sg is not None and len(sg) == 4, "seagrass has 4 planes (back-to-back twins merged)")
if sg:
    planes = sorted({round(q["pts"][0][2], 3) for q in sg if q["face"] in (2, 3)})
    check(planes == [0.25, 0.75], f"seagrass z planes {planes}")
    check(all(q["shade"] == 0 for q in sg), "seagrass unshaded")
    check(all(q["cull"] == -1 for q in sg), "seagrass never culled")

# cross: rotated 45 degrees + rescaled -> spans corner to corner
cr = model_of("minecraft:poppy")
check(cr is not None and len(cr) == 2, "poppy cross has 2 double-sided quads")
if cr:
    xs = [p[0] for q in cr for p in q["pts"]]
    zs = [p[2] for q in cr for p in q["pts"]]
    check(near(min(xs), 0.05) and near(max(xs), 0.95) and near(min(zs), 0.05) and near(max(zs), 0.95), f"cross diagonal extents x {min(xs):.3f}..{max(xs):.3f}")

# leaf litter: flat tinted quarter at y = 1/64, object-style texture reference resolved
ll = model_of("minecraft:leaf_litter")
check(ll is not None and len(ll) == 1, "leaf litter is one double-sided quad")
if ll:
    check(all(q["tint"] == 1 for q in ll), "leaf litter tinted")
    check(all(near(p[1], 0.25 / 16) for q in ll for p in q["pts"]), "leaf litter is flat at 0.25px")
    check(all(q["tile"] >= 0 for q in ll), "leaf litter texture found via {'sprite': ...}")
    check(max(p[0] for q in ll for p in q["pts"]) <= 0.5 + 1e-6, "one segment covers a quarter")
blocks = open(os.path.join(out, "scripts", "mc_blocks.txt")).read()
check("block minecraft:leaf_litter " in blocks and "tint=dry_foliage" in blocks, "leaf litter uses the dry foliage tint")

# stairs facing north = the east model rotated y=270: the step is now on the north half (z < 0.5)
st_e = model_of("minecraft:oak_stairs", "facing=east,half=bottom,shape=straight")
st_n = model_of("minecraft:oak_stairs", "facing=north,half=bottom,shape=straight")
check(st_e is not None and st_n is not None, "both stairs variants baked")
if st_e and st_n:
    top_e = [q for q in st_e if q["cull"] == 1]
    top_n = [q for q in st_n if q["cull"] == 1]
    check(len(top_e) == 1 and min(p[0] for p in top_e[0]["pts"]) >= 0.5 - 1e-6, "east stairs: step top on the east half")
    check(len(top_n) == 1 and max(p[2] for p in top_n[0]["pts"]) <= 0.5 + 1e-6, "north stairs: step top on the north half")
    # cullfaces rotate with the model: east model's 'east' cull face becomes 'north' (2)
    check(any(q["cull"] == 2 for q in st_n) and any(q["cull"] == 5 for q in st_e), "cullfaces rotated")
    # uvlock: the rotated top face samples the texture by world position
    uq = top_n[0]
    check(all(near(u, p[0]) and near(v, p[2]) for p, (u, v) in zip(uq["pts"], uq["uvs"])), "uvlock keeps the top texture world aligned")

# fence multipart rules incl. OR condition
fr = rules.get("minecraft:oak_fence", [])
check([c for _, c, _ in fr] == ["-", "north=true", "east=true;west=true"], f"fence conditions {[c for _, c, _ in fr]}")

# block entities / liquids without elements are not baked (simple-shape fallback keeps working)
check("minecraft:chest" not in rules and "minecraft:water" not in rules, "no models for chest/water")

# full cube keeps 6 culled faces with a whole-tile UV
sto = model_of("minecraft:stone")
check(sto is not None and len(sto) == 6 and sorted(q["cull"] for q in sto) == [0, 1, 2, 3, 4, 5], "stone is 6 culled faces")
if sto:
    check(all(sorted({round(u, 3) for u, v in q["uvs"]}) == [0.0, 1.0] for q in sto), "stone faces use the whole tile")

# Mob textures: geometry specs resolved (incl. fallback names), packed without overlap
et = open(os.path.join(out, "scripts", "mc_entity_textures.txt")).read().splitlines()
rects = {l.split()[1]: tuple(map(int, l.split()[2:6])) for l in et if l.startswith("t ")}
check("entity/zombie/zombie" in rects and rects["entity/zombie/zombie"][2:] == (64, 64), "zombie texture packed")
check("entity/cow/temperate_cow|entity/cow/cow" in rects and rects["entity/cow/temperate_cow|entity/cow/cow"][2:] == (128, 64), "cow found under its old name")
check("entity/sheep/sheep_wool|entity/sheep/sheep_fur" in rects, "sheep wool layer packed")
rs = list(rects.values())
check(all(not (a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]) for i, a in enumerate(rs) for b in rs[i + 1:]), "no overlapping rectangles")
check(os.path.isfile(os.path.join(out, "materials", "mc", "entities.vtf")), "entities.vtf written")

# Sounds: converted to mono 16-bit 22050 Hz WAV, event table with references resolved, music skipped
if have_ffmpeg:
    import wave
    st = open(os.path.join(out, "scripts", "mc_sounds.txt")).read()
    check("e minecraft:entity.zombie.ambient\nf mc/mob/zombie/say1.wav 1 1 1\nf mc/mob/zombie/say2.wav 0.8 1.2 3" in st, "zombie ambient event with per-file volume/pitch/weight")
    check("e minecraft:entity.zombie.step_alias\nf mc/step/stone1.wav 1 1 1" in st, "event reference resolved")
    check("music.game" not in st and "entity.missing" not in st, "music skipped, missing files dropped")
    w = wave.open(os.path.join(out, "sound", "mc", "mob", "zombie", "say2.wav"))
    check(w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == 22050, "wav is mono 16-bit 22050 Hz")
    check(abs(w.getnframes() - 11025) < 400, f"resampled length {w.getnframes()}")
else:
    print("(ffmpeg not found: sound conversion not tested)")

print(f"assets: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
