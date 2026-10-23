#!/usr/bin/env python3
"""Enforce composition-owned renderer pass wiring.

The renderer feature layer must exchange immutable plans and graph resources,
not retain a mutable pass registry or pass pointers.  This deliberately scans
the small construction boundary rather than attempting to infer C++ ownership
from the whole renderer.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Iterable

from architecture_gate_common import resolve_repo_root, run_gate


SOURCE_SUFFIXES = frozenset({".h", ".hpp", ".cpp", ".cxx", ".inl"})
DEFAULT_ADAPTER_ROOTS = (
    "Source/Runtime/Render/Include/Render/Features",
    "Source/Runtime/Render/Private/Features",
)
DEFAULT_SCENE_RENDERER = "Source/Runtime/Render/Include/Render/Renderer/SceneRenderer.h"
DEFAULT_COMPOSITION = "Source/Runtime/Render/Private/Features/DefaultForwardComposition.cpp"
DEFAULT_BUNDLE_FACTORY = "CreateDefaultRenderPassBundle"

# This intentionally matches a type name ending in Pass, not RHIRenderPassDesc
# or another descriptor containing that word.
FORBIDDEN_PASS_TYPE = (
    r"(?:RenderPassRegistry|IRenderPass|(?:[A-Za-z_]\w*::)*[A-Za-z_]\w*Pass)\b"
)
CONSTRUCTOR = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*FeatureAdapter)\s*\((?P<parameters>[^)]*)\)",
    re.DOTALL,
)
MEMBER = re.compile(
    rf"(?:{FORBIDDEN_PASS_TYPE})\s*(?:\*\s*)?(?:const\s*)?"
    r"(?P<member>m_[A-Za-z_]\w*)\b"
)
SMART_MEMBER = re.compile(
    rf"std::(?:unique_ptr|shared_ptr)\s*<\s*(?:const\s+)?"
    rf"{FORBIDDEN_PASS_TYPE}\s*>\s*(?P<member>m_[A-Za-z_]\w*)\b"
)
SCENE_ADAPTER_MEMBER = re.compile(
    r"\b[A-Za-z_]\w*FeatureAdapter\s*\*\s*(?:const\s*)?"
    r"(?P<member>m_[A-Za-z_]\w*)\b"
)
SCENE_PASS_MEMBER = re.compile(
    rf"(?:{FORBIDDEN_PASS_TYPE})\s*\*\s*(?:const\s*)?"
    r"(?P<member>m_[A-Za-z_]\w*)\b"
)
DIRECT_PASS_CONSTRUCTION = re.compile(
    rf"(?:std::)?make_(?:unique|shared)\s*<\s*(?:{FORBIDDEN_PASS_TYPE})\s*>",
)
DIRECT_PASS_NEW = re.compile(rf"\bnew\s+(?:{FORBIDDEN_PASS_TYPE})\b")


def _line(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _relative(root: Path, path: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def _read_required(root: Path, raw_path: str, label: str, failures: list[str]) -> tuple[Path, str] | None:
    path = root / raw_path
    if not path.is_file():
        failures.append(f"required {label} is missing: {raw_path}")
        return None
    return path, path.read_text(encoding="utf-8", errors="replace")


def _iter_adapter_files(root: Path, adapter_roots: Iterable[str], failures: list[str]) -> list[Path]:
    paths: set[Path] = set()
    for raw_root in adapter_roots:
        adapter_root = root / raw_root
        if not adapter_root.is_dir():
            failures.append(f"required feature adapter directory is missing: {raw_root}")
            continue
        paths.update(
            path for path in adapter_root.rglob("*")
            if path.is_file()
            and path.suffix.lower() in SOURCE_SUFFIXES
            and "FeatureAdapter" in path.stem
        )
    if not paths:
        failures.append("no renderer FeatureAdapter header or implementation was found")
    return sorted(paths)


def _forbidden_constructor_parameter(parameters: str) -> bool:
    # A composition bundle is the only legal path for a feature to receive pass
    # implementation.  References are also forbidden here: changing '*' to '&'
    # must not bypass the ownership boundary.
    return re.search(FORBIDDEN_PASS_TYPE, parameters) is not None


def _validate_feature_adapters(root: Path, adapter_roots: Iterable[str], failures: list[str]) -> None:
    for path in _iter_adapter_files(root, adapter_roots, failures):
        text = path.read_text(encoding="utf-8", errors="replace")
        relative = _relative(root, path)
        for match in MEMBER.finditer(text):
            failures.append(
                f"{relative}:{_line(text, match.start())}: FeatureAdapter retains "
                f"forbidden pass ownership in member {match.group('member')}"
            )
        for match in SMART_MEMBER.finditer(text):
            failures.append(
                f"{relative}:{_line(text, match.start())}: FeatureAdapter retains "
                f"forbidden pass ownership in member {match.group('member')}"
            )
        for match in CONSTRUCTOR.finditer(text):
            if _forbidden_constructor_parameter(match.group("parameters")):
                failures.append(
                    f"{relative}:{_line(text, match.start())}: "
                    f"{match.group('name')} constructor injects a pass implementation"
                )


def _validate_scene_renderer(root: Path, scene_renderer: str, failures: list[str]) -> None:
    required = _read_required(root, scene_renderer, "SceneRenderer header", failures)
    if required is None:
        return
    path, text = required
    relative = _relative(root, path)
    for match in SCENE_ADAPTER_MEMBER.finditer(text):
        failures.append(
            f"{relative}:{_line(text, match.start())}: SceneRenderer caches "
            f"built-in feature adapter {match.group('member')}"
        )
    for match in SCENE_PASS_MEMBER.finditer(text):
        failures.append(
            f"{relative}:{_line(text, match.start())}: SceneRenderer caches "
            f"concrete pass pointer {match.group('member')}"
        )


def _validate_composition(
    root: Path,
    composition: str,
    bundle_factory: str,
    failures: list[str],
) -> None:
    required = _read_required(root, composition, "DefaultForwardComposition source", failures)
    if required is None:
        return
    path, text = required
    relative = _relative(root, path)
    for pattern in (DIRECT_PASS_CONSTRUCTION, DIRECT_PASS_NEW):
        for match in pattern.finditer(text):
            failures.append(
                f"{relative}:{_line(text, match.start())}: DefaultForwardComposition "
                "must not construct a concrete render pass directly"
            )

    factory_call = re.compile(rf"\b{re.escape(bundle_factory)}\s*\(")
    calls = list(factory_call.finditer(text))
    if len(calls) != 1:
        failures.append(
            f"{relative}: DefaultForwardComposition must invoke bundle factory "
            f"{bundle_factory} exactly once, found {len(calls)}"
        )


def validate_renderer_feature_ownership(
    root: Path,
    *,
    adapter_roots: Iterable[str] = DEFAULT_ADAPTER_ROOTS,
    scene_renderer: str = DEFAULT_SCENE_RENDERER,
    composition: str = DEFAULT_COMPOSITION,
    bundle_factory: str = DEFAULT_BUNDLE_FACTORY,
) -> list[str]:
    """Return deterministic ownership-boundary violations for *root*."""
    failures: list[str] = []
    _validate_feature_adapters(root, adapter_roots, failures)
    _validate_scene_renderer(root, scene_renderer, failures)
    _validate_composition(root, composition, bundle_factory, failures)
    return sorted(set(failures))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument(
        "--adapter-root",
        action="append",
        dest="adapter_roots",
        help="FeatureAdapter directory relative to --root; repeatable.",
    )
    parser.add_argument(
        "--scene-renderer",
        default=DEFAULT_SCENE_RENDERER,
        help="SceneRenderer header relative to --root.",
    )
    parser.add_argument(
        "--composition",
        default=DEFAULT_COMPOSITION,
        help="Default composition source relative to --root.",
    )
    parser.add_argument(
        "--bundle-factory",
        default=DEFAULT_BUNDLE_FACTORY,
        help="Required single pass-bundle factory call in the composition source.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    failures = validate_renderer_feature_ownership(
        root,
        adapter_roots=args.adapter_roots or DEFAULT_ADAPTER_ROOTS,
        scene_renderer=args.scene_renderer,
        composition=args.composition,
        bundle_factory=args.bundle_factory,
    )
    if failures:
        print("Renderer feature ownership violations:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("Renderer feature ownership check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
