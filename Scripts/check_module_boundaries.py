#!/usr/bin/env python3
"""Validate RenderVerseX project include boundaries."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^">]+)[">]')
SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inl",
}


@dataclass(frozen=True)
class IncludeUse:
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
    parser.add_argument(
        "--fail-on-legacy",
        action="store_true",
        help="Treat tolerated legacy edges as failures.",
    )
    return parser.parse_args()


def load_config(root: Path, config_path: str) -> dict:
    path = Path(config_path)
    if not path.is_absolute():
        path = root / path
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def iter_source_files(module_root: Path):
    if not module_root.exists():
        return
    for path in module_root.rglob("*"):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


def include_prefix(include: str) -> str:
    normalized = include.replace("\\", "/")
    return normalized.split("/", 1)[0]


def scan(root: Path, config: dict) -> tuple[list[IncludeUse], int]:
    project_prefixes = set(config["projectIncludePrefixes"])
    uses: list[IncludeUse] = []
    file_count = 0

    for module_name, module in config["modules"].items():
        module_root = root / module["path"]
        for path in iter_source_files(module_root):
            file_count += 1
            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except UnicodeDecodeError:
                lines = path.read_text(encoding="utf-8-sig").splitlines()

            for line_number, line in enumerate(lines, start=1):
                match = INCLUDE_RE.match(line)
                if not match:
                    continue

                include = match.group(1)
                prefix = include_prefix(include)
                if prefix not in project_prefixes:
                    continue

                uses.append(
                    IncludeUse(
                        from_module=module_name,
                        to_module=prefix,
                        include=include,
                        path=path.relative_to(root),
                        line=line_number,
                    )
                )

    return uses, file_count


def edge_allowed(use: IncludeUse, config: dict) -> bool:
    allowed = set(config["modules"][use.from_module].get("allowed", []))
    return "*" in allowed or use.to_module in allowed


def group_counts(uses: list[IncludeUse]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for use in uses:
        counts[use.edge()] = counts.get(use.edge(), 0) + 1
    return dict(sorted(counts.items()))


def print_use(use: IncludeUse) -> None:
    print(f"  {use.path}:{use.line}: {use.edge()} includes {use.include}")


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

    print(f"Scanned {file_count} source files.")
    print(f"Found {len(uses)} project include directives.")

    if violations:
        print("")
        print("Module boundary violations:")
        for use in violations[:100]:
            print_use(use)
        if len(violations) > 100:
            print(f"  ... {len(violations) - 100} more")
        return 1

    print("No module boundary violations.")

    if legacy_uses:
        print("")
        print("Tolerated legacy edges:")
        legacy_counts = group_counts(legacy_uses)
        notes = config.get("legacyEdges", {})
        for edge, count in legacy_counts.items():
            note = notes.get(edge, "")
            if note:
                print(f"  {edge}: {count} include(s) - {note}")
            else:
                print(f"  {edge}: {count} include(s)")

        budget_violations = []
        for edge, count in legacy_counts.items():
            budget = legacy_budgets.get(edge)
            if budget is not None and count > int(budget):
                budget_violations.append((edge, count, int(budget)))

        if budget_violations:
            print("")
            print("Legacy edge budget violations:")
            for edge, count, budget in budget_violations:
                print(f"  {edge}: {count} include(s), budget is {budget}")
            return 1

        if args.fail_on_legacy:
            print("")
            print("Legacy edges are present and --fail-on-legacy was set.")
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
