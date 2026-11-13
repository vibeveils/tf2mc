#!/usr/bin/env python3
"""Builds a tiny stand-in for a Minecraft client jar (real file formats, made-up content) so
tools/mc_assets.py can be tested without Mojang's assets."""
import io
import json
import sys
import zipfile

from PIL import Image


def png(color, size=(16, 16)):
    im = Image.new("RGBA", size, color)
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


def faces(tex, cull=True, names=("down", "up", "north", "south", "west", "east")):
    return {f: ({"texture": tex, "cullface": f} if cull else {"texture": tex}) for f in names}


MODELS = {
    # Mojang's cross: two planes rotated 45 degrees with rescale
    "block/cross": {"ambientocclusion": False, "textures": {"particle": "#cross"}, "elements": [
        {"from": [0.8, 0, 8], "to": [15.2, 16, 8], "rotation": {"origin": [8, 8, 8], "axis": "y", "angle": 45, "rescale": True},
         "shade": False, "faces": {"north": {"uv": [0, 0, 16, 16], "texture": "#cross"}, "south": {"uv": [0, 0, 16, 16], "texture": "#cross"}}},
        {"from": [8, 0, 0.8], "to": [8, 16, 15.2], "rotation": {"origin": [8, 8, 8], "axis": "y", "angle": 45, "rescale": True},
         "shade": False, "faces": {"west": {"uv": [0, 0, 16, 16], "texture": "#cross"}, "east": {"uv": [0, 0, 16, 16], "texture": "#cross"}}}]},
    "block/poppy": {"parent": "block/cross", "textures": {"cross": "block/poppy"}},
    # seagrass: four upright planes (not a 'cross' parent, union box is a full cube)
    "block/template_seagrass": {"textures": {"particle": "#texture"}, "elements": [
        {"from": [0, 0, 4], "to": [16, 16, 4], "shade": False, "faces": {"north": {"uv": [0, 0, 16, 16], "texture": "#texture"}, "south": {"uv": [16, 0, 0, 16], "texture": "#texture"}}},
        {"from": [0, 0, 12], "to": [16, 16, 12], "shade": False, "faces": {"north": {"uv": [16, 0, 0, 16], "texture": "#texture"}, "south": {"uv": [0, 0, 16, 16], "texture": "#texture"}}},
        {"from": [4, 0, 0], "to": [4, 16, 16], "shade": False, "faces": {"west": {"uv": [0, 0, 16, 16], "texture": "#texture"}, "east": {"uv": [16, 0, 0, 16], "texture": "#texture"}}},
        {"from": [12, 0, 0], "to": [12, 16, 16], "shade": False, "faces": {"west": {"uv": [16, 0, 0, 16], "texture": "#texture"}, "east": {"uv": [0, 0, 16, 16], "texture": "#texture"}}}]},
    "block/seagrass": {"parent": "block/template_seagrass", "textures": {"texture": "block/seagrass"}},
    # leaf litter (1 segment): one flat tinted quarter, newer object-style texture reference
    "block/leaf_litter_1": {"textures": {"particle": "block/leaf_litter", "texture": {"sprite": "minecraft:block/leaf_litter"}}, "elements": [
        {"from": [0, 0.25, 0], "to": [8, 0.25, 8], "shade": False,
         "faces": {"up": {"uv": [0, 0, 8, 8], "texture": "#texture", "tintindex": 0}, "down": {"uv": [0, 8, 8, 0], "texture": "#texture", "tintindex": 0}}}]},
    # stairs (east-facing in the model, like Mojang's)
    "block/stairs": {"textures": {"particle": "#side"}, "elements": [
        {"from": [0, 0, 0], "to": [16, 8, 16], "faces": {
            "down": {"uv": [0, 0, 16, 16], "texture": "#bottom", "cullface": "down"}, "up": {"uv": [0, 0, 16, 16], "texture": "#top"},
            "north": {"uv": [0, 8, 16, 16], "texture": "#side", "cullface": "north"}, "south": {"uv": [0, 8, 16, 16], "texture": "#side", "cullface": "south"},
            "west": {"uv": [0, 8, 16, 16], "texture": "#side", "cullface": "west"}, "east": {"uv": [0, 8, 16, 16], "texture": "#side", "cullface": "east"}}},
        {"from": [8, 8, 0], "to": [16, 16, 16], "faces": {
            "up": {"uv": [8, 0, 16, 16], "texture": "#top", "cullface": "up"}, "north": {"uv": [0, 0, 8, 8], "texture": "#side", "cullface": "north"},
            "south": {"uv": [8, 0, 16, 8], "texture": "#side", "cullface": "south"}, "west": {"uv": [0, 0, 16, 8], "texture": "#side"},
            "east": {"uv": [0, 0, 16, 8], "texture": "#side", "cullface": "east"}}}]},
    "block/oak_stairs": {"parent": "block/stairs", "textures": {"bottom": "block/oak_planks", "top": "block/oak_planks", "side": "block/oak_planks"}},
    "block/cube": {"elements": [{"from": [0, 0, 0], "to": [16, 16, 16], "faces": {
        f: {"texture": "#" + f, "cullface": f} for f in ("down", "up", "north", "south", "west", "east")}}]},
    "block/cube_all": {"parent": "block/cube", "textures": {"particle": "#all", "down": "#all", "up": "#all", "north": "#all", "south": "#all", "west": "#all", "east": "#all"}},
    "block/stone": {"parent": "block/cube_all", "textures": {"all": "block/stone"}},
    "block/oak_planks": {"parent": "block/cube_all", "textures": {"all": "block/oak_planks"}},
    "block/oak_fence_post": {"textures": {"texture": "block/oak_planks"}, "elements": [
        {"from": [6, 0, 6], "to": [10, 16, 10], "faces": {"down": {"texture": "#texture", "cullface": "down"}, "up": {"texture": "#texture", "cullface": "up"},
                                                          "north": {"texture": "#texture"}, "south": {"texture": "#texture"}, "west": {"texture": "#texture"}, "east": {"texture": "#texture"}}}]},
    "block/oak_fence_side": {"textures": {"texture": "block/oak_planks"}, "elements": [
        {"from": [7, 12, 0], "to": [9, 15, 9], "faces": {"up": {"texture": "#texture"}, "down": {"texture": "#texture"}, "north": {"texture": "#texture", "cullface": "north"},
                                                         "west": {"texture": "#texture"}, "east": {"texture": "#texture"}}}]},
    # block entity style: no elements -> left to the simple-shape fallback
    "block/chest": {"textures": {"particle": "block/oak_planks"}},
    "block/water": {"textures": {"particle": "block/water_still"}},
}

