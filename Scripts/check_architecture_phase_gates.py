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
        rel = path.relative_to(root)
        if any(part in excluded_dirs for part in rel.parts):
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


def require_not_contains(findings: list[Finding], phase: str, root: Path, rel_path: str, needle: str, message: str) -> None:
    path = root / rel_path
    text = read_text(path)
    offset = text.find(needle)
    if offset >= 0:
        findings.append(Finding(phase, Path(rel_path), line_number(text, offset), message))


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
        ("Tests/RHIContractValidation/main.cpp", "RejectsAsyncComputeWithoutComputePipeline"),
        ("Tests/GPUDrivenValidation/main.cpp", "GpuExecutionDecisionReportsCapabilityAndPipelineFallbacks"),
        ("Tests/GPUDrivenValidation/main.cpp", "SceneRendererFrameDiagnosticsExposeGPUDrivenExecutionDecision"),
        ("Tests/RenderPassValidation/main.cpp", "RayTracingSceneManagerReportsStructuredFallbackCodes"),
        ("Tests/RenderPassValidation/main.cpp", "SceneRendererFrameDiagnosticsExposeRayTracingSceneStats"),
        ("Tests/RenderPassValidation/main.cpp", "SceneRendererFrameDiagnosticsExposeGPUResourceStats"),
        ("Tests/RenderPassValidation/main.cpp", "ToolDiagnosticsSnapshotIsVersionedAndCarriesRenderGraphState"),
        ("Tests/RenderPassValidation/main.cpp", "ExportToolDiagnosticsText"),
        ("Tests/RenderPassValidation/main.cpp", "SaveToolDiagnosticsText"),
        ("Tests/RenderPassValidation/main.cpp", "ExportToolRenderGraphGraphviz"),
        ("Tests/RenderPassValidation/main.cpp", "SaveToolRenderGraphGraphviz"),
        ("Tests/RenderPassValidation/main.cpp", "ExportToolRenderGraphDiagnosticsText"),
        ("Tests/RenderPassValidation/main.cpp", "SaveToolRenderGraphDiagnosticsText"),
        ("Tests/RenderPassValidation/main.cpp", "ExportToolDiagnosticsManifestJson"),
        ("Tests/RenderPassValidation/main.cpp", "SaveToolDiagnosticsManifestJson"),
        ("Tests/RenderPassValidation/main.cpp", "SaveToolDiagnosticsArtifacts"),
        ("Tests/RenderPassValidation/main.cpp", "PostProcessStackReportsEffectExecutionPlanDomainsAndTargets"),
        ("Tests/RenderPassValidation/main.cpp", "SceneRendererFrameDiagnosticsExposePostProcessEffectPlans"),
        ("Tests/RenderGraphValidation/main.cpp", "ExecuteDiagnosticsRecordsGraphicsQueueTimeline"),
        ("Tests/RenderGraphValidation/main.cpp", "DiagnosticsSnapshotReportsPassResourcesLifetimesAndMemory"),
        ("Tests/RenderGraphValidation/main.cpp", "ExecuteAsyncFallsBackToGraphicsWhenBackendDoesNotSupportQueueSync"),
        ("Tests/RenderGraphValidation/main.cpp", "ExecuteAsyncSchedulesComputePassesWhenQueueSyncIsSupported"),
        ("Tests/RenderGraphValidation/main.cpp", "DiagnosticsReportsQueueBatchesAndSyncPoints"),
        ("Tests/RenderGraphValidation/main.cpp", "DiagnosticsReportsReadyListPlannedQueueBatches"),
        ("Tests/RenderGraphValidation/main.cpp", "DiagnosticsReportsPlannedSubmissionSyncGraph"),
        ("Tests/RenderGraphValidation/main.cpp", "SubmissionPlanExposesReusableReadyListPlan"),
        ("Tests/RenderGraphValidation/main.cpp", "ExecuteAsyncUsesSubmissionPlanForCrossQueueSyncStats"),
        ("Tests/RenderGraphValidation/main.cpp", "DiagnosticsReportsPlannedActualSyncCoverage"),
        ("Tests/RenderGraphValidation/main.cpp", "DiagnosticsReportsAsyncEfficiencyStats"),
        ("Tests/RenderGraphValidation/main.cpp", "ExecuteAsyncDoesNotFenceIndependentComputeAndGraphicsPasses"),
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


def check_p8_resource_package_closure(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Resource/Include/Resource/RuntimeResourcePolicy.h",
            "ResourceRuntimePolicy",
            "P8 must expose a source/cooked/package runtime resource policy.",
        ),
        (
            "Resource/Include/Resource/RuntimeResourcePolicy.h",
            "ResourceLoadFailureCode",
            "P8 runtime resource policy must expose structured failure codes.",
        ),
        (
            "Resource/Include/Resource/RuntimeResourcePolicy.h",
            "ResourceLoadDiagnostic",
            "P8 resource loads must expose structured diagnostics.",
        ),
        (
            "Resource/Include/Resource/RuntimeResourcePolicy.h",
            "MakeResourceRuntimePolicyForAppMode",
            "P8/P9 resource runtime policy must be derivable from shared app modes.",
        ),
        (
            "Resource/Include/Resource/Types/ShaderResource.h",
            "ShaderRuntimeContract",
            "P8 shader resources must expose a stable runtime contract.",
        ),
        (
            "Resource/Include/Resource/Types/ShaderResource.h",
            "GetRuntimeContractHash",
            "P8 shader resources must expose a stable runtime contract hash.",
        ),
        (
            "Resource/Include/Resource/Types/MaterialResource.h",
            "MaterialShaderContractSnapshot",
            "P8 material resources must expose shader runtime contract state.",
        ),
        (
            "Resource/Include/Resource/Types/MaterialResource.h",
            "SetShader",
            "P8 material resources must bind shader resources explicitly.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "ResourceRuntimePolicy runtimePolicy",
            "ResourceManagerConfig must carry the active runtime resource policy.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "GetLastLoadDiagnostic",
            "ResourceManager must expose the last load diagnostic for tooling and tests.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "ResolveRuntimeResourcePath",
            "ResourceManager loads must pass through the runtime resource resolver.",
        ),
        (
            "Tests/CMakeLists.txt",
            "ResourceRuntimePolicyValidation",
            "P8 runtime resource policy validation must be registered in CTest.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "RejectsSourceAssetsWhenCookedArtifactsAreRequired",
            "P8 tests must prove runtime/cooked policy rejects source assets.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "MapsCookedArtifactsThroughCookedRoot",
            "P8 tests must prove cooked artifacts resolve through the cooked root.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "ReportsMissingRuntimePackageRoot",
            "P8 tests must prove package loads fail explicitly without a mounted root.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "MapsRuntimePackageEntriesThroughMountedPackageRoot",
            "P8 tests must prove package entries resolve through a mounted package root.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "CookedShaderArtifactExposesStableRuntimeContract",
            "P8 tests must prove cooked shader artifacts expose a stable runtime contract.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "AppModeBuildsEditorPreviewAndRuntimeResourcePolicies",
            "P8/P9 tests must prove app modes resolve to deterministic resource policies.",
        ),
        (
            "Tests/MaterialSystemValidation/main.cpp",
            "MaterialResourceTracksShaderRuntimeContractAndDependency",
            "P8 tests must prove materials expose shader runtime contract dependencies.",
        ),
        (
            "Tests/MaterialSystemValidation/main.cpp",
            "MaterialResourceReportsInvalidShaderRuntimeContract",
            "P8 tests must prove materials report invalid shader runtime contracts.",
        ),
    ]:
        require_contains(findings, "P8", root, rel_path, needle, message)
    return findings


def check_p9_editor_runtime_boundary(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Core/Include/Core/App/AppMode.h",
            "enum class AppMode",
            "P9 must expose shared app-mode seams in Core.",
        ),
        (
            "Core/Include/Core/App/AppMode.h",
            "PlayInEditor",
            "P9 app-mode seams must include PIE-style runtime worlds.",
        ),
        (
            "Core/Include/Core/App/AppMode.h",
            "AppModeTraits",
            "P9 app-mode seams must expose stable mode traits.",
        ),
        (
            "Core/Include/Core/App/AppMode.h",
            "GetAppModeTraits",
            "P9 app-mode traits must be queryable by shared core code.",
        ),
        (
            "Core/Include/Core/Camera/Camera.h",
            "class Camera",
            "P9 shared viewport rendering must use a Core camera contract.",
        ),
        (
            "Runtime/Include/Runtime/Camera/Camera.h",
            "Core/Camera/Camera.h",
            "P9 Runtime camera include must remain a compatibility wrapper over Core.",
        ),
        (
            "Scripts/check_editor_runtime_boundary.py",
            "Editor/runtime shared-core boundary passed",
            "P9 must provide an editor/runtime shared-core boundary checker.",
        ),
        (
            "Tests/CMakeLists.txt",
            "Architecture.EditorRuntimeBoundary",
            "P9 editor/runtime boundary checker must be registered in CTest.",
        ),
        (
            "Tests/CMakeLists.txt",
            "AppModeBoundaryValidation",
            "P9 app-mode validation must be registered in CTest.",
        ),
        (
            "Tests/AppModeBoundaryValidation/main.cpp",
            "RuntimeModeUsesCookedRuntimeContracts",
            "P9 tests must prove runtime mode keeps editor/source access disabled.",
        ),
        (
            "Tests/AppModeBoundaryValidation/main.cpp",
            "EditorAndPreviewModesPermitAuthoringShells",
            "P9 tests must prove editor and preview modes own authoring shells.",
        ),
        (
            "Tests/AppModeBoundaryValidation/main.cpp",
            "PlayInEditorKeepsRuntimeWorldInsideEditorShell",
            "P9 tests must prove PIE keeps runtime execution distinct from editor authoring.",
        ),
        (
            "Tests/AppModeBoundaryValidation/main.cpp",
            "CameraContractLivesInCoreForSharedEditorRuntimeUse",
            "P9 tests must prove Camera is available through Core without Runtime linkage.",
        ),
    ]:
        require_contains(findings, "P9", root, rel_path, needle, message)
    return findings


def check_p10_modern_rendering_capability_closure(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "SceneRenderFeatureReport",
            "P10 must expose a coherent modern rendering feature capability report.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "SceneRenderFeatureStatus",
            "P10 feature report must classify supported/fallback/unsupported/skipped states.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "GetRenderFeatureReport",
            "P10 SceneRenderer must expose the latest feature report through public diagnostics.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "BuildRenderFeatureReport",
            "P10 SceneRenderer must build feature reports from runtime state.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "supportsRaytracing+supportsRaytracingPipeline",
            "P10 ray tracing features must map to explicit RHI capability bits.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "supportsComputePipeline+supportsDescriptorSets+supportsIndirectDrawCount",
            "P10 GPU-driven features must map to explicit RHI capability bits.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"features\\\":",
            "P10 feature report must be exported through tool diagnostics manifests.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "RenderFeatureReportMapsModernFeaturesToCapabilities",
            "P10 tests must prove feature reports classify supported, fallback, unsupported, and skipped states.",
        ),
    ]:
        require_contains(findings, "P10", root, rel_path, needle, message)
    return findings


def check_p11_resource_hot_reload_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Resource/Include/Resource/ResourceManager.h",
            "ResourceHotReloadDiagnostic",
            "P11 must expose explicit hot-reload diagnostics through ResourceManager.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "ResourceHotReloadStatus",
            "P11 hot reload must classify active, disabled, and unsupported policy states.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "ConfigureHotReload",
            "P11 ResourceManager must configure hot reload through runtime resource policy.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "RegisterHotReloadResource",
            "P11 ResourceManager must register source-loaded resources with the hot-reload watcher.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "UnsupportedRuntimePolicy",
            "P11 cooked/package runtime hot reload requests must be rejected explicitly.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "HotReloadRejectedByCookedRuntimePolicy",
            "P11 tests must prove cooked runtime policies reject hot reload explicitly.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "EditorHotReloadTracksSourceResourceLoads",
            "P11 tests must prove editor/source loads are registered for hot reload tracking.",
        ),
        (
            "Resource/Private/FileWatcher.cpp",
            "const uint32_t watchId = entry.id;",
            "P11 FileWatcher watch ids must remain stable for diagnostics.",
        ),
    ]:
        require_contains(findings, "P11", root, rel_path, needle, message)
    return findings


