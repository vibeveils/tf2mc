#!/usr/bin/env python3
"""
Build the TF2MC block atlas and block definitions from YOUR OWN Minecraft client jar.

Mojang's textures are not redistributed with TF2MC; this script converts the copy you own:

    python3 tools/mc_assets.py ~/.minecraft/versions/26.3/26.3.jar  <path-to>/game/mod_tf  [--assets DIR] [--no-sounds]

Outputs (inside the mod folder):
    materials/mc/atlas.vtf      block texture atlas, point sampled, no mips (VTF 7.2 written directly)
    scripts/mc_blocks.txt       one line per block: shape, per-face atlas tiles, tint, box, flags
    materials/mc/items.vtf      item icon atlas (32px icons; blocks drawn as little isometric cubes)
    scripts/mc_items.txt        item id -> icon tile
    scripts/mc_models.txt       every block model, baked to quads (element rotations, face UVs,
                                variant x/y rotation, uvlock), plus blockstate variant / multipart rules
    materials/mc/entities.vtf   mob textures named in scripts/mc_entity_geometry.txt, packed
    scripts/mc_entity_textures.txt   texture -> atlas rectangle
    sound/mc/...                Minecraft sounds as mono 16-bit WAV (Source can't play Ogg)
    scripts/mc_sounds.txt       sound event -> files (from sounds.json)

Sounds are not in the client jar: they come from the launcher's asset store. The script finds it
next to the jar (.minecraft/assets, via versions/<ver>/<ver>.json's assetIndex); pass --assets to
point elsewhere. Converting needs `pip install soundfile numpy`, or ffmpeg on the PATH.

Requires Pillow (pip install pillow).
"""
import io
import json
import os
import struct
import sys
import zipfile

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

TILE = 16
TILES_PER_ROW = 64
FACES = ["down", "up", "north", "south", "west", "east"]   # == mc::Face order

CROSS_PARENTS = {"cross", "tinted_cross", "flower_pot_cross", "crop", "stem_growth0", "cross_emissive"}
FULL_CUBE_PARENTS = {"cube", "cube_all", "cube_column", "cube_column_horizontal", "cube_bottom_top", "cube_top",
                     "cube_mirrored_all", "cube_mirrored", "cube_directional", "leaves", "orientable",
                     "orientable_with_bottom", "orientable_vertical", "cube_all_inner_faces", "template_glazed_terracotta",
                     "block", "cube_north_west_mirrored_all", "cube_column_mirrored", "cube_column_uv_locked_x",
                     "cube_column_uv_locked_y", "cube_column_uv_locked_z"}


class Jar:
    def __init__(self, path):
        self.z = zipfile.ZipFile(path)
        self.names = set(self.z.namelist())
        self.cache = {}

    def json(self, path):
        if path not in self.cache:
            self.cache[path] = json.loads(self.z.read(path)) if path in self.names else None
        return self.cache[path]

    def image(self, path):
        if path not in self.names:
            return None
        im = Image.open(io.BytesIO(self.z.read(path))).convert("RGBA")
        # animated textures are vertical strips: take the first frame
        if im.height > im.width:
            im = im.crop((0, 0, im.width, im.width))
        if im.size != (TILE, TILE):
            im = im.resize((TILE, TILE), Image.NEAREST)
        return im


def strip_ns(ref):
    return ref.split(":", 1)[1] if ":" in ref else ref


def resolve_model(jar, name, depth=0):
    """Returns (merged textures, elements, chain of parent names)."""
    name = strip_ns(name)
    m = jar.json(f"assets/minecraft/models/{name}.json")
    if m is None or depth > 16:
        return {}, None, [name]
    textures, elements, chain = {}, None, [name]
    if "parent" in m:
        textures, elements, chain = resolve_model(jar, m["parent"], depth + 1)
        chain = [name] + chain
    textures = {**textures, **m.get("textures", {})}
    if "elements" in m:
        elements = m["elements"]
    return textures, elements, chain


def tex_ref(v):
    """A texture reference: a string, or (newer Minecraft) an object like {"sprite": "...", ...}."""
    if isinstance(v, dict):
        v = v.get("sprite") or v.get("texture") or v.get("name")
    return v if isinstance(v, str) else None


def lookup(textures, ref, depth=0):
    ref = tex_ref(ref)
    while ref and ref.startswith("#") and depth < 16:
        ref = tex_ref(textures.get(ref[1:]))
        depth += 1
    return strip_ns(ref) if ref else None


