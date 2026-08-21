#!/usr/bin/env python3
"""Enforce the instance-owned, prepare-only resource loading contract."""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from architecture_gate_common import resolve_repo_root, run_gate


SOURCE_SUFFIXES = frozenset({".h", ".hpp", ".cpp", ".cxx", ".inl"})
FORBIDDEN_PATTERNS = (
    (re.compile(r"ResourceManager\s*::\s*Get\s*\("), "ResourceManager singleton access"),
    (re.compile(r"\bSupportsPreparedLoading\b"), "prepared-loading fallback capability"),
    (re.compile(r"\bFindOnlyByType\s*\("), "type-only loader selection"),
    (re.compile(r"\bRegisterLoader\s*\("), "runtime loader registration"),
    (re.compile(r"\bGetLoader\s*\("), "public loader lookup"),
    (re.compile(r"\bCanLoad\s*\("), "legacy loader path predicate"),
    (
        re.compile(r"\b(?:virtual\s+)?(?:Resource::)?IResource\s*\*\s*Load\s*\("),
        "legacy loader Load entry point",
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    return parser.parse_args()


def iter_source_files(source_root: Path):
    for path in source_root.rglob("*"):
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
            yield path


def validate_loader_interface(root: Path, failures: list[str]) -> None:
    interface_path = (
        root / "Source/Runtime/ResourceRuntime/Include/ResourceRuntime/ResourceLoader.h"
    )
    if not interface_path.is_file():
        failures.append(
            "required loader contract header is missing: "
            "Source/Runtime/ResourceRuntime/Include/ResourceRuntime/ResourceLoader.h"
        )
        return

    compact = re.sub(r"\s+", " ", interface_path.read_text(encoding="utf-8"))
    required = (
        (
            r"virtual bool Supports\s*\([^;]+\)\s*const\s+noexcept\s*=\s*0\s*;",
            "IResourceLoader::Supports must be const noexcept and pure virtual",
        ),
        (
            r"virtual bool Prepare\s*\([^;]+\)\s*=\s*0\s*;",
            "IResourceLoader::Prepare must be pure virtual",
        ),
    )
    for pattern, diagnostic in required:
        if re.search(pattern, compact) is None:
            failures.append(diagnostic)


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    source_root = root / "Source"
    failures: list[str] = []
    if not source_root.is_dir():
        failures.append("repository Source directory is missing")
    else:
        for path in iter_source_files(source_root):
            text = path.read_text(encoding="utf-8", errors="replace")
            relative = path.relative_to(root).as_posix()
            for pattern, description in FORBIDDEN_PATTERNS:
                for match in pattern.finditer(text):
                    line = text.count("\n", 0, match.start()) + 1
                    failures.append(f"{relative}:{line}: {description}")

    validate_loader_interface(root, failures)
    if failures:
        print("Resource loader contract failures:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("Resource loader prepare-only contract check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
