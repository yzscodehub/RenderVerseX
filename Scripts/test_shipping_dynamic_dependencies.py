#!/usr/bin/env python3
"""Cross-host tests for Shipping dynamic dependency inspection."""

from __future__ import annotations

import json
import re
import struct
import tempfile
import unittest
from pathlib import Path

from architecture_gate_common import GateInputError
from check_shipping_dynamic_dependencies import (
    POLICY_SCHEMA,
    CommandResult,
    DependencyPolicy,
    detect_binary_platform,
    load_dependency_policy,
    parse_elf_dependencies,
    parse_macho_dependencies,
    parse_pe_dependencies,
    scan_dynamic_dependencies,
    validate_dependencies,
)


def make_pe_fixture() -> bytes:
    data = bytearray(0x800)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    coff = 0x84
    struct.pack_into("<H", data, coff, 0x8664)
    struct.pack_into("<H", data, coff + 2, 1)
    struct.pack_into("<H", data, coff + 16, 0xF0)
    optional = coff + 20
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<I", data, optional + 108, 16)
    directories = optional + 112
    struct.pack_into("<II", data, directories + 8, 0x1100, 40)
    struct.pack_into("<II", data, directories + 13 * 8, 0x1180, 64)
    section = optional + 0xF0
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x600, 0x1000, 0x600, 0x200)

    def raw(rva: int) -> int:
        return 0x200 + rva - 0x1000

    struct.pack_into("<IIIII", data, raw(0x1100), 0, 0, 0, 0x1200, 0)
    struct.pack_into("<IIIII", data, raw(0x1114), 0, 0, 0, 0, 0)
    data[raw(0x1200):raw(0x1200) + 13] = b"KERNEL32.dll\0"
    struct.pack_into("<IIIIIIII", data, raw(0x1180), 1, 0x1220, 0, 0, 0, 0, 0, 0)
    struct.pack_into("<IIIIIIII", data, raw(0x11A0), 0, 0, 0, 0, 0, 0, 0, 0)
    data[raw(0x1220):raw(0x1220) + 15] = b"D3D12Core.dll\0"
    return bytes(data)


class ShippingDynamicDependencyTests(unittest.TestCase):
    def test_parses_pe_import_and_delay_import_without_external_tools(self) -> None:
        data = make_pe_fixture()
        self.assertEqual(detect_binary_platform(data), "windows")
        self.assertEqual(
            parse_pe_dependencies(data),
            ["D3D12Core.dll", "KERNEL32.dll"],
        )

    def test_malformed_pe_fails_closed(self) -> None:
        with self.assertRaises(GateInputError):
            parse_pe_dependencies(b"MZ" + bytes(128))

    def test_parses_linux_and_macos_tool_output_on_any_host(self) -> None:
        elf = """
 0x0000000000000001 (NEEDED)             Shared library: [libvulkan.so.1]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
"""
        macho = """RVXShippingConsumer:
\t/System/Library/Frameworks/Metal.framework/Versions/A/Metal (compatibility version 1.0.0, current version 370.65.0)
\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 1800.65.0)
"""
        self.assertEqual(parse_elf_dependencies(elf), ["libc.so.6", "libvulkan.so.1"])
        self.assertEqual(
            parse_macho_dependencies(macho),
            [
                "/System/Library/Frameworks/Metal.framework/Versions/A/Metal",
                "/usr/lib/libc++.1.dylib",
            ],
        )

    def test_cross_host_scan_uses_injected_linux_and_macos_tools(self) -> None:
        def runner(command: tuple[str, ...]) -> CommandResult:
            if command[0] == "readelf-test":
                return CommandResult(0, "(NEEDED) Shared library: [libc.so.6]", "")
            return CommandResult(
                0,
                "app:\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)\n",
                "",
            )

        self.assertEqual(
            scan_dynamic_dependencies(
                Path("app"), "linux", b"\x7fELF", runner=runner, tool="readelf-test"
            ),
            ["libc.so.6"],
        )
        self.assertEqual(
            scan_dynamic_dependencies(
                Path("app"), "macos", b"\xcf\xfa\xed\xfe", runner=runner, tool="otool-test"
            ),
            ["/usr/lib/libSystem.B.dylib"],
        )

    def test_forbidden_precedes_allow_and_unknown_is_rejected(self) -> None:
        policy = DependencyPolicy(
            allowed_exact=frozenset({"kernel32.dll", "dxcompiler.dll"}),
            allowed_patterns=(),
            forbidden_patterns=(re.compile("dxcompiler", re.IGNORECASE),),
        )
        findings = validate_dependencies(
            ["KERNEL32.dll", "dxcompiler.dll", "thirdparty.dll"],
            policy,
            platform="windows",
        )
        self.assertEqual(len(findings), 2)
        self.assertTrue(any("forbidden" in item for item in findings))
        self.assertTrue(any("unapproved" in item for item in findings))

    def test_policy_loader_is_strict_and_platform_complete(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "policy.json"
            data = {
                "schema": POLICY_SCHEMA,
                "schemaVersion": 1,
                "platforms": {
                    name: {
                        "allowedExact": [],
                        "allowedPatterns": [".*"],
                        "forbiddenPatterns": ["compiler"],
                    }
                    for name in ("windows", "linux", "macos")
                },
            }
            path.write_text(json.dumps(data), encoding="utf-8")
            policy = load_dependency_policy(path, "linux")
            self.assertEqual(len(policy.allowed_patterns), 1)
            del data["platforms"]["macos"]
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load_dependency_policy(path, "linux")

    def test_windows_policy_allows_atomic_wait_runtime_but_not_arbitrary_suffixes(self) -> None:
        policy = load_dependency_policy(
            Path(__file__).with_name("shipping-dynamic-dependency-policy.json"),
            "windows",
        )
        findings = validate_dependencies(
            ["MSVCP140_ATOMIC_WAIT.dll", "MSVCP140_UNREVIEWED.dll"],
            policy,
            platform="windows",
        )
        self.assertEqual(
            findings,
            ["unapproved Shipping dynamic dependency MSVCP140_UNREVIEWED.dll"],
        )


if __name__ == "__main__":
    unittest.main()