def first_model(blockstate):
    if not blockstate:
        return None
    if "variants" in blockstate:
        v = next(iter(blockstate["variants"].values()))
        v = v[0] if isinstance(v, list) else v
        return v.get("model")
    if "multipart" in blockstate:
        for part in blockstate["multipart"]:
            a = part.get("apply")
            a = a[0] if isinstance(a, list) else a
            if a and "model" in a:
                return a["model"]
    return None


def classify(name, jar, tiles):
    bs = jar.json(f"assets/minecraft/blockstates/{name}.json")
    model = first_model(bs)
    if model is None:
        return None
    textures, elements, chain = resolve_model(jar, model)
    roots = {c.split("/")[-1] for c in chain}

    def tile_for(ref):
        tex = lookup(textures, ref)
        if not tex:
            return -1
        return tiles.get(tex, -1)

    face_tiles = [-1] * 6
    tint_mask = 0
    shape = "cube"
    box = None

    if roots & CROSS_PARENTS:
        t = tile_for("#cross") if "cross" in textures else tile_for("#plant") if "plant" in textures else tile_for("#crop")
        if t < 0:
            t = tile_for("#particle")
        face_tiles = [t] * 6
        shape = "cross"
        if "tinted_cross" in roots:
            tint_mask = 63
    elif elements:
        # Union box of all elements; face textures from the element that defines them
        mn = [16, 16, 16]
        mx = [0, 0, 0]
        for el in elements:
            for i in range(3):
                mn[i] = min(mn[i], el["from"][i])
                mx[i] = max(mx[i], el["to"][i])
            for fi, f in enumerate(FACES):
                face = el.get("faces", {}).get(f)
                if face and face_tiles[fi] < 0:
                    face_tiles[fi] = tile_for(face.get("texture"))
                    if "tintindex" in face:
                        tint_mask |= 1 << fi
        missing = [i for i in range(6) if face_tiles[i] < 0]
        if missing:
            p = tile_for("#particle")
            for i in missing:
                face_tiles[i] = p
        if mn == [0, 0, 0] and mx == [16, 16, 16]:
            shape = "cube"
        else:
            shape = "box"
            box = mn + mx
    else:
        p = tile_for("#particle")
        if p < 0:
            return None
        face_tiles = [p] * 6

    if name in ("water", "lava"):
        still = tiles.get(f"block/{name}_still", -1)
        face_tiles = [still] * 6
        shape = "liquid"
    return shape, face_tiles, tint_mask, box


# ----------------------------------------------------------------------------------------------
# Block models -> baked quads
# ----------------------------------------------------------------------------------------------
import math

FACE_DIR = {"down": (0, -1, 0), "up": (0, 1, 0), "north": (0, 0, -1), "south": (0, 0, 1), "west": (-1, 0, 0), "east": (1, 0, 0)}
DIR_FACE = {v: k for k, v in FACE_DIR.items()}


def face_corners(f, a, b):
    """Minecraft FaceInfo vertex order (counter-clockwise seen from outside), 0..16 space."""
    x0, y0, z0 = a
    x1, y1, z1 = b
    return {
        "down":  [(x0, y0, z1), (x0, y0, z0), (x1, y0, z0), (x1, y0, z1)],
        "up":    [(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)],
        "north": [(x1, y1, z0), (x1, y0, z0), (x0, y0, z0), (x0, y1, z0)],
        "south": [(x0, y1, z1), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1)],
        "west":  [(x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1)],
        "east":  [(x1, y1, z1), (x1, y0, z1), (x1, y0, z0), (x1, y1, z0)],
    }[f]


def default_uv(f, a, b):
    x0, y0, z0 = a
    x1, y1, z1 = b
    return {
        "down":  [x0, 16 - z1, x1, 16 - z0],
        "up":    [x0, z0, x1, z1],
        "north": [16 - x1, 16 - y1, 16 - x0, 16 - y0],
        "south": [x0, 16 - y1, x1, 16 - y0],
        "west":  [z0, 16 - y1, z1, 16 - y0],
        "east":  [16 - z1, 16 - y1, 16 - z0, 16 - y0],
    }[f]


def locked_uv(f, p):
    """uvlock: texture stays aligned to the world - project the (rotated) position."""
    x, y, z = p
    return {
        "up": (x, z), "down": (x, 16 - z), "north": (16 - x, 16 - y),
        "south": (x, 16 - y), "west": (z, 16 - y), "east": (16 - z, 16 - y),
    }[f]


