#!/usr/bin/env python3
"""Validate CMake target links against RenderVerseX module boundaries."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


SCOPE_TOKENS = {"PUBLIC", "PRIVATE", "INTERFACE"}
LINK_MODIFIER_TOKENS = {"DEBUG", "OPTIMIZED", "GENERAL"}
PROJECT_ALIAS_PREFIX = "RVX::"
PROJECT_TARGET_PREFIX = "RVX_"


@dataclass(frozen=True)
class LinkUse:
    from_module: str
    to_module: str
    target: str
    dependency: str
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
    parser.add_argument(
        "--fail-on-legacy",
        action="store_true",
        help="Treat tolerated legacy edges as failures.",
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


def edge_allowed(use: LinkUse, config: dict) -> bool:
    allowed = set(config["modules"][use.from_module].get("allowed", []))
    return "*" in allowed or use.to_module in allowed


def group_counts(uses: list[LinkUse]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for use in uses:
        counts[use.edge()] = counts.get(use.edge(), 0) + 1
    return dict(sorted(counts.items()))


def scan(root: Path, config: dict) -> tuple[list[LinkUse], int]:
    modules = set(config["modules"].keys())
    target_to_module = build_target_module_map(modules)
    discover_aliases(root, target_to_module)

    uses: list[LinkUse] = []
    file_count = 0
    for path in iter_cmake_files(root):
        text = read_text(path)
        variables = read_simple_variables(text)
        rel = path.relative_to(root)
        file_count += 1
        for block, start_line in find_command_blocks(text, "target_link_libraries"):
            tokens = tokenize(block, start_line)
            if len(tokens) < 2:
                continue

            target = expand_token(tokens[0][0], variables)
            from_module = token_module(target, target_to_module)
            if not from_module:
                continue

            for token, line in tokens[1:]:
                dependency = expand_token(token, variables)
                upper = dependency.upper()
                if upper in SCOPE_TOKENS or upper in LINK_MODIFIER_TOKENS:
                    continue

                to_module = token_module(dependency, target_to_module)
                if not to_module:
                    continue

                uses.append(
                    LinkUse(
                        from_module=from_module,
                        to_module=to_module,
                        target=target,
                        dependency=dependency,
                        path=rel,
                        line=line,
                    )
                )

    return uses, file_count


def print_use(use: LinkUse) -> None:
    print(
        f"  {use.path}:{use.line}: {use.edge()} target {use.target} "
        f"links {use.dependency}"
    )


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    config = load_config(root, args.config)
    legacy_edges = set(config.get("legacyEdges", {}).keys())
    legacy_budgets = config.get("legacyBudgets", {})

    uses, file_count = scan(root, config)
    violations = [
        use
        for use in uses
        if not edge_allowed(use, config) and use.edge() not in legacy_edges
    ]
    legacy_uses = [use for use in uses if use.edge() in legacy_edges]

    print(f"Scanned {file_count} CMakeLists.txt files.")
    print(f"Found {len(uses)} project target link directives.")

    if violations:
        print("")
        print("CMake module link boundary violations:")
        for use in violations[:100]:
            print_use(use)
        if len(violations) > 100:
            print(f"  ... {len(violations) - 100} more")
        return 1

    print("No CMake module link boundary violations.")

    if legacy_uses:
        print("")
        print("Tolerated legacy CMake link edges:")
        legacy_counts = group_counts(legacy_uses)
        notes = config.get("legacyEdges", {})
        for edge, count in legacy_counts.items():
            note = notes.get(edge, "")
            if note:
                print(f"  {edge}: {count} link(s) - {note}")
            else:
                print(f"  {edge}: {count} link(s)")

        budget_violations = []
        for edge, count in legacy_counts.items():
            budget = legacy_budgets.get(edge)
            if budget is not None and count > int(budget):
                budget_violations.append((edge, count, int(budget)))

        if budget_violations:
            print("")
            print("Legacy CMake link budget violations:")
            for edge, count, budget in budget_violations:
                print(f"  {edge}: {count} link(s), budget is {budget}")
            return 1

        if args.fail_on_legacy:
            print("")
            print("Legacy CMake link edges are present and --fail-on-legacy was set.")
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