BLOCKSTATES = {
    "poppy": {"variants": {"": {"model": "minecraft:block/poppy"}}},
    "seagrass": {"variants": {"": {"model": "minecraft:block/seagrass"}}},
    "leaf_litter": {"multipart": [{"when": {"segment_amount": "1|2|3|4"}, "apply": {"model": "minecraft:block/leaf_litter_1"}}]},
    "stone": {"variants": {"": {"model": "minecraft:block/stone"}}},
    "oak_stairs": {"variants": {
        "facing=east,half=bottom,shape=straight": {"model": "minecraft:block/oak_stairs"},
        "facing=north,half=bottom,shape=straight": {"model": "minecraft:block/oak_stairs", "y": 270, "uvlock": True}}},
    "oak_fence": {"multipart": [
        {"apply": {"model": "minecraft:block/oak_fence_post"}},
        {"when": {"north": "true"}, "apply": {"model": "minecraft:block/oak_fence_side", "uvlock": True}},
        {"when": {"OR": [{"east": "true"}, {"west": "true"}]}, "apply": {"model": "minecraft:block/oak_fence_side", "y": 90, "uvlock": True}}]},
    "chest": {"variants": {"facing=north": {"model": "minecraft:block/chest"}}},
    "water": {"variants": {"": {"model": "minecraft:block/water"}}},
}

TEXTURES = {
    "block/poppy": (200, 20, 20, 255), "block/seagrass": (40, 160, 60, 255), "block/leaf_litter": (150, 110, 60, 255),
    "block/oak_planks": (160, 130, 80, 255), "block/stone": (125, 125, 125, 255), "block/water_still": (60, 90, 200, 180),
}


def build(path, extra=None):
    with zipfile.ZipFile(path, "w") as z:
        for name, m in MODELS.items():
            z.writestr(f"assets/minecraft/models/{name}.json", json.dumps(m))
        for name, bs in BLOCKSTATES.items():
            z.writestr(f"assets/minecraft/blockstates/{name}.json", json.dumps(bs))
        for name, c in TEXTURES.items():
            z.writestr(f"assets/minecraft/textures/{name}.png", png(c))
        for name, data in (extra or {}).items():
            z.writestr(name, data)


if __name__ == "__main__":
    build(sys.argv[1])