def check_p12_render_proxy_snapshot_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "RenderContracts/Include/RenderContracts/RenderProxy.h",
            "RVX_RENDER_PROXY_SNAPSHOT_SCHEMA_VERSION",
            "P12 RenderProxySnapshot must be schema-versioned.",
        ),
        (
            "RenderContracts/Include/RenderContracts/RenderProxy.h",
            "RenderProxySnapshotMetadata",
            "P12 RenderProxySnapshot must expose stable metadata for tools and RenderScene.",
        ),
        (
            "RenderContracts/Include/RenderContracts/RenderProxy.h",
            "RenderProxySnapshotStatus",
            "P12 RenderProxySnapshot must classify complete and incomplete snapshots.",
        ),
        (
            "RenderExtraction/Include/RenderExtraction/RenderProxySceneBridge.h",
            "snapshotSequence",
            "P12 RenderProxySceneBridge results must carry snapshot sequence metadata.",
        ),
        (
            "RenderExtraction/Private/RenderProxySceneBridge.cpp",
            "MarkComplete",
            "P12 RenderProxySceneBridge must mark successful snapshots complete.",
        ),
        (
            "RenderExtraction/Private/RenderProxySceneBridge.cpp",
            "MarkIncomplete",
            "P12 RenderProxySceneBridge must mark rejected snapshots incomplete.",
        ),
        (
            "Render/Include/Render/Renderer/RenderScene.h",
            "GetSourceSnapshotMetadata",
            "P12 RenderScene must expose the metadata for the consumed proxy snapshot.",
        ),
        (
            "Tests/RenderSceneValidation/main.cpp",
            "RenderProxyBridgePublishesCompleteSnapshotContract",
            "P12 tests must prove proxy bridge results and snapshots share a complete versioned contract.",
        ),
    ]:
        require_contains(findings, "P12", root, rel_path, needle, message)
    return findings


def check_p13_rhi_capability_report_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION",
            "P13 RHI capability reports must be schema-versioned.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "RHICapabilityReport",
            "P13 must expose a structured RHI capability report.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "RHICapabilityStatus",
            "P13 RHI capability report entries must classify supported, emulated, and unsupported states.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "BuildRHICapabilityReport",
            "P13 must build capability reports from RHICapabilities.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "supportsExplicitResourceBarriers|emulatesResourceBarriers",
            "P13 barrier capabilities must expose explicit and emulated paths.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "supportsRaytracing+supportsRaytracingPipeline",
            "P13 ray tracing report entries must map to explicit RHI capability bits.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "CapabilityReportClassifiesCoreBackendContracts",
            "P13 tests must prove capability reports classify core backend contracts.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "CapabilityReportSurfacesEmulationAndValidationFailures",
            "P13 tests must prove capability reports expose emulation and validation failures.",
        ),
    ]:
        require_contains(findings, "P13", root, rel_path, needle, message)
    return findings


def check_p14_rhi_device_capability_diagnostics(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "RHI/Include/RHI/RHIDevice.h",
            "GetCapabilityReport",
            "P14 RHI devices must expose a public capability report surface.",
        ),
        (
            "RHI/Include/RHI/RHIDevice.h",
            "ExportCapabilityReportText",
            "P14 RHI devices must expose a tool-readable capability text export.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "ExportRHICapabilityReportText",
            "P14 must provide a stable RHI capability report text exporter.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "RHI Capability Report",
            "P14 text export must identify the RHI capability report artifact.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "DeviceCapabilityReportUsesPublicDeviceCapabilities",
            "P14 tests must prove device-level reports are derived from public capabilities.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "CapabilityReportTextExportIsStableForTools",
            "P14 tests must prove text export is stable enough for tools.",
        ),
    ]:
        require_contains(findings, "P14", root, rel_path, needle, message)
    return findings


def check_p15_renderer_tool_rhi_capability_snapshot(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P15 renderer tool diagnostics schema must stay explicit for tool payloads.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "rhiCapabilityReportAvailable",
            "P15 renderer tool diagnostics must expose RHI capability report availability.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RHICapabilityReport rhiCapabilityReport",
            "P15 renderer tool diagnostics must carry the RHI capability report payload.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "device->GetCapabilityReport()",
            "P15 renderer diagnostics must capture the device-level RHI capability report.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "rhiCapabilities",
            "P15 renderer diagnostics manifest must export RHI capability data.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "ToolDiagnosticsSnapshotCarriesRHICapabilityReport",
            "P15 tests must prove renderer tool snapshots carry RHI capability reports.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "rhiCapabilityReportAvailable",
            "P15 tests must prove renderer diagnostics manifests expose RHI capability availability.",
        ),
    ]:
        require_contains(findings, "P15", root, rel_path, needle, message)
    return findings


def check_p16_render_graph_diagnostics_json_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P16 renderer tool diagnostics schema must be bumped for RenderGraph JSON artifacts.",
        ),
        (
            "Render/Include/Render/Graph/RenderGraph.h",
            "ExportDiagnosticsJson",
            "P16 RenderGraph must expose machine-readable diagnostics JSON.",
        ),
        (
            "Render/Include/Render/Graph/RenderGraph.h",
            "SaveDiagnosticsJson",
            "P16 RenderGraph must save diagnostics JSON artifacts directly.",
        ),
        (
            "Render/Private/Graph/RenderGraph.cpp",
            "\\\"plannedQueueBatches\\\"",
            "P16 RenderGraph JSON must export planned queue batches for FrameDebugger tooling.",
        ),
        (
            "Render/Private/Graph/RenderGraph.cpp",
            "\\\"queueSyncs\\\"",
            "P16 RenderGraph JSON must export actual queue syncs for profiler tooling.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "ExportToolRenderGraphDiagnosticsJson",
            "P16 SceneRenderer tooling must expose RenderGraph diagnostics JSON.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            ".rendergraph.json",
            "P16 SceneRenderer diagnostics artifacts must include a RenderGraph JSON file.",
        ),
        (
            "Tests/RenderGraphValidation/main.cpp",
            "ExportDiagnosticsJson",
            "P16 RenderGraph validation must prove diagnostics JSON export is stable.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "ExportToolRenderGraphDiagnosticsJson",
            "P16 render pass validation must prove SceneRenderer exposes RenderGraph JSON artifacts.",
        ),
    ]:
        require_contains(findings, "P16", root, rel_path, needle, message)
    return findings


def check_p17_tool_diagnostics_artifact_summary_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P17 renderer tool diagnostics schema must exist for artifact summary manifests.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P17 artifact summary JSON must have its own schema version.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "ExportToolDiagnosticsArtifactSummaryJson",
            "P17 SceneRenderer tooling must expose artifact summary JSON export.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            ".diagnostics-artifacts.json",
            "P17 SceneRenderer diagnostics artifacts must include a machine-readable artifact summary.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"artifactCount\\\"",
            "P17 artifact summary JSON must publish an artifact count for tooling.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "artifactSummaryJson",
            "P17 diagnostics manifest must advertise the artifact summary file.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "ExportToolDiagnosticsArtifactSummaryJson",
            "P17 render pass validation must prove artifact summary JSON export is stable.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "Frame001.diagnostics-artifacts.json",
            "P17 render pass validation must prove artifact summary files are saved.",
        ),
    ]:
        require_contains(findings, "P17", root, rel_path, needle, message)
    return findings


def check_p18_tool_diagnostics_artifact_integrity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "allPrimaryArtifactsSaved",
            "P18 artifact results must expose aggregate all-saved integrity.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "totalPrimaryArtifactBytes",
            "P18 artifact results must expose aggregate artifact byte size.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "GetToolArtifactFileSize",
            "P18 SceneRenderer tooling must inspect saved artifact files.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"byteSize\\\"",
            "P18 artifact summary JSON must expose per-artifact byte sizes.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"allArtifactsSaved\\\"",
            "P18 artifact summary JSON must expose aggregate all-saved state.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "RefreshToolDiagnosticsPrimaryArtifactStats",
            "P18 artifact integrity must be refreshed after saves.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "allPrimaryArtifactsSaved",
            "P18 render pass validation must prove artifact results expose all-saved state.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "totalPrimaryArtifactBytes",
            "P18 render pass validation must prove artifact byte totals are stable.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"exists\\\": true",
            "P18 render pass validation must prove artifact summary exports existence checks.",
        ),
    ]:
        require_contains(findings, "P18", root, rel_path, needle, message)
    return findings


def check_p19_tool_diagnostics_capture_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P19 renderer tool diagnostics schema must exist for capture metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P19 artifact summary schema must exist for capture metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "captureMetadataAvailable",
            "P19 artifact results must expose capture metadata availability.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "captureBaseName",
            "P19 artifact results must expose the capture base name.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "PopulateToolDiagnosticsCaptureMetadata",
            "P19 SceneRenderer artifacts must populate capture metadata before saving.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"frameIndex\\\"",
            "P19 diagnostics JSON must expose the captured frame index.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"renderGraphPassCount\\\"",
            "P19 diagnostics JSON must expose RenderGraph capture scale.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "captureMetadataAvailable",
            "P19 render pass validation must prove capture metadata is populated.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"baseName\\\": \\\"Frame001\\\"",
            "P19 render pass validation must prove capture base names are exported.",
        ),
    ]:
        require_contains(findings, "P19", root, rel_path, needle, message)
    return findings


def check_p20_tool_diagnostics_portable_artifact_paths_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P20 renderer tool diagnostics schema must be bumped for relative artifact paths.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P20 artifact summary schema must be bumped for relative artifact paths.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "toolDiagnosticsTextRelativePath",
            "P20 artifact results must expose portable relative paths.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "GetToolArtifactRelativePath",
            "P20 SceneRenderer artifacts must derive bundle-relative paths.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"relativePath\\\"",
            "P20 diagnostics JSON must export relative artifact paths.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "Frame001.rendergraph.json",
            "P20 render pass validation must prove RenderGraph JSON relative paths are stable.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"relativePath\\\": \\\"Frame001.diagnostics-artifacts.json\\\"",
            "P20 render pass validation must prove summary relative paths are exported.",
        ),
    ]:
        require_contains(findings, "P20", root, rel_path, needle, message)
    return findings


def check_p21_tool_diagnostics_artifact_hash_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P21 renderer tool diagnostics schema must be bumped for artifact content hashes.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P21 artifact summary schema must be bumped for artifact content hashes.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "toolDiagnosticsTextContentHash",
            "P21 artifact results must expose content hashes for primary artifacts.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "GetToolArtifactContentHash",
            "P21 SceneRenderer artifacts must compute stable content hashes.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"contentHash\\\"",
            "P21 artifact summary JSON must export content hashes.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "renderGraphDiagnosticsJsonContentHash.size()",
            "P21 render pass validation must prove RenderGraph JSON artifact hashes are populated.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"contentHash\\\": \\\"",
            "P21 render pass validation must prove artifact hashes are exported.",
        ),
    ]:
        require_contains(findings, "P21", root, rel_path, needle, message)
    return findings


def check_p22_tool_diagnostics_bundle_hash_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P22 renderer tool diagnostics schema must be bumped for bundle hashes.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P22 artifact summary schema must be bumped for bundle hashes.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "primaryArtifactBundleHash",
            "P22 artifact results must expose an aggregate primary artifact bundle hash.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "GetToolArtifactBundleHash",
            "P22 SceneRenderer artifacts must compute a stable bundle hash.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"primaryArtifactBundleHash\\\"",
            "P22 artifact summary JSON must export the bundle hash.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "primaryArtifactBundleHash.size()",
            "P22 render pass validation must prove bundle hashes are populated.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"primaryArtifactBundleHash\\\": \\\"",
            "P22 render pass validation must prove bundle hashes are exported.",
        ),
    ]:
        require_contains(findings, "P22", root, rel_path, needle, message)
    return findings


def check_p23_tool_diagnostics_bundle_validation_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P23 renderer tool diagnostics schema must be bumped for bundle validation.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION",
            "P23 artifact validation results must expose a schema version.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "SceneRendererToolDiagnosticsArtifactValidationResult",
            "P23 artifact validation must expose a public result contract.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "ValidateToolDiagnosticsArtifacts",
            "P23 SceneRenderer must expose a tool diagnostics artifact validation API.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "actualPrimaryArtifactBundleHash",
            "P23 artifact validation must recompute actual bundle hashes.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "allPrimaryArtifactsValid",
            "P23 render pass validation must prove valid bundles pass validation.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "modifiedValidation",
            "P23 render pass validation must prove mutated bundles fail validation.",
        ),
    ]:
        require_contains(findings, "P23", root, rel_path, needle, message)
    return findings


def check_p24_tool_diagnostics_validation_json_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P24 renderer tool diagnostics schema must be bumped for validation JSON export.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "ExportToolDiagnosticsArtifactValidationJson",
            "P24 SceneRenderer must expose validation JSON export.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "SaveToolDiagnosticsArtifactValidationJson",
            "P24 SceneRenderer must expose validation JSON save.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"allPrimaryArtifactsValid\\\"",
            "P24 validation JSON must export aggregate validity.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"diagnosticMessage\\\"",
            "P24 validation JSON must export per-artifact diagnostics.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validationJson",
            "P24 render pass validation must prove validation JSON is exported.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "modifiedValidationJson",
            "P24 render pass validation must prove failed validation JSON is exported.",
        ),
    ]:
        require_contains(findings, "P24", root, rel_path, needle, message)
    return findings


