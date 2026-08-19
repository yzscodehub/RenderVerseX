#!/usr/bin/env python3
"""Validate public header include edges have public CMake link coverage."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from architecture_gate_common import resolve_repo_root, resolve_required_file, run_gate


INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^">]+)[">]')
PROJECT_ALIAS_PREFIX = "RVX::"
PROJECT_TARGET_PREFIX = "RVX_"
SCOPE_TOKENS = {"PUBLIC", "PRIVATE", "INTERFACE"}
LINK_MODIFIER_TOKENS = {"DEBUG", "OPTIMIZED", "GENERAL"}
SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".inl"}


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str


@dataclass(frozen=True)
class PublicIncludeUse:
    from_module: str
    to_module: str
    include: str
    path: Path
    line: int

    def edge(self) -> str:
        return f"{self.from_module}->{self.to_module}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument(
        "--config",
        default="Docs/module-boundaries.json",
        help="Boundary configuration file.",
    )
    return parser.parse_args()


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8-sig")


def load_config(root: Path, config_path: str) -> dict:
    path = Path(config_path)
    if not path.is_absolute():
        path = root / path
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


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

        chars: list[str] = []
        while index < len(block) and not block[index].isspace() and block[index] != "#":
            chars.append(block[index])
            index += 1
        tokens.append(("".join(chars), token_line))

    return tokens


def read_simple_variables(text: str) -> dict[str, str]:
    variables: dict[str, str] = {}
    for block, start_line in find_command_blocks(text, "set"):
        tokens = tokenize(block, start_line)
        if len(tokens) >= 2:
            variables[tokens[0][0]] = tokens[1][0]
    return variables


def expand_token(token: str, variables: dict[str, str]) -> str:
    match = re.fullmatch(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", token)
    if match:
        return variables.get(match.group(1), token)
    return token


def include_prefix(include: str) -> str:
    return include.replace("\\", "/").split("/", 1)[0]


def build_target_module_map(modules: set[str]) -> dict[str, str]:
    target_to_module: dict[str, str] = {}
    for module in modules:
        target_to_module[module] = module
        target_to_module[f"{PROJECT_TARGET_PREFIX}{module}"] = module
        target_to_module[f"{PROJECT_ALIAS_PREFIX}{module}"] = module
    return target_to_module


def discover_aliases(root: Path, target_to_module: dict[str, str]) -> None:
    for path in iter_cmake_files(root):
        text = read_text(path)
        variables = read_simple_variables(text)
        for block, start_line in find_command_blocks(text, "add_library"):
            tokens = tokenize(block, start_line)
            if len(tokens) >= 3 and tokens[1][0].upper() == "ALIAS":
                alias = expand_token(tokens[0][0], variables)
                target = expand_token(tokens[2][0], variables)
                module = target_to_module.get(target)
                if module:
                    target_to_module[alias] = module


def token_module(token: str, target_to_module: dict[str, str]) -> str | None:
    if "$" in token or token.startswith("-"):
        return None
    if token in target_to_module:
        return target_to_module[token]
    if token.startswith(PROJECT_ALIAS_PREFIX):
        return target_to_module.get(token[len(PROJECT_ALIAS_PREFIX):])
    if token.startswith(PROJECT_TARGET_PREFIX):
        return target_to_module.get(token[len(PROJECT_TARGET_PREFIX):])
    return None


def build_public_link_edges(root: Path, modules: set[str]) -> dict[str, set[str]]:
    target_to_module = build_target_module_map(modules)
    discover_aliases(root, target_to_module)

    public_edges: dict[str, set[str]] = {module: {module} for module in modules}
    for path in iter_cmake_files(root):
        text = read_text(path)
        variables = read_simple_variables(text)
        for block, start_line in find_command_blocks(text, "target_link_libraries"):
            tokens = tokenize(block, start_line)
            if len(tokens) < 2:
                continue

            target = expand_token(tokens[0][0], variables)
            from_module = token_module(target, target_to_module)
            if not from_module:
                continue

            current_scope: str | None = None
            for token, _line in tokens[1:]:
                dependency = expand_token(token, variables)
                upper = dependency.upper()
                if upper in SCOPE_TOKENS:
                    current_scope = upper
                    continue
                if upper in LINK_MODIFIER_TOKENS:
                    continue

                to_module = token_module(dependency, target_to_module)
                if not to_module:
                    continue
                if current_scope in {"PUBLIC", "INTERFACE"}:
                    public_edges.setdefault(from_module, {from_module}).add(to_module)

    return public_edges


def iter_public_headers(root: Path, config: dict):
    for module_name, module in config["modules"].items():
        if module_name in {"Samples", "Tests"}:
            continue
        include_root = root / module["path"] / "Include"
        if not include_root.exists():
            continue
        for path in include_root.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield module_name, path


def scan_public_includes(root: Path, config: dict) -> list[PublicIncludeUse]:
    prefixes = set(config["projectIncludePrefixes"])
    uses: list[PublicIncludeUse] = []
    for module_name, path in iter_public_headers(root, config):
        lines = read_text(path).splitlines()
        for line_number, line in enumerate(lines, start=1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            include = match.group(1)
            prefix = include_prefix(include)
            if prefix not in prefixes:
                continue
            uses.append(
                PublicIncludeUse(
                    from_module=module_name,
                    to_module=prefix,
                    include=include,
                    path=path.relative_to(root),
                    line=line_number,
                )
            )
    return uses


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    config_path = resolve_required_file(root, args.config, "module boundary configuration")
    config = load_config(root, str(config_path))
    modules = set(config["modules"].keys())

    public_edges = build_public_link_edges(root, modules)
    include_uses = scan_public_includes(root, config)
    findings: list[Finding] = []

    for use in include_uses:
        if use.to_module in public_edges.get(use.from_module, {use.from_module}):
            continue
        findings.append(
            Finding(
                use.path,
                use.line,
                f"Public header include {use.include} exposes {use.edge()}, "
                f"but {use.from_module} does not link {use.to_module} as PUBLIC or INTERFACE.",
            )
        )

    print(f"Scanned {len(include_uses)} public project include directives.")
    print(f"Resolved PUBLIC/INTERFACE CMake link edges for {len(public_edges)} modules.")

    if findings:
        print("")
        print("Public header linkage violations:")
        for finding in findings[:100]:
            print(f"  {finding.path}:{finding.line}: {finding.message}")
        if len(findings) > 100:
            print(f"  ... {len(findings) - 100} more")
        return 1

    print("Public header linkage passed.")
    print("Every cross-module public header include is covered by a PUBLIC or INTERFACE CMake link.")
    return 0


if __name__ == "__main__":
    sys.exit(run_gate(main))
