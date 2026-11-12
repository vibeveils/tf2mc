#!/usr/bin/env python3
"""
Build the TF2MC block atlas and block definitions from YOUR OWN Minecraft client jar.

Mojang's textures are not redistributed with TF2MC; this script converts the copy you own:

    python3 tools/mc_assets.py ~/.minecraft/versions/26.3/26.3.jar  <path-to>/game/mod_tf

Outputs (inside the mod folder):
    materials/mc/atlas.vtf      block texture atlas, point sampled, no mips (VTF 7.2 written directly)
    scripts/mc_blocks.txt       one line per block: shape, per-face atlas tiles, tint, box, flags
    materials/mc/items.vtf      item icon atlas (32px icons; blocks drawn as little isometric cubes)
    scripts/mc_items.txt        item id -> icon tile

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


def lookup(textures, ref, depth=0):
    while ref and ref.startswith("#") and depth < 16:
        ref = textures.get(ref[1:])
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


def tint_for(name, tint_mask):
    if name in ("water", "bubble_column"):
        return "water"
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
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    jar = Jar(sys.argv[1])
    out = sys.argv[2]

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
    lines = [f"# generated by tools/mc_assets.py from {os.path.basename(sys.argv[1])}",
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
    print(f"atlas: {len(images)} textures -> {atlas.size[0]}x{atlas.size[1]}; {len(lines) - 2} block definitions; {n_items} item icons")


if __name__ == "__main__":
    main()