def check_p25_tool_diagnostics_validation_sidecar_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P25 renderer tool diagnostics schema must be present for validation sidecars.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P25 artifact summary schema must be present for validation sidecars.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationJsonSaved",
            "P25 artifact results must expose validation sidecar save state.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            ".diagnostics-validation.json",
            "P25 SceneRenderer must save validation JSON sidecars with stable names.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"validationReport\\\"",
            "P25 artifact summary JSON must expose the validation sidecar.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactValidationJsonContentHash",
            "P25 render pass validation must prove validation sidecar hashes are populated.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "Frame001.diagnostics-validation.json",
            "P25 render pass validation must prove validation sidecar paths are stable.",
        ),
    ]:
        require_contains(findings, "P25", root, rel_path, needle, message)
    return findings


def check_p26_tool_diagnostics_validation_capture_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P26 renderer tool diagnostics schema must be present for validation capture identity.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION",
            "P26 artifact validation schema must be present for capture identity.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "captureMetadataAvailable",
            "P26 validation results must expose capture metadata availability.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.captureBaseName",
            "P26 artifact validation must copy capture identity from saved artifacts.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"capture\\\"",
            "P26 validation JSON must export a capture object.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.captureBaseName",
            "P26 render pass validation must prove validation results carry capture identity.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"baseName\\\": \\\"Frame001\\\"",
            "P26 render pass validation must prove validation JSON exports capture identity.",
        ),
    ]:
        require_contains(findings, "P26", root, rel_path, needle, message)
    return findings


def check_p27_tool_diagnostics_summary_validation_verdict_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P27 renderer tool diagnostics schema must be present for summary validation verdicts.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P27 artifact summary schema must be present for validation verdicts.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationAllPrimaryArtifactsValid",
            "P27 artifact results must expose validation verdicts for summary tooling.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "PopulateToolDiagnosticsValidationSummaryStats",
            "P27 SceneRenderer must copy validation results into artifact summary state.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"allPrimaryArtifactsValid\\\"",
            "P27 artifact summary JSON must export validation verdicts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactValidationAllPrimaryArtifactsValid",
            "P27 render pass validation must prove artifact results carry validation verdicts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"resultAvailable\\\": true",
            "P27 render pass validation must prove artifact summaries export validation verdicts.",
        ),
    ]:
        require_contains(findings, "P27", root, rel_path, needle, message)
    return findings


def check_p28_tool_diagnostics_manifest_validation_sidecar_discovery_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P28 renderer tool diagnostics schema must be present for manifest validation sidecar discovery.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"artifactValidationJson\\\"",
            "P28 diagnostics manifest must expose the validation sidecar discovery node.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "ToolDiagnosticsArtifactValidationJson",
            "P28 diagnostics manifest must classify the validation sidecar kind.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"artifactValidationJson\\\": {",
            "P28 render pass validation must prove manifests expose the validation sidecar node.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"relativePath\\\": \\\"Frame001.diagnostics-validation.json\\\"",
            "P28 render pass validation must prove manifests expose the validation sidecar relative path.",
        ),
    ]:
        require_contains(findings, "P28", root, rel_path, needle, message)
    return findings


def check_p29_tool_diagnostics_manifest_artifact_type_metadata_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P29 renderer tool diagnostics schema must be present for manifest artifact type metadata.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"kind\\\": \\\"SceneRendererText\\\"",
            "P29 diagnostics manifest must classify scene renderer text artifacts.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"contentType\\\": \\\"text/vnd.graphviz\\\"",
            "P29 diagnostics manifest must expose Graphviz artifact content type.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "ToolDiagnosticsArtifactSummaryJson",
            "P29 diagnostics manifest must classify the artifact summary sidecar.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"kind\\\": \\\"RenderGraphDiagnosticsJson\\\"",
            "P29 render pass validation must prove manifests classify RenderGraph JSON artifacts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"kind\\\": \\\"ToolDiagnosticsManifestJson\\\"",
            "P29 render pass validation must prove manifests classify manifest artifacts.",
        ),
    ]:
        require_contains(findings, "P29", root, rel_path, needle, message)
    return findings


def check_p30_tool_diagnostics_manifest_sidecar_schema_metadata_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P30 renderer tool diagnostics schema must be present for manifest sidecar schema metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactSummarySchemaVersion",
            "P30 artifact results must expose artifact summary schema version.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationSchemaVersion",
            "P30 artifact results must expose artifact validation schema version.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"artifactSummarySchemaVersion\\\"",
            "P30 diagnostics manifest must export artifact summary schema version.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"artifactValidationSchemaVersion\\\"",
            "P30 diagnostics manifest must export artifact validation schema version.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactSummarySchemaVersion",
            "P30 render pass validation must prove artifact results carry sidecar schema versions.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"artifactValidationSchemaVersion\\\"",
            "P30 render pass validation must prove manifests export validation sidecar schema version.",
        ),
    ]:
        require_contains(findings, "P30", root, rel_path, needle, message)
    return findings


def check_p31_tool_diagnostics_stable_capture_id_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P31 renderer tool diagnostics schema must remain versioned for stable capture ids.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P31 artifact summary schema must remain versioned for capture ids.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION",
            "P31 artifact validation schema must remain versioned for capture ids.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "captureId",
            "P31 artifact contracts must expose a stable capture id.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "GetToolDiagnosticsCaptureId",
            "P31 SceneRenderer must compute deterministic capture ids.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"captureId\\\"",
            "P31 tool JSON exports must include capture ids.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.captureId.size()",
            "P31 render pass validation must prove capture ids are populated.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.captureId",
            "P31 render pass validation must prove validation reports carry capture ids.",
        ),
    ]:
        require_contains(findings, "P31", root, rel_path, needle, message)
    return findings


def check_p32_tool_diagnostics_summary_sidecar_schema_metadata_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P32 renderer tool diagnostics schema must remain versioned for summary sidecar schema metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P32 artifact summary schema must remain versioned for summary sidecar schema metadata.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"artifactSummarySchemaVersion\\\"",
            "P32 artifact summary JSON must export the summary sidecar schema version.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"artifactValidationSchemaVersion\\\"",
            "P32 artifact summary JSON must export the validation sidecar schema version.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"artifactSummarySchemaVersion\\\":",
            "P32 render pass validation must prove summaries carry the summary sidecar schema version.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"artifactValidationSchemaVersion\\\":",
            "P32 render pass validation must prove summaries carry the validation sidecar schema version.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"validationReport\\\": {",
            "P32 render pass validation must prove summary validation reports carry sidecar schema versions.",
        ),
    ]:
        require_contains(findings, "P32", root, rel_path, needle, message)
    return findings


def check_p33_tool_diagnostics_validation_sidecar_schema_metadata_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P33 renderer tool diagnostics schema must remain versioned for validation sidecar schema metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION",
            "P33 artifact validation schema must remain versioned for validation sidecar schema metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactSummarySchemaVersion",
            "P33 validation result contract must carry the summary sidecar schema version.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.artifactSummarySchemaVersion = artifacts.artifactSummarySchemaVersion",
            "P33 validation must copy the summary sidecar schema version from artifact results.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"artifactValidationSchemaVersion\\\"",
            "P33 validation JSON must export the validation sidecar schema version.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.artifactSummarySchemaVersion",
            "P33 render pass validation must prove validation results carry sidecar schema versions.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"artifactValidationSchemaVersion\\\":",
            "P33 render pass validation must prove validation JSON exports the validation sidecar schema version.",
        ),
    ]:
        require_contains(findings, "P33", root, rel_path, needle, message)
    return findings


def check_p34_tool_diagnostics_json_sidecar_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION",
            "P34 renderer tool diagnostics schema must remain versioned for JSON sidecar identity metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION",
            "P34 artifact summary schema must remain versioned for JSON sidecar identity metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION",
            "P34 artifact validation schema must remain versioned for JSON sidecar identity metadata.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"kind\\\": \\\"ToolDiagnosticsManifestJson\\\"",
            "P34 manifest JSON must self-identify its artifact kind.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"kind\\\": \\\"ToolDiagnosticsArtifactSummaryJson\\\"",
            "P34 artifact summary JSON must self-identify its artifact kind.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"kind\\\": \\\"ToolDiagnosticsArtifactValidationJson\\\"",
            "P34 artifact validation JSON must self-identify its artifact kind.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"contentType\\\": \\\"application/json\\\"",
            "P34 JSON sidecars must self-identify their content type.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"kind\\\": \\\"ToolDiagnosticsManifestJson\\\"",
            "P34 render pass validation must prove manifest JSON exports top-level artifact kind.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"kind\\\": \\\"ToolDiagnosticsArtifactSummaryJson\\\"",
            "P34 render pass validation must prove summary JSON exports top-level artifact kind.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"kind\\\": \\\"ToolDiagnosticsArtifactValidationJson\\\"",
            "P34 render pass validation must prove validation JSON exports top-level artifact kind.",
        ),
    ]:
        require_contains(findings, "P34", root, rel_path, needle, message)
    return findings


def check_p35_tool_diagnostics_json_sidecar_artifact_id_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION = 24",
            "Renderer tool diagnostics schema must stay bumped past the JSON sidecar artifact id contract.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P35 artifact summary schema must be bumped for JSON sidecar artifact ids.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P35 artifact validation schema must be bumped for JSON sidecar artifact ids.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"id\\\": \\\"manifestJson\\\"",
            "P35 manifest JSON must self-identify its stable artifact id.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"id\\\": \\\"artifactSummaryJson\\\"",
            "P35 artifact summary JSON must self-identify its stable artifact id.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"id\\\": \\\"artifactValidationJson\\\"",
            "P35 artifact validation JSON must self-identify its stable artifact id.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"id\\\": \\\"manifestJson\\\"",
            "P35 render pass validation must prove manifest JSON exports top-level artifact id.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"id\\\": \\\"artifactSummaryJson\\\"",
            "P35 render pass validation must prove summary JSON exports top-level artifact id.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"id\\\": \\\"artifactValidationJson\\\"",
            "P35 render pass validation must prove validation JSON exports top-level artifact id.",
        ),
    ]:
        require_contains(findings, "P35", root, rel_path, needle, message)
    return findings


def check_p36_render_graph_diagnostics_json_artifact_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Graph/RenderGraph.h",
            "RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION = 3",
            "P36 RenderGraph diagnostics schema must be bumped for JSON artifact identity metadata.",
        ),
        (
            "Render/Private/Graph/RenderGraph.cpp",
            "\\\"id\\\": \\\"renderGraphDiagnosticsJson\\\"",
            "P36 RenderGraph diagnostics JSON must self-identify its stable artifact id.",
        ),
        (
            "Render/Private/Graph/RenderGraph.cpp",
            "\\\"kind\\\": \\\"RenderGraphDiagnosticsJson\\\"",
            "P36 RenderGraph diagnostics JSON must self-identify its artifact kind.",
        ),
        (
            "Render/Private/Graph/RenderGraph.cpp",
            "\\\"contentType\\\": \\\"application/json\\\"",
            "P36 RenderGraph diagnostics JSON must self-identify its content type.",
        ),
        (
            "Tests/RenderGraphValidation/main.cpp",
            "\\\"id\\\": \\\"renderGraphDiagnosticsJson\\\"",
            "P36 RenderGraph validation must prove diagnostics JSON exports top-level artifact id.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"kind\\\": \\\"RenderGraphDiagnosticsJson\\\"",
            "P36 render pass validation must prove SceneRenderer preserves RenderGraph JSON kind metadata.",
        ),
    ]:
        require_contains(findings, "P36", root, rel_path, needle, message)
    return findings


def check_p37_render_graph_diagnostics_schema_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Graph/RenderGraph.h",
            "RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID",
            "P37 RenderGraph diagnostics must expose a stable schema id constant.",
        ),
        (
            "Render/Private/Graph/RenderGraph.cpp",
            "\\\"schemaId\\\":",
            "P37 RenderGraph diagnostics JSON must export its schema id.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "renderGraphDiagnosticsSchemaId",
            "P37 SceneRenderer artifact contracts must carry the RenderGraph diagnostics schema id.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.renderGraphDiagnosticsSchemaId = artifacts.renderGraphDiagnosticsSchemaId",
            "P37 artifact validation must preserve the RenderGraph diagnostics schema id.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"renderGraphDiagnosticsSchemaId\\\"",
            "P37 manifest, summary, and validation JSON must export the RenderGraph diagnostics schema id.",
        ),
        (
            "Tests/RenderGraphValidation/main.cpp",
            "RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID",
            "P37 RenderGraph validation must prove diagnostics schema id exposure.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"renderGraphDiagnosticsSchemaId\\\": \\\"RVX.RenderGraph.Diagnostics\\\"",
            "P37 render pass validation must prove tool sidecars carry the RenderGraph diagnostics schema id.",
        ),
    ]:
        require_contains(findings, "P37", root, rel_path, needle, message)
    return findings


