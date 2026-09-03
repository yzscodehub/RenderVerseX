#!/usr/bin/env python3
"""Generate the hermetic glTF used by the PBR materials sample."""

from __future__ import annotations

import argparse
import base64
import json
import math
import struct
import zlib
from pathlib import Path


# Keep factor-driven and RG8 texture-driven fixtures numerically identical.
# The report intentionally presents the conventional rounded labels.
METALLIC_LEVELS = (0.0, 64.0 / 255.0, 128.0 / 255.0, 191.0 / 255.0, 1.0)
ROUGHNESS_LEVELS = (13.0 / 255.0, 64.0 / 255.0, 128.0 / 255.0, 191.0 / 255.0, 1.0)
SPHERE_RADIUS = 0.55
GRID_SPACING = 1.45
SEGMENT_COUNT = 16
RING_COUNT = 12
BASE_COLOR_SLICES = (
    ("terracotta", (0.72, 0.18, 0.06, 1.0)),
    ("gold", (0.72, 0.48, 0.06, 1.0)),
    ("green", (0.08, 0.45, 0.12, 1.0)),
    ("blue", (0.06, 0.18, 0.72, 1.0)),
    ("neutral", (0.50, 0.50, 0.50, 1.0)),
)
FACTOR_REQUIRED_EXTENSIONS = (
    "KHR_materials_specular",
    "KHR_materials_clearcoat",
    "KHR_materials_ior",
    "KHR_materials_emissive_strength",
)
EMISSIVE_FACTOR = (0.08, 0.03, 0.01)


def factor_layer_extensions(layer: int) -> dict[str, object]:
    """Return the fixed Material V2 extension coverage for one color layer."""
    if layer == 0:
        return {}
    if layer == 1:
        return {
            "KHR_materials_specular": {
                "specularFactor": 0.65,
                "specularColorFactor": [0.85, 0.95, 1.0],
            }
        }
    if layer == 2:
        return {
            "KHR_materials_clearcoat": {
                "clearcoatFactor": 0.8,
                "clearcoatRoughnessFactor": 0.2,
            }
        }
    if layer == 3:
        return {"KHR_materials_ior": {"ior": 1.45}}
    if layer == 4:
        return {
            "KHR_materials_emissive_strength": {
                "emissiveStrength": 3.0,
            }
        }
    raise ValueError(f"unexpected PBR factor layer: {layer}")


def build_sphere() -> tuple[list[float], list[float], list[float], list[int]]:
    positions: list[float] = []
    normals: list[float] = []
    uvs: list[float] = []
    indices: list[int] = []

    for ring in range(RING_COUNT + 1):
        phi = math.pi * ring / RING_COUNT
        y = math.cos(phi)
        radial = math.sin(phi)
        for segment in range(SEGMENT_COUNT + 1):
            theta = math.tau * segment / SEGMENT_COUNT
            x = radial * math.cos(theta)
            z = radial * math.sin(theta)
            positions.extend((x * SPHERE_RADIUS, y * SPHERE_RADIUS, z * SPHERE_RADIUS))
            normals.extend((x, y, z))
            uvs.extend(
                (
                    segment / SEGMENT_COUNT,
                    ring / RING_COUNT,
                )
            )

    row_width = SEGMENT_COUNT + 1
    for ring in range(RING_COUNT):
        for segment in range(SEGMENT_COUNT):
            top_left = ring * row_width + segment
            top_right = top_left + 1
            bottom_left = top_left + row_width
            bottom_right = bottom_left + 1
            indices.extend((top_left, bottom_left, top_right))
            indices.extend((top_right, bottom_left, bottom_right))

    return positions, normals, uvs, indices


def pack_floats(values: list[float]) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


def pack_uint16(values: list[int]) -> bytes:
    return struct.pack(f"<{len(values)}H", *values)


