#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path
from typing import Callable


class GateInputError(RuntimeError):
    pass


def resolve_repo_root(raw_root: str) -> Path:
    root = Path(raw_root).resolve()
    if not root.is_dir():
        raise GateInputError(f"repository root does not exist: {root}")
    marker = root / "CMakeLists.txt"
    if not marker.is_file():
        raise GateInputError(f"repository root has no CMakeLists.txt: {root}")
    return root


def resolve_required_file(root: Path, raw_path: str, label: str) -> Path:
    path = Path(raw_path)
    if not path.is_absolute():
        path = root / path
    path = path.resolve()
    if not path.is_file():
        raise GateInputError(f"{label} does not exist: {path}")
    return path


def run_gate(main: Callable[[], int]) -> int:
    try:
        return main()
    except GateInputError as error:
        print(f"Architecture gate input error: {error}", file=sys.stderr)
        return 2
