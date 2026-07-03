param(
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [switch]$ListOnly
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildPath = Join-Path $repoRoot $BuildDir

if (-not (Test-Path $buildPath)) {
    Write-Error "Build directory '$buildPath' does not exist. Configure this worktree first, for example: cmake --preset win_x64_debug"
}

$baselineRegex = @(
    "Architecture\.ModuleBoundaries",
    "Architecture\.ModuleBoundaryManifest",
    "Architecture\.CMakeModuleVisibility",
    "Architecture\.CMakeModuleIncludeEdges",
    "Architecture\.CMakeModuleLinks",
    "Architecture\.PublicHeaderLinkage",
    "Architecture\.EditorRuntimeBoundary",
    "Architecture\.PhaseGates",
    "CoreDebugConfigValidation\.DebugConfigurationDefinesRVXDebug",
    "AudioResourceValidation\.",
    "JobGraphValidation\.",
    "PhysicsWorldIntegrationValidation\.(BackendQueryAndShapeStubsReportExplicitMisses|UnsupportedColliderTypesDoNotCreateFallbackShapesOrQueryHits)",
    "RHIContractValidation\.",
    "RenderGraphValidation\.(GraphCreation|PassChain|MemoryAliasing|ExecuteAsyncFallsBackToGraphicsWhenBackendDoesNotSupportQueueSync)",
    "RenderSceneValidation\.(RenderSceneApplyProxySnapshotPopulatesObjectsAndLights|RenderProxyBridgeBuildsPrimitiveAndLightSnapshot|RenderProxyBridgeBuildsLegacyMeshRendererProxy)",
    "ActorComponentValidation\.(ActorOwnsComponentsAndDispatchesLifecycle|WorldSpawnActorDelegatesToSceneManager|ActorAddComponentRejectsLegacyComponentToAvoidContainerSplit)",
    "AppModeBoundaryValidation\.",
    "ParticleValidation\.FeaturePublicHeadersDoNotIncludeRenderOrRHI",
    "ResourceInstantiationValidation\.(ModelResourceInstantiateActorUsesStaticMeshComponent|LegacyInstantiateDelegatesToActorPath|ProductionPathsDoNotCreateLegacyMeshRendererComponents|WorldLoadModelResourceReplacesSceneContent)",
    "ResourceRuntimePolicyValidation\.",
    "RenderHonestyValidationFixture\.(JsonArchiveReadPathReportsUnsupportedInsteadOfPretendingSuccess|PlaceholderAssetImportersFailInsteadOfReportingSuccess|PostProcessStubPassesAreUnsupportedAndDisabled|TerrainMaterialLayerBufferMapFailureIsNotInitialized|TerrainHeightmapGpuTextureUploadIsHonestWhenUnavailable|TerrainLODFallbacksExposeDeterministicStatus|TerrainPlaceholderPathsExposeHonestDiagnostics|SceneRendererLegacyCollectionFallbackIsRemoved|RenderGraphCompileDiagnosticsExposeReadBeforeWrite)",
    "SystemIntegration\.(SceneEntityAndManager|ResourceBasics)"
) -join "|"

$ctestArgs = @("--test-dir", $buildPath, "-C", $Configuration, "-R", $baselineRegex, "--output-on-failure")
if ($ListOnly) {
    $ctestArgs += "-N"
}

Write-Host "Running architecture baseline from $repoRoot"
Write-Host "BuildDir: $buildPath"
Write-Host "Configuration: $Configuration"
Write-Host "Regex: $baselineRegex"

& ctest @ctestArgs
exit $LASTEXITCODE