def check_p38_validation_entry_artifact_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P38 artifact validation schema must be bumped for entry-level artifact identity.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string kind;",
            "P38 validation entries must carry artifact kind metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string contentType;",
            "P38 validation entries must carry artifact content type metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string schemaId;",
            "P38 validation entries must carry schema identity metadata for versioned artifacts.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "entry.kind = source.kind",
            "P38 artifact validation must copy entry artifact kind from validation sources.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"contentType\\\":",
            "P38 validation JSON entries must export artifact content type metadata.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"schemaId\\\":",
            "P38 validation JSON entries must export schema identity for versioned artifacts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "entry.kind, \"RenderGraphDiagnosticsJson\"",
            "P38 render pass validation must prove validation entries preserve artifact kind.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"kind\\\": \\\"RenderGraphDiagnosticsJson\\\"",
            "P38 validation JSON must expose RenderGraph artifact entry kind.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"schemaId\\\": \\\"RVX.RenderGraph.Diagnostics\\\"",
            "P38 validation JSON must expose RenderGraph artifact entry schema id.",
        ),
    ]:
        require_contains(findings, "P38", root, rel_path, needle, message)
    return findings


def check_p39_validation_entry_artifact_identity_verification_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "bool identityChecked",
            "P39 validation entries must report whether artifact identity was checked.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "bool schemaMatches",
            "P39 validation entries must report whether artifact schema metadata matched.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "JsonExtractStringField",
            "P39 validation must inspect saved JSON artifact identity fields.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "entry.identityMatches",
            "P39 validation must compute artifact identity match state.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "artifact identity metadata mismatch",
            "P39 validation must produce a dedicated identity mismatch diagnostic.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"identityChecked\\\"",
            "P39 validation JSON must export identity check state.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"schemaMatches\\\"",
            "P39 validation JSON must export schema match state.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "EXPECT_FALSE(entry.identityMatches)",
            "P39 render pass validation must prove JSON identity mismatch is detected.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"identityMatches\\\": false",
            "P39 validation JSON must expose failed identity matches.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifact identity metadata mismatch",
            "P39 render pass validation must prove identity mismatch diagnostics are exported.",
        ),
    ]:
        require_contains(findings, "P39", root, rel_path, needle, message)
    return findings


def check_p40_validation_entry_actual_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P40 artifact validation schema must be bumped for actual artifact identity values.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string actualId;",
            "P40 validation entries must carry the actual artifact id read from JSON.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "uint32 actualSchemaVersion",
            "P40 validation entries must carry the actual schema version read from JSON.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "entry.actualId = JsonExtractStringField",
            "P40 validation must extract actual artifact id metadata.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "JsonExtractUIntField",
            "P40 validation must extract actual numeric schema metadata.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"actualId\\\"",
            "P40 validation JSON must export actual artifact id metadata.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"actualSchemaVersion\\\"",
            "P40 validation JSON must export actual schema version metadata.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "entry.actualId, \"unexpectedRenderGraphDiagnosticsJson\"",
            "P40 render pass validation must prove actual mismatched artifact id is captured.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"actualId\\\": \\\"unexpectedRenderGraphDiagnosticsJson\\\"",
            "P40 validation JSON must expose actual mismatched artifact id.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"actualSchemaVersion\\\": 3",
            "P40 validation JSON must expose actual RenderGraph schema version.",
        ),
    ]:
        require_contains(findings, "P40", root, rel_path, needle, message)
    return findings


def check_p41_validation_entry_diagnostic_code_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P41 artifact validation schema must be bumped for stable diagnostic codes.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string diagnosticCode",
            "P41 validation entries must carry a stable machine-readable diagnostic code.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "IdentityMetadataMismatch",
            "P41 validation must classify identity metadata mismatches with a stable code.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "ByteSizeMismatch",
            "P41 validation must classify byte-size mismatches with a stable code.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"diagnosticCode\\\"",
            "P41 validation JSON must export the stable diagnostic code.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "entry.diagnosticCode, \"IdentityMetadataMismatch\"",
            "P41 render pass validation must prove identity mismatch diagnostic code is set.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"diagnosticCode\\\": \\\"IdentityMetadataMismatch\\\"",
            "P41 validation JSON must expose identity mismatch diagnostic code.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"diagnosticCode\\\": \\\"ByteSizeMismatch\\\"",
            "P41 validation JSON must expose byte-size mismatch diagnostic code.",
        ),
    ]:
        require_contains(findings, "P41", root, rel_path, needle, message)
    return findings


def check_p42_validation_diagnostic_code_summary_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P42 artifact validation schema must be bumped for diagnostic code summaries.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "diagnosticCodeCounts",
            "P42 validation result must expose aggregate diagnostic code counts.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "AccumulateToolArtifactDiagnosticCodeCount",
            "P42 validation must aggregate diagnostic codes while validating artifacts.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"diagnosticCodeCounts\\\"",
            "P42 validation JSON must export aggregate diagnostic code counts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.diagnosticCodeCounts",
            "P42 render pass validation must prove successful diagnostic code summaries.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"code\\\": \\\"IdentityMetadataMismatch\\\"",
            "P42 render pass validation must prove identity mismatch summary codes are exported.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"code\\\": \\\"ByteSizeMismatch\\\"",
            "P42 render pass validation must prove byte-size mismatch summary codes are exported.",
        ),
    ]:
        require_contains(findings, "P42", root, rel_path, needle, message)
    return findings


def check_p43_artifact_summary_validation_diagnostic_code_snapshot_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P43 artifact summary schema must be bumped for validation diagnostic code snapshots.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationDiagnosticCodeCounts",
            "P43 artifact results must carry validation diagnostic code counts for summary export.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "result.artifactValidationDiagnosticCodeCounts = validation.diagnosticCodeCounts",
            "P43 validation summary stats must preserve diagnostic code counts.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"diagnosticCodeCounts\\\"",
            "P43 artifact summary JSON must export validation diagnostic code counts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.artifactValidationDiagnosticCodeCounts",
            "P43 render pass validation must prove artifact results preserve validation code counts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"diagnosticCodeCounts\\\": []",
            "P43 render pass validation must prove empty artifact summaries export an empty code count array.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"artifactSummarySchemaVersion\\\": 24",
            "P43 render pass validation must prove summary schema metadata is bumped.",
        ),
    ]:
        require_contains(findings, "P43", root, rel_path, needle, message)
    return findings


def check_p44_manifest_validation_verdict_scope_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactManifestJson.find(\"\\\"validationReport\\\": {\")",
            "P44 render pass validation must prove manifests stay free of validation verdict payloads.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactManifestJson.find(\"\\\"diagnosticCodeCounts\\\": [\")",
            "P44 render pass validation must prove manifests stay free of diagnostic code summaries.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Manifest Validation Verdict Scope Guardrail",
            "P44 implementation plan must document the manifest discovery-only boundary.",
        ),
    ]:
        require_contains(findings, "P44", root, rel_path, needle, message)
    return findings


def check_p45_validation_verdict_code_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P45 artifact validation schema must be bumped for top-level verdict codes.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string verdictCode",
            "P45 validation results must expose a stable top-level verdict code.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationVerdictCode",
            "P45 artifact results must preserve validation verdict codes for summary export.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "GetToolArtifactValidationVerdictCode",
            "P45 validation must compute verdict codes from aggregate validation state.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"verdictCode\\\"",
            "P45 validation and summary JSON must export verdict codes.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.verdictCode, \"Valid\"",
            "P45 render pass validation must prove successful validations report Valid.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "bundleMismatchValidation.verdictCode, \"BundleHashMismatch\"",
            "P45 render pass validation must prove bundle-only mismatches report BundleHashMismatch.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "identityMismatchValidation.verdictCode, \"InvalidArtifacts\"",
            "P45 render pass validation must prove entry failures report InvalidArtifacts.",
        ),
    ]:
        require_contains(findings, "P45", root, rel_path, needle, message)
    return findings


def check_p46_validation_primary_failure_summary_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P46 artifact validation schema must be bumped for primary failure summaries.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string primaryFailureCode",
            "P46 validation results must expose a top-level primary failure code.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationPrimaryFailureArtifactId",
            "P46 artifact results must preserve the primary failure artifact id for summary export.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "PopulateToolArtifactValidationPrimaryFailure",
            "P46 validation must compute primary failure summaries from validation entries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"primaryFailureArtifactId\\\"",
            "P46 validation and summary JSON must export primary failure artifact ids.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.primaryFailureCode, \"None\"",
            "P46 render pass validation must prove successful validations report no primary failure.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "identityMismatchValidation.primaryFailureArtifactId, \"renderGraphDiagnosticsJson\"",
            "P46 render pass validation must prove entry failures identify the primary artifact.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "modifiedValidation.primaryFailureCode, \"ByteSizeMismatch\"",
            "P46 render pass validation must prove byte-size failures identify the primary failure code.",
        ),
    ]:
        require_contains(findings, "P46", root, rel_path, needle, message)
    return findings


def check_p47_validation_primary_failure_detail_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P47 artifact summary schema must be bumped for primary failure detail snapshots.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P47 artifact validation schema must be bumped for primary failure detail fields.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string primaryFailureArtifactRelativePath",
            "P47 validation results must expose the primary failing artifact relative path.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationPrimaryFailureMessage",
            "P47 artifact results must preserve the primary failure message for summary export.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.primaryFailureMessage = failedEntry.diagnosticMessage",
            "P47 primary failure detail must carry the actionable validation entry message.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"primaryFailureArtifactRelativePath\\\"",
            "P47 validation and summary JSON must export the primary failing artifact relative path.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"primaryFailureMessage\\\"",
            "P47 validation and summary JSON must export the primary failure message.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "identityMismatchValidation.primaryFailureArtifactRelativePath, \"Frame001.rendergraph.json\"",
            "P47 render pass validation must prove identity failures expose the failing artifact path.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "modifiedValidation.primaryFailureMessage, \"artifact byte size mismatch\"",
            "P47 render pass validation must prove byte-size failures expose the primary failure message.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"primaryFailureMessage\\\": \\\"primary artifact bundle hash mismatch\\\"",
            "P47 render pass validation must prove bundle hash mismatches expose a top-level failure message.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Primary Failure Detail",
            "P47 implementation plan must document the primary failure detail contract.",
        ),
    ]:
        require_contains(findings, "P47", root, rel_path, needle, message)
    return findings


def check_p48_validation_primary_failure_artifact_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P48 artifact summary schema must be bumped for primary failure artifact identity snapshots.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P48 artifact validation schema must be bumped for primary failure artifact identity fields.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string primaryFailureArtifactKind",
            "P48 validation results must expose the primary failing artifact kind.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationPrimaryFailureArtifactSchemaVersion",
            "P48 artifact results must preserve primary failure artifact schema metadata for summaries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.primaryFailureArtifactKind = failedEntry.kind",
            "P48 primary failure identity must copy artifact kind from the failed validation entry.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"primaryFailureArtifactContentType\\\"",
            "P48 validation and summary JSON must export primary failure artifact content type.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"primaryFailureArtifactSchemaVersion\\\"",
            "P48 validation and summary JSON must export primary failure artifact schema version.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "identityMismatchValidation.primaryFailureArtifactKind, \"RenderGraphDiagnosticsJson\"",
            "P48 render pass validation must prove identity failures expose primary artifact kind.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "modifiedValidation.primaryFailureArtifactSchemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID",
            "P48 render pass validation must prove byte-size failures expose primary artifact schema id.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"primaryFailureArtifactSchemaVersion\\\": 3",
            "P48 render pass validation must prove primary failure artifact schema version is exported.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Primary Failure Artifact Identity",
            "P48 implementation plan must document the primary failure artifact identity contract.",
        ),
    ]:
        require_contains(findings, "P48", root, rel_path, needle, message)
    return findings


def check_p49_validation_primary_failure_entry_index_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P49 artifact summary schema must be bumped for primary failure entry index snapshots.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P49 artifact validation schema must be bumped for primary failure entry index fields.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "uint32 primaryFailureEntryIndex = RVX_INVALID_INDEX",
            "P49 validation results must expose a nullable/sentinel primary failure entry index.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationPrimaryFailureEntryIndex",
            "P49 artifact results must preserve the primary failure entry index for summary export.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.primaryFailureEntryIndex = static_cast<uint32>(entryIndex)",
            "P49 primary failure computation must capture the failed validation entry index.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "JsonOptionalIndex",
            "P49 JSON export must write null for missing entry indices.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "identityMismatchValidation.primaryFailureEntryIndex, 3u",
            "P49 render pass validation must prove identity failures expose the entry index.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "bundleMismatchValidation.primaryFailureEntryIndex, RVX_INVALID_INDEX",
            "P49 render pass validation must prove bundle-only mismatches have no entry index.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"primaryFailureEntryIndex\\\": 3",
            "P49 render pass validation must prove entry indices are exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Primary Failure Entry Index",
            "P49 implementation plan must document the primary failure entry index contract.",
        ),
    ]:
        require_contains(findings, "P49", root, rel_path, needle, message)
    return findings


