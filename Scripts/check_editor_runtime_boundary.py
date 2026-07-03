#!/usr/bin/env python3
"""Validate the shared-core boundary between Editor and Runtime."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^">]+)[">]')
SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl", ".mm"}
SCOPE_TOKENS = {"PUBLIC", "PRIVATE", "INTERFACE"}


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    return parser.parse_args()


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8-sig")


def iter_source_files(root: Path, module: str):
    module_root = root / module
    if not module_root.exists():
        return
    for path in module_root.rglob("*"):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


def include_prefix(include: str) -> str:
    return include.replace("\\", "/").split("/", 1)[0]


def find_command_blocks(text: str, command: str) -> list[tuple[str, int]]:
    blocks: list[tuple[str, int]] = []
    pattern = re.compile(rf"\b{re.escape(command)}\s*\(", re.IGNORECASE)
    for match in pattern.finditer(text):
        open_index = text.find("(", match.start())
        if open_index < 0:
            continue

        depth = 0
        in_quote = False
        escaped = False
        index = open_index
        while index < len(text):
            char = text[index]
            if in_quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    in_quote = False
            else:
                if char == '"':
                    in_quote = True
                elif char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
                    if depth == 0:
                        line = text.count("\n", 0, open_index) + 1
                        blocks.append((text[open_index + 1:index], line))
                        break
            index += 1
    return blocks


def tokenize(block: str, start_line: int) -> list[tuple[str, int]]:
    tokens: list[tuple[str, int]] = []
    index = 0
    line = start_line

    while index < len(block):
        char = block[index]
        if char.isspace():
            if char == "\n":
                line += 1
            index += 1
            continue

        if char == "#":
            while index < len(block) and block[index] != "\n":
                index += 1
            continue

        token_line = line
        if char == '"':
            index += 1
            chars: list[str] = []
            escaped = False
            while index < len(block):
                current = block[index]
                if current == "\n":
                    line += 1
                if escaped:
                    chars.append(current)
                    escaped = False
                elif current == "\\":
                    escaped = True
                elif current == '"':
                    index += 1
                    break
                else:
                    chars.append(current)
                index += 1
            tokens.append(("".join(chars), token_line))
            continue

        chars: list[str] = []
        while index < len(block) and not block[index].isspace() and block[index] != "#":
            chars.append(block[index])
            index += 1
        tokens.append(("".join(chars), token_line))

    return tokens


def scan_includes(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    restricted_edges = {
        ("Runtime", "Editor"): "Runtime modules must not include editor-only headers.",
        ("Editor", "Runtime"): "Editor modules must use shared Core/Scene/Resource/Render contracts, not Runtime headers.",
    }

    for from_module, to_module in restricted_edges:
        for path in iter_source_files(root, from_module):
            rel = path.relative_to(root)
            lines = read_text(path).splitlines()
            for line_number, line in enumerate(lines, start=1):
                match = INCLUDE_RE.match(line)
                if not match:
                    continue
                if include_prefix(match.group(1)) == to_module:
                    findings.append(Finding(rel, line_number, restricted_edges[(from_module, to_module)]))

    return findings


def scan_cmake_links(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    forbidden_links = {
        "RVX_Runtime": {"RVX_Editor", "RVX::Editor"},
        "RVX_Editor": {"RVX_Runtime", "RVX::Runtime"},
    }

    for cmake_path in [root / "Runtime" / "CMakeLists.txt", root / "Editor" / "CMakeLists.txt"]:
        if not cmake_path.exists():
            continue
        text = read_text(cmake_path)
        rel = cmake_path.relative_to(root)
        for block, start_line in find_command_blocks(text, "target_link_libraries"):
            tokens = tokenize(block, start_line)
            if len(tokens) < 2:
                continue
            target = tokens[0][0]
            forbidden = forbidden_links.get(target)
            if not forbidden:
                continue
            for token, line in tokens[1:]:
                if token.upper() in SCOPE_TOKENS:
                    continue
                if token in forbidden:
                    findings.append(
                        Finding(
                            rel,
                            line,
                            f"{target} must not link {token}; use shared core contracts instead.",
                        )
                    )

    return findings


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()

    findings = scan_includes(root)
    findings.extend(scan_cmake_links(root))

    if findings:
        print("Editor/runtime boundary violations:")
        for finding in findings:
            print(f"  {finding.path}:{finding.line}: {finding.message}")
        return 1

    print("Editor/runtime shared-core boundary passed.")
    print("Runtime and Editor do not include or link each other directly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
