param(
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [string]$ReportPath = "",
    [switch]$ListOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$gitSafeDirectory = $repoRoot.ToString().Replace([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
$buildPath = Join-Path $repoRoot $BuildDir

if (-not (Test-Path $buildPath)) {
    Write-Error "Build directory '$buildPath' does not exist. Configure this worktree first, for example: cmake --preset win_x64_debug"
}

if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $buildPath "BuildTruth\ArchitectureBaseline.json"
}
$reportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null
$junitPath = Join-Path $reportDirectory "ArchitectureBaseline.junit.xml"

$baselinePatterns = @(
    "Architecture\.GateInputsFailClosed",
    "Architecture\.RHIBackendFactoryLinkClosure",
    "Architecture\.ModuleBoundaries",
    "Architecture\.ModuleBoundaryManifest",
    "Architecture\.CMakeModuleVisibility",
    "Architecture\.CMakeModuleIncludeEdges",
    "Architecture\.CMakeModuleLinks",
    "Architecture\.PublicHeaderLinkage",
    "Architecture\.EditorRuntimeBoundary",
    "Architecture\.PhaseGates",
    "Architecture\.M1ArchitectureCut",
    "CoreDebugConfigValidation\.DebugConfigurationDefinesRVXDebug",
    "AudioResourceValidation\.",
    "JobGraphValidation\.",
    "PhysicsWorldIntegrationValidation\.(BackendQueryAndShapeStubsReportExplicitMisses|UnsupportedColliderTypesDoNotCreateFallbackShapesOrQueryHits)",
    "RHIContractValidation\.",
    "EngineRenderCompositionValidation\.",
    "SampleCLIValidation\.",
    "RenderPassValidationFixture\.PostProcessFrameInputContractReportsMissingVelocityDepthAndHistory",
    "RenderPostProcessStackValidation\.(EvaluateEffectsCountsRuntimeSupportedEffects|RenderVisualQualityPresetAppliesExplicitEffectPolicy|SceneRendererFrameDiagnosticsExposePostProcessEffectPlans|SceneRendererFrameDiagnosticsExposeFeatureExtractionStats|EvaluateEffectsReportsRequestedButUnsupportedResources|PostProcessStackReportsEffectExecutionPlanDomainsAndTargets)",
    "RenderGraphValidation\.(GraphCreation|PassChain|MemoryAliasing|ExecuteAsyncFallsBackToGraphicsWhenBackendDoesNotSupportQueueSync)",
    "RenderSceneValidation\.(AppliesTransactionallyAndOwnsPacketValues|RejectsSchemaOrderAndStaleHandlesWithoutMutation|StampsExactResourceClosureTransactionally)",
    "ActorComponentValidation\.(ActorOwnsComponentsAndDispatchesLifecycle|WorldSpawnActorDelegatesToSceneManager|ActorAddComponentRejectsLegacyComponentToAvoidContainerSplit)",
    "AppModeBoundaryValidation\.",
    "ParticleValidation\.FeaturePublicHeadersDoNotIncludeRenderOrRHI",
    "FeatureBoundaryValidation\.(WaterComponentPublicHeaderDoesNotExposeRenderOrRHI|WaterModuleDoesNotIncludeOrLinkRHI|WaterComponentBuildsRenderSnapshotWithoutGPUHandles|TerrainComponentPublicHeaderDoesNotExposeRenderOrRHI|TerrainModuleDoesNotIncludeOrLinkRHI|TerrainComponentBuildsRenderSnapshotWithoutGPUHandles|RenderFeatureSceneBridgeCollectsFeatureSnapshotsThroughProviderContract)",
    "ResourceInstantiationValidation\.(ModelResourceInstantiateActorUsesStaticMeshComponent|LegacyInstantiateDelegatesToActorPath|ProductionPathsDoNotCreateLegacyMeshRendererComponents|WorldLoadModelResourceReplacesSceneContent)",
    "ResourceRuntimePolicyValidation\.",
    "RenderHonestyValidationFixture\.(JsonArchiveReadPathReportsUnsupportedInsteadOfPretendingSuccess|PlaceholderAssetImportersFailInsteadOfReportingSuccess|PostProcessStubPassesAreUnsupportedAndDisabled|TerrainMaterialRenderDataExportDoesNotPretendGpuUpload|TerrainHeightmapGpuTextureUploadIsHonestWhenUnavailable|TerrainLODFallbacksExposeDeterministicStatus|TerrainPlaceholderPathsExposeHonestDiagnostics|SceneRendererLegacyCollectionFallbackIsRemoved|RenderGraphCompileDiagnosticsExposeReadBeforeWrite)",
    "SystemIntegration\.(SceneEntityAndManager|ResourceBasics)"
)
$baselineRegex = $baselinePatterns -join "|"

function Get-CTestInventory([string]$Pattern) {
    $jsonLines = & ctest --test-dir $buildPath -C $Configuration -R $Pattern --show-only=json-v1
    if ($LASTEXITCODE -ne 0) {
        throw "CTest inventory query failed for pattern '$Pattern' with exit code $LASTEXITCODE"
    }
    return (($jsonLines -join "`n") | ConvertFrom-Json)
}

$sourceCommit = ""
try {
    $sourceCommitLines = & git -c "safe.directory=$gitSafeDirectory" -C $repoRoot rev-parse HEAD 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Git source commit query failed with exit code $LASTEXITCODE`: $($sourceCommitLines -join ' ')"
    }
    $sourceCommit = ($sourceCommitLines -join "`n").Trim()

    # Capture one authoritative inventory snapshot. Re-querying CTest once per
    # pattern can observe different deferred-discovery states and obscures
    # which exact test set the baseline is about to execute.
    $selectedInventory = Get-CTestInventory $baselineRegex
    $selectedNames = @($selectedInventory.tests | ForEach-Object { $_.name })
    $missingPatterns = @(
        foreach ($pattern in $baselinePatterns) {
            $matches = @($selectedNames | Where-Object { $_ -match $pattern })
            if ($matches.Count -eq 0) {
                $pattern
            }
        }
    )

    if ($missingPatterns.Count -ne 0) {
        throw "Architecture baseline requirements are not discovered: $($missingPatterns -join ', ')"
    }

    $unbuiltNames = @($selectedNames | Where-Object { $_ -match "_NOT_BUILT$" })
    if ($unbuiltNames.Count -ne 0) {
        throw "Architecture baseline contains unbuilt tests: $($unbuiltNames -join ', ')"
    }
}
catch {
    $report = [ordered]@{
        schema = "RVX.BuildTruth.ArchitectureBaseline"
        schemaVersion = 1
        sourceCommit = $sourceCommit
        buildDir = $buildPath.ToString()
        configuration = $Configuration
        selectedTestCount = 0
        selectedTests = @()
        status = "failed"
        failure = $_.Exception.Message
        ctestExitCode = 1
        junitPath = $junitPath
    }
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding utf8
    Write-Error $report.failure -ErrorAction Continue
    exit 1
}

if ($ListOnly) {
    $selectedNames | Write-Output
    Write-Host "Selected tests: $($selectedNames.Count)"
    $report = [ordered]@{
        schema = "RVX.BuildTruth.ArchitectureBaseline"
        schemaVersion = 1
        sourceCommit = $sourceCommit
        buildDir = $buildPath.ToString()
        configuration = $Configuration
        selectedTestCount = $selectedNames.Count
        selectedTests = $selectedNames
        status = "listed"
        ctestExitCode = 0
        junitPath = $junitPath
    }
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding utf8
    exit 0
}

$ctestArgs = @(
    "--test-dir", $buildPath,
    "-C", $Configuration,
    "-R", $baselineRegex,
    "--output-on-failure",
    "--output-junit", $junitPath
)

Write-Host "Running architecture baseline from $repoRoot"
Write-Host "BuildDir: $buildPath"
Write-Host "Configuration: $Configuration"
Write-Host "Regex: $baselineRegex"

& ctest @ctestArgs
$ctestExitCode = $LASTEXITCODE
$report = [ordered]@{
    schema = "RVX.BuildTruth.ArchitectureBaseline"
    schemaVersion = 1
    sourceCommit = $sourceCommit
    buildDir = $buildPath.ToString()
    configuration = $Configuration
    selectedTestCount = $selectedNames.Count
    selectedTests = $selectedNames
    status = if ($ctestExitCode -eq 0) { "passed" } else { "failed" }
    ctestExitCode = $ctestExitCode
    junitPath = $junitPath
}
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding utf8
exit $ctestExitCode