def check_p50_validation_failure_count_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P50 artifact summary schema must be bumped for validation failure counts.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P50 artifact validation schema must be bumped for validation failure counts.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "uint32 failedPrimaryArtifactCount = 0",
            "P50 validation results must expose the failed primary artifact count.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationFailedPrimaryArtifactCount",
            "P50 artifact results must preserve the failed primary artifact count for summaries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "++validation.failedPrimaryArtifactCount",
            "P50 validation must count invalid primary artifacts while checking entries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "artifactValidationFailedPrimaryArtifactCount = validation.failedPrimaryArtifactCount",
            "P50 artifact summary stats must copy the failed primary artifact count.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"failedPrimaryArtifactCount\\\"",
            "P50 summary and validation JSON must export failed primary artifact counts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.artifactValidationFailedPrimaryArtifactCount, 0u",
            "P50 render pass validation must prove successful artifact captures have zero failures.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "identityMismatchValidation.failedPrimaryArtifactCount, 1u",
            "P50 render pass validation must prove entry-level identity failures are counted.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"failedPrimaryArtifactCount\\\": 1",
            "P50 render pass validation must prove failed counts are exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Failure Count Summary",
            "P50 implementation plan must document the validation failure count contract.",
        ),
    ]:
        require_contains(findings, "P50", root, rel_path, needle, message)
    return findings


def check_p51_validation_entry_index_metadata_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P51 artifact validation schema must be bumped for entry index metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "uint32 entryIndex = RVX_INVALID_INDEX",
            "P51 validation entries must expose their stable zero-based entry index.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "entry.entryIndex = static_cast<uint32>(sourceIndex)",
            "P51 validation must assign entry indices while constructing validation entries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"entryIndex\\\"",
            "P51 validation JSON must export entry indices.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "entry.entryIndex, static_cast<uint32>(entryIndex)",
            "P51 render pass validation must prove successful entries preserve array indices.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "entry.entryIndex, 3u",
            "P51 render pass validation must prove renderGraphDiagnosticsJson keeps index 3.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"entryIndex\\\": 3",
            "P51 render pass validation must prove entry indices are exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Entry Index Metadata",
            "P51 implementation plan must document validation entry index metadata.",
        ),
    ]:
        require_contains(findings, "P51", root, rel_path, needle, message)
    return findings


def check_p52_validation_entry_primary_failure_flag_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P52 artifact validation schema must be bumped for entry primary failure flags.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "bool primaryFailure = false",
            "P52 validation entries must expose whether they are the primary failure.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "entry.primaryFailure = false",
            "P52 primary failure computation must reset per-entry primary failure flags.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "failedEntry.primaryFailure = true",
            "P52 primary failure computation must mark the first failed entry.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"primaryFailure\\\"",
            "P52 validation JSON must export entry primary failure flags.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "EXPECT_FALSE(entry.primaryFailure)",
            "P52 render pass validation must prove valid and non-entry failures do not mark entries.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "EXPECT_TRUE(entry.primaryFailure)",
            "P52 render pass validation must prove entry-level failures mark the failing entry.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"primaryFailure\\\": true",
            "P52 render pass validation must prove primary failure flags are exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Entry Primary Failure Flag",
            "P52 implementation plan must document validation entry primary failure flags.",
        ),
    ]:
        require_contains(findings, "P52", root, rel_path, needle, message)
    return findings


def check_p53_validation_primary_failure_entry_count_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P53 artifact summary schema must be bumped for primary failure entry count snapshots.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P53 artifact validation schema must be bumped for primary failure entry counts.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "uint32 primaryFailureEntryCount = 0",
            "P53 validation results must expose the count of entries marked as primary failure.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationPrimaryFailureEntryCount",
            "P53 artifact results must preserve the primary failure entry count for summaries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.primaryFailureEntryCount = 0",
            "P53 primary failure computation must reset the marked-entry count.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.primaryFailureEntryCount = 1",
            "P53 primary failure computation must count the first failed entry marker.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "artifactValidationPrimaryFailureEntryCount = validation.primaryFailureEntryCount",
            "P53 artifact summary stats must copy the marked-entry count.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"primaryFailureEntryCount\\\"",
            "P53 validation and summary JSON must export the marked-entry count.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.artifactValidationPrimaryFailureEntryCount, 0u",
            "P53 render pass validation must prove valid captures have no marked primary failure entry.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "bundleMismatchValidation.primaryFailureEntryCount, 0u",
            "P53 render pass validation must prove bundle-only mismatches have no marked entry.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "identityMismatchValidation.primaryFailureEntryCount, 1u",
            "P53 render pass validation must prove entry-level failures mark exactly one entry.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"primaryFailureEntryCount\\\": 1",
            "P53 render pass validation must prove marked-entry counts are exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Primary Failure Entry Count",
            "P53 implementation plan must document primary failure entry count snapshots.",
        ),
    ]:
        require_contains(findings, "P53", root, rel_path, needle, message)
    return findings


def check_p54_validation_entry_count_summary_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P54 artifact summary schema must be bumped for validation entry count snapshots.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P54 artifact validation schema must be bumped for validation entry counts.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "uint32 entryCount = 0",
            "P54 validation results must expose the generated validation entry count.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationEntryCount",
            "P54 artifact results must preserve the validation entry count for summaries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.entryCount = static_cast<uint32>(validation.entries.size())",
            "P54 validation must derive entryCount from the generated entries array.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "artifactValidationEntryCount = validation.entryCount",
            "P54 artifact summary stats must copy the validation entry count.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"entryCount\\\"",
            "P54 validation and summary JSON must export the validation entry count.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.artifactValidationEntryCount, 5u",
            "P54 render pass validation must prove artifact summaries preserve entry counts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.entryCount, 5u",
            "P54 render pass validation must prove validation results count generated entries.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "bundleMismatchValidation.entryCount, 5u",
            "P54 render pass validation must prove bundle-only failures keep entry counts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"entryCount\\\": 5",
            "P54 render pass validation must prove validation entry counts are exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Entry Count Summary",
            "P54 implementation plan must document validation entry count snapshots.",
        ),
    ]:
        require_contains(findings, "P54", root, rel_path, needle, message)
    return findings


def check_p55_validation_entry_coverage_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P55 artifact summary schema must be bumped for validation entry coverage state.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P55 artifact validation schema must be bumped for validation entry coverage state.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "entryCountMatchesCheckedPrimaryArtifactCount",
            "P55 validation results must expose entry coverage completeness.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount",
            "P55 artifact results must preserve entry coverage completeness for summaries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.entryCount == validation.checkedPrimaryArtifactCount",
            "P55 validation must compare generated entry count against checked primary artifacts.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount",
            "P55 artifact summary stats must copy entry coverage completeness.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"entryCountMatchesCheckedPrimaryArtifactCount\\\"",
            "P55 validation and summary JSON must export entry coverage completeness.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount",
            "P55 render pass validation must prove artifact summaries preserve entry coverage.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.entryCountMatchesCheckedPrimaryArtifactCount",
            "P55 render pass validation must prove validation results expose entry coverage.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "bundleMismatchValidation.entryCountMatchesCheckedPrimaryArtifactCount",
            "P55 render pass validation must prove bundle-only failures keep entry coverage.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"entryCountMatchesCheckedPrimaryArtifactCount\\\": true",
            "P55 render pass validation must prove entry coverage is exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Entry Coverage Summary",
            "P55 implementation plan must document validation entry coverage snapshots.",
        ),
    ]:
        require_contains(findings, "P55", root, rel_path, needle, message)
    return findings


def check_p56_validation_entry_coverage_code_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P56 artifact summary schema must be bumped for validation entry coverage codes.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P56 artifact validation schema must be bumped for validation entry coverage codes.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string entryCoverageCode = \"Unavailable\"",
            "P56 validation results must expose a stable entry coverage code.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationEntryCoverageCode",
            "P56 artifact results must preserve the entry coverage code for summaries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "GetToolArtifactValidationEntryCoverageCode",
            "P56 validation must compute entry coverage code through a stable helper.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "return validation.entryCountMatchesCheckedPrimaryArtifactCount ? \"Complete\" : \"Mismatch\"",
            "P56 validation must distinguish complete and mismatched coverage.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "artifactValidationEntryCoverageCode = validation.entryCoverageCode",
            "P56 artifact summary stats must copy the entry coverage code.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"entryCoverageCode\\\"",
            "P56 validation and summary JSON must export the entry coverage code.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.artifactValidationEntryCoverageCode, \"Complete\"",
            "P56 render pass validation must prove artifact summaries preserve complete coverage codes.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.entryCoverageCode, \"Complete\"",
            "P56 render pass validation must prove validation results expose complete coverage codes.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"entryCoverageCode\\\": \\\"Unavailable\\\"",
            "P56 render pass validation must prove unavailable summary coverage code is exported.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"entryCoverageCode\\\": \\\"Complete\\\"",
            "P56 render pass validation must prove complete coverage code is exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Entry Coverage Code",
            "P56 implementation plan must document validation entry coverage codes.",
        ),
    ]:
        require_contains(findings, "P56", root, rel_path, needle, message)
    return findings


def check_p57_validation_entry_coverage_message_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION = 24",
            "P57 artifact summary schema must be bumped for validation entry coverage messages.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION = 25",
            "P57 artifact validation schema must be bumped for validation entry coverage messages.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "std::string entryCoverageMessage = \"validation result is unavailable\"",
            "P57 validation results must expose a user-facing entry coverage message.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "artifactValidationEntryCoverageMessage",
            "P57 artifact results must preserve the entry coverage message for summaries.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "GetToolArtifactValidationEntryCoverageMessage",
            "P57 validation must compute entry coverage message through a stable helper.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation entries cover all checked primary artifacts",
            "P57 validation must describe complete entry coverage.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation entry count does not match checked primary artifact count",
            "P57 validation must describe mismatched entry coverage.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "artifactValidationEntryCoverageMessage = validation.entryCoverageMessage",
            "P57 artifact summary stats must copy the entry coverage message.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"entryCoverageMessage\\\"",
            "P57 validation and summary JSON must export the entry coverage message.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.artifactValidationEntryCoverageMessage",
            "P57 render pass validation must prove artifact summaries preserve coverage messages.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.entryCoverageMessage",
            "P57 render pass validation must prove validation results expose coverage messages.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"entryCoverageMessage\\\": \\\"validation result is unavailable\\\"",
            "P57 render pass validation must prove unavailable summary coverage message is exported.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"entryCoverageMessage\\\": \\\"validation entries cover all checked primary artifacts\\\"",
            "P57 render pass validation must prove complete coverage message is exported to JSON.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Entry Coverage Message",
            "P57 implementation plan must document validation entry coverage messages.",
        ),
    ]:
        require_contains(findings, "P57", root, rel_path, needle, message)
    return findings


def check_p58_validation_entry_coverage_helper_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "const char* GetToolArtifactValidationEntryCoverageCode",
            "P58 validation must centralize entry coverage code mapping in a helper.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "const char* GetToolArtifactValidationEntryCoverageMessage",
            "P58 validation must centralize entry coverage message mapping in a helper.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.entryCoverageCode = GetToolArtifactValidationEntryCoverageCode(validation)",
            "P58 validation must assign entry coverage code through the helper.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "validation.entryCoverageMessage = GetToolArtifactValidationEntryCoverageMessage(validation)",
            "P58 validation must assign entry coverage message through the helper.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "return \"validation entry count does not match checked primary artifact count\"",
            "P58 validation helper must preserve the mismatch explanation.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.entryCoverageCode, \"Complete\"",
            "P58 render pass validation must keep proving helper-backed complete coverage codes.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "validation.entryCoverageMessage, \"validation entries cover all checked primary artifacts\"",
            "P58 render pass validation must keep proving helper-backed complete coverage messages.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "Tool Diagnostics Validation Entry Coverage Helper",
            "P58 implementation plan must document centralized entry coverage helpers.",
        ),
    ]:
        require_contains(findings, "P58", root, rel_path, needle, message)
    return findings


def check_p59_architecture_baseline_contract_coverage(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Scripts/run_architecture_baseline.ps1",
            "Architecture\\.CMakeModuleVisibility",
            "P59 architecture baseline must run CMake module visibility checks.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "Architecture\\.EditorRuntimeBoundary",
            "P59 architecture baseline must run editor/runtime shared-core boundary checks.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "RHIContractValidation\\.",
            "P59 architecture baseline must run the public RHI contract suite.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "AppModeBoundaryValidation\\.",
            "P59 architecture baseline must run app-mode shared-core contract tests.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "ResourceRuntimePolicyValidation\\.",
            "P59 architecture baseline must run resource runtime policy contract tests.",
        ),
    ]:
        require_contains(findings, "P59", root, rel_path, needle, message)
    return findings


