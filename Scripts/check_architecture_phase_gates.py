#!/usr/bin/env python3
"""Validate phase-specific architecture guardrails for RenderVerseX."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl"}


@dataclass
class Finding:
    phase: str
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


def iter_sources(root: Path):
    excluded_dirs = {".git", ".claude", "build"}
    for path in root.rglob("*"):
        if any(part in excluded_dirs for part in path.parts):
            continue
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def add_regex_findings(findings: list[Finding],
                       phase: str,
                       root: Path,
                       pattern: re.Pattern[str],
                       message: str,
                       allow_path) -> None:
    for path in iter_sources(root):
        rel = path.relative_to(root)
        text = read_text(path)
        for match in pattern.finditer(text):
            if allow_path(rel, text, match):
                continue
            findings.append(Finding(phase, rel, line_number(text, match.start()), message))


def require_contains(findings: list[Finding], phase: str, root: Path, rel_path: str, needle: str, message: str) -> None:
    path = root / rel_path
    text = read_text(path)
    if needle not in text:
        findings.append(Finding(phase, Path(rel_path), 1, message))


def check_p3_actor_component(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    add_regex_findings(
        findings,
        "P3",
        root,
        re.compile(r"AddComponent\s*<\s*MeshRendererComponent\s*>"),
        "New production code must not instantiate legacy MeshRendererComponent.",
        lambda rel, _text, _match: rel.parts[0] == "Tests",
    )
    require_contains(
        findings,
        "P3",
        root,
        "Tests/ResourceInstantiationValidation/main.cpp",
        "EXPECT_FALSE(entity->HasComponent<MeshRendererComponent>())",
        "Resource instantiation must continue proving legacy MeshRendererComponent is not created.",
    )
    return findings


def check_p4_services(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    add_regex_findings(
        findings,
        "P4",
        root,
        re.compile(r"Services::Get\s*<"),
        "Services::Get is a compatibility escape hatch; prefer subsystem ownership/injection.",
        lambda rel, _text, _match: rel.as_posix() == "Core/Include/Core/Services.h",
    )
    add_regex_findings(
        findings,
        "P4",
        root,
        re.compile(r'#\s*include\s+[<"]Core/SystemManager\.h[">]'),
        "SystemManager must not spread outside the Core compatibility surface.",
        lambda rel, _text, _match: rel.parts[0] == "Core",
    )
    return findings


def check_p5_resource_runtime(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    resource_handle = read_text(root / "Resource/Include/Resource/ResourceHandle.h")
    iresource_header = read_text(root / "Resource/Include/Resource/IResource.h")
    iresource_cpp = read_text(root / "Resource/Private/IResource.cpp")

    if "Busy wait for now" in resource_handle or "while (m_resource && m_resource->IsLoading())" in resource_handle:
        findings.append(
            Finding("P5", Path("Resource/Include/Resource/ResourceHandle.h"), 1,
                    "ResourceHandle wait must not busy-wait on IsLoading.")
        )

    for needle, message in [
        ("std::condition_variable", "IResource must expose condition-variable backed load waiting."),
        ("WaitForLoadFor", "IResource must expose timeout-aware load waiting."),
    ]:
        if needle not in iresource_header:
            findings.append(Finding("P5", Path("Resource/Include/Resource/IResource.h"), 1, message))

    if "m_stateCondition.notify_all()" not in iresource_cpp:
        findings.append(
            Finding("P5", Path("Resource/Private/IResource.cpp"), 1,
                    "Resource state transitions must notify waiters.")
        )

    require_contains(
        findings,
        "P5",
        root,
        "Render/Include/Render/GPUResourceManager.h",
        "SetMemoryBudget",
        "GPU resource residency must keep an explicit memory budget control point.",
    )
    return findings


def check_p6_quality(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    required_tests = [
        ("Tests/RenderHonestyValidation/main.cpp", "QueryCapabilitiesDefaultToUnsupported"),
        ("Tests/RenderHonestyValidation/main.cpp", "PostProcessStubPassesAreUnsupportedAndDisabled"),
        ("Tests/RenderHonestyValidation/main.cpp", "SceneRendererLegacyCollectionFallbackIsRemoved"),
        ("Tests/GPUResourceManagerValidation/main.cpp", "UnsupportedTextureUploadMarksResourceFailed"),
        ("Tests/RenderGraphValidation/main.cpp", "ExecuteAsyncFallsBackToGraphicsUntilQueueSchedulerExists"),
    ]
    for rel_path, needle in required_tests:
        require_contains(
            findings,
            "P6",
            root,
            rel_path,
            needle,
            f"Quality/capability gate must keep coverage for {needle}.",
        )
    return findings


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()

    findings: list[Finding] = []
    findings.extend(check_p3_actor_component(root))
    findings.extend(check_p4_services(root))
    findings.extend(check_p5_resource_runtime(root))
    findings.extend(check_p6_quality(root))

    if findings:
        print("Architecture phase gate failures:")
        for finding in findings:
            print(f"  [{finding.phase}] {finding.path}:{finding.line}: {finding.message}")
        return 1

    print("Architecture phase gates passed.")
    print("P3 Actor/Component, P4 Service lifetime, P5 Resource runtime, and P6 Quality gates are covered.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