def rotate_axis(p, origin, axis, deg, rescale=False):
    t = math.radians(deg)
    c, s = math.cos(t), math.sin(t)
    x, y, z = (p[0] - origin[0], p[1] - origin[1], p[2] - origin[2])
    if axis == "x":
        y, z = y * c - z * s, y * s + z * c
    elif axis == "y":
        x, z = x * c + z * s, -x * s + z * c
    else:
        x, y = x * c - y * s, x * s + y * c
    if rescale and abs(c) > 1e-6:
        k = 1.0 / c
        if axis == "x":
            y, z = y * k, z * k
        elif axis == "y":
            x, z = x * k, z * k
        else:
            x, y = x * k, y * k
    return (x + origin[0], y + origin[1], z + origin[2])


def element_rotations(rot):
    """Element rotation -> list of (axis, angle). Supports {"axis","angle"} and {"x","y","z"} forms."""
    if not rot:
        return []
    if "axis" in rot:
        return [(rot["axis"], float(rot.get("angle", 0)))]
    return [(a, float(rot[a])) for a in ("x", "y", "z") if rot.get(a)]


def variant_rotate(p, xdeg, ydeg, center=8.0):
    """Blockstate x then y rotation in 90 degree steps (x=90: north -> down, y=90: north -> east)."""
    x, y, z = p[0] - center, p[1] - center, p[2] - center
    for _ in range((int(xdeg) // 90) % 4):
        y, z = z, -y
    for _ in range((int(ydeg) // 90) % 4):
        x, z = -z, x
    return (x + center, y + center, z + center)


def rotate_dir(d, xdeg, ydeg):
    v = variant_rotate((d[0] + 8, d[1] + 8, d[2] + 8), xdeg, ydeg)
    return (round(v[0] - 8), round(v[1] - 8), round(v[2] - 8))


def compile_model(jar, tiles, name, xdeg=0, ydeg=0, uvlock=False):
    textures, elements, chain = resolve_model(jar, name)
    if not elements:
        return None
    particle = tiles.get(lookup(textures, "#particle") or "", -1)
    quads, seen = [], set()
    for el in elements:
        a, b = el.get("from", [0, 0, 0]), el.get("to", [16, 16, 16])
        rot = el.get("rotation")
        rots = element_rotations(rot)
        origin = rot.get("origin", [8, 8, 8]) if rot else [8, 8, 8]
        rescale = bool(rot.get("rescale")) if rot else False
        shade = 1 if el.get("shade", True) else 0
        for f, face in el.get("faces", {}).items():
            if f not in FACE_DIR:
                continue
            tex = lookup(textures, face.get("texture"))
            tile = tiles.get(tex, -1) if tex else -1
            if tile < 0:
                tile = particle
            uv = face.get("uv") or default_uv(f, a, b)
            frot = (int(face.get("rotation", 0)) // 90) % 4
            corners = face_corners(f, a, b)
            uvs = []
            for i in range(4):
                idx = (i + frot) % 4
                uvs.append((uv[0] if idx in (0, 1) else uv[2], uv[1] if idx in (0, 3) else uv[3]))
            pts = []
            for p in corners:
                for axis, ang in rots:
                    p = rotate_axis(p, origin, axis, ang, rescale)
                pts.append(variant_rotate(p, xdeg, ydeg))
            # face direction after the variant rotation (element rotation keeps the nominal face)
            fdir = rotate_dir(FACE_DIR[f], xdeg, ydeg)
            fname = DIR_FACE.get(fdir, f)
            if uvlock and not rots:
                uvs = [locked_uv(fname, p) for p in pts]
            cull = face.get("cullface")
            cull_idx = -1
            if cull in FACE_DIR:
                cull_idx = FACES.index(DIR_FACE[rotate_dir(FACE_DIR[cull], xdeg, ydeg)])
            # shading direction: dominant axis of the real normal
            e1 = [pts[1][k] - pts[0][k] for k in range(3)]
            e2 = [pts[2][k] - pts[0][k] for k in range(3)]
            n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            if abs(n[0]) + abs(n[1]) + abs(n[2]) < 1e-6:
                e2 = [pts[3][k] - pts[0][k] for k in range(3)]
                n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                if abs(n[0]) + abs(n[1]) + abs(n[2]) < 1e-6:
                    continue   # zero-area face
            k = max(range(3), key=lambda i: abs(n[i]))
            nd = [0, 0, 0]
            nd[k] = 1 if n[k] > 0 else -1
            shade_face = FACES.index(DIR_FACE[tuple(nd)])
            tint = 1 if face.get("tintindex", -1) is not None and int(face.get("tintindex", -1)) >= 0 else 0
            # Plants define both sides of one plane (mirrored UVs). TF2MC's world materials are
            # double sided ($nocull), so the second, back-to-back quad would only z-fight: keep one.
            key = tuple(sorted((round(p[0], 3), round(p[1], 3), round(p[2], 3)) for p in pts))
            if key in seen:
                continue
            seen.add(key)
            quads.append((tile, cull_idx, shade_face, tint, shade,
                          [(p[0] / 16, p[1] / 16, p[2] / 16) for p in pts], [(u / 16, v / 16) for u, v in uvs]))
    return quads


def flatten_when(w):
    """Multipart 'when' -> list of AND-dicts (OR of ANDs)."""
    if w is None:
        return []
    if "OR" in w:
        out = []
        for x in w["OR"]:
            out += flatten_when(x)
        return out
    if "AND" in w:
        combos = [{}]
        for x in w["AND"]:
            combos = [{**c, **alt} for c in combos for alt in (flatten_when(x) or [{}])]
        return combos
    return [{k: (str(v).lower() if isinstance(v, bool) else str(v)) for k, v in w.items()}]


def cond_str(when):
    alts = flatten_when(when)
    if not alts:
        return "-"
    return ";".join(",".join(f"{k}={v}" for k, v in sorted(d.items())) for d in alts) or "-"


def fmt(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def build_models(jar, tiles, out):
    blocks = sorted(n[len("assets/minecraft/blockstates/"):-5] for n in jar.names
                    if n.startswith("assets/minecraft/blockstates/") and n.endswith(".json"))
    cache, models, rules = {}, [], []

    def model_id(ref):
        key = (strip_ns(ref.get("model", "")), int(ref.get("x", 0)), int(ref.get("y", 0)), bool(ref.get("uvlock", False)))
        if key not in cache:
            q = compile_model(jar, tiles, key[0], key[1], key[2], key[3]) if key[0] else None
            if q:
                cache[key] = len(models)
                models.append(q)
            else:
                cache[key] = -1
        return cache[key]

    for b in blocks:
        bs = jar.json(f"assets/minecraft/blockstates/{b}.json")
        if not bs:
            continue
        lines = []
        if "variants" in bs:
            for sel, v in bs["variants"].items():
                v = v[0] if isinstance(v, list) else v
                mid = model_id(v)
                if mid >= 0:
                    lines.append(f"v minecraft:{b} {sel or '-'} {mid}")
        elif "multipart" in bs:
            for part in bs["multipart"]:
                a = part.get("apply")
                a = a[0] if isinstance(a, list) else a
                if not a:
                    continue
                mid = model_id(a)
                if mid >= 0:
                    lines.append(f"p minecraft:{b} {cond_str(part.get('when'))} {mid}")
        rules += lines

    with open(os.path.join(out, "scripts", "mc_models.txt"), "w") as f:
        f.write("# generated by tools/mc_assets.py: baked block models + blockstate rules\n")
        f.write(f"models {len(models)}\n")
        for i, q in enumerate(models):
            f.write(f"m {i} {len(q)}\n")
            for tile, cull, sf, tint, shade, pts, uvs in q:
                f.write(f"q {tile} {cull} {sf} {tint} {shade} " +
                        " ".join(f"{fmt(p[0])} {fmt(p[1])} {fmt(p[2])} {fmt(u)} {fmt(v)}" for p, (u, v) in zip(pts, uvs)) + "\n")
        f.write("\n".join(rules) + "\n")
    return len(models), len(rules)


# ----------------------------------------------------------------------------------------------
# Sounds: launcher asset store (hashed objects) -> sound/mc/*.wav + scripts/mc_sounds.txt
# ----------------------------------------------------------------------------------------------
SKIP_SOUND_PREFIXES = ("music.", "music_disc.", "ambient.cave", "ambient.basalt_deltas", "ambient.crimson_forest",
                       "ambient.nether_wastes", "ambient.soul_sand_valley", "ambient.warped_forest", "ambient.underwater",
                       "weather.rain")
SOUND_RATE = 22050   # Source plays 44100/22050/11025 Hz; 22050 mono keeps the folder a sensible size


def find_asset_index(jar_path, assets_dir=None):
    """Returns (assets_dir, index dict) or (None, None)."""
    jar_path = os.path.abspath(jar_path)
    ver_dir = os.path.dirname(jar_path)
    ver = os.path.splitext(os.path.basename(jar_path))[0]
    candidates = [assets_dir] if assets_dir else []
    candidates += [os.path.join(os.path.dirname(os.path.dirname(ver_dir)), "assets")]   # .minecraft/versions/<v>/<v>.jar
    for base in (os.environ.get("APPDATA"), os.path.expanduser("~")):
        if base:
            candidates += [os.path.join(base, ".minecraft", "assets"), os.path.join(base, "Library", "Application Support", "minecraft", "assets")]
    index_id = None
    meta = os.path.join(ver_dir, ver + ".json")
    if os.path.isfile(meta):
        try:
            index_id = json.load(open(meta)).get("assetIndex", {}).get("id")
        except (OSError, ValueError):
            pass
    for d in candidates:
        if not d or not os.path.isdir(os.path.join(d, "indexes")):
            continue
        idx = os.path.join(d, "indexes", index_id + ".json") if index_id else None
        if not idx or not os.path.isfile(idx):
            files = sorted((os.path.join(d, "indexes", f) for f in os.listdir(os.path.join(d, "indexes")) if f.endswith(".json")),
                           key=os.path.getmtime)
            idx = files[-1] if files else None
        if idx:
            return d, json.load(open(idx))
    return None, None


def make_decoder():
    """Returns decode(ogg_path) -> (mono float samples list-like, rate) via soundfile, or None."""
    try:
        import numpy as np
        import soundfile as sf

        def decode(path):
            data, rate = sf.read(path, dtype="float32", always_2d=True)
            mono = data.mean(axis=1)
            if rate != SOUND_RATE and len(mono) > 1:
                n = max(1, int(round(len(mono) * SOUND_RATE / rate)))
                mono = np.interp(np.linspace(0, len(mono) - 1, n), np.arange(len(mono)), mono)
            pcm = (np.clip(mono, -1, 1) * 32767).astype("<i2").tobytes()
            return pcm
        return decode
    except ImportError:
        pass
    import shutil
    import subprocess
    ff = shutil.which("ffmpeg")
    if not ff:
        return None

    def decode_ff(path):
        r = subprocess.run([ff, "-v", "error", "-i", path, "-ac", "1", "-ar", str(SOUND_RATE), "-f", "s16le", "-"],
                           capture_output=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.decode(errors="replace")[:200])
        return r.stdout
    return decode_ff


def write_wav(path, pcm):
    import wave
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SOUND_RATE)
        w.writeframes(pcm)


def build_sounds(jar_path, out, assets_dir=None):
    adir, index = find_asset_index(jar_path, assets_dir)
    if not index:
        print("sounds: launcher asset store not found (pass --assets <.minecraft/assets>) - skipping sounds")
        return 0, 0
    objects = index.get("objects", {})

    def obj_path(key):
        o = objects.get(key)
        return os.path.join(adir, "objects", o["hash"][:2], o["hash"]) if o else None

    sj = obj_path("minecraft/sounds.json")
    if not sj or not os.path.isfile(sj):
        print("sounds: sounds.json missing from the asset index - skipping sounds")
        return 0, 0
    decode = make_decoder()
    if decode is None:
        print("sounds: install `pip install soundfile numpy` or put ffmpeg on the PATH to convert sounds - skipping")
        return 0, 0
    events = json.load(open(sj))

    def entries(name, depth=0):
        """(file, volume, pitch, weight) for an event, following {"type": "event"} references."""
        ev = events.get(name)
        if not ev or depth > 4:
            return []
        out_l = []
        for snd in ev.get("sounds", []):
            if isinstance(snd, str):
                snd = {"name": snd}
            nm = strip_ns(snd.get("name", ""))
            if snd.get("type") == "event":
                out_l += [(f, v * snd.get("volume", 1.0), p * snd.get("pitch", 1.0), w) for f, v, p, w in entries(nm, depth + 1)]
            else:
                out_l.append((nm, float(snd.get("volume", 1.0)), float(snd.get("pitch", 1.0)), int(snd.get("weight", 1))))
        return out_l

    converted, failed, lines = {}, 0, []
    for name in sorted(events):
        if name.startswith(SKIP_SOUND_PREFIXES):
            continue
        files = entries(name)
        good = []
        for f, vol, pitch, weight in files:
            if f not in converted:
                src = obj_path(f"minecraft/sounds/{f}.ogg")
                ok = False
                if src and os.path.isfile(src):
                    try:
                        write_wav(os.path.join(out, "sound", "mc", f + ".wav"), decode(src))
                        ok = True
                    except Exception as e:   # noqa: BLE001 - keep going, report at the end
                        failed += 1
                        if failed <= 5:
                            print(f"sounds: could not convert {f}: {e}")
                converted[f] = ok
            if converted[f]:
                good.append(f"f mc/{f}.wav {fmt(vol)} {fmt(pitch)} {weight}")
        if good:
            lines.append(f"e minecraft:{name}")
            lines += good
    with open(os.path.join(out, "scripts", "mc_sounds.txt"), "w") as fh:
        fh.write("# generated by tools/mc_assets.py from sounds.json\n" + "\n".join(lines) + "\n")
    n_files = sum(1 for v in converted.values() if v)
    if failed:
        print(f"sounds: {failed} files could not be converted")
    return sum(1 for l in lines if l.startswith("e ")), n_files


# ----------------------------------------------------------------------------------------------
# Mob textures: everything scripts/mc_entity_geometry.txt names, packed into one atlas
# ----------------------------------------------------------------------------------------------
def entity_texture_specs(geometry_text):
    specs = []
    for line in geometry_text.splitlines():
        p = line.split("//")[0].split()
        if len(p) >= 3 and p[0] == "tex":
            specs.append(p[2])
        elif len(p) >= 4 and p[0] == "skin":
            specs.append(p[3])
    return list(dict.fromkeys(specs))


def build_entity_atlas(jar, out):
    geo = os.path.join(out, "scripts", "mc_entity_geometry.txt")
    if not os.path.isfile(geo):
        geo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "game", "mod_tf", "scripts", "mc_entity_geometry.txt")
    if not os.path.isfile(geo):
        return 0
    images = []
    for spec in entity_texture_specs(open(geo).read()):
        for alt in spec.split("|"):
            path = f"assets/minecraft/textures/{alt}.png"
            if path in jar.names:
                im = Image.open(io.BytesIO(jar.z.read(path))).convert("RGBA")
                images.append((spec, im))
                break
    if not images:
        return 0
    # shelf packing, tallest first
    width = max(1024, max(im.width for _, im in images))
    w2 = 1
    while w2 < width:
        w2 *= 2
    order = sorted(images, key=lambda t: (-t[1].height, -t[1].width))
    x = y = shelf = 0
    rects = {}
    for spec, im in order:
        if x + im.width > w2:
            x, y, shelf = 0, y + shelf, 0
        rects[spec] = (x, y, im.width, im.height)
        x += im.width
        shelf = max(shelf, im.height)
    h2 = 1
    while h2 < y + shelf:
        h2 *= 2
    atlas = Image.new("RGBA", (w2, h2), (0, 0, 0, 0))
    for spec, im in images:
        r = rects[spec]
        atlas.paste(im, (r[0], r[1]))
    write_vtf(os.path.join(out, "materials", "mc", "entities.vtf"), atlas)
    with open(os.path.join(out, "scripts", "mc_entity_textures.txt"), "w") as f:
        f.write(f"# generated by tools/mc_assets.py\natlas {w2} {h2}\n")
        for spec, im in images:
            r = rects[spec]
            f.write(f"t {spec} {r[0]} {r[1]} {r[2]} {r[3]}\n")
    return len(images)


def tint_for(name, tint_mask):
    if name in ("water", "bubble_column"):
        return "water"
    if name == "leaf_litter":
        return "dry_foliage"
    if name == "redstone_wire":
        return "redstone"
    if name == "lily_pad":
        return "lily"
    if name in ("pumpkin_stem", "melon_stem", "attached_pumpkin_stem", "attached_melon_stem"):
        return "stem"
    if "spruce_leaves" in name:
        return "spruce"
    if "birch_leaves" in name:
        return "birch"
    if "leaves" in name or name in ("vine",):
        return "foliage" if name not in ("cherry_leaves", "azalea_leaves", "flowering_azalea_leaves", "pale_oak_leaves") else None
    if tint_mask:
        return "grass"
    return None


def write_vtf(path, im):
    w, h = im.size
    flags = 0x0001 | 0x0100 | 0x0200 | 0x2000   # POINTSAMPLE | NOMIP | NOLOD | EIGHTBITALPHA
    hdr = struct.pack("<4sIIIHHIHH4s3f4sfIBIBBH",
                      b"VTF\0", 7, 2, 80, w, h, flags, 1, 0, b"\0" * 4,
                      0.5, 0.5, 0.5, b"\0" * 4, 1.0, 0,    # IMAGE_FORMAT_RGBA8888
                      1, 0xFFFFFFFF, 0, 0, 1)
    hdr = hdr.ljust(80, b"\0")
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(im.tobytes())   # RGBA8888, top-down


ITEM_TILE = 32
ITEMS_PER_ROW = 32
TINTS = {"grass": (0x91, 0xBD, 0x59), "foliage": (0x77, 0xAB, 0x2F), "water": (0x3F, 0x76, 0xE4),
         "spruce": (0x61, 0x99, 0x61), "birch": (0x80, 0xA7, 0x55)}


def find_model_ref(node):
    """26.x item definitions nest models in select/condition/range_dispatch: take the first plain model."""
    if isinstance(node, dict):
        if node.get("type", "").endswith("model") and isinstance(node.get("model"), str):
            return node["model"]
        for k in ("model", "fallback", "on_true", "on_false", "cases", "entries"):
            if k in node:
                r = find_model_ref(node[k])
                if r:
                    return r
        for v in node.values():
            r = find_model_ref(v)
            if r:
                return r
    elif isinstance(node, list):
        for v in node:
            r = find_model_ref(v)
            if r:
                return r
    return None


def tinted(im, rgb):
    r, g, b, a = im.split()
    r = r.point(lambda v: v * rgb[0] // 255)
    g = g.point(lambda v: v * rgb[1] // 255)
    b = b.point(lambda v: v * rgb[2] // 255)
    return Image.merge("RGBA", (r, g, b, a))


def iso_cube(top, left, right):
    """32x32 isometric block icon from three 16x16 face textures."""
    out = Image.new("RGBA", (ITEM_TILE, ITEM_TILE), (0, 0, 0, 0))
    px = out.load()
    tp, lp, rp = top.load(), left.load(), right.load()
    L, T, R, B = (1.0, 8.0), (16.0, 1.0), (31.0, 8.0), (16.0, 15.0)
    LB, BB = (1.0, 24.0), (16.0, 31.0)

    def solve(p, o, a, b):
        ax, ay, bx, by = a[0] - o[0], a[1] - o[1], b[0] - o[0], b[1] - o[1]
        det = ax * by - ay * bx
        dx, dy = p[0] - o[0], p[1] - o[1]
        return (dx * by - dy * bx) / det, (ax * dy - ay * dx) / det

    faces = [(L, T, B, tp, 1.0), (L, B, LB, lp, 0.8), (B, R, BB, rp, 0.6)]
    for y in range(ITEM_TILE):
        for x in range(ITEM_TILE):
            p = (x + 0.5, y + 0.5)
            for o, a, b, tex, shade in faces:
                u, v = solve(p, o, a, b)
                if 0 <= u < 1 and 0 <= v < 1:
                    c = tex[min(15, int(u * 16)), min(15, int(v * 16))]
                    if c[3] > 0:
                        px[x, y] = (int(c[0] * shade), int(c[1] * shade), int(c[2] * shade), c[3])
                    break
    return out


def build_items(jar, out):
    names = sorted(n for n in jar.names if n.startswith("assets/minecraft/items/") and n.endswith(".json"))
    defs = []
    if names:
        for n in names:
            item = n[len("assets/minecraft/items/"):-5]
            d = jar.json(n) or {}
            defs.append((item, find_model_ref(d.get("model", d))))
    else:   # older layout: one item model per item
        for n in sorted(jar.names):
            if n.startswith("assets/minecraft/models/item/") and n.endswith(".json"):
                item = n[len("assets/minecraft/models/item/"):-5]
                defs.append((item, "minecraft:item/" + item))

    icons, lines = [], []
    for item, ref in defs:
        if not ref:
            continue
        textures, elements, chain = resolve_model(jar, ref)
        roots = {c.split("/")[-1] for c in chain}
        icon = None
        if "layer0" in textures:
            base = jar.image(f"assets/minecraft/textures/{lookup(textures, '#layer0')}.png")
            if base is not None:
                icon = base.resize((ITEM_TILE, ITEM_TILE), Image.NEAREST)
                if "layer1" in textures:
                    over = jar.image(f"assets/minecraft/textures/{lookup(textures, '#layer1')}.png")
                    if over is not None:
                        icon.alpha_composite(over.resize((ITEM_TILE, ITEM_TILE), Image.NEAREST))
        elif elements or roots & FULL_CUBE_PARENTS:
            def face(*keys):
                for k in keys:
                    if k in textures:
                        t = lookup(textures, "#" + k)
                        im = jar.image(f"assets/minecraft/textures/{t}.png") if t else None
                        if im is not None:
                            return im
                return None
            top = face("up", "top", "end", "all", "texture", "particle", "side")
            side = face("north", "side", "front", "all", "texture", "particle", "up")
            if top is not None and side is not None:
                if item in ("grass_block",):
                    top = tinted(top, TINTS["grass"])
                if "leaves" in item and "cherry" not in item and "azalea" not in item:
                    tint = TINTS["spruce"] if "spruce" in item else TINTS["birch"] if "birch" in item else TINTS["foliage"]
                    top, side = tinted(top, tint), tinted(side, tint)
                icon = iso_cube(top, side, side)
        elif roots & CROSS_PARENTS:
            t = lookup(textures, "#cross")
            im = jar.image(f"assets/minecraft/textures/{t}.png") if t else None
            if im is not None:
                icon = im.resize((ITEM_TILE, ITEM_TILE), Image.NEAREST)
        if icon is None:
            continue
        lines.append(f"item minecraft:{item} {len(icons)}")
        icons.append(icon)

    rows = max(1, (len(icons) + ITEMS_PER_ROW - 1) // ITEMS_PER_ROW)
    rows_pow2 = 1
    while rows_pow2 < rows:
        rows_pow2 *= 2
    atlas = Image.new("RGBA", (ITEMS_PER_ROW * ITEM_TILE, rows_pow2 * ITEM_TILE), (0, 0, 0, 0))
    for i, im in enumerate(icons):
        atlas.paste(im, ((i % ITEMS_PER_ROW) * ITEM_TILE, (i // ITEMS_PER_ROW) * ITEM_TILE))
    write_vtf(os.path.join(out, "materials", "mc", "items.vtf"), atlas)
    with open(os.path.join(out, "scripts", "mc_items.txt"), "w") as f:
        f.write(f"# generated by tools/mc_assets.py\natlas {ITEMS_PER_ROW} {rows_pow2}\n" + "\n".join(lines) + "\n")
    return len(icons)


def main():
    args = [a for a in sys.argv[1:]]
    want_sounds = "--no-sounds" not in args
    assets_dir = None
    if "--assets" in args:
        i = args.index("--assets")
        assets_dir = args[i + 1] if i + 1 < len(args) else None
        del args[i:i + 2]
    args = [a for a in args if not a.startswith("--")]
    if len(args) < 2:
        sys.exit(__doc__)
    jar_path = args[0]
    jar = Jar(jar_path)
    out = args[1]

    # 1. collect block textures
    tex_names = sorted(n[len("assets/minecraft/textures/"):-4] for n in jar.names
                       if n.startswith("assets/minecraft/textures/block/") and n.endswith(".png"))
    tiles, images = {}, []
    for t in tex_names:
        im = jar.image(f"assets/minecraft/textures/{t}.png")
        if im is None:
            continue
        tiles[t] = len(images)
        images.append(im)
    rows = max(1, (len(images) + TILES_PER_ROW - 1) // TILES_PER_ROW)
    rows_pow2 = 1
    while rows_pow2 < rows:
        rows_pow2 *= 2
    atlas = Image.new("RGBA", (TILES_PER_ROW * TILE, rows_pow2 * TILE), (0, 0, 0, 0))
    for i, im in enumerate(images):
        atlas.paste(im, ((i % TILES_PER_ROW) * TILE, (i // TILES_PER_ROW) * TILE))

    # 2. classify blocks
    blocks = sorted(n[len("assets/minecraft/blockstates/"):-5] for n in jar.names
                    if n.startswith("assets/minecraft/blockstates/") and n.endswith(".json"))
    lines = [f"# generated by tools/mc_assets.py from {os.path.basename(jar_path)}",
             f"atlas {TILES_PER_ROW} {rows_pow2}"]
    for b in blocks:
        r = classify(b, jar, tiles)
        if r is None:
            continue
        shape, ft, tint_mask, box = r
        kv = []
        tint = tint_for(b, tint_mask)
        if tint:
            kv.append(f"tint={tint}")
            if tint_mask and tint_mask != 63 and shape != "cross":
                kv.append(f"tintmask={tint_mask}")
        if box:
            kv.append("box=" + ",".join(f"{v:g}" for v in box))
        if shape == "cross":
            kv.append("collide=0")
        lines.append(f"block minecraft:{b} {shape} " + " ".join(str(t) for t in ft) + (" " + " ".join(kv) if kv else ""))

    os.makedirs(os.path.join(out, "materials", "mc"), exist_ok=True)
    os.makedirs(os.path.join(out, "scripts"), exist_ok=True)
    write_vtf(os.path.join(out, "materials", "mc", "atlas.vtf"), atlas)
    with open(os.path.join(out, "scripts", "mc_blocks.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    n_items = build_items(jar, out)
    n_models, n_rules = build_models(jar, tiles, out)
    n_mob = build_entity_atlas(jar, out)
    print(f"atlas: {len(images)} textures -> {atlas.size[0]}x{atlas.size[1]}; {len(lines) - 2} block definitions; "
          f"{n_models} baked models / {n_rules} blockstate rules; {n_items} item icons; {n_mob} mob textures")
    if want_sounds:
        n_events, n_files = build_sounds(jar_path, out, assets_dir)
        if n_events:
            print(f"sounds: {n_events} sound events, {n_files} files -> sound/mc")


if __name__ == "__main__":
    main()