def check_p60_cmake_module_link_boundary_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Scripts/check_cmake_module_links.py",
            "target_link_libraries",
            "P60 CMake link boundary checker must scan target_link_libraries blocks.",
        ),
        (
            "Scripts/check_cmake_module_links.py",
            "module-boundaries.json",
            "P60 CMake link boundary checker must use the shared module boundary config.",
        ),
        (
            "Scripts/check_cmake_module_links.py",
            "discover_aliases",
            "P60 CMake link boundary checker must resolve project target aliases.",
        ),
        (
            "Scripts/check_cmake_module_links.py",
            "CMake module link boundary violations",
            "P60 CMake link boundary checker must report link-boundary violations.",
        ),
        (
            "Tests/CMakeLists.txt",
            "Architecture.CMakeModuleLinks",
            "P60 CMake module link boundary checker must be registered in CTest.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "Architecture\\.CMakeModuleLinks",
            "P60 architecture baseline must run CMake module link boundary checks.",
        ),
    ]:
        require_contains(findings, "P60", root, rel_path, needle, message)

    for rel_path, needle, message in [
        (
            "Physics/CMakeLists.txt",
            "RVX::Geometry",
            "P60 Physics must not keep an unused Geometry target link.",
        ),
        (
            "Picking/CMakeLists.txt",
            "\n    Spatial",
            "P60 Picking compatibility target must not keep an unused Spatial target link.",
        ),
        (
            "RHI_DX11/CMakeLists.txt",
            "RVX::ShaderCompiler",
            "P60 DX11 backend must not link ShaderCompiler through the RHI backend target.",
        ),
        (
            "RHI_DX12/CMakeLists.txt",
            "RVX::ShaderCompiler",
            "P60 DX12 backend must not link ShaderCompiler through the RHI backend target.",
        ),
        (
            "RHI_Metal/CMakeLists.txt",
            "RVX::ShaderCompiler",
            "P60 Metal backend must not link ShaderCompiler through the RHI backend target.",
        ),
        (
            "RHI_Vulkan/CMakeLists.txt",
            "RVX::ShaderCompiler",
            "P60 Vulkan backend must not link ShaderCompiler through the RHI backend target.",
        ),
        (
            "Terrain/CMakeLists.txt",
            "\n    Spatial",
            "P60 Terrain must not keep an unused Spatial target link.",
        ),
        (
            "UI/CMakeLists.txt",
            "RVX::Render",
            "P60 UI must stay on the Core/RHI boundary instead of linking Render.",
        ),
        (
            "Water/CMakeLists.txt",
            "\n    Spatial",
            "P60 Water must not keep an unused Spatial target link.",
        ),
    ]:
        require_not_contains(findings, "P60", root, rel_path, needle, message)

    return findings


def check_p61_cmake_module_include_edge_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Scripts/check_cmake_module_include_edges.py",
            "target_include_directories",
            "P61 CMake include-edge checker must scan target_include_directories blocks.",
        ),
        (
            "Scripts/check_cmake_module_include_edges.py",
            "module-boundaries.json",
            "P61 CMake include-edge checker must use the shared module boundary config.",
        ),
        (
            "Scripts/check_cmake_module_include_edges.py",
            "cmake_include_module",
            "P61 CMake include-edge checker must map include directories back to modules.",
        ),
        (
            "Scripts/check_cmake_module_include_edges.py",
            "CMake module include-directory boundary violations",
            "P61 CMake include-edge checker must report include-directory boundary violations.",
        ),
        (
            "Tests/CMakeLists.txt",
            "Architecture.CMakeModuleIncludeEdges",
            "P61 CMake include-edge boundary checker must be registered in CTest.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "Architecture\\.CMakeModuleIncludeEdges",
            "P61 architecture baseline must run CMake include-edge boundary checks.",
        ),
    ]:
        require_contains(findings, "P61", root, rel_path, needle, message)

    for rel_path, needle, message in [
        (
            "Terrain/CMakeLists.txt",
            "${CMAKE_SOURCE_DIR}/Engine/Include",
            "P61 Terrain must not add Engine include directories directly.",
        ),
        (
            "Terrain/CMakeLists.txt",
            "${CMAKE_SOURCE_DIR}/World/Include",
            "P61 Terrain must not add World include directories directly.",
        ),
        (
            "Terrain/CMakeLists.txt",
            "${CMAKE_SOURCE_DIR}/Resource/Include",
            "P61 Terrain must not add Resource include directories directly.",
        ),
        (
            "Terrain/CMakeLists.txt",
            "${CMAKE_SOURCE_DIR}/Geometry/Include",
            "P61 Terrain must not add Geometry include directories directly.",
        ),
        (
            "Water/CMakeLists.txt",
            "${CMAKE_SOURCE_DIR}/Engine/Include",
            "P61 Water must not add Engine include directories directly.",
        ),
        (
            "Water/CMakeLists.txt",
            "${CMAKE_SOURCE_DIR}/World/Include",
            "P61 Water must not add World include directories directly.",
        ),
        (
            "Water/CMakeLists.txt",
            "${CMAKE_SOURCE_DIR}/Resource/Include",
            "P61 Water must not add Resource include directories directly.",
        ),
    ]:
        require_not_contains(findings, "P61", root, rel_path, needle, message)

    return findings


def check_p62_module_boundary_manifest_coverage_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Scripts/check_module_boundary_manifest.py",
            "projectIncludePrefixes",
            "P62 module-boundary manifest checker must validate project include prefixes.",
        ),
        (
            "Scripts/check_module_boundary_manifest.py",
            "allowed edges",
            "P62 module-boundary manifest checker must validate allowed dependency edges.",
        ),
        (
            "Scripts/check_module_boundary_manifest.py",
            "validate_cmake_coverage",
            "P62 module-boundary manifest checker must validate CMake project target coverage.",
        ),
        (
            "Scripts/check_module_boundary_manifest.py",
            "Module boundary manifest failures",
            "P62 module-boundary manifest checker must report manifest failures.",
        ),
        (
            "Tests/CMakeLists.txt",
            "Architecture.ModuleBoundaryManifest",
            "P62 module-boundary manifest checker must be registered in CTest.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "Architecture\\.ModuleBoundaryManifest",
            "P62 architecture baseline must run module-boundary manifest coverage checks.",
        ),
    ]:
        require_contains(findings, "P62", root, rel_path, needle, message)
    return findings


def check_p63_public_header_linkage_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Scripts/check_public_header_linkage.py",
            "scan_public_includes",
            "P63 public-header linkage checker must scan public module headers.",
        ),
        (
            "Scripts/check_public_header_linkage.py",
            "build_public_link_edges",
            "P63 public-header linkage checker must inspect PUBLIC and INTERFACE CMake links.",
        ),
        (
            "Scripts/check_public_header_linkage.py",
            "Public header linkage violations",
            "P63 public-header linkage checker must report uncovered public include edges.",
        ),
        (
            "Tests/CMakeLists.txt",
            "Architecture.PublicHeaderLinkage",
            "P63 public-header linkage checker must be registered in CTest.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "Architecture\\.PublicHeaderLinkage",
            "P63 architecture baseline must run public-header linkage checks.",
        ),
        (
            "Tools/CMakeLists.txt",
            "RVX::Audio",
            "P63 Tools must publicly link Audio because a public Tools header exposes Audio types.",
        ),
    ]:
        require_contains(findings, "P63", root, rel_path, needle, message)

    require_not_contains(
        findings,
        "P63",
        root,
        "Tools/CMakeLists.txt",
        "${CMAKE_SOURCE_DIR}/Audio/Include",
        "P63 Tools must not paper over public Audio headers with a private include directory.",
    )
    return findings


def check_p64_public_include_directory_scope_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Scripts/check_cmake_module_include_edges.py",
            "publicly_exposes_cross_module_include",
            "P64 CMake include-edge checker must classify public cross-module include directory exposure.",
        ),
        (
            "Scripts/check_cmake_module_include_edges.py",
            "CMake public include-directory exposure violations",
            "P64 CMake include-edge checker must report public include-directory exposure violations.",
        ),
        (
            "Scripts/check_cmake_module_include_edges.py",
            "No PUBLIC or INTERFACE include directories expose another module directly.",
            "P64 CMake include-edge checker must report clean public include-directory scope.",
        ),
        (
            "Tests/CMakeLists.txt",
            "Architecture.CMakeModuleIncludeEdges",
            "P64 public include-directory scope must stay covered by the include-edge CTest.",
        ),
        (
            "Scripts/run_architecture_baseline.ps1",
            "Architecture\\.CMakeModuleIncludeEdges",
            "P64 architecture baseline must keep running CMake include-edge scope checks.",
        ),
    ]:
        require_contains(findings, "P64", root, rel_path, needle, message)
    return findings


def check_p65_rhi_render_graph_baseline_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P65: RHI RenderGraph Baseline Capability Contract",
            "P65 must document the RHI RenderGraph baseline capability contract.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION = 3",
            "P65 must bump the RHI capability report schema for baseline readiness.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "renderGraphBaselineSupported",
            "P65 capability reports must expose RenderGraph baseline readiness.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "renderGraphBaselineMissingRequirements",
            "P65 capability reports must expose missing RenderGraph baseline requirements.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "BuildRenderGraphBaselineMissingRequirements",
            "P65 must compute RenderGraph baseline requirements in one helper path.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "RenderGraphBaselineMissing:",
            "P65 text export must include missing RenderGraph baseline requirements.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "CapabilityReportExposesRenderGraphBaselineReadiness",
            "P65 tests must prove RenderGraph baseline readiness and missing requirements.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "RenderGraphBaseline: Passed",
            "P65 tests must prove the text export reports baseline readiness.",
        ),
    ]:
        require_contains(findings, "P65", root, rel_path, needle, message)
    return findings


def check_p66_renderer_rhi_baseline_diagnostics_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P66: Renderer Tool RHI RenderGraph Baseline Diagnostics",
            "P66 must document renderer tool propagation of RHI RenderGraph baseline diagnostics.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION = 24",
            "P66 must bump renderer tool diagnostics schema for RHI baseline payload fields.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "renderGraphBaseline=",
            "P66 text diagnostics must export the RHI RenderGraph baseline verdict.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "RenderGraphBaselineMissing:",
            "P66 text diagnostics must export missing RHI RenderGraph baseline requirements.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "renderGraphBaselineSupported",
            "P66 manifest diagnostics must export the RHI RenderGraph baseline verdict.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "renderGraphBaselineMissingRequirements",
            "P66 manifest diagnostics must export missing RHI RenderGraph baseline requirements.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "renderGraphBaseline=Passed",
            "P66 tests must prove text diagnostics expose RHI RenderGraph baseline readiness.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"renderGraphBaselineSupported\\\": true",
            "P66 tests must prove manifest diagnostics expose RHI RenderGraph baseline readiness.",
        ),
    ]:
        require_contains(findings, "P66", root, rel_path, needle, message)
    return findings


def check_p67_cross_backend_rhi_baseline_report_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P67: Cross-Backend RHI Baseline Report Consistency",
            "P67 must document cross-backend RHI baseline report consistency.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "DeviceCapabilityReportRenderGraphBaselineConsistency",
            "P67 must add a cross-backend GPU validation test for RHI baseline reports.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "device->GetCapabilityReport()",
            "P67 cross-backend validation must read the public device capability report.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION",
            "P67 cross-backend validation must assert the current RHI capability report schema.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "report.renderGraphBaselineSupported",
            "P67 cross-backend validation must assert RenderGraph baseline support.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "report.renderGraphBaselineMissingRequirements.empty()",
            "P67 cross-backend validation must assert no missing RenderGraph baseline requirements.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "device->ExportCapabilityReportText()",
            "P67 cross-backend validation must inspect the public capability text export.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "RenderGraphBaseline: Passed",
            "P67 cross-backend validation must prove text export reports baseline success.",
        ),
    ]:
        require_contains(findings, "P67", root, rel_path, needle, message)
    return findings


def check_p68_rhi_capability_report_identity_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P68: RHI Capability Report Backend Identity Metadata",
            "P68 must document RHI capability report backend identity metadata.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION = 3",
            "P68 must bump the RHI capability report schema for backend identity metadata.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "std::string adapterName",
            "P68 RHI capability reports must expose adapter identity.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "std::string driverVersion",
            "P68 RHI capability reports must expose driver identity when available.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "report.adapterName = capabilities.adapterName",
            "P68 report construction must copy adapter identity from public capabilities.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "DriverVersion:",
            "P68 text export must expose driver identity metadata.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION = 24",
            "P68 must bump renderer tool diagnostics schema for RHI identity payload fields.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "adapterName",
            "P68 renderer manifest diagnostics must export RHI adapter identity.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "driverVersion",
            "P68 renderer manifest diagnostics must export RHI driver identity.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "OpenGL Test Adapter",
            "P68 RHI contract tests must prove report/text export preserve adapter identity.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"adapterName\\\": \\\"RenderPassValidation Test Adapter\\\"",
            "P68 render pass tests must prove renderer manifest exports adapter identity.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "EXPECT_EQ(report.adapterName, caps.adapterName)",
            "P68 cross-backend validation must compare report adapter identity to device capabilities.",
        ),
    ]:
        require_contains(findings, "P68", root, rel_path, needle, message)
    return findings


