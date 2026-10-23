#!/usr/bin/env python3
"""Audit direct dynamic dependencies of a Shipping executable.

PE imports are parsed directly so Windows validation does not depend on a
Visual Studio developer shell.  ELF and Mach-O use their platform-standard
inspection tools; their output parsers and policy evaluation remain testable
on every host.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Sequence

from architecture_gate_common import (
    GateInputError,
    resolve_repo_root,
    resolve_required_file,
    run_gate,
)


POLICY_SCHEMA = "RVX.ShippingDynamicDependencyPolicy"
SUPPORTED_PLATFORMS = ("windows", "linux", "macos")
MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}


@dataclass(frozen=True)
class DependencyPolicy:
    allowed_exact: frozenset[str]
    allowed_patterns: tuple[re.Pattern[str], ...]
    forbidden_patterns: tuple[re.Pattern[str], ...]


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


Runner = Callable[[Sequence[str]], CommandResult]


def _strings(value: Any, context: str) -> list[str]:
    if not isinstance(value, list) or any(
        not isinstance(item, str) or not item.strip() for item in value
    ):
        raise GateInputError(f"{context} must be an array of non-empty strings")
    if len({item.casefold() for item in value}) != len(value):
        raise GateInputError(f"{context} contains duplicate entries ignoring case")
    return [item.strip() for item in value]


def load_dependency_policy(path: Path, platform: str) -> DependencyPolicy:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise GateInputError(f"dynamic dependency policy is invalid JSON: {error}") from error
    if not isinstance(data, dict) or set(data) != {
        "schema", "schemaVersion", "platforms"
    }:
        raise GateInputError("dynamic dependency policy root fields are invalid")
    if data["schema"] != POLICY_SCHEMA or data["schemaVersion"] != 1:
        raise GateInputError(
            f"dynamic dependency policy must use {POLICY_SCHEMA} schemaVersion 1"
        )
    platforms = data["platforms"]
    if not isinstance(platforms, dict) or set(platforms) != set(SUPPORTED_PLATFORMS):
        raise GateInputError(
            "dynamic dependency policy must define windows, linux, and macos"
        )
    entry = platforms[platform]
    required = {"allowedExact", "allowedPatterns", "forbiddenPatterns"}
    if not isinstance(entry, dict) or set(entry) != required:
        raise GateInputError(f"dynamic dependency policy {platform} fields are invalid")

    flags = re.IGNORECASE if platform == "windows" else 0

    def compile_patterns(field: str) -> tuple[re.Pattern[str], ...]:
        result: list[re.Pattern[str]] = []
        for raw in _strings(entry[field], f"platforms.{platform}.{field}"):
            try:
                result.append(re.compile(raw, flags))
            except re.error as error:
                raise GateInputError(
                    f"platforms.{platform}.{field} has invalid regex {raw!r}: {error}"
                ) from error
        return tuple(result)

    exact = _strings(entry["allowedExact"], f"platforms.{platform}.allowedExact")
    if platform == "windows":
        exact = [item.casefold() for item in exact]
    return DependencyPolicy(
        allowed_exact=frozenset(exact),
        allowed_patterns=compile_patterns("allowedPatterns"),
        forbidden_patterns=compile_patterns("forbiddenPatterns"),
    )


def detect_binary_platform(data: bytes) -> str:
    if data.startswith(b"MZ"):
        return "windows"
    if data.startswith(b"\x7fELF"):
        return "linux"
    if data[:4] in MACHO_MAGICS:
        return "macos"
    raise GateInputError("Shipping binary has an unsupported or malformed file format")


def _u16(data: bytes, offset: int, label: str) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise GateInputError(f"PE {label} is outside the file")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int, label: str) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise GateInputError(f"PE {label} is outside the file")
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: bytes, offset: int, label: str) -> int:
    if offset < 0 or offset + 8 > len(data):
        raise GateInputError(f"PE {label} is outside the file")
    return struct.unpack_from("<Q", data, offset)[0]


def _cstring(data: bytes, offset: int, label: str) -> str:
    if offset < 0 or offset >= len(data):
        raise GateInputError(f"PE {label} points outside the file")
    end = data.find(b"\0", offset, min(len(data), offset + 4096))
    if end == -1:
        raise GateInputError(f"PE {label} is not null terminated")
    try:
        value = data[offset:end].decode("ascii")
    except UnicodeDecodeError as error:
        raise GateInputError(f"PE {label} is not ASCII") from error
    if not value:
        raise GateInputError(f"PE {label} is empty")
    return value


def parse_pe_dependencies(data: bytes) -> list[str]:
    if not data.startswith(b"MZ") or len(data) < 0x40:
        raise GateInputError("Shipping binary is not a valid PE image")
    pe_offset = _u32(data, 0x3C, "header offset")
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise GateInputError("Shipping binary has no valid PE signature")

    coff = pe_offset + 4
    section_count = _u16(data, coff + 2, "section count")
    optional_size = _u16(data, coff + 16, "optional header size")
    optional = coff + 20
    if optional + optional_size > len(data):
        raise GateInputError("PE optional header is truncated")
    magic = _u16(data, optional, "optional header magic")
    if magic == 0x10B:
        directory_offset = optional + 96
        image_base = _u32(data, optional + 28, "image base")
    elif magic == 0x20B:
        directory_offset = optional + 112
        image_base = _u64(data, optional + 24, "image base")
    else:
        raise GateInputError(f"unsupported PE optional header magic: 0x{magic:04x}")

    if directory_offset + 16 * 8 > optional + optional_size:
        raise GateInputError("PE data-directory table is truncated")
    section_offset = optional + optional_size
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        header = section_offset + index * 40
        if header + 40 > len(data):
            raise GateInputError("PE section table is truncated")
        virtual_size = _u32(data, header + 8, "section virtual size")
        virtual_address = _u32(data, header + 12, "section virtual address")
        raw_size = _u32(data, header + 16, "section raw size")
        raw_offset = _u32(data, header + 20, "section raw offset")
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))

    def rva_to_offset(rva: int, label: str) -> int:
        for virtual_address, virtual_size, raw_offset, raw_size in sections:
            span = max(virtual_size, raw_size)
            if virtual_address <= rva < virtual_address + span:
                result = raw_offset + (rva - virtual_address)
                if result >= len(data):
                    break
                return result
        raise GateInputError(f"PE {label} RVA 0x{rva:x} is not file-backed")

    dependencies: set[str] = set()

    import_rva = _u32(data, directory_offset + 8, "import directory RVA")
    import_size = _u32(data, directory_offset + 12, "import directory size")
    if import_rva:
        descriptor = rva_to_offset(import_rva, "import directory")
        maximum = max(1, min(65536, import_size or 65536))
        consumed = 0
        while consumed + 20 <= maximum:
            if descriptor + consumed + 20 > len(data):
                raise GateInputError("PE import descriptor table is truncated")
            fields = struct.unpack_from("<IIIII", data, descriptor + consumed)
            if not any(fields):
                break
            name_rva = fields[3]
            if not name_rva:
                raise GateInputError("PE import descriptor has no DLL name")
            dependencies.add(_cstring(
                data, rva_to_offset(name_rva, "import name"), "import DLL name"
            ))
            consumed += 20
        else:
            raise GateInputError("PE import descriptor table has no terminator")

    delay_rva = _u32(data, directory_offset + 13 * 8, "delay-import RVA")
    delay_size = _u32(data, directory_offset + 13 * 8 + 4, "delay-import size")
    if delay_rva:
        descriptor = rva_to_offset(delay_rva, "delay-import directory")
        maximum = max(1, min(65536, delay_size or 65536))
        consumed = 0
        while consumed + 32 <= maximum:
            if descriptor + consumed + 32 > len(data):
                raise GateInputError("PE delay-import descriptor table is truncated")
            fields = struct.unpack_from("<IIIIIIII", data, descriptor + consumed)
            if not any(fields):
                break
            attributes, name_value = fields[0], fields[1]
            name_rva = name_value if attributes & 1 else name_value - image_base
            if name_rva <= 0:
                raise GateInputError("PE delay-import descriptor has an invalid DLL name")
            dependencies.add(_cstring(
                data,
                rva_to_offset(name_rva, "delay-import name"),
                "delay-import DLL name",
            ))
            consumed += 32
        else:
            raise GateInputError("PE delay-import descriptor table has no terminator")

    return sorted(dependencies, key=str.casefold)


def parse_elf_dependencies(output: str) -> list[str]:
    return sorted(set(re.findall(
        r"\(NEEDED\)\s+Shared library:\s*\[([^\]]+)\]", output
    )))


def parse_macho_dependencies(output: str) -> list[str]:
    dependencies: set[str] = set()
    lines = output.splitlines()
    for line in lines[1:]:
        match = re.match(r"^\s+(\S.*?)\s+\(compatibility version ", line)
        if match:
            dependencies.add(match.group(1).strip())
    return sorted(dependencies)


def subprocess_runner(command: Sequence[str]) -> CommandResult:
    completed = subprocess.run(
        list(command),
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return CommandResult(completed.returncode, completed.stdout, completed.stderr)


def _find_tool(candidates: tuple[str, ...]) -> str:
    for candidate in candidates:
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise GateInputError(
        "required dynamic dependency inspection tool is unavailable: "
        + " or ".join(candidates)
    )


def scan_dynamic_dependencies(
    binary: Path,
    platform: str,
    data: bytes,
    *,
    runner: Runner = subprocess_runner,
    tool: str | None = None,
) -> list[str]:
    if platform == "windows":
        return parse_pe_dependencies(data)
    if platform == "linux":
        command_tool = tool or _find_tool(("readelf", "llvm-readelf"))
        result = runner((command_tool, "--dynamic", "--wide", str(binary)))
        if result.returncode != 0:
            raise GateInputError(
                f"ELF dependency inspection failed ({result.returncode}): "
                f"{result.stderr.strip()}"
            )
        return parse_elf_dependencies(result.stdout)
    if platform == "macos":
        command_tool = tool or _find_tool(("otool",))
        result = runner((command_tool, "-L", str(binary)))
        if result.returncode != 0:
            raise GateInputError(
                f"Mach-O dependency inspection failed ({result.returncode}): "
                f"{result.stderr.strip()}"
            )
        return parse_macho_dependencies(result.stdout)
    raise GateInputError(f"unsupported dependency scan platform: {platform}")


def validate_dependencies(
    dependencies: list[str],
    policy: DependencyPolicy,
    *,
    platform: str,
    extra_allowed: tuple[str, ...] = (),
) -> list[str]:
    exact = set(policy.allowed_exact)
    exact.update(
        value.casefold() if platform == "windows" else value
        for value in extra_allowed
    )
    findings: list[str] = []
    for dependency in sorted(set(dependencies), key=str.casefold):
        basename = dependency.replace("\\", "/").rsplit("/", 1)[-1]
        candidates = (dependency, basename)
        normalized = basename.casefold() if platform == "windows" else basename
        forbidden = next((
            pattern.pattern for pattern in policy.forbidden_patterns
            if any(pattern.search(candidate) for candidate in candidates)
        ), None)
        if forbidden is not None:
            findings.append(
                f"forbidden Shipping dynamic dependency {dependency} "
                f"(matched {forbidden})"
            )
            continue
        allowed = normalized in exact or any(
            pattern.fullmatch(candidate)
            for pattern in policy.allowed_patterns
            for candidate in candidates
        )
        if not allowed:
            findings.append(f"unapproved Shipping dynamic dependency {dependency}")
    return findings


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument("--binary", required=True, help="Shipping executable to inspect.")
    parser.add_argument(
        "--platform",
        choices=("auto", *SUPPORTED_PLATFORMS),
        default="auto",
    )
    parser.add_argument(
        "--policy",
        default="Scripts/shipping-dynamic-dependency-policy.json",
    )
    parser.add_argument(
        "--allow",
        action="append",
        default=[],
        help="Additional exact dependency basename approved by this product.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    binary = resolve_required_file(root, args.binary, "Shipping binary")
    policy_path = resolve_required_file(
        root, args.policy, "Shipping dynamic dependency policy"
    )
    data = binary.read_bytes()
    detected = detect_binary_platform(data)
    platform = detected if args.platform == "auto" else args.platform
    if platform != detected:
        raise GateInputError(
            f"Shipping binary format is {detected}, not requested platform {platform}"
        )
    policy = load_dependency_policy(policy_path, platform)
    dependencies = scan_dynamic_dependencies(binary, platform, data)
    findings = validate_dependencies(
        dependencies,
        policy,
        platform=platform,
        extra_allowed=tuple(args.allow),
    )
    if findings:
        print("Shipping dynamic dependency violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print(
        f"Shipping dynamic dependency scan passed for {binary} "
        f"({platform}, {len(dependencies)} direct dependencies)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
