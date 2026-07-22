#!/usr/bin/env python3
"""Validate the M1 completion-token ownership inventory for stored GPU objects."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from architecture_gate_common import GateInputError, resolve_repo_root, resolve_required_file, run_gate


SOURCE_ROOTS = (
    "Render",
    "RHI",
    "RHI_DX11",
    "RHI_DX12",
    "RHI_Vulkan",
    "RHI_Metal",
    "RHI_OpenGL",
)
SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl", ".mm"}
VALID_PHASES = {"pre-delete", "final"}
VALID_POLICIES = {
    "RegistryExactGeneration",
    "SubmissionBatch",
    "PoolAvailability",
    "OwnerSnapshot",
    "SurfaceGeneration",
    "ShutdownAfterDrain",
}

OWNERSHIP_TYPE = re.compile(
    r"(?:\bRHI[A-Za-z0-9_]*Ref\b|"
    r"\bRef\s*<\s*RHI[A-Za-z0-9_]*\b|"
    r"\b(?:Microsoft::WRL::)?ComPtr\s*<|"
    r"\bid\s*<\s*MTL[A-Za-z0-9_]*\s*>|"
    r"\bVk(?:Buffer|Image|ImageView|Sampler|ShaderModule|Semaphore|Fence|DeviceMemory|"
    r"DescriptorSet|DescriptorSetLayout|Pipeline|PipelineLayout|SwapchainKHR|SurfaceKHR|"
    r"QueryPool|CommandPool|CommandBuffer|AccelerationStructureKHR)\b|"
    r"\bGLsync\b)"
)
TYPE_DECLARATION = re.compile(r"\b(?:class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)[^;{]*\{")

FIXED_FRAME_PATTERNS = (
    (re.compile(r"\bRVX_MAX_FRAME_COUNT\s*\+\s*1\b"), "fixed-frame retention window"),
    (re.compile(r"\bkViewRetireFrameLag\b"), "fixed-frame view retirement"),
    (re.compile(r"\bretiredFrameResources\b"), "frame-indexed RenderGraph retirement"),
)
GLOBAL_SYMBOL = re.compile(r"\b(?:DeferredDeleterRegistry|IDeferredDeleter|FrameResourceManager)\b")
PRE_DELETE_DEFINITION_PATHS = {
    "Core/Include/Core/RefCounted.h",
    "Render/Private/Graph/FrameResourceManager.cpp",
}
ACTIVE_GLOBAL_USE = re.compile(
    r"(?:DeferredDeleterRegistry\s*::\s*Get\s*\(|\.Register\s*\(|\.Unregister\s*\(|"
    r"GetDeleter\s*\(|ScheduleDeletion\s*\(|ProcessDeferredDeletions\s*\(|"
    r"SetCurrentFrame\s*\(|Private/Graph/FrameResourceManager\.cpp)"
)


@dataclass(frozen=True, order=True)
class Holder:
    path: str
    owner: str
    symbol: str
    line: int

    @property
    def key(self) -> tuple[str, str, str]:
        return self.path, self.owner, self.symbol


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    message: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument(
        "--inventory",
        default="Scripts/rhi_ownership_inventory_m1.json",
        help="Ownership inventory JSON.",
    )
    parser.add_argument("--phase", required=True, help="pre-delete or final.")
    parser.add_argument(
        "--list-holders",
        action="store_true",
        help="Print discovered holders as JSON and skip inventory comparison.",
    )
    parser.add_argument(
        "--holder-prefix",
        default="",
        help="With --list-holders, include only paths under this exact prefix.",
    )
    return parser.parse_args()


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8-sig")


def relative_path(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def iter_production_sources(root: Path):
    for source_root in SOURCE_ROOTS:
        directory = root / source_root
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield path


def matching_brace(text: str, open_offset: int) -> int:
    depth = 0
    for offset in range(open_offset, len(text)):
        character = text[offset]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return offset
    return len(text)


def owner_spans(text: str) -> list[tuple[int, int, str]]:
    spans: list[tuple[int, int, str]] = []
    for match in TYPE_DECLARATION.finditer(text):
        open_offset = text.find("{", match.start(), match.end())
        spans.append((open_offset, matching_brace(text, open_offset), match.group(1)))
    return spans


def owner_span_at(spans: list[tuple[int, int, str]], offset: int) -> tuple[int, int, str] | None:
    candidates = [span for span in spans if span[0] < offset < span[1]]
    if not candidates:
        return None
    return max(candidates, key=lambda span: span[0])


def brace_depth(text: str, start: int, end: int) -> int:
    return text.count("{", start, end) - text.count("}", start, end)


def remove_comments(text: str) -> str:
    def blank(match: re.Match[str]) -> str:
        return "".join("\n" if character == "\n" else " " for character in match.group(0))

    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", blank, text)


def discover_holders(root: Path) -> list[Holder]:
    discovered: dict[tuple[str, str, str], Holder] = {}
    for path in iter_production_sources(root):
        rel = relative_path(path, root)
        text = read_text(path)
        clean = remove_comments(text)
        spans = owner_spans(clean)

        statement_start = 0
        for terminator in re.finditer(r";", clean):
            statement = clean[statement_start:terminator.end()]
            absolute_start = statement_start
            statement_start = terminator.end()
            type_matches = list(OWNERSHIP_TYPE.finditer(statement))
            if not type_matches:
                continue
            if re.search(r"\b(?:using|typedef)\b", statement):
                continue

            type_match = type_matches[-1]
            declaration_prefix = statement[:type_match.start()]
            declaration_fragment = statement[type_match.start():]
            if "(" in declaration_fragment or (
                "(" in declaration_prefix and ")" in declaration_fragment
            ):
                continue

            declaration = re.search(
                r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*"
                r"(?:=\s*[^;]*|\{[^;]*\})?\s*;\s*$",
                declaration_fragment,
                flags=re.DOTALL,
            )
            if not declaration:
                continue

            symbol = declaration.group(1)
            symbol_offset = absolute_start + type_match.start() + declaration.start(1)
            span = owner_span_at(spans, symbol_offset)
            owner = span[2] if span else "FileScope"
            if span and brace_depth(clean, span[0], symbol_offset) != 1:
                continue
            # Source-file temporaries are not stored ownership. File-scope statics must use s_.
            if owner == "FileScope" and not symbol.startswith("s_"):
                continue
            holder = Holder(
                path=rel,
                owner=owner,
                symbol=symbol,
                line=clean.count("\n", 0, symbol_offset) + 1,
            )
            discovered[holder.key] = holder

    return sorted(discovered.values())


def load_inventory(root: Path, raw_path: str) -> tuple[dict, str]:
    path = resolve_required_file(root, raw_path, "RHI ownership inventory")
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle), relative_path(path, root)
    except json.JSONDecodeError as error:
        raise GateInputError(f"RHI ownership inventory is invalid JSON: {error}") from error


def validate_inventory(inventory: dict, inventory_path: str, holders: list[Holder]) -> list[Finding]:
    findings: list[Finding] = []
    if inventory.get("schemaVersion") != 1:
        findings.append(Finding(inventory_path, 1, "schemaVersion must be exactly 1."))

    entries = inventory.get("entries")
    if not isinstance(entries, list):
        return findings + [Finding(inventory_path, 1, "entries must be an array.")]

    required_fields = {
        "path", "owner", "symbol", "policy", "completionSource", "normalRelease", "deviceLostRelease"
    }
    by_key: dict[tuple[str, str, str], int] = {}
    holder_keys = {holder.key for holder in holders}
    for index, entry in enumerate(entries):
        location = index + 1
        if not isinstance(entry, dict):
            findings.append(Finding(inventory_path, location, "inventory entry must be an object."))
            continue
        missing = sorted(required_fields - entry.keys())
        extra = sorted(entry.keys() - required_fields)
        if missing:
            findings.append(Finding(inventory_path, location, f"entry misses fields: {', '.join(missing)}."))
            continue
        if extra:
            findings.append(Finding(inventory_path, location, f"entry has unsupported fields: {', '.join(extra)}."))
        key = (entry["path"], entry["owner"], entry["symbol"])
        by_key[key] = by_key.get(key, 0) + 1
        if any(not isinstance(entry[field], str) or not entry[field].strip() for field in required_fields):
            findings.append(Finding(inventory_path, location, "all entry fields must be non-empty strings."))
        if "*" in entry["path"] or "?" in entry["path"]:
            findings.append(Finding(inventory_path, location, "inventory paths must be exact; wildcards are forbidden."))
        if entry["policy"] not in VALID_POLICIES:
            findings.append(Finding(inventory_path, location, f"invalid lifetime policy: {entry['policy']}."))
        if key not in holder_keys:
            findings.append(Finding(inventory_path, location, f"stale holder entry: {'::'.join(key)}."))

    for key, count in by_key.items():
        if count > 1:
            findings.append(Finding(inventory_path, 1, f"duplicate holder entry: {'::'.join(key)}."))

    entry_keys = set(by_key)
    for holder in holders:
        if holder.key not in entry_keys:
            findings.append(
                Finding(holder.path, holder.line, f"unclassified RHI owner: {holder.owner}::{holder.symbol}.")
            )
    return findings


def validate_legacy_paths(root: Path, phase: str) -> list[Finding]:
    findings: list[Finding] = []
    search_paths = [root / source_root for source_root in ("Core",) + SOURCE_ROOTS if (root / source_root).is_dir()]
    for directory in search_paths:
        for path in directory.rglob("*"):
            if not path.is_file() or (path.suffix not in SOURCE_SUFFIXES and path.name != "CMakeLists.txt"):
                continue
            rel = relative_path(path, root)
            text = read_text(path)
            for pattern, description in FIXED_FRAME_PATTERNS:
                for match in pattern.finditer(text):
                    findings.append(
                        Finding(rel, text.count("\n", 0, match.start()) + 1, f"forbidden {description}.")
                    )

            for match in GLOBAL_SYMBOL.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                if phase == "pre-delete" and rel in PRE_DELETE_DEFINITION_PATHS:
                    line_text = text.splitlines()[line - 1]
                    if not ACTIVE_GLOBAL_USE.search(line_text) and "FrameResourceManager.cpp" not in line_text:
                        continue
                findings.append(Finding(rel, line, "forbidden global/frame-count deletion path."))
    return findings


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    if args.phase not in VALID_PHASES:
        raise GateInputError(f"unsupported RHI ownership phase: {args.phase}")

    holders = discover_holders(root)
    if args.list_holders:
        if args.holder_prefix:
            holders = [holder for holder in holders if holder.path.startswith(args.holder_prefix)]
        print(json.dumps([holder.__dict__ for holder in holders], separators=(",", ":")))
        return 0

    inventory, inventory_path = load_inventory(root, args.inventory)
    findings = validate_inventory(inventory, inventory_path, holders)
    findings.extend(validate_legacy_paths(root, args.phase))
    if findings:
        for finding in sorted(findings, key=lambda item: (item.path, item.line, item.message)):
            print(f"{finding.path}:{finding.line}: {finding.message}")
        print(f"RHI ownership inventory failed with {len(findings)} finding(s).")
        return 1

    print(f"RHI ownership inventory passed: {len(holders)} exact holder(s), phase={args.phase}.")
    return 0


if __name__ == "__main__":
    sys.exit(run_gate(main))