def check_p69_rhi_capability_report_json_artifact_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P69: RHI Capability Report JSON Artifact",
            "P69 must document standalone RHI capability report JSON artifacts.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID",
            "P69 RHI capability reports must expose a stable schema id.",
        ),
        (
            "RHI/Include/RHI/RHICapabilities.h",
            "ExportRHICapabilityReportJson",
            "P69 must expose a standalone RHI capability report JSON exporter.",
        ),
        (
            "RHI/Include/RHI/RHIDevice.h",
            "ExportCapabilityReportJson",
            "P69 RHI devices must expose a default JSON capability report API.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "RHICapabilityReportJson",
            "P69 JSON export must identify the RHI capability report artifact kind.",
        ),
        (
            "RHI/Private/RHIValidation.cpp",
            "\\\"renderGraphBaseline\\\"",
            "P69 JSON export must include RenderGraph baseline diagnostics.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "CapabilityReportJsonExportIsStableForTools",
            "P69 RHI contract tests must prove JSON export is stable for tools.",
        ),
        (
            "Tests/RHIContractValidation/main.cpp",
            "\\\"schemaId\\\": \\\"RVX.RHI.CapabilityReport\\\"",
            "P69 RHI contract tests must prove JSON export carries the schema id.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "device->ExportCapabilityReportJson()",
            "P69 cross-backend validation must inspect the public device JSON export.",
        ),
        (
            "Tests/CrossBackendValidation/main.cpp",
            "\\\"kind\\\": \\\"RHICapabilityReportJson\\\"",
            "P69 cross-backend validation must prove JSON export carries the artifact kind.",
        ),
    ]:
        require_contains(findings, "P69", root, rel_path, needle, message)
    return findings


def check_p70_renderer_rhi_capability_json_sidecar_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P70: Renderer Tool RHI Capability JSON Sidecar",
            "P70 must document renderer-saved RHI capability JSON sidecars.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "ExportToolRHICapabilityReportJson",
            "P70 SceneRenderer must expose a renderer-level RHI capability JSON export.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "SaveToolRHICapabilityReportJson",
            "P70 SceneRenderer must expose a renderer-level RHI capability JSON save helper.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "rhiCapabilityReportJsonExpected",
            "P70 artifact results must track conditional RHI capability JSON sidecar expectation.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "rhiCapabilityReportJsonContentHash",
            "P70 artifact results must track RHI capability JSON integrity metadata.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            ".rhi-capabilities.json",
            "P70 saved diagnostics bundles must use a stable RHI capability JSON sidecar filename.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "ExportRHICapabilityReportJson(m_toolDiagnosticsSnapshot.rhiCapabilityReport)",
            "P70 renderer export must reuse the public RHI capability JSON serializer.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "\\\"rhiCapabilityReportJson\\\"",
            "P70 manifest/summary/validation output must identify the RHI capability JSON sidecar.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID",
            "P70 renderer artifact validation must reuse the RHI capability report schema id.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "RHIFrame001.rhi-capabilities.json",
            "P70 render pass validation must prove the RHI sidecar filename is saved and discoverable.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "\\\"kind\\\": \\\"RHICapabilityReportJson\\\"",
            "P70 render pass validation must prove the RHI sidecar carries artifact kind metadata.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "artifactResult.primaryArtifactCount, 6u",
            "P70 render pass validation must prove RHI-capable bundles include six primary artifacts.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "entry.id == \"rhiCapabilityReportJson\"",
            "P70 render pass validation must inspect the RHI sidecar validation entry.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "entry.actualSchemaId, RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID",
            "P70 render pass validation must prove sidecar schema metadata is verified.",
        ),
    ]:
        require_contains(findings, "P70", root, rel_path, needle, message)
    return findings


def check_p71_shader_runtime_contract_json_artifact_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P71: Shader Runtime Contract JSON Artifact",
            "P71 must document shader runtime contract JSON artifacts.",
        ),
        (
            "Resource/Include/Resource/Types/ShaderResource.h",
            "RVX_SHADER_RUNTIME_CONTRACT_SCHEMA_ID",
            "P71 shader runtime contracts must expose a stable schema id.",
        ),
        (
            "Resource/Include/Resource/Types/ShaderResource.h",
            "ExportRuntimeContractJson",
            "P71 ShaderResource must expose a runtime contract JSON exporter.",
        ),
        (
            "Resource/Include/Resource/Types/ShaderResource.h",
            "SaveRuntimeContractJson",
            "P71 ShaderResource must expose a runtime contract JSON save helper.",
        ),
        (
            "Resource/Private/Types/ShaderResource.cpp",
            "ShaderRuntimeContractJson",
            "P71 JSON export must identify the shader runtime contract artifact kind.",
        ),
        (
            "Resource/Private/Types/ShaderResource.cpp",
            "GetShaderRuntimeContractStatusName",
            "P71 JSON export must use stable shader contract status names.",
        ),
        (
            "Resource/Private/Types/ShaderResource.cpp",
            "\\\"payloadHash\\\"",
            "P71 JSON export must include shader payload identity.",
        ),
        (
            "Resource/Private/Types/ShaderResource.cpp",
            "\\\"reflectionResourceCount\\\"",
            "P71 JSON export must include shader reflection resource count.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "ShaderRuntimeContractJsonReportsInvalidContracts",
            "P71 tests must prove invalid shader contracts still export diagnostic JSON.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "fullscreen.shader-contract.json",
            "P71 tests must prove shader contract JSON save round trips to disk.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "\\\"schemaId\\\": \\\"RVX.Resource.ShaderRuntimeContract\\\"",
            "P71 tests must prove shader contract JSON carries the schema id.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "\\\"status\\\": \\\"MissingSourcePath\\\"",
            "P71 tests must prove invalid shader contract status is exported.",
        ),
    ]:
        require_contains(findings, "P71", root, rel_path, needle, message)
    return findings


def check_p72_material_shader_contract_snapshot_json_artifact_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P72: Material Shader Contract Snapshot JSON Artifact",
            "P72 must document material shader contract snapshot JSON artifacts.",
        ),
        (
            "Resource/Include/Resource/Types/MaterialResource.h",
            "RVX_MATERIAL_SHADER_CONTRACT_SNAPSHOT_SCHEMA_ID",
            "P72 material shader contract snapshots must expose a stable schema id.",
        ),
        (
            "Resource/Include/Resource/Types/MaterialResource.h",
            "ExportShaderContractSnapshotJson",
            "P72 MaterialResource must expose a shader contract snapshot JSON exporter.",
        ),
        (
            "Resource/Include/Resource/Types/MaterialResource.h",
            "SaveShaderContractSnapshotJson",
            "P72 MaterialResource must expose a shader contract snapshot JSON save helper.",
        ),
        (
            "Resource/Private/Types/MaterialResource.cpp",
            "MaterialShaderContractSnapshotJson",
            "P72 JSON export must identify the material shader contract snapshot artifact kind.",
        ),
        (
            "Resource/Private/Types/MaterialResource.cpp",
            "\\\"shader\\\"",
            "P72 JSON export must include shader assignment and contract state.",
        ),
        (
            "Resource/Private/Types/MaterialResource.cpp",
            "\\\"contractHash\\\"",
            "P72 JSON export must include shader contract hash.",
        ),
        (
            "Resource/Private/Types/MaterialResource.cpp",
            "\\\"payloadHash\\\"",
            "P72 JSON export must include shader payload hash.",
        ),
        (
            "Tests/MaterialSystemValidation/main.cpp",
            "MaterialResourceShaderContractSnapshotJsonReportsMissingShader",
            "P72 tests must prove missing shader snapshots export diagnostic JSON.",
        ),
        (
            "Tests/MaterialSystemValidation/main.cpp",
            "SaveShaderContractSnapshotJson",
            "P72 tests must prove material shader contract snapshot JSON saves to disk.",
        ),
        (
            "Tests/MaterialSystemValidation/main.cpp",
            "\\\"schemaId\\\": \\\"RVX.Resource.MaterialShaderContractSnapshot\\\"",
            "P72 tests must prove material snapshot JSON carries the schema id.",
        ),
        (
            "Tests/MaterialSystemValidation/main.cpp",
            "\\\"contractValid\\\": false",
            "P72 tests must prove invalid and missing shader contracts are exported.",
        ),
    ]:
        require_contains(findings, "P72", root, rel_path, needle, message)
    return findings


def check_p73_resource_load_diagnostic_json_artifact_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P73: Resource Load Diagnostic JSON Artifact",
            "P73 must document resource load diagnostic JSON artifacts.",
        ),
        (
            "Resource/Include/Resource/RuntimeResourcePolicy.h",
            "RVX_RESOURCE_LOAD_DIAGNOSTIC_SCHEMA_ID",
            "P73 resource load diagnostics must expose a stable schema id.",
        ),
        (
            "Resource/Include/Resource/RuntimeResourcePolicy.h",
            "ExportResourceLoadDiagnosticJson",
            "P73 must expose a standalone resource load diagnostic JSON exporter.",
        ),
        (
            "Resource/Include/Resource/RuntimeResourcePolicy.h",
            "SaveResourceLoadDiagnosticJson",
            "P73 must expose a standalone resource load diagnostic JSON save helper.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "ExportLastLoadDiagnosticJson",
            "P73 ResourceManager must expose a last-load diagnostic JSON exporter.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "SaveLastLoadDiagnosticJson",
            "P73 ResourceManager must expose a last-load diagnostic JSON save helper.",
        ),
        (
            "Resource/Private/RuntimeResourcePolicy.cpp",
            "ResourceLoadDiagnosticJson",
            "P73 JSON export must identify the resource load diagnostic artifact kind.",
        ),
        (
            "Resource/Private/RuntimeResourcePolicy.cpp",
            "\\\"failureCode\\\"",
            "P73 JSON export must include structured failure code metadata.",
        ),
        (
            "Resource/Private/RuntimeResourcePolicy.cpp",
            "\\\"runtimePackageRead\\\"",
            "P73 JSON export must include source/cooked/package read flags.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "ExportResourceLoadDiagnosticJson(GetLastLoadDiagnostic())",
            "P73 ResourceManager JSON export must use the standalone diagnostic serializer.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "\\\"schemaId\\\": \\\"RVX.Resource.LoadDiagnostic\\\"",
            "P73 tests must prove resource load diagnostic JSON carries the schema id.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "\\\"failure\\\": \\\"CookedArtifactRequired\\\"",
            "P73 tests must prove denied loads export structured failure metadata.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "SaveLastLoadDiagnosticJson",
            "P73 tests must prove ResourceManager last-load diagnostics save to disk.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "SaveResourceLoadDiagnosticJson",
            "P73 tests must prove standalone load diagnostics save to disk.",
        ),
    ]:
        require_contains(findings, "P73", root, rel_path, needle, message)
    return findings


def check_p74_resource_hot_reload_diagnostic_json_artifact_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P74: Resource Hot Reload Diagnostic JSON Artifact",
            "P74 must document resource hot reload diagnostic JSON artifacts.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_ID",
            "P74 resource hot reload diagnostics must expose a stable schema id.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "ExportResourceHotReloadDiagnosticJson",
            "P74 must expose a standalone hot reload diagnostic JSON exporter.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "SaveResourceHotReloadDiagnosticJson",
            "P74 must expose a standalone hot reload diagnostic JSON save helper.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "ExportHotReloadDiagnosticJson",
            "P74 ResourceManager must expose a current hot reload diagnostic JSON exporter.",
        ),
        (
            "Resource/Include/Resource/ResourceManager.h",
            "SaveHotReloadDiagnosticJson",
            "P74 ResourceManager must expose a current hot reload diagnostic JSON save helper.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "resourceHotReloadDiagnosticJson",
            "P74 JSON export must identify the resource hot reload diagnostic artifact id.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "ResourceHotReloadDiagnosticJson",
            "P74 JSON export must identify the resource hot reload diagnostic artifact kind.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "watcherInitialized",
            "P74 JSON export must include watcher initialization state.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "registeredResourceCount",
            "P74 JSON export must include tracked resource counts.",
        ),
        (
            "Resource/Private/ResourceManager.cpp",
            "ExportResourceHotReloadDiagnosticJson(GetHotReloadDiagnostic())",
            "P74 ResourceManager JSON export must use the standalone diagnostic serializer.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "\\\"schemaId\\\": \\\"RVX.Resource.HotReloadDiagnostic\\\"",
            "P74 tests must prove hot reload diagnostic JSON carries the schema id.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "\\\"status\\\": \\\"UnsupportedRuntimePolicy\\\"",
            "P74 tests must prove cooked runtime policy rejection is exported.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "\\\"status\\\": \\\"Active\\\"",
            "P74 tests must prove active editor hot reload state is exported.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "SaveHotReloadDiagnosticJson",
            "P74 tests must prove ResourceManager hot reload diagnostics save to disk.",
        ),
        (
            "Tests/ResourceRuntimePolicyValidation/main.cpp",
            "SaveResourceHotReloadDiagnosticJson",
            "P74 tests must prove standalone hot reload diagnostics save to disk.",
        ),
    ]:
        require_contains(findings, "P74", root, rel_path, needle, message)
    return findings


