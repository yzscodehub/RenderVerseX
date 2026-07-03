#!/usr/bin/env python3
"""Validate CMake include-directory visibility for RenderVerseX modules."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


SCOPE_TOKENS = {"PUBLIC", "PRIVATE", "INTERFACE"}
MODIFIER_TOKENS = {"SYSTEM", "BEFORE", "AFTER"}
PRIVATE_INCLUDE_RE = re.compile(r"(^|[\\/\}])Private([\\/>]|$)")


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    target: str
    scope: str
    entry: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    return parser.parse_args()


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8-sig")


def iter_cmake_files(root: Path):
    excluded_dirs = {".git", ".claude", "build"}
    for path in root.rglob("CMakeLists.txt"):
        rel = path.relative_to(root)
        if any(part in excluded_dirs for part in rel.parts):
            continue
        yield path


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
                        start_line = text.count("\n", 0, open_index) + 1
                        blocks.append((text[open_index + 1:index], start_line))
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

        chars = []
        while index < len(block) and not block[index].isspace() and block[index] != "#":
            chars.append(block[index])
            index += 1
        tokens.append(("".join(chars), token_line))

    return tokens


def exposes_private_include(entry: str) -> bool:
    normalized = entry.replace("\\", "/")
    return PRIVATE_INCLUDE_RE.search(normalized) is not None


def scan(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for path in iter_cmake_files(root):
        text = read_text(path)
        rel = path.relative_to(root)
        for block, start_line in find_command_blocks(text, "target_include_directories"):
            tokens = tokenize(block, start_line)
            if len(tokens) < 2:
                continue

            target = tokens[0][0]
            current_scope: str | None = None
            for token, line in tokens[1:]:
                upper = token.upper()
                if upper in MODIFIER_TOKENS:
                    continue
                if upper in SCOPE_TOKENS:
                    current_scope = upper
                    continue
                if current_scope in {"PUBLIC", "INTERFACE"} and exposes_private_include(token):
                    findings.append(Finding(rel, line, target, current_scope, token))

    return findings


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    findings = scan(root)

    if findings:
        print("CMake module visibility violations:")
        for finding in findings:
            print(
                f"  {finding.path}:{finding.line}: {finding.target} exposes "
                f"{finding.entry} as {finding.scope}"
            )
        return 1

    print("CMake module visibility passed.")
    print("No PUBLIC or INTERFACE target include directories expose module Private paths.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