def float32(value: float) -> float:
    """Round one value exactly as the glTF loader/GPU float path does."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def build_metallic_roughness_png() -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", checksum)
        )

    pixels = bytearray()
    for roughness in ROUGHNESS_LEVELS:
        pixels.append(0)
        for metallic in METALLIC_LEVELS:
            pixels.extend(
                (
                    255,
                    round(roughness * 255.0),
                    round(metallic * 255.0),
                    255,
                )
            )

    header = struct.pack(
        ">IIBBBBB",
        len(METALLIC_LEVELS),
        len(ROUGHNESS_LEVELS),
        8,
        6,
        0,
        0,
        0,
    )
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(pixels), level=9))
        + chunk(b"IEND", b"")
    )


def build_document(workflow: str) -> dict[str, object]:
    sphere_positions, sphere_normals, sphere_uvs, sphere_indices = build_sphere()
    texture_positions: list[float] = []
    texture_normals: list[float] = []
    texture_uvs: list[float] = []
    texture_indices: list[int] = []

    for row, roughness in enumerate(ROUGHNESS_LEVELS):
        for column, metallic in enumerate(METALLIC_LEVELS):
            translation = (
                (column - 2) * GRID_SPACING,
                (2 - row) * GRID_SPACING,
                0.0,
            )
            base_vertex = len(texture_positions) // 3
            for vertex in range(0, len(sphere_positions), 3):
                # The factor fixture adds float32 node translations in the
                # vertex shader. Reproduce that same float32 addition when
                # baking the texture fixture's combined grid so workflow
                # parity compares material semantics, not two subtly
                # different edge rasterizations.
                texture_positions.extend(
                    (
                        float32(
                            float32(sphere_positions[vertex])
                            + float32(translation[0])
                        ),
                        float32(
                            float32(sphere_positions[vertex + 1])
                            + float32(translation[1])
                        ),
                        float32(
                            float32(sphere_positions[vertex + 2])
                            + float32(translation[2])
                        ),
                    )
                )
                texture_uvs.extend(
                    (
                        (column + 0.5) / len(METALLIC_LEVELS),
                        (row + 0.5) / len(ROUGHNESS_LEVELS),
                    )
                )
            texture_normals.extend(sphere_normals)
            texture_indices.extend(
                base_vertex + value for value in sphere_indices
            )

    sphere_position_bytes = pack_floats(sphere_positions)
    sphere_normal_bytes = pack_floats(sphere_normals)
    sphere_uv_bytes = pack_floats(sphere_uvs)
    sphere_index_bytes = pack_uint16(sphere_indices)
    texture_position_bytes = pack_floats(texture_positions)
    texture_normal_bytes = pack_floats(texture_normals)
    texture_uv_bytes = pack_floats(texture_uvs)
    texture_index_bytes = pack_uint16(texture_indices)
    binary_parts = (
        sphere_position_bytes,
        sphere_normal_bytes,
        sphere_uv_bytes,
        sphere_index_bytes,
        texture_position_bytes,
        texture_normal_bytes,
        texture_uv_bytes,
        texture_index_bytes,
    )
    binary = b"".join(binary_parts)

    buffer_views: list[dict[str, object]] = []
    byte_offset = 0
    for data, target in zip(
        binary_parts,
        (34962, 34962, 34962, 34963, 34962, 34962, 34962, 34963),
    ):
        buffer_views.append(
            {
                "buffer": 0,
                "byteOffset": byte_offset,
                "byteLength": len(data),
                "target": target,
            }
        )
        byte_offset += len(data)

    encoded = base64.b64encode(binary).decode("ascii")
    texture_png = base64.b64encode(build_metallic_roughness_png()).decode(
        "ascii"
    )
    grid_extent = 2 * GRID_SPACING + SPHERE_RADIUS
    materials: list[dict[str, object]] = []
    meshes: list[dict[str, object]] = []
    nodes: list[dict[str, object]] = []
    if workflow == "factor":
        for layer, (color_name, base_color) in enumerate(BASE_COLOR_SLICES):
            for row, roughness in enumerate(ROUGHNESS_LEVELS):
                for column, metallic in enumerate(METALLIC_LEVELS):
                    material_index = len(materials)
                    material_name = (
                        f"PBRFactor_C{layer}_{color_name}_"
                        f"M{metallic:.2f}_R{roughness:.2f}"
                    )
                    material = {
                        "name": material_name,
                        "pbrMetallicRoughness": {
                            "baseColorFactor": list(base_color),
                            "metallicFactor": metallic,
                            "roughnessFactor": roughness,
                        },
                        "alphaMode": "OPAQUE",
                    }
                    extensions = factor_layer_extensions(layer)
                    if extensions:
                        material["extensions"] = extensions
                    if layer == 4:
                        material["emissiveFactor"] = list(EMISSIVE_FACTOR)
                    materials.append(material)
                    nodes.append(
                        {
                            "name": material_name,
                            "mesh": 0,
                            "translation": [
                                (column - 2) * GRID_SPACING,
                                (2 - row) * GRID_SPACING,
                                (2 - layer) * GRID_SPACING,
                            ],
                        }
                    )
        meshes.append(
            {
                "name": "PBRFactorSharedSphereMesh",
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": 0,
                            "NORMAL": 1,
                            "TEXCOORD_0": 2,
                        },
                        "indices": 3,
                        # glTF materials are primitive-owned. The sample
                        # applies each node's same-named material as a scene
                        # component override after formal instantiation.
                        "material": 0,
                        "mode": 4,
                    }
                ],
            }
        )
    else:
        for layer, (color_name, base_color) in enumerate(BASE_COLOR_SLICES):
            material_index = len(materials)
            material_name = f"PBRTexture_C{layer}_{color_name}"
            materials.append(
                {
                    "name": material_name,
                    "pbrMetallicRoughness": {
                        "baseColorFactor": list(base_color),
                        "metallicFactor": 1.0,
                        "roughnessFactor": 1.0,
                        "metallicRoughnessTexture": {"index": 0},
                    },
                    "alphaMode": "OPAQUE",
                }
            )
            meshes.append(
                {
                    "name": material_name + "Mesh",
                    "primitives": [
                        {
                            "attributes": {
                                "POSITION": 4,
                                "NORMAL": 5,
                                "TEXCOORD_0": 6,
                            },
                            "indices": 7,
                            "material": material_index,
                            "mode": 4,
                        }
                    ],
                }
            )
            nodes.append(
                {
                    "name": material_name,
                    "mesh": layer,
                    "translation": [0.0, 0.0, (2 - layer) * GRID_SPACING],
                }
            )

    texture_resources = workflow == "texture"
    document = {
        "asset": {
            "version": "2.0",
            "generator":
                f"RenderVerseX PBR 5x5x5 {workflow} cube generator",
        },
        "scene": 0,
        "scenes": [
            {
                "name": "PBRMaterialMatrixScene",
                "nodes": list(range(len(nodes))),
            }
        ],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "textures": ([{"sampler": 0, "source": 0}]
                     if texture_resources else []),
        "samplers": ([
            {
                "magFilter": 9728,
                "minFilter": 9728,
                "wrapS": 33071,
                "wrapT": 33071,
            }
        ] if texture_resources else []),
        "images": ([
            {
                "name": "PBRMetallicRoughnessMatrix",
                "mimeType": "image/png",
                "uri": "data:image/png;base64," + texture_png,
            }
        ] if texture_resources else []),
        "buffers": [
            {
                "byteLength": len(binary),
                "uri": "data:application/octet-stream;base64," + encoded,
            }
        ],
        "bufferViews": buffer_views,
        "accessors": [
            {
                "bufferView": 0,
                "byteOffset": 0,
                "componentType": 5126,
                "count": len(sphere_positions) // 3,
                "type": "VEC3",
                "min": [-SPHERE_RADIUS, -SPHERE_RADIUS, -SPHERE_RADIUS],
                "max": [SPHERE_RADIUS, SPHERE_RADIUS, SPHERE_RADIUS],
            },
            {
                "bufferView": 1,
                "byteOffset": 0,
                "componentType": 5126,
                "count": len(sphere_normals) // 3,
                "type": "VEC3",
            },
            {
                "bufferView": 2,
                "byteOffset": 0,
                "componentType": 5126,
                "count": len(sphere_uvs) // 2,
                "type": "VEC2",
                "min": [0.0, 0.0],
                "max": [1.0, 1.0],
            },
            {
                "bufferView": 3,
                "byteOffset": 0,
                "componentType": 5123,
                "count": len(sphere_indices),
                "type": "SCALAR",
                "min": [min(sphere_indices)],
                "max": [max(sphere_indices)],
            },
            {
                "bufferView": 4,
                "byteOffset": 0,
                "componentType": 5126,
                "count": len(texture_positions) // 3,
                "type": "VEC3",
                "min": [-grid_extent, -grid_extent, -SPHERE_RADIUS],
                "max": [grid_extent, grid_extent, SPHERE_RADIUS],
            },
            {
                "bufferView": 5,
                "byteOffset": 0,
                "componentType": 5126,
                "count": len(texture_normals) // 3,
                "type": "VEC3",
            },
            {
                "bufferView": 6,
                "byteOffset": 0,
                "componentType": 5126,
                "count": len(texture_uvs) // 2,
                "type": "VEC2",
                "min": [0.1, 0.1],
                "max": [0.9, 0.9],
            },
            {
                "bufferView": 7,
                "byteOffset": 0,
                "componentType": 5123,
                "count": len(texture_indices),
                "type": "SCALAR",
                "min": [min(texture_indices)],
                "max": [max(texture_indices)],
            },
        ],
    }
    if workflow == "factor":
        document["extensionsUsed"] = list(FACTOR_REQUIRED_EXTENSIONS)
        document["extensionsRequired"] = list(FACTOR_REQUIRED_EXTENSIONS)
        validate_factor_document(document)
    return document


def validate_factor_document(document: dict[str, object]) -> None:
    """Fail closed if the deterministic factor matrix drifts from its contract."""
    materials = document.get("materials")
    nodes = document.get("nodes")
    meshes = document.get("meshes")
    if not isinstance(materials, list) or len(materials) != 125:
        raise ValueError("factor fixture must contain exactly 125 materials")
    if not isinstance(nodes, list) or len(nodes) != 125:
        raise ValueError("factor fixture must contain exactly 125 nodes")
    if not isinstance(meshes, list) or len(meshes) != 1:
        raise ValueError("factor fixture must keep its single shared mesh")
    expected_extensions = list(FACTOR_REQUIRED_EXTENSIONS)
    if document.get("extensionsUsed") != expected_extensions:
        raise ValueError("factor fixture extensionsUsed must be the exact V2 set")
    if document.get("extensionsRequired") != expected_extensions:
        raise ValueError("factor fixture extensionsRequired must be the exact V2 set")

    for layer, (color_name, base_color) in enumerate(BASE_COLOR_SLICES):
        layer_start = layer * len(METALLIC_LEVELS) * len(ROUGHNESS_LEVELS)
        layer_materials = materials[layer_start:layer_start + 25]
        expected_layer_extensions = factor_layer_extensions(layer)
        if len(layer_materials) != 25:
            raise ValueError(f"factor fixture layer {layer} must contain 25 materials")
        for row, roughness in enumerate(ROUGHNESS_LEVELS):
            for column, metallic in enumerate(METALLIC_LEVELS):
                local_index = row * len(METALLIC_LEVELS) + column
                material = layer_materials[local_index]
                node = nodes[layer_start + local_index]
                expected_name = (
                    f"PBRFactor_C{layer}_{color_name}_"
                    f"M{metallic:.2f}_R{roughness:.2f}"
                )
                if material.get("name") != expected_name or node.get("name") != expected_name:
                    raise ValueError("factor fixture material or node naming drifted")
                if material.get("alphaMode") != "OPAQUE":
                    raise ValueError("factor fixture must remain opaque")
                if material.get("pbrMetallicRoughness") != {
                    "baseColorFactor": list(base_color),
                    "metallicFactor": metallic,
                    "roughnessFactor": roughness,
                }:
                    raise ValueError("factor fixture PBR matrix drifted")
                if material.get("extensions", {}) != expected_layer_extensions:
                    raise ValueError("factor fixture extension layer drifted")
                if layer == 4:
                    if material.get("emissiveFactor") != list(EMISSIVE_FACTOR):
                        raise ValueError("emissive-strength layer must have a non-zero factor")
                elif "emissiveFactor" in material:
                    raise ValueError("only the emissive-strength layer may set emissiveFactor")
                if any(
                    "Texture" in key
                    for extension in material.get("extensions", {}).values()
                    for key in extension
                ):
                    raise ValueError("factor fixture must not declare extension textures")
    if "KHR_materials_transmission" in json.dumps(document, sort_keys=True):
        raise ValueError("factor fixture must not declare transmission")


def serialize_document(workflow: str) -> bytes:
    return (
        json.dumps(build_document(workflow), indent=2) + "\n"
    ).encode("utf-8")


def run_self_test() -> None:
    first = serialize_document("factor")
    second = serialize_document("factor")
    if first != second:
        raise RuntimeError("factor fixture generation is not byte-identical")
    document = json.loads(first)
    validate_factor_document(document)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument(
        "--workflow",
        choices=("factor", "texture"),
        default="factor",
    )
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()

    if arguments.self_test:
        run_self_test()
        print("PBR material grid generator self-test passed")
        return 0
    if arguments.output is None:
        parser.error("output is required unless --self-test is specified")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_bytes(serialize_document(arguments.workflow))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