def check_p75_p80_runtime_visible_rendering_contract(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for rel_path, needle, message in [
        (
            "Docs/architecture-implementation-plan.md",
            "### P75: Runtime Visible Output Baseline",
            "P75 must document the runtime visible output baseline.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "### P76: PostProcess Shader/PSO Readiness Diagnostics",
            "P76 must document post-process shader/PSO readiness diagnostics.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "### P77: PostProcess Frame Input Contract",
            "P77 must document the post-process frame input contract.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "### P78: TAA Minimal Runtime Path",
            "P78 must document the minimal TAA runtime path.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "### P79: Depth and Motion Effect Input Contracts",
            "P79 must document depth/motion effect input contracts.",
        ),
        (
            "Docs/architecture-implementation-plan.md",
            "### P80: SSAO and SSR Opt-In Fallback Diagnostics",
            "P80 must document SSAO/SSR opt-in fallback diagnostics.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "settings.toneMappingOperator = ToneMappingOperator::ACES;",
            "P75 runtime defaults must use ACES tone mapping.",
        ),
        (
            "Render/Private/Renderer/SceneRenderer.cpp",
            "settings.enableFXAA = false;",
            "P75 must keep FXAA as a fallback instead of the default AA mainline.",
        ),
        (
            "Render/Include/Render/Renderer/SceneRenderer.h",
            "postProcessFallbackCopyApplied",
            "P75 SceneRenderer diagnostics must expose post-process fallback copy state.",
        ),
        (
            "Render/Include/Render/PostProcess/PostProcessStack.h",
            "pipelineReady",
            "P76 effect execution plans must expose pipeline readiness.",
        ),
        (
            "Render/Include/Render/PostProcess/PostProcessStack.h",
            "PostProcessFrameInputs",
            "P77 must expose a post-process frame input contract.",
        ),
        (
            "Render/Include/Render/PostProcess/PostProcessStack.h",
            "PostProcessFrameInputRequirements",
            "P77 effects must declare frame input requirements.",
        ),
        (
            "Render/Include/Render/PostProcess/TAA.h",
            "TAAResolveStats",
            "P78 TAA must expose minimal resolve diagnostics.",
        ),
        (
            "Render/Private/PostProcess/TAA.cpp",
            "ctx.CopyTexture(currentColor, m_result.Get())",
            "P78 TAA minimal resolve must copy current color to the result.",
        ),
        (
            "Render/Include/Render/PostProcess/DOF.h",
            "requiresDepth = true",
            "P79 DOF must declare a depth input requirement.",
        ),
        (
            "Render/Include/Render/PostProcess/MotionBlur.h",
            "requiresVelocity = true",
            "P79 MotionBlur must declare a velocity input requirement.",
        ),
        (
            "Render/Include/Render/PostProcess/SSAO.h",
            "SSAOComputeStats",
            "P80 SSAO must expose opt-in fallback diagnostics.",
        ),
        (
            "Render/Include/Render/PostProcess/SSR.h",
            "SSRComputeStats",
            "P80 SSR must expose opt-in fallback diagnostics.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "TAAMinimalResolveCopiesCurrentFrameAndUpdatesHistory",
            "P78 tests must prove TAA minimal resolve updates history.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "PostProcessFrameInputContractReportsMissingVelocityDepthAndHistory",
            "P77 tests must prove missing post-process frame inputs are reported.",
        ),
        (
            "Tests/RenderPassValidation/main.cpp",
            "StandaloneSSAOAndSSRReportMissingFrameInputs",
            "P80 tests must prove SSAO/SSR missing inputs are reported.",
        ),
    ]:
        require_contains(findings, "P75-P80", root, rel_path, needle, message)
    return findings


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()

    findings: list[Finding] = []
    findings.extend(check_p3_actor_component(root))
    findings.extend(check_p4_services(root))
    findings.extend(check_p5_resource_runtime(root))
    findings.extend(check_p6_quality(root))
    findings.extend(check_p8_resource_package_closure(root))
    findings.extend(check_p9_editor_runtime_boundary(root))
    findings.extend(check_p10_modern_rendering_capability_closure(root))
    findings.extend(check_p11_resource_hot_reload_contract(root))
    findings.extend(check_p12_render_proxy_snapshot_contract(root))
    findings.extend(check_p13_rhi_capability_report_contract(root))
    findings.extend(check_p14_rhi_device_capability_diagnostics(root))
    findings.extend(check_p15_renderer_tool_rhi_capability_snapshot(root))
    findings.extend(check_p16_render_graph_diagnostics_json_contract(root))
    findings.extend(check_p17_tool_diagnostics_artifact_summary_contract(root))
    findings.extend(check_p18_tool_diagnostics_artifact_integrity_contract(root))
    findings.extend(check_p19_tool_diagnostics_capture_identity_contract(root))
    findings.extend(check_p20_tool_diagnostics_portable_artifact_paths_contract(root))
    findings.extend(check_p21_tool_diagnostics_artifact_hash_contract(root))
    findings.extend(check_p22_tool_diagnostics_bundle_hash_contract(root))
    findings.extend(check_p23_tool_diagnostics_bundle_validation_contract(root))
    findings.extend(check_p24_tool_diagnostics_validation_json_contract(root))
    findings.extend(check_p25_tool_diagnostics_validation_sidecar_contract(root))
    findings.extend(check_p26_tool_diagnostics_validation_capture_identity_contract(root))
    findings.extend(check_p27_tool_diagnostics_summary_validation_verdict_contract(root))
    findings.extend(check_p28_tool_diagnostics_manifest_validation_sidecar_discovery_contract(root))
    findings.extend(check_p29_tool_diagnostics_manifest_artifact_type_metadata_contract(root))
    findings.extend(check_p30_tool_diagnostics_manifest_sidecar_schema_metadata_contract(root))
    findings.extend(check_p31_tool_diagnostics_stable_capture_id_contract(root))
    findings.extend(check_p32_tool_diagnostics_summary_sidecar_schema_metadata_contract(root))
    findings.extend(check_p33_tool_diagnostics_validation_sidecar_schema_metadata_contract(root))
    findings.extend(check_p34_tool_diagnostics_json_sidecar_identity_contract(root))
    findings.extend(check_p35_tool_diagnostics_json_sidecar_artifact_id_contract(root))
    findings.extend(check_p36_render_graph_diagnostics_json_artifact_identity_contract(root))
    findings.extend(check_p37_render_graph_diagnostics_schema_identity_contract(root))
    findings.extend(check_p38_validation_entry_artifact_identity_contract(root))
    findings.extend(check_p39_validation_entry_artifact_identity_verification_contract(root))
    findings.extend(check_p40_validation_entry_actual_identity_contract(root))
    findings.extend(check_p41_validation_entry_diagnostic_code_contract(root))
    findings.extend(check_p42_validation_diagnostic_code_summary_contract(root))
    findings.extend(check_p43_artifact_summary_validation_diagnostic_code_snapshot_contract(root))
    findings.extend(check_p44_manifest_validation_verdict_scope_contract(root))
    findings.extend(check_p45_validation_verdict_code_contract(root))
    findings.extend(check_p46_validation_primary_failure_summary_contract(root))
    findings.extend(check_p47_validation_primary_failure_detail_contract(root))
    findings.extend(check_p48_validation_primary_failure_artifact_identity_contract(root))
    findings.extend(check_p49_validation_primary_failure_entry_index_contract(root))
    findings.extend(check_p50_validation_failure_count_contract(root))
    findings.extend(check_p51_validation_entry_index_metadata_contract(root))
    findings.extend(check_p52_validation_entry_primary_failure_flag_contract(root))
    findings.extend(check_p53_validation_primary_failure_entry_count_contract(root))
    findings.extend(check_p54_validation_entry_count_summary_contract(root))
    findings.extend(check_p55_validation_entry_coverage_contract(root))
    findings.extend(check_p56_validation_entry_coverage_code_contract(root))
    findings.extend(check_p57_validation_entry_coverage_message_contract(root))
    findings.extend(check_p58_validation_entry_coverage_helper_contract(root))
    findings.extend(check_p59_architecture_baseline_contract_coverage(root))
    findings.extend(check_p60_cmake_module_link_boundary_contract(root))
    findings.extend(check_p61_cmake_module_include_edge_contract(root))
    findings.extend(check_p62_module_boundary_manifest_coverage_contract(root))
    findings.extend(check_p63_public_header_linkage_contract(root))
    findings.extend(check_p64_public_include_directory_scope_contract(root))
    findings.extend(check_p65_rhi_render_graph_baseline_contract(root))
    findings.extend(check_p66_renderer_rhi_baseline_diagnostics_contract(root))
    findings.extend(check_p67_cross_backend_rhi_baseline_report_contract(root))
    findings.extend(check_p68_rhi_capability_report_identity_contract(root))
    findings.extend(check_p69_rhi_capability_report_json_artifact_contract(root))
    findings.extend(check_p70_renderer_rhi_capability_json_sidecar_contract(root))
    findings.extend(check_p71_shader_runtime_contract_json_artifact_contract(root))
    findings.extend(check_p72_material_shader_contract_snapshot_json_artifact_contract(root))
    findings.extend(check_p73_resource_load_diagnostic_json_artifact_contract(root))
    findings.extend(check_p74_resource_hot_reload_diagnostic_json_artifact_contract(root))
    findings.extend(check_p75_p80_runtime_visible_rendering_contract(root))

    if findings:
        print("Architecture phase gate failures:")
        for finding in findings:
            print(f"  [{finding.phase}] {finding.path}:{finding.line}: {finding.message}")
        return 1

    print("Architecture phase gates passed.")
    print("P3 Actor/Component, P4 Service lifetime, P5 Resource runtime, P6 Quality, P8 Resource package, P9 Editor/runtime boundary, P10 Modern rendering capability, P11 Resource hot-reload, P12 Render proxy snapshot, P13 RHI capability report, P14 RHI device capability diagnostics, P15 renderer tool RHI capability snapshot, P16 RenderGraph diagnostics JSON, P17 tool diagnostics artifact summary, P18 artifact integrity, P19 capture identity, P20 portable artifact path, P21 artifact hash, P22 bundle hash, P23 bundle validation, P24 validation JSON, P25 validation sidecar, P26 validation capture identity, P27 summary validation verdict, P28 manifest validation sidecar discovery, P29 manifest artifact type metadata, P30 manifest sidecar schema metadata, P31 stable capture id, P32 summary sidecar schema metadata, P33 validation sidecar schema metadata, P34 JSON sidecar identity, P35 JSON sidecar artifact id, P36 RenderGraph JSON artifact identity, P37 RenderGraph schema identity, P38 validation entry artifact identity, P39 validation entry identity verification, P40 validation entry actual identity, P41 validation entry diagnostic code, P42 validation diagnostic code summary, P43 artifact summary validation diagnostic code snapshot, P44 manifest validation verdict scope, P45 validation verdict code, P46 validation primary failure summary, P47 validation primary failure detail, P48 validation primary failure artifact identity, P49 validation primary failure entry index, P50 validation failure count summary, P51 validation entry index metadata, P52 validation entry primary failure flag, P53 validation primary failure entry count, P54 validation entry count summary, P55 validation entry coverage summary, P56 validation entry coverage code, P57 validation entry coverage message, P58 validation entry coverage helper, P59 architecture baseline contract coverage, P60 CMake module link boundary, P61 CMake module include-edge boundary, P62 module-boundary manifest coverage, P63 public-header linkage, P64 public include-directory scope, P65 RHI RenderGraph baseline, P66 renderer RHI baseline diagnostics, P67 cross-backend RHI baseline report, P68 RHI report identity metadata, P69 RHI capability JSON artifact, P70 renderer RHI capability JSON sidecar, P71 shader runtime contract JSON artifact, P72 material shader contract snapshot JSON artifact, P73 resource load diagnostic JSON artifact, P74 resource hot reload diagnostic JSON artifact, and P75-P80 runtime visible rendering gates are covered.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
