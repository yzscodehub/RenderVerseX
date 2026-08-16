#!/usr/bin/env python3
"""Deterministically convert the pinned McGuire Crytek Sponza OBJ to glTF 2.0.

The converter intentionally implements only the OBJ/MTL surface used by the
2016-06-28 archive. It never downloads data, rejects paths outside the source
tree, emits stable JSON, and copies only texture files referenced by map_Kd.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import struct
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath


TOOL_NAME = "rvx-crytek-sponza-obj-to-gltf"
TOOL_VERSION = "1.0.0"


def fail(message: str) -> None:
    raise RuntimeError(message)


def resolve_source_file(root: Path, value: str) -> Path:
    normalized = value.replace("\\", "/")
    relative = PurePosixPath(normalized)
    if relative.is_absolute() or not relative.parts or any(
        part in ("", ".", "..") for part in relative.parts
    ):
        fail(f"unsafe source-relative path: {value}")
    candidate = (root / Path(*relative.parts)).resolve(strict=True)
    try:
        candidate.relative_to(root.resolve(strict=True))
    except ValueError:
        fail(f"source path escapes root: {value}")
    if not candidate.is_file() or candidate.is_symlink():
        fail(f"source dependency is not a regular non-symlink file: {value}")
    return candidate


@dataclass
class Material:
    name: str
    base_color: tuple[float, float, float] = (1.0, 1.0, 1.0)
    shininess: float = 10.0
    opacity: float = 1.0
    base_color_texture: str | None = None
    alpha_texture: str | None = None


@dataclass
class Primitive:
    material: str
    vertices: list[tuple[float, float, float, float, float, float, float, float]] = field(
        default_factory=list
    )
    indices: list[int] = field(default_factory=list)
    vertex_map: dict[tuple[int, int, int], int] = field(default_factory=dict)


def parse_mtl(path: Path, source_root: Path) -> OrderedDict[str, Material]:
    materials: OrderedDict[str, Material] = OrderedDict()
    current: Material | None = None
    for line_number, raw in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        command = fields[0]
        values = fields[1:]
        if command == "newmtl":
            if len(values) != 1 or values[0] in materials:
                fail(f"invalid or duplicate material at {path}:{line_number}")
            current = Material(values[0])
            materials[current.name] = current
        elif current is None:
            fail(f"material property before newmtl at {path}:{line_number}")
        elif command == "Kd" and len(values) >= 3:
            current.base_color = tuple(float(value) for value in values[:3])
        elif command == "Ns" and values:
            current.shininess = max(0.0, float(values[0]))
        elif command == "d" and values:
            current.opacity = min(1.0, max(0.0, float(values[0])))
        elif command == "Tr" and values:
            current.opacity = min(1.0, max(0.0, 1.0 - float(values[0])))
        elif command == "map_Kd" and values:
            texture = " ".join(values)
            resolve_source_file(source_root, texture)
            current.base_color_texture = texture.replace("\\", "/")
        elif command == "map_d" and values:
            texture = " ".join(values)
            resolve_source_file(source_root, texture)
            current.alpha_texture = texture.replace("\\", "/")
    if not materials:
        fail("MTL contains no materials")
    return materials


def obj_index(text: str, count: int, label: str, line_number: int) -> int:
    if not text:
        return -1
    value = int(text)
    index = value - 1 if value > 0 else count + value
    if index < 0 or index >= count:
        fail(f"OBJ {label} index out of range at line {line_number}")
    return index


def parse_obj(path: Path, materials: OrderedDict[str, Material]) -> OrderedDict[str, Primitive]:
    positions: list[tuple[float, float, float]] = []
    texcoords: list[tuple[float, float]] = []
    normals: list[tuple[float, float, float]] = []
    primitives: OrderedDict[str, Primitive] = OrderedDict(
        (name, Primitive(name)) for name in materials
    )
    active: Primitive | None = None

    with path.open("r", encoding="utf-8-sig") as stream:
        for line_number, raw in enumerate(stream, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            command = fields[0]
            values = fields[1:]
            if command == "v" and len(values) >= 3:
                positions.append(tuple(float(value) for value in values[:3]))
            elif command == "vt" and len(values) >= 2:
                texcoords.append((float(values[0]), 1.0 - float(values[1])))
            elif command == "vn" and len(values) >= 3:
                normals.append(tuple(float(value) for value in values[:3]))
            elif command == "usemtl":
                if len(values) != 1 or values[0] not in primitives:
                    fail(f"unknown OBJ material at line {line_number}")
                active = primitives[values[0]]
            elif command == "f":
                if active is None or len(values) < 3:
                    fail(f"face without material or with fewer than 3 vertices at line {line_number}")
                face_indices: list[int] = []
                for value in values:
                    parts = value.split("/")
                    position_index = obj_index(parts[0], len(positions), "position", line_number)
                    texcoord_index = obj_index(parts[1], len(texcoords), "texcoord", line_number) if len(parts) > 1 else -1
                    normal_index = obj_index(parts[2], len(normals), "normal", line_number) if len(parts) > 2 else -1
                    key = (position_index, texcoord_index, normal_index)
                    vertex_index = active.vertex_map.get(key)
                    if vertex_index is None:
                        position = positions[position_index]
                        texcoord = texcoords[texcoord_index] if texcoord_index >= 0 else (0.0, 0.0)
                        normal = normals[normal_index] if normal_index >= 0 else (0.0, 1.0, 0.0)
                        vertex_index = len(active.vertices)
                        active.vertex_map[key] = vertex_index
                        active.vertices.append((*position, *normal, *texcoord))
                    face_indices.append(vertex_index)
                for corner in range(1, len(face_indices) - 1):
                    active.indices.extend((face_indices[0], face_indices[corner], face_indices[corner + 1]))

    primitives = OrderedDict(
        (name, primitive) for name, primitive in primitives.items() if primitive.indices
    )
    if not primitives:
        fail("OBJ contains no drawable primitives")
    return primitives


def align4(blob: bytearray) -> None:
    while len(blob) % 4:
        blob.append(0)


def append_blob(blob: bytearray, payload: bytes, target: int) -> tuple[int, int]:
    align4(blob)
    offset = len(blob)
    blob.extend(payload)
    return offset, len(payload)


def convert(source_root: Path, output_root: Path) -> None:
    source_root = source_root.resolve(strict=True)
    obj_path = resolve_source_file(source_root, "sponza.obj")
    mtl_path = resolve_source_file(source_root, "sponza.mtl")
    materials = parse_mtl(mtl_path, source_root)
    primitives = parse_obj(obj_path, materials)

    if output_root.exists():
        fail(f"refusing to overwrite output directory: {output_root}")
    output_root.mkdir(parents=True)
    texture_output = output_root / "textures"
    texture_output.mkdir()

    used_textures = sorted(
        {material.base_color_texture for material in materials.values() if material.base_color_texture}
    )
    images = []
    textures = []
    texture_indices: dict[str, int] = {}
    for relative in used_textures:
        assert relative is not None
        source = resolve_source_file(source_root, relative)
        destination = texture_output / source.name
        if destination.exists():
            fail(f"duplicate texture basename: {source.name}")
        shutil.copyfile(source, destination)
        texture_indices[relative] = len(textures)
        images.append({"uri": f"textures/{source.name}"})
        textures.append({"source": len(images) - 1})

    gltf_materials = []
    material_indices: dict[str, int] = {}
    for name, material in materials.items():
        roughness = math.sqrt(2.0 / (material.shininess + 2.0))
        pbr = {
            "baseColorFactor": [*material.base_color, material.opacity],
            "metallicFactor": 0.0,
            "roughnessFactor": min(1.0, max(0.04, roughness)),
        }
        if material.base_color_texture:
            pbr["baseColorTexture"] = {"index": texture_indices[material.base_color_texture]}
        entry = {"name": name, "pbrMetallicRoughness": pbr, "doubleSided": True}
        if material.alpha_texture or material.opacity < 1.0:
            entry["alphaMode"] = "MASK"
            entry["alphaCutoff"] = 0.5
        material_indices[name] = len(gltf_materials)
        gltf_materials.append(entry)

    blob = bytearray()
    buffer_views = []
    accessors = []
    gltf_primitives = []
    for name, primitive in primitives.items():
        positions = b"".join(struct.pack("<3f", *vertex[:3]) for vertex in primitive.vertices)
        normals = b"".join(struct.pack("<3f", *vertex[3:6]) for vertex in primitive.vertices)
        texcoords = b"".join(struct.pack("<2f", *vertex[6:8]) for vertex in primitive.vertices)
        indices = b"".join(struct.pack("<I", index) for index in primitive.indices)

        min_position = [min(vertex[axis] for vertex in primitive.vertices) for axis in range(3)]
        max_position = [max(vertex[axis] for vertex in primitive.vertices) for axis in range(3)]
        attributes = {}
        for semantic, payload, stride, accessor_type, component_type in (
            ("POSITION", positions, 12, "VEC3", 5126),
            ("NORMAL", normals, 12, "VEC3", 5126),
            ("TEXCOORD_0", texcoords, 8, "VEC2", 5126),
        ):
            offset, length = append_blob(blob, payload, 34962)
            buffer_views.append({"buffer": 0, "byteOffset": offset, "byteLength": length, "byteStride": stride, "target": 34962})
            accessor = {"bufferView": len(buffer_views) - 1, "componentType": component_type, "count": len(primitive.vertices), "type": accessor_type}
            if semantic == "POSITION":
                accessor["min"] = min_position
                accessor["max"] = max_position
            accessors.append(accessor)
            attributes[semantic] = len(accessors) - 1

        offset, length = append_blob(blob, indices, 34963)
        buffer_views.append({"buffer": 0, "byteOffset": offset, "byteLength": length, "target": 34963})
        accessors.append({"bufferView": len(buffer_views) - 1, "componentType": 5125, "count": len(primitive.indices), "type": "SCALAR"})
        gltf_primitives.append({"attributes": attributes, "indices": len(accessors) - 1, "material": material_indices[name], "mode": 4})

    binary_name = "scene.bin"
    (output_root / binary_name).write_bytes(blob)
    document = {
        "asset": {"version": "2.0", "generator": f"{TOOL_NAME}/{TOOL_VERSION}"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Crytek Sponza", "mesh": 0, "scale": [0.01, 0.01, 0.01]}],
        "meshes": [{"name": "Crytek Sponza", "primitives": gltf_primitives}],
        "materials": gltf_materials,
        "textures": textures,
        "images": images,
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "buffers": [{"uri": binary_name, "byteLength": len(blob)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
        "extras": {
            "sourceArchiveSha256": "da005cbee0be2df2abc8513f3ceb61bcb6f69aac112babcd9c00169a27c2770c",
            "sourceVersion": "2016-06-28",
            "recipeSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        },
    }
    (output_root / "scene.gltf").write_text(
        json.dumps(document, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    arguments = parser.parse_args()
    convert(arguments.source_root, arguments.output_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
