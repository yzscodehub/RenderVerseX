#!/usr/bin/env python3
"""Generate the RenderVerseSamples schema-v3 catalog from pinned local assets."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "Samples" / "RenderVerseSamples" / "Assets"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_record(relative: str, purpose: str) -> dict:
    posix = PurePosixPath(relative)
    if posix.is_absolute() or any(part in ("", ".", "..") for part in posix.parts):
        raise RuntimeError(f"unsafe catalog path: {relative}")
    path = ASSETS.joinpath(*posix.parts).resolve(strict=True)
    path.relative_to(ASSETS.resolve(strict=True))
    if not path.is_file() or path.is_symlink():
        raise RuntimeError(f"catalog dependency is not a regular file: {relative}")
    return {
        "path": posix.as_posix(),
        "purpose": purpose,
        "byteCount": path.stat().st_size,
        "sha256": sha256(path),
    }


def asset_content_id(files: list[dict]) -> dict:
    ordered = sorted(files, key=lambda item: item["path"])
    if files != ordered:
        raise RuntimeError("file records must be supplied in POSIX path order")
    payload = "".join(
        f'{item["sha256"]} {item["byteCount"]} {item["path"]}\n' for item in files
    ).encode("utf-8")
    return {
        "schemaVersion": 1,
        "algorithm": "sha256",
        "digest": hashlib.sha256(payload).hexdigest(),
        "byteCount": sum(item["byteCount"] for item in files),
        "fileCount": len(files),
    }


def self_identity(record: dict) -> dict:
    return {
        "schemaVersion": 1,
        "domain": "source",
        "scope": "self-contained-artifact",
        "algorithm": "sha256",
        "digest": record["sha256"],
        "byteCount": record["byteCount"],
        "fileCount": 1,
    }


def dependency_identity(records: list[dict]) -> dict:
    root = next(item for item in records if item["purpose"] == "runtime-root")
    root_parent = PurePosixPath(root["path"]).parent
    ordered: list[tuple[str, dict]] = []
    for item in records:
        uri = "" if item is root else PurePosixPath(item["path"]).relative_to(root_parent).as_posix()
        ordered.append((uri, item))
    ordered.sort(key=lambda pair: (0 if pair[0] == "" else 1, pair[0]))
    digest = hashlib.sha256()
    digest.update(b"RVX.ResourceContentIdentity.DependencyClosure.v1\0")
    for uri, item in ordered:
        digest.update(uri.encode("utf-8"))
        digest.update(b"\0")
        digest.update(int(item["byteCount"]).to_bytes(8, "big"))
        digest.update(bytes.fromhex(item["sha256"]))
    return {
        "schemaVersion": 1,
        "domain": "source",
        "scope": "dependency-closure",
        "algorithm": "sha256",
        "digest": digest.hexdigest(),
        "byteCount": sum(item["byteCount"] for item in records),
        "fileCount": len(records),
    }


def entry(
    *,
    asset_id: str,
    kind: str,
    path: str,
    license_spdx: str,
    license_file: str,
    source_name: str,
    source_uri: str,
    author: str,
    version: str,
    attribution: str,
    modification: str,
    files: list[dict],
    content_identity: dict,
    archive_sha256: str = "",
    derived_from: list[str] | None = None,
    generation: dict | None = None,
    cook: dict | None = None,
) -> dict:
    value = {
        "id": asset_id,
        "kind": kind,
        "path": path,
        "license": {
            "spdxId": license_spdx,
            "file": license_file,
            "coverage": "complete-declared-package",
        },
        "source": {
            "name": source_name,
            "uri": source_uri,
            "author": author,
            "version": version,
        },
        "contentIdentity": content_identity,
        "assetContentId": asset_content_id(files),
        "files": files,
        "attribution": attribution,
        "modificationNotice": modification,
        "redistributable": True,
    }
    if archive_sha256:
        value["source"]["archiveSha256"] = archive_sha256
    if derived_from is not None or generation is not None:
        value["derivedFrom"] = derived_from
        value["generation"] = generation
    if cook is not None:
        value["cook"] = cook
    return value


def content_identity(
    domain: str,
    scope: str,
    digest: str,
    byte_count: int,
    file_count: int,
) -> dict:
    return {
        "schemaVersion": 1,
        "domain": domain,
        "scope": scope,
        "algorithm": "sha256",
        "digest": digest,
        "byteCount": byte_count,
        "fileCount": file_count,
    }


def generated_fixture(
    asset_id: str, path: str, source_name: str, source_uri: str
) -> dict:
    records = sorted(
        [file_record(path, "runtime-root")], key=lambda item: item["path"]
    )
    return entry(
        asset_id=asset_id,
        kind="model",
        path=path,
        license_spdx="LicenseRef-RenderVerseX-Generated-Fixture",
        license_file="LICENSE.generated-fixtures.txt",
        source_name=source_name,
        source_uri=source_uri,
        author="RenderVerseX contributors",
        version="1",
        attribution="Generated test geometry; no third-party source asset.",
        modification="Generated deterministically from repository source; no upstream bytes modified.",
        files=records,
        content_identity=self_identity(records[0]),
    )


def main() -> int:
    assets: list[dict] = []
    fixture_sources = {
        "models/r7-triangle/scene.gltf": ROOT / "Tests" / "Fixtures" / "ModelViewer" / "R7Triangle.gltf",
        "models/shadow-plane-caster/scene.gltf": ROOT / "Tests" / "Fixtures" / "ModelViewer" / "ShadowPlaneCaster.gltf",
        "models/pbr-material-grid/scene.gltf": ROOT / "Tests" / "Fixtures" / "Samples" / "PBRMaterialGrid.gltf",
        "models/pbr-material-texture-cube/scene.gltf": ROOT / "Tests" / "Fixtures" / "Samples" / "PBRMaterialTextureCube.gltf",
    }
    for relative, source in fixture_sources.items():
        destination = ASSETS.joinpath(*PurePosixPath(relative).parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            destination.write_bytes(source.read_bytes())
    fixture_tools = ROOT / "build" / "win_x64_debug" / "Samples" / "Common" / "Debug"
    generated_fixture_commands = (
        (fixture_tools / "RVXSampleInteriorFixtureWriter.exe", ASSETS / "models" / "interior-rendering-p0a" / "scene.gltf"),
        (fixture_tools / "RVXSampleHDRFixtureWriter.exe", ASSETS / "environments" / "pbr-reference" / "environment.hdr"),
    )
    for tool, output in generated_fixture_commands:
        if not output.exists():
            if not tool.is_file():
                raise RuntimeError(f"required fixture writer is not built: {tool}")
            output.parent.mkdir(parents=True, exist_ok=True)
            import subprocess
            subprocess.run([str(tool), "--output", str(output)], check=True)
    assets.append(generated_fixture("r7-triangle", "models/r7-triangle/scene.gltf", "RenderVerseX R7 visual gate fixture", "repo://Tests/Fixtures/ModelViewer/R7Triangle.gltf"))
    assets.append(generated_fixture("shadow-plane-caster", "models/shadow-plane-caster/scene.gltf", "RenderVerseX shadow visual gate fixture", "repo://Tests/Fixtures/ModelViewer/ShadowPlaneCaster.gltf"))
    assets.append(generated_fixture("pbr-material-grid", "models/pbr-material-grid/scene.gltf", "RenderVerseX PBR 5x5x5 factor cube", "repo://Tests/Fixtures/Samples/PBRMaterialGrid.gltf"))
    assets.append(generated_fixture("pbr-material-texture-cube", "models/pbr-material-texture-cube/scene.gltf", "RenderVerseX PBR 5x5x5 texture cube", "repo://Tests/Fixtures/Samples/PBRMaterialTextureCube.gltf"))
    assets.append(generated_fixture("interior-rendering-p0a", "models/interior-rendering-p0a/scene.gltf", "RenderVerseX deterministic interior fixture", "repo://Samples/Common/Tools/SampleInteriorFixtureWriter.cpp"))

    env_record = file_record("environments/pbr-reference/environment.hdr", "runtime-root")
    assets.append(entry(asset_id="pbr-reference-environment", kind="environment", path=env_record["path"], license_spdx="LicenseRef-RenderVerseX-Generated-Fixture", license_file="LICENSE.generated-fixtures.txt", source_name="RenderVerseX hermetic PBR reference environment", source_uri="repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp", author="RenderVerseX contributors", version="1", attribution="Deterministically generated HDR fixture; no third-party source asset.", modification="Generated deterministically from repository source.", files=[env_record], content_identity=self_identity(env_record)))

    for asset_id, relative, name, expected_digest in (
        ("water-bottle", "models/water-bottle/WaterBottle.glb", "Khronos Water Bottle", "b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b"),
        ("corset", "models/corset/Corset.glb", "Khronos Corset", "9582c0dc0dee813be77f60e6ddf7213987c7e11497bf3cc66fd7b18957ae0d26"),
    ):
        record = file_record(relative, "runtime-root")
        if record["sha256"] != expected_digest:
            raise RuntimeError(f"pinned upstream digest mismatch for {asset_id}")
        assets.append(entry(asset_id=asset_id, kind="model", path=relative, license_spdx="CC0-1.0", license_file="LICENSE.khronos-sample-assets-cc0.txt", source_name=name, source_uri=f"https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/2bac6f8c57bf471df0d2a1e8a8ec023c7801dddf/Models/{'WaterBottle' if asset_id == 'water-bottle' else 'Corset'}/glTF-Binary/{Path(relative).name}", author="Microsoft / Khronos glTF Sample Assets contributors", version="2bac6f8c57bf471df0d2a1e8a8ec023c7801dddf", attribution=f"{name}, CC0 1.0 Universal.", modification="Vendored GLB bytes are unmodified.", files=[record], content_identity=self_identity(record)))

    casual_female_relative = "models/casual-female/Casual_Female.gltf"
    casual_female_record = file_record(casual_female_relative, "runtime-root")
    casual_female_digest = "87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a"
    if casual_female_record["sha256"] != casual_female_digest:
        raise RuntimeError("pinned upstream digest mismatch for casual-female")
    casual_female_cooked_records = [
        file_record(
            "cooked/casual-female/CookManifest.rvxmanifest",
            "cook-manifest",
        ),
        file_record(
            "cooked/casual-female/models/casual-female/Casual_Female.rva",
            "cooked-artifact",
        ),
        file_record(
            "cooked/casual-female/models/casual-female/"
            "Casual_Female.rvdeps/animation.rvxanim",
            "cooked-artifact",
        ),
        *[
            file_record(
                "cooked/casual-female/models/casual-female/"
                f"Casual_Female.rvdeps/material_{index}.rvmat",
                "cooked-artifact",
            )
            for index in range(6)
        ],
        file_record(
            "cooked/casual-female/models/casual-female/"
            "Casual_Female.rvdeps/meshes.rva",
            "cooked-artifact",
        ),
    ]
    expected_casual_female_cooked_files = {
        "cooked/casual-female/CookManifest.rvxmanifest": (
            4_642,
            "ecbb1e6d425bdb269d93045cdb5fd7c09b87fed6da7bec688ff485408b5d5949",
        ),
        "cooked/casual-female/models/casual-female/Casual_Female.rva": (
            13_183,
            "a389f29eb345f80df8e8231ff36e4bb39a4c1a05fe54270735885a6827e2c416",
        ),
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/animation.rvxanim": (
            927_584,
            "36f4824d1d82aeaae42ce33208b3c774622e09d4547290c89ace8560eeb3ad11",
        ),
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/material_0.rvmat": (
            365,
            "d3c42cf7ce8629f7a1e6c7e4a977cb09c61f52fa06c5a726ef1341e3b14dc580",
        ),
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/material_1.rvmat": (
            365,
            "89d89a562576e9d1e5e44a4e8d5121bd808712d9fc5ef46d8cf30ad7ca2d859b",
        ),
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/material_2.rvmat": (
            365,
            "ac7106bad8face93755cfc2688fba28fbad597b8ebad697c9cd3b062b6b9596a",
        ),
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/material_3.rvmat": (
            365,
            "84c78973be6d63cc5902edfa7f019d3a62e0ba7a4b54d0ad5826ae187f170513",
        ),
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/material_4.rvmat": (
            365,
            "7fc705e485b0eaf09c03e1d5f340bd749ed1967957cb7fca654339119a785cc6",
        ),
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/material_5.rvmat": (
            365,
            "3a841e152a7e058e0c3e89073e5cf1f26a4ddb523dae69ec57d293df6a7c3f9b",
        ),
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/meshes.rva": (
            591_364,
            "2d637a7fff424f3b46d52cc40beb577e9345abda013a993ddba3738d950c61cd",
        ),
    }
    for record in casual_female_cooked_records:
        expected_size, expected_hash = expected_casual_female_cooked_files[
            record["path"]
        ]
        if record["byteCount"] != expected_size or record["sha256"] != expected_hash:
            raise RuntimeError(
                f"pinned cooked Casual Female artifact mismatch: {record['path']}"
            )
    casual_female_files = sorted(
        [casual_female_record, *casual_female_cooked_records],
        key=lambda item: item["path"],
    )
    casual_female_source_identity = self_identity(casual_female_record)
    casual_female_cook = {
        "manifestPath": "cooked/casual-female/CookManifest.rvxmanifest",
        "cookedRoot": "cooked/casual-female",
        "selector": {
            "sourcePath": casual_female_relative,
            "outputPath": "models/casual-female/Casual_Female.rva",
            "type": "Model",
        },
        "sourceContentIdentity": casual_female_source_identity,
        "cookedContentIdentity": content_identity(
            "cooked-artifact",
            "dependency-closure",
            "28267d61f327c52df64b087235ca1754da54e6ff66511e18506a6564a8cc1a6f",
            1_534_321,
            9,
        ),
        "manifestContentIdentity": content_identity(
            "cook-manifest",
            "self-contained-artifact",
            "ecbb1e6d425bdb269d93045cdb5fd7c09b87fed6da7bec688ff485408b5d5949",
            4_642,
            1,
        ),
        "cookSettingsHash": (
            "d47a3f3b166ac705cfd06128506f127d776f43ad59eaeaa5cb4d52c1946829fa"
        ),
        "recipeHash": (
            "28f13886f1ecc018d5daa7501d0e17d70a52769ae3623d10f2e6c98d8452f676"
        ),
        "tool": {"name": "RVXCook", "version": "2.0.0"},
    }
    casual_female_document = json.loads(
        ASSETS.joinpath(*PurePosixPath(casual_female_relative).parts).read_text(
            encoding="utf-8"
        )
    )
    skins = casual_female_document.get("skins", [])
    clip_names = {
        animation.get("name", "")
        for animation in casual_female_document.get("animations", [])
    }
    if len(skins) != 1 or len(skins[0].get("joints", [])) != 23:
        raise RuntimeError("casual-female must contain exactly one 23-joint skin")
    if not {"Idle", "Walk", "Run"}.issubset(clip_names):
        raise RuntimeError("casual-female is missing required Idle/Walk/Run clips")
    assets.append(
        entry(
            asset_id="casual-female",
            kind="model",
            path=casual_female_relative,
            license_spdx="CC0-1.0",
            license_file="LICENSE.quaternius-cc0.txt",
            source_name="Quaternius Casual Female",
            source_uri="https://drive.google.com/file/d/1E79ks2jbMt5iIrI8Ag9lRgA0VRnfU4pk/view",
            author="Quaternius",
            version="Ultimate Animated Character Pack, November 2019",
            attribution="Casual Female from the Quaternius Ultimate Animated Character Pack, CC0 1.0 Universal.",
            modification="Vendored Casual_Female.gltf bytes are unmodified; cooked artifacts retain the source identity.",
            files=casual_female_files,
            content_identity=casual_female_source_identity,
            cook=casual_female_cook,
        )
    )

    root_motion_artifact_relative = (
        "cooked/casual-female-root-motion/"
        "casual-female-walk-root-motion.rvxanim"
    )
    root_motion_manifest_relative = (
        "cooked/casual-female-root-motion/CookManifest.rvxmanifest"
    )
    root_motion_artifact_record = file_record(
        root_motion_artifact_relative, "runtime-root"
    )
    root_motion_manifest_record = file_record(
        root_motion_manifest_relative, "cook-manifest"
    )
    expected_root_motion_files = {
        root_motion_artifact_relative: (
            54_464,
            "1402b3b629c587ba1168ee961029e9b8504ffac89f763592571ae836f13f9a7f",
        ),
        root_motion_manifest_relative: (
            1_695,
            "bf369d106c2bf53a48c329f84ee7c977fa64c58d45c9c15d2426d9bf15546f2c",
        ),
    }
    root_motion_files = sorted(
        [root_motion_artifact_record, root_motion_manifest_record],
        key=lambda item: item["path"],
    )
    for record in root_motion_files:
        expected_size, expected_hash = expected_root_motion_files[record["path"]]
        if record["byteCount"] != expected_size or record["sha256"] != expected_hash:
            raise RuntimeError(
                f"pinned derived root-motion artifact mismatch: {record['path']}"
            )

    parent_animation_relative = (
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/animation.rvxanim"
    )
    parent_animation_identity = content_identity(
        "source",
        "self-contained-artifact",
        "36f4824d1d82aeaae42ce33208b3c774622e09d4547290c89ace8560eeb3ad11",
        927_584,
        1,
    )
    root_motion_identity = content_identity(
        "cooked-artifact",
        "self-contained-artifact",
        "1402b3b629c587ba1168ee961029e9b8504ffac89f763592571ae836f13f9a7f",
        54_464,
        1,
    )
    assets.append(
        entry(
            asset_id="casual-female-walk-root-motion",
            kind="animation",
            path=root_motion_artifact_relative,
            license_spdx="CC0-1.0",
            license_file="LICENSE.quaternius-cc0.txt",
            source_name="Casual Female Walk Root Motion (deterministic derivative)",
            source_uri="https://drive.google.com/file/d/1E79ks2jbMt5iIrI8Ag9lRgA0VRnfU4pk/view",
            author="Quaternius; deterministic derivative by RenderVerseX contributors",
            version="rvx-animation-root-motion-derive-v1",
            attribution="Derived from Quaternius Casual Female Walk, CC0 1.0 Universal.",
            modification="The Walk clip root track is deterministically replaced with +Z translation at 1.0 metre/second; all non-root animation data is retained.",
            files=root_motion_files,
            content_identity=root_motion_identity,
            derived_from=["casual-female"],
            generation={
                "recipe": "rvx-animation-root-motion-derive-v1",
                "toolVersion": "1.0.0",
                "recipeHash": "0e473d3e0511827cff7dd51e786c7d41f6bcbaadbe3f1dee4b0659900261ee7e",
            },
            cook={
                "manifestPath": root_motion_manifest_relative,
                "cookedRoot": "cooked/casual-female-root-motion",
                "selector": {
                    "sourcePath": parent_animation_relative,
                    "outputPath": "casual-female-walk-root-motion.rvxanim",
                    "type": "Animation",
                },
                "sourceContentIdentity": parent_animation_identity,
                "cookedContentIdentity": content_identity(
                    "cooked-artifact",
                    "self-contained-artifact",
                    "3a7e27e1745b292b282682df0ab6fee9c4b1b1ddb8d0b080d69dc723ab122a97",
                    54_464,
                    1,
                ),
                "manifestContentIdentity": content_identity(
                    "cook-manifest",
                    "self-contained-artifact",
                    "bf369d106c2bf53a48c329f84ee7c977fa64c58d45c9c15d2426d9bf15546f2c",
                    1_695,
                    1,
                ),
                "cookSettingsHash": "37d257abb1762b56436a0b3a1de267860f0507fbf0ff169ce395039ae2053232",
                "recipeHash": "5d814e26fdad96238fa707f11537e7ac66f9aa335252fafe525ef59338c478a7",
                "tool": {"name": "RVXCook", "version": "2.0.0"},
            },
        )
    )

    sponza_paths = sorted(
        path.relative_to(ASSETS).as_posix()
        for path in (ASSETS / "models" / "crytek-sponza").rglob("*")
        if path.is_file()
    )
    sponza_records = [
        file_record(path, "runtime-root" if path.endswith("scene.gltf") else "runtime-dependency")
        for path in sponza_paths
    ]
    recipe = ROOT / "Scripts" / "Assets" / "convert_crytek_sponza.py"
    recipe_hash = sha256(recipe)
    assets.append(entry(asset_id="crytek-sponza", kind="model", path="models/crytek-sponza/scene.gltf", license_spdx="CC-BY-3.0", license_file="LICENSE.crytek-sponza-cc-by-3.0.txt", source_name="Crytek Sponza via McGuire Computer Graphics Archive", source_uri="https://casual-effects.com/g3d/data10/common/model/crytek_sponza/sponza.zip", author="Frank Meinl, Crytek; archive maintenance by Morgan McGuire", version="2016-06-28", archive_sha256="da005cbee0be2df2abc8513f3ceb61bcb6f69aac112babcd9c00169a27c2770c", attribution="Crytek Sponza © 2010 Frank Meinl, Crytek, CC BY 3.0; archive and conversion attribution retained.", modification="Converted from the unmodified OBJ/MTL archive to deterministic glTF 2.0; see the license and source README.", files=sponza_records, content_identity=dependency_identity(sponza_records), derived_from=["crytek-sponza-archive-2016-06-28"], generation={"recipe":"Scripts/Assets/convert_crytek_sponza.py","toolVersion":"1.0.0","recipeHash":recipe_hash}))

    sky_record = file_record("environments/kloofendal-48d-partly-cloudy-pure-sky/kloofendal_48d_partly_cloudy_puresky_2k.hdr", "runtime-root")
    assets.append(entry(asset_id="kloofendal-48d-partly-cloudy-pure-sky", kind="environment", path=sky_record["path"], license_spdx="CC0-1.0", license_file="LICENSE.polyhaven-cc0.txt", source_name="Kloofendal 48d Partly Cloudy (Pure Sky) 2K HDR", source_uri="https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_48d_partly_cloudy_puresky_2k.hdr", author="Greg Zaal; pure-sky edit by Jarod Guest", version="polyhaven-2k-hdr-2026-08-12", attribution="Poly Haven Kloofendal 48d Partly Cloudy Pure Sky, CC0.", modification="Vendored HDR bytes are unmodified; IBL derivatives are produced by the Resource environment loader/cook contract.", files=[sky_record], content_identity=self_identity(sky_record)))

    assets.sort(key=lambda item: item["id"])
    document = {"schemaId":"RVX.SampleAssetCatalog","schemaVersion":3,"assets":assets}
    output = ASSETS / "catalog.json"
    output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
