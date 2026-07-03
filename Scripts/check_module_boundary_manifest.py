#!/usr/bin/env python3
"""Validate the module-boundary manifest covers project modules and targets."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


PROJECT_ALIAS_PREFIX = "RVX::"
PROJECT_TARGET_PREFIX = "RVX_"
IGNORED_ROOTS = {
    ".git",
    ".github",
    ".claude",
    "build",
    "Docs",
    "Scripts",
    "ShaderCache",
}
KNOWN_NON_MODULE_TARGETS = {
    "RVX_Options",
    "RVX::Options",
}
KNOWN_SUBMODULE_TARGETS = {
    "RVX_SampleCommon",
    "RVX::SampleCommon",
    "RVX_TestCommon",
    "RVX::RenderGraph",
}


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str


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


def load_config(root: Path, config_path: str) -> tuple[dict, Path]:
    path = Path(config_path)
    if not path.is_absolute():
        path = root / path
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle), path


def iter_cmake_files(root: Path):
    for path in root.rglob("CMakeLists.txt"):
        rel = path.relative_to(root)
        if any(part in IGNORED_ROOTS for part in rel.parts):
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


def config_line(config_path: Path, needle: str) -> int:
    text = read_text(config_path)
    offset = text.find(needle)
    if offset < 0:
        return 1
    return text.count("\n", 0, offset) + 1


def module_for_path(rel: Path, modules: dict) -> str | None:
    for name, module in modules.items():
        module_path = Path(module["path"])
        if rel == module_path or rel.parts[:len(module_path.parts)] == module_path.parts:
            return name
    return None


def target_module_name(target: str) -> str | None:
    if target.startswith(PROJECT_ALIAS_PREFIX):
        return target[len(PROJECT_ALIAS_PREFIX):]
    if target.startswith(PROJECT_TARGET_PREFIX):
        return target[len(PROJECT_TARGET_PREFIX):]
    return None


def validate_config(root: Path, config: dict, config_rel: Path, config_path: Path) -> list[Finding]:
    findings: list[Finding] = []
    modules = config.get("modules", {})
    module_names = set(modules.keys())
    prefixes = config.get("projectIncludePrefixes", [])
    prefix_set = set(prefixes)

    if len(prefixes) != len(prefix_set):
        findings.append(
            Finding(config_rel, config_line(config_path, "projectIncludePrefixes"),
                    "projectIncludePrefixes must not contain duplicates.")
        )

    if prefix_set != module_names:
        missing = sorted(module_names - prefix_set)
        extra = sorted(prefix_set - module_names)
        if missing:
            findings.append(
                Finding(config_rel, config_line(config_path, "projectIncludePrefixes"),
                        f"projectIncludePrefixes misses modules: {', '.join(missing)}.")
            )
        if extra:
            findings.append(
                Finding(config_rel, config_line(config_path, "projectIncludePrefixes"),
                        f"projectIncludePrefixes contains entries without modules: {', '.join(extra)}.")
            )

    paths: dict[str, str] = {}
    for name, module in modules.items():
        rel_path = module.get("path")
        if not rel_path:
            findings.append(Finding(config_rel, config_line(config_path, f'"{name}"'), f"{name} must declare a path."))
            continue
        if rel_path in paths:
            findings.append(
                Finding(config_rel, config_line(config_path, f'"path": "{rel_path}"'),
                        f"{name} and {paths[rel_path]} share the same module path {rel_path}.")
            )
        paths[rel_path] = name

        if not (root / rel_path).exists():
            findings.append(
                Finding(config_rel, config_line(config_path, f'"path": "{rel_path}"'),
                        f"{name} module path does not exist: {rel_path}.")
            )

        allowed = module.get("allowed", [])
        if not isinstance(allowed, list) or not allowed:
            findings.append(
                Finding(config_rel, config_line(config_path, f'"{name}"'),
                        f"{name} must declare at least one allowed dependency edge.")
            )
            continue
        unknown = sorted(edge for edge in allowed if edge != "*" and edge not in module_names)
        if unknown:
            findings.append(
                Finding(config_rel, config_line(config_path, f'"{name}"'),
                        f"{name} allows unknown modules: {', '.join(unknown)}.")
            )

    for edge in config.get("legacyEdges", {}).keys():
        if "->" not in edge:
            findings.append(Finding(config_rel, config_line(config_path, edge), f"Legacy edge {edge} must use A->B form."))
            continue
        source, target = edge.split("->", 1)
        if source not in module_names or target not in module_names:
            findings.append(
                Finding(config_rel, config_line(config_path, edge),
                        f"Legacy edge {edge} must reference registered modules.")
            )

    return findings


def validate_cmake_coverage(root: Path, config: dict) -> list[Finding]:
    findings: list[Finding] = []
    modules = config["modules"]
    module_names = set(modules.keys())

    for path in iter_cmake_files(root):
        rel = path.relative_to(root)
        owner = module_for_path(rel.parent, modules)
        text = read_text(path)
        variables = read_simple_variables(text)
        project_target_lines: list[tuple[str, int]] = []
        for command in ("add_library", "add_executable"):
            for block, start_line in find_command_blocks(text, command):
                tokens = tokenize(block, start_line)
                if not tokens:
                    continue
                target = expand_token(tokens[0][0], variables)
                if target.startswith(PROJECT_TARGET_PREFIX) or target.startswith(PROJECT_ALIAS_PREFIX):
                    project_target_lines.append((target, tokens[0][1]))
                elif target in module_names:
                    project_target_lines.append((target, tokens[0][1]))

        if project_target_lines and owner is None:
            for target, line in project_target_lines:
                if target in KNOWN_NON_MODULE_TARGETS:
                    continue
                findings.append(
                    Finding(rel, line, f"Project target {target} is outside any registered module path.")
                )
            continue

        for target, line in project_target_lines:
            if target in KNOWN_NON_MODULE_TARGETS or target in KNOWN_SUBMODULE_TARGETS:
                continue
            module_name = target_module_name(target) or target
            if module_name not in module_names:
                findings.append(
                    Finding(rel, line, f"Project target {target} is not represented in module-boundaries.json.")
                )
            elif owner and module_name != owner and owner not in {"Samples", "Tests"}:
                findings.append(
                    Finding(rel, line, f"Project target {target} is declared under {owner}, not {module_name}.")
                )

    return findings


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    config, config_path = load_config(root, args.config)
    config_rel = config_path.relative_to(root)

    findings = validate_config(root, config, config_rel, config_path)
    findings.extend(validate_cmake_coverage(root, config))

    if findings:
        print("Module boundary manifest failures:")
        for finding in findings:
            print(f"  {finding.path}:{finding.line}: {finding.message}")
        return 1

    print("Module boundary manifest passed.")
    print("projectIncludePrefixes, module paths, allowed edges, legacy edges, and project targets are covered.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
