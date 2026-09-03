#!/usr/bin/env python3
"""Generate and verify the optional external-development asset catalog."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path, PurePosixPath, PureWindowsPath


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
ASSET_ROOT = (REPOSITORY_ROOT / "assets").resolve(strict=True)
DEFAULT_OUTPUT = ASSET_ROOT / "catalog.json"


class CatalogError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def strict_relative_path(text: str) -> PurePosixPath:
    if not text or "\\" in text or ":" in text:
        raise CatalogError(f"catalog path must be canonical POSIX relative: {text!r}")
    posix = PurePosixPath(text)
    windows = PureWindowsPath(text)
    if (
        posix.is_absolute()
        or windows.is_absolute()
        or windows.drive
        or any(part in ("", ".", "..") for part in posix.parts)
        or posix.as_posix() != text
    ):
        raise CatalogError(f"unsafe catalog path: {text!r}")
    return posix


def resolve_file(text: str) -> Path:
    relative = strict_relative_path(text)
    candidate = ASSET_ROOT.joinpath(*relative.parts)
    if candidate.is_symlink():
        raise CatalogError(f"catalog file must not be a symlink: {text}")
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(ASSET_ROOT)
    except (FileNotFoundError, ValueError, OSError) as error:
        raise CatalogError(f"catalog file escapes or is missing: {text}") from error
    if not resolved.is_file() or resolved.is_symlink():
        raise CatalogError(f"catalog file must be a regular file: {text}")
    return resolved


def file_record(text: str, purpose: str) -> dict[str, object]:
    if not purpose:
        raise CatalogError("catalog file purpose must be non-empty")
    path = resolve_file(text)
    return {
        "path": strict_relative_path(text).as_posix(),
        "purpose": purpose,
        "byteCount": path.stat().st_size,
        "sha256": sha256(path),
    }


def sorted_records(records: list[dict[str, object]]) -> list[dict[str, object]]:
    result = sorted(records, key=lambda item: str(item["path"]))
    paths = [str(item["path"]) for item in result]
    if len(paths) != len(set(paths)):
        raise CatalogError("catalog package declares duplicate file paths")
    return result


def asset_content_id(records: list[dict[str, object]]) -> dict[str, object]:
    if records != sorted_records(records):
        raise CatalogError("catalog package records must be sorted by path")
    payload = "".join(
        f'{record["sha256"]} {record["byteCount"]} {record["path"]}\n'
        for record in records
    ).encode("utf-8")
    return {
        "schemaVersion": 1,
        "algorithm": "sha256",
        "digest": hashlib.sha256(payload).hexdigest(),
        "byteCount": sum(int(record["byteCount"]) for record in records),
        "fileCount": len(records),
    }


def self_identity(record: dict[str, object]) -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "domain": "source",
        "scope": "self-contained-artifact",
        "algorithm": "sha256",
        "digest": record["sha256"],
        "byteCount": record["byteCount"],
        "fileCount": 1,
    }


def dependency_identity(records: list[dict[str, object]]) -> dict[str, object]:
    roots = [record for record in records if record["purpose"] == "runtime-root"]
    if len(roots) != 1:
        raise CatalogError("runtime closure requires exactly one root")
    root = roots[0]
    root_parent = PurePosixPath(str(root["path"])).parent
    closure: list[tuple[str, dict[str, object]]] = []
    for record in records:
        if not str(record["purpose"]).startswith("runtime-"):
            continue
        uri = "" if record is root else PurePosixPath(str(record["path"])).relative_to(root_parent).as_posix()
        if record is not root:
            strict_relative_path(uri)
        closure.append((uri, record))
    closure.sort(key=lambda item: (0 if item[0] == "" else 1, item[0]))
    if not closure or closure[0][0] != "":
        raise CatalogError("dependency closure root is absent")
    if any(closure[index - 1][0] >= closure[index][0] for index in range(2, len(closure))):
        raise CatalogError("dependency closure URIs must be unique and sorted")

    digest = hashlib.sha256()
    digest.update(b"RVX.ResourceContentIdentity.DependencyClosure.v1\0")
    for uri, record in closure:
        digest.update(uri.encode("utf-8"))
        digest.update(b"\0")
        digest.update(int(record["byteCount"]).to_bytes(8, "big"))
        digest.update(bytes.fromhex(str(record["sha256"])))
    return {
        "schemaVersion": 1,
        "domain": "source",
        "scope": "dependency-closure",
        "algorithm": "sha256",
        "digest": digest.hexdigest(),
        "byteCount": sum(int(record["byteCount"]) for _, record in closure),
        "fileCount": len(closure),
    }


def parse_porsche_runtime_paths() -> list[str]:
    root = "models/free_1975_porsche_911_930_turbo"
    try:
        document = json.loads(resolve_file(f"{root}/scene.gltf").read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CatalogError("Porsche scene.gltf is not valid UTF-8 JSON") from error
    if len(document.get("meshes", [])) != 75:
        raise CatalogError("Porsche catalog contract requires exactly 75 meshes")
    if len(document.get("materials", [])) != 14:
        raise CatalogError("Porsche catalog contract requires exactly 14 materials")
    if len(document.get("images", [])) != 27:
        raise CatalogError("Porsche catalog contract requires exactly 27 textures")

    dependencies: list[str] = []
    for collection in ("buffers", "images"):
        entries = document.get(collection, [])
        if not isinstance(entries, list):
            raise CatalogError(f"Porsche glTF {collection} must be an array")
        for item in entries:
            if not isinstance(item, dict) or not isinstance(item.get("uri"), str):
                raise CatalogError(f"Porsche glTF {collection} must use external URI files")
            uri = item["uri"]
            if uri.startswith("data:"):
                raise CatalogError(f"Porsche glTF {collection} must not use data URIs")
            dependencies.append(strict_relative_path(uri).as_posix())
    if len(dependencies) != 28 or len(set(dependencies)) != 28:
        raise CatalogError("Porsche glTF must reference one buffer and 27 unique textures")
    result = [f"{root}/scene.gltf", *(f"{root}/{uri}" for uri in dependencies)]
    for path in result:
        resolve_file(path)
    return result


def entry(
    asset_id: str,
    kind: str,
    path: str,
    license_spdx: str,
    license_file: str,
    source_name: str,
    source_uri: str,
    source_author: str,
    source_version: str,
    attribution: str,
    modification_notice: str,
    records: list[dict[str, object]],
    content_identity: dict[str, object],
) -> dict[str, object]:
    files = sorted_records(records)
    if not any(record["path"] == path for record in files):
        raise CatalogError(f"asset root is missing from package: {asset_id}")
    if not any(record["path"] == license_file for record in files):
        raise CatalogError(f"license is missing from package: {asset_id}")
    return {
        "id": asset_id,
        "kind": kind,
        "usagePolicy": "optional-nightly",
        "qualificationEligible": False,
        "path": path,
        "license": {"spdxId": license_spdx, "file": license_file, "coverage": "complete-declared-package"},
        "source": {"name": source_name, "uri": source_uri, "author": source_author, "version": source_version},
        "contentIdentity": content_identity,
        "assetContentId": asset_content_id(files),
        "files": files,
        "attribution": attribution,
        "modificationNotice": modification_notice,
        "redistributable": True,
    }


def build_catalog() -> dict[str, object]:
    porsche_root = "models/free_1975_porsche_911_930_turbo"
    porsche = [
        file_record(path, "runtime-root" if path.endswith("/scene.gltf") else "runtime-dependency")
        for path in parse_porsche_runtime_paths()
    ]
    porsche_license = f"{porsche_root}/license.txt"
    porsche.append(file_record(porsche_license, "license"))
    porsche = sorted_records(porsche)
    if len(porsche) != 30 or sum(str(record["purpose"]).startswith("runtime-") for record in porsche) != 29:
        raise CatalogError("Porsche package must be 30 declared files and 29 runtime files")

    assets = [entry(
        "free-1975-porsche-911-930-turbo", "model", f"{porsche_root}/scene.gltf",
        "CC-BY-4.0", porsche_license, "FREE 1975 Porsche 911 (930) Turbo",
        "https://sketchfab.com/3d-models/free-1975-porsche-911-930-turbo-8568d9d14a994b9cae59499f0dbed21e",
        "Lionsharp Studios", "Sketchfab model 8568d9d14a994b9cae59499f0dbed21e",
        'This work is based on "FREE 1975 Porsche 911 (930) Turbo" by Lionsharp Studios, licensed under CC-BY-4.0.',
        "Vendored scene.gltf, scene.bin, and the declared textures are unmodified source-export bytes.",
        porsche, dependency_identity(porsche))]

    for asset_id, path, name, uri, author in (
        ("cowboy-town-hall-8k", "textures/cowboy_town_hall_8k.exr", "Cowboy Town Hall HDRI", "https://polyhaven.com/a/cowboy_town_hall", "Dimitrios Savva (photography); Jarod Guest (processing)"),
        ("valley-of-desolation-4k", "textures/valley_of_desolation_4k.exr", "Valley Of Desolation HDRI", "https://polyhaven.com/a/valley_of_desolation", "Dario Barresi"),
    ):
        root = file_record(path, "runtime-root")
        files = sorted_records([root, file_record("LICENSE.polyhaven-cc0.txt", "license")])
        assets.append(entry(
            asset_id, "environment", path, "CC0-1.0", "LICENSE.polyhaven-cc0.txt",
            name, uri, author, "polyhaven-hdri-source-bytes-recorded-2026-08-06",
            f"{name} from Poly Haven, CC0 1.0 Universal.",
            "Vendored EXR bytes are unmodified; environment derivatives are produced by the cook contract.",
            files, self_identity(root)))

    assets.sort(key=lambda item: str(item["id"]))
    return {"schemaId": "RVX.SampleAssetCatalog", "schemaVersion": 4, "assets": assets}


def serialized_catalog() -> bytes:
    return (json.dumps(build_catalog(), indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def self_test() -> None:
    for unsafe in ("", "../escape", "/absolute", "C:/drive", "\\\\server\\share", "slash\\mixed", "a/./b"):
        try:
            strict_relative_path(unsafe)
        except CatalogError:
            continue
        raise CatalogError(f"self-test accepted unsafe path: {unsafe!r}")
    if strict_relative_path("models/example/scene.gltf").as_posix() != "models/example/scene.gltf":
        raise CatalogError("self-test failed canonical path round-trip")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args(argv)
    try:
        if options.self_test:
            self_test()
        payload = serialized_catalog()
        output = options.output.resolve()
        if options.verify:
            if not output.is_file() or output.read_bytes() != payload:
                raise CatalogError(f"catalog is missing or byte-drifted: {output}")
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(payload)
    except (CatalogError, OSError) as error:
        print(f"external asset catalog V4: {error}", file=sys.stderr)
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
