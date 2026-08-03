param(
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [string]$ReportPath = "",
    [switch]$ListOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $repoRoot $BuildDir
}
if (-not (Test-Path -LiteralPath $buildPath)) {
    throw "Build directory '$buildPath' does not exist. Configure this worktree before running the M2 exit gate."
}
$buildPath = (Resolve-Path -LiteralPath $buildPath).Path

if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $buildPath "M2Exit\M2RenderPolicyExit.json"
} elseif (-not [System.IO.Path]::IsPathRooted($ReportPath)) {
    $ReportPath = Join-Path $buildPath $ReportPath
}
$ReportPath = [System.IO.Path]::GetFullPath($ReportPath)
$reportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null
$runId = Get-Date -Format "yyyyMMddTHHmmssfff"

$artifacts = New-Object System.Collections.Generic.List[object]
$gateResults = New-Object System.Collections.Generic.List[object]
$gateInventory = New-Object System.Collections.Generic.List[object]
$metricSamples = New-Object System.Collections.Generic.List[object]
$sourceCommit = ""
$workingTreeDirty = $true
$isWindowsHost = [System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT
$isMacHost = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
    [System.Runtime.InteropServices.OSPlatform]::OSX)
$hostPlatform = if ($isWindowsHost) { "Windows" } elseif ($isMacHost) { "macOS" } else { "Linux" }
$coverage = [ordered]@{
    metal = if ($isWindowsHost) {
        [ordered]@{ status = "HostPlatformUnsupported"; required = $false }
    } else {
        [ordered]@{ status = "NotRun"; required = $false }
    }
}

function Add-Artifact([string]$Path, [string]$Kind) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $item = Get-Item -LiteralPath $Path
    $artifacts.Add([ordered]@{
        kind = $Kind
        path = $item.FullName
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes = $item.Length
    })
}

function Write-ExitReport([string]$Status, [string]$Failure = "", [int]$CTestExitCode = 0) {
    $report = [ordered]@{
        schema = "RVX.M2.RenderPolicyExit"
        schemaVersion = 1
        sourceCommit = $sourceCommit
        workingTreeDirty = $workingTreeDirty
        hostPlatform = $hostPlatform
        sourceRoot = $repoRoot.ToString()
        buildDir = $buildPath.ToString()
        configuration = $Configuration
        status = $Status
        failure = $Failure
        ctestExitCode = $CTestExitCode
        coverage = $coverage
        gateInventory = $gateInventory.ToArray()
        gateResults = $gateResults.ToArray()
        metrics = [ordered]@{
            nonGating = $true
            usedForAutoDecision = $false
            thresholds = $null
            samples = $metricSamples.ToArray()
        }
        artifacts = $artifacts.ToArray()
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding utf8
    Add-Artifact $ReportPath "exit-report"
}

function Get-CTestInventory([string]$Pattern) {
    $jsonLines = & ctest --test-dir $buildPath -C $Configuration -R $Pattern --show-only=json-v1
    if ($LASTEXITCODE -ne 0) {
        throw "CTest inventory query failed for '$Pattern' with exit code $LASTEXITCODE"
    }
    return (($jsonLines -join "`n") | ConvertFrom-Json)
}

function Test-MeasurementArtifact(
    [string]$Path,
    [string]$GateName,
    [bool]$RequireGroupOccupancy = $false) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$GateName did not write required measurement artifact '$Path'"
    }
    $measurement = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($measurement.schema -ne "RVX.RenderPolicyMeasurement" -or
        $measurement.schemaVersion -ne 1) {
        throw "$GateName measurement artifact has an invalid schema"
    }
    foreach ($name in @(
        "frameSequence",
        "planCpuNanoseconds",
        "submissionCpuNanoseconds",
        "candidatePacketCount",
        "drawGroupCount")) {
        if ($null -eq $measurement.$name -or
            $measurement.$name -isnot [System.ValueType]) {
            throw "$GateName measurement artifact has no numeric '$name'"
        }
    }
    foreach ($name in @(
        "planCpuTimingAvailable",
        "submissionCpuTimingAvailable",
        "averageGroupOccupancyAvailable",
        "nonGating",
        "usedForAutoDecision")) {
        if ($measurement.$name -isnot [bool]) {
            throw "$GateName measurement artifact has no boolean '$name'"
        }
    }
    if (-not $measurement.nonGating -or $measurement.usedForAutoDecision) {
        throw "$GateName measurement violated the non-decision contract"
    }
    if ($null -eq $measurement.PSObject.Properties["averageGroupOccupancy"]) {
        throw "$GateName measurement artifact is missing 'averageGroupOccupancy'"
    }
    if ($measurement.averageGroupOccupancyAvailable) {
        if ($null -eq $measurement.averageGroupOccupancy -or
            $measurement.averageGroupOccupancy -isnot [System.ValueType]) {
            throw "$GateName has no numeric occupancy for available draw groups"
        }
    } elseif ($null -ne $measurement.averageGroupOccupancy) {
        throw "$GateName must encode unavailable group occupancy as null"
    }
    if ($RequireGroupOccupancy -and
        ($measurement.drawGroupCount -le 0 -or
         -not $measurement.averageGroupOccupancyAvailable)) {
        throw "$GateName did not expose a selected GPU/hybrid group occupancy"
    }
    $metricSamples.Add([ordered]@{
        gate = $GateName
        path = (Get-Item -LiteralPath $Path).FullName
        frameSequence = $measurement.frameSequence
        planCpuNanoseconds = $measurement.planCpuNanoseconds
        submissionCpuNanoseconds = $measurement.submissionCpuNanoseconds
        candidatePacketCount = $measurement.candidatePacketCount
        drawGroupCount = $measurement.drawGroupCount
        averageGroupOccupancy = $measurement.averageGroupOccupancy
        averageGroupOccupancyAvailable = $measurement.averageGroupOccupancyAvailable
        nonGating = $measurement.nonGating
        usedForAutoDecision = $measurement.usedForAutoDecision
    })
    Add-Artifact $Path "render-policy-measurement"
}

function Invoke-DX12QueueGate(
    [string]$TestName,
    [string]$GateName,
    [bool]$RequireGPUValidation = $false) {
    $safeName = $TestName -replace "[^A-Za-z0-9_.-]", "_"
    $queueReport = Join-Path $reportDirectory "$safeName.$runId.dx12-debug-queue.json"
    $logPath = Join-Path $reportDirectory "$safeName.$runId.log"
    $oldQueueReport = $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH
    try {
        $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH = $queueReport
        $output = & ctest --test-dir $buildPath -C $Configuration -R "^$([regex]::Escape($TestName))$" -V 2>&1
        $exitCode = $LASTEXITCODE
        $output | Set-Content -LiteralPath $logPath -Encoding utf8
    }
    finally {
        if ($null -eq $oldQueueReport) {
            Remove-Item Env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH -ErrorAction SilentlyContinue
        } else {
            $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH = $oldQueueReport
        }
    }
    Add-Artifact $logPath "ctest-log"
    if ($exitCode -ne 0) {
        throw "$GateName failed with CTest exit code $exitCode"
    }
    $logText = Get-Content -LiteralPath $logPath -Raw
    if ($RequireGPUValidation -and
        $logText -notmatch "DX12 GPU-based validation enabled") {
        throw "$GateName did not enable DX12 GPU-based validation"
    }
    if ($logText -notmatch "RVX_DX12_DEBUG_QUEUE_SUMMARY debugLayerEnabled=true available=true readComplete=true .*errorCount=0 corruptionCount=0 reportWritten=true") {
        throw "$GateName did not emit a clean DX12 debug queue summary marker"
    }
    if (-not (Test-Path -LiteralPath $queueReport)) {
        throw "$GateName did not write a DX12 debug queue report"
    }
    $queue = Get-Content -LiteralPath $queueReport -Raw | ConvertFrom-Json
    if ($queue.schema -ne "RVX.DX12DebugQueue" -or $queue.schemaVersion -ne 1 -or
        -not $queue.debugLayerEnabled -or -not $queue.available -or
        -not $queue.readComplete -or -not $queue.passed -or
        $queue.errorCount -ne 0 -or $queue.corruptionCount -ne 0) {
        throw "$GateName reported DX12 ERROR/CORRUPTION or unavailable diagnostics"
    }
    Add-Artifact $queueReport "dx12-debug-queue"
    $gateResults.Add([ordered]@{ name = $GateName; status = "passed"; ctestExitCode = $exitCode })
}

function Invoke-DX12NativeGate([string]$TestName, [string]$GateName) {
    $safeName = $TestName -replace "[^A-Za-z0-9_.-]", "_"
    $queueReport = Join-Path $reportDirectory "$safeName.$runId.dx12-debug-queue.json"
    $logPath = Join-Path $reportDirectory "$safeName.$runId.log"
    $oldQueueReport = $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH
    try {
        $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH = $queueReport
        $output = & ctest --test-dir $buildPath -C $Configuration -R "^$([regex]::Escape($TestName))$" -V 2>&1
        $exitCode = $LASTEXITCODE
        $output | Set-Content -LiteralPath $logPath -Encoding utf8
    }
    finally {
        if ($null -eq $oldQueueReport) {
            Remove-Item Env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH -ErrorAction SilentlyContinue
        } else {
            $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH = $oldQueueReport
        }
    }
    Add-Artifact $logPath "ctest-log"
    if ($exitCode -ne 0) {
        throw "$GateName failed with CTest exit code $exitCode"
    }
    $logText = Get-Content -LiteralPath $logPath -Raw
    if ($logText -match "\[  SKIPPED \]" -or
        $logText -notmatch "\[ RUN      \] $([regex]::Escape($TestName))" -or
        $logText -notmatch "\[       OK \] $([regex]::Escape($TestName))") {
        throw "$GateName was skipped or did not complete its native DX12 test body"
    }
    if ($logText -notmatch "RVX_DX12_DEBUG_QUEUE_SUMMARY debugLayerEnabled=true available=true readComplete=true .*errorCount=0 corruptionCount=0 reportWritten=true" -or
        -not (Test-Path -LiteralPath $queueReport)) {
        throw "$GateName did not produce clean DX12 InfoQueue shutdown evidence"
    }
    $queue = Get-Content -LiteralPath $queueReport -Raw | ConvertFrom-Json
    if ($queue.schema -ne "RVX.DX12DebugQueue" -or $queue.schemaVersion -ne 1 -or
        -not $queue.debugLayerEnabled -or -not $queue.available -or
        -not $queue.readComplete -or -not $queue.passed -or
        $queue.errorCount -ne 0 -or $queue.corruptionCount -ne 0) {
        throw "$GateName reported DX12 ERROR/CORRUPTION or unavailable diagnostics"
    }
    Add-Artifact $queueReport "dx12-debug-queue"
    $gateResults.Add([ordered]@{ name = $GateName; status = "passed"; ctestExitCode = $exitCode })
}

function Invoke-VulkanDirectGate([string]$TestName) {
    $logPath = Join-Path $reportDirectory "$TestName.vulkan.log"
    $output = & ctest --test-dir $buildPath -C $Configuration -R "^$([regex]::Escape($TestName))$" -V 2>&1
    $exitCode = $LASTEXITCODE
    $output | Set-Content -LiteralPath $logPath -Encoding utf8
    Add-Artifact $logPath "ctest-log"
    if ($exitCode -ne 0) {
        throw "Vulkan Direct smoke failed with CTest exit code $exitCode"
    }
    $logText = Get-Content -LiteralPath $logPath -Raw
    if ($logText -notmatch "Creating RHI Device with backend: Vulkan" -or
        $logText -notmatch "Vulkan validation layers enabled" -or
        $logText -notmatch "GPU-driven direct-draw fallback ready") {
        throw "Vulkan Direct smoke lacks realized-backend, validation, or Direct readiness evidence"
    }
    $vvlErrorCount = [regex]::Matches($logText, "Validation Error:").Count
    if ($vvlErrorCount -ne 0) {
        throw "Vulkan Direct smoke emitted $vvlErrorCount Vulkan validation errors"
    }
    $gateResults.Add([ordered]@{
        name = "Vulkan.Direct.Validation"
        status = "passed"
        ctestExitCode = $exitCode
        vvlErrorCount = $vvlErrorCount
    })
}

# The patterns intentionally name concrete Task 7-10, shared-RHI, and native
# gates. Missing discovery is a failure, never an implicit skip.
$gateDefinitions = @(
    [pscustomobject]@{ name = "Task7.ExactlyOnceFailureInjection"; pattern = "^(RenderPolicyValidation\.FramePlanCompilerBuildsStablePacketIdsAndExactlyOnceAccounting|RenderPassValidationFixture\.(DepthPrepassRecordsMixedPublishedGPUAndDirectLanesInSingleRenderPass|DepthPrepassKeepsPreflightedDirectLaneAfterMixedGPULaneFailure|DepthPrepassDoesNotReplayDirectAfterPlannedGPULateFailure|OpaquePassRecordsMixedPublishedGPUAndDirectLanesInSingleRenderPass|OpaquePassDoesNotReplayDirectAfterPlannedGPULateFailure))$" },
    [pscustomobject]@{ name = "Task8.VisibilityParity"; pattern = "^(RenderVisibilityValidation\.(CanonicalAABBFrustumKeepsBoundaryAndRejectsBehindCamera|ReverseZAndZeroExtentUseSameClipInequalities|InvalidBoundsAreConservativeAndSanitizedForGPU|PlaneDistanceEqualityAndZeroCandidatesRemainVisibleAndStable|ProvidersPreservePassAwareSourceIdentityWithoutReadback|PassProjectionReusesCanonicalObjectVisibility)|GPUDrivenValidationFixture\.(CpuFallbackCullsInstancesAndBuildsIndirectCommands|FrameSlotsIsolatePersistentInputsAndRestoreTheirAccessSnapshots|IndependentCullingOwnersKeepDepthAndOpaqueStreamsIsolated|VisibilityCandidateInstanceRequiresStablePacketObjectIdentity)|ModelViewerGPUDrivenSmoke|GPUDrivenCrossPathVisualParityValidation)$" },
    [pscustomobject]@{ name = "Task9.MultiViewLifetime"; pattern = "^(RenderPolicyValidation\.FramePlanCompilerKeepsTwoViewsIndependentAndValueOwned|RenderPassStatusValidation\.(DefaultGraphAdapterValueCapturesViewsAcrossGraphsAndFrames|RecordContextRejectsStaleMismatchedAndIncompleteGpuSlices|GraphOwnedFrameSnapshotRebindsPointersAndRejectsClearedGraph)|RenderPassValidationFixture\.(MigratedPassesRejectForeignAndStaleRecordGraphs|OpaquePassRetainsFrameBindingsPerSubmissionAcrossOutOfOrderGraphs)|GPUDrivenValidationFixture\.SealedRecordingSubmissionRetainsGpuObjectsUntilCompletion|RenderGraphValidation\.ClearInvalidatesOldHandleGenerationWithoutChangingGraphIdentity)$" },
    [pscustomobject]@{ name = "Task7.RenderPolicy"; pattern = "^RenderPolicyValidation\." },
    [pscustomobject]@{ name = "Task8.GPUDriven"; pattern = "^(GPUDrivenValidationFixture|RenderSubmissionStrategyValidation)\." },
    [pscustomobject]@{ name = "Task9.FrameOwnership"; pattern = "^SceneRendererDiagnosticsValidation\.RenderPolicyPlanUsesFrameLifetimeAndInvalidatesAtOwnershipBoundaries$" },
    [pscustomobject]@{ name = "Task9.Resize"; pattern = "^RenderThreadRuntimeValidation\.(TerminalSealWaitsForInFlightResizePublication|ResizeValidationAndCoalescingAreExplicit)$" },
    [pscustomobject]@{ name = "Task9.Retirement"; pattern = "^RenderResourceRuntimeFixture\.(NthCreationFailureRetiresPartialObjects|SubmissionFailureRetiresCreatedObjects|ReadyReleaseRetiresUntilRecordedTokenCompletes)$" },
    [pscustomobject]@{ name = "Task10.Submission"; pattern = "^DX12Validation\." },
    [pscustomobject]@{ name = "Task10.RHIConformance"; pattern = "^RHIConformanceValidation\." },
    [pscustomobject]@{ name = "Task10.RenderSubmissionLifetime"; pattern = "^RenderSubmissionValidation\." },
    [pscustomobject]@{ name = "Shared.RHI"; pattern = "^(RHIContractValidation|RenderGraphValidation|GPUResourceManagerValidation|ResourceViewCacheValidation)\." },
    [pscustomobject]@{ name = "CrossBackend"; pattern = "^CrossBackendValidation\." },
    [pscustomobject]@{ name = "DX12.GPUDriven"; pattern = "^(ModelViewerGPUDrivenSmoke|ModelViewerGPUDrivenAutoPolicySmoke|ModelViewerGPUDrivenParityGPUSmoke|ModelViewerGPUDrivenParityDirectSmoke|GPUDrivenCrossPathVisualParityValidation|ModelViewerGPUDrivenDisabledSmoke|ModelViewerGPUDrivenGBVSmoke)$" },
    [pscustomobject]@{ name = "M2.Resize"; pattern = "^ModelViewerGPUDrivenResizeSmoke$" },
    [pscustomobject]@{ name = "M2.ZeroVisible"; pattern = "^ModelViewerGPUDrivenZeroVisibleSmoke$" },
    [pscustomobject]@{ name = "DX12.NativeIndexedIndirectZeroCount"; pattern = "^DX12Validation\.IndexedIndirectZeroCountExecutesNoDraw$" },
    [pscustomobject]@{ name = "DX12.NativeIndexedIndirectMaximumCount"; pattern = "^DX12Validation\.IndexedIndirectMaximumCountStaysWithinValidatedRanges$" },
    [pscustomobject]@{ name = "DX12.NativeIndexedIndirectRetirement"; pattern = "^DX12Validation\.IndexedIndirectResourcesRetireAfterFenceCompletion$" },
    [pscustomobject]@{ name = "Vulkan.Direct"; pattern = "^ModelViewerVulkanDirectSmoke$" },
    [pscustomobject]@{ name = "Vulkan.Validation"; pattern = "^VulkanValidation\." }
)

try {
    $gitCommand = (Get-Command git -ErrorAction Stop).Source
    $gitSafeDirectory = $repoRoot.ToString().Replace(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $gitExcludeFile = (Join-Path $repoRoot ".gitignore").ToString().Replace(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $sourceCommitLines = & $gitCommand -c "safe.directory=$gitSafeDirectory" -c "core.excludesFile=$gitExcludeFile" -C $repoRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Git source commit query failed with exit code $LASTEXITCODE"
    }
    $sourceCommit = ($sourceCommitLines -join "`n").Trim()
    $workingTreeLines = & $gitCommand -c "safe.directory=$gitSafeDirectory" -c "core.excludesFile=$gitExcludeFile" -C $repoRoot status --porcelain --untracked-files=no 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Git working-tree query failed with exit code $LASTEXITCODE"
    }
    $workingTreeDirty = @($workingTreeLines).Count -ne 0

    $allPattern = "(" + (($gateDefinitions | ForEach-Object { $_.pattern }) -join "|") + ")"
    $inventory = Get-CTestInventory $allPattern
    $selectedTests = @($inventory.tests | ForEach-Object { $_.name } | Sort-Object -Unique)
    $missing = @()
    foreach ($gate in $gateDefinitions) {
        $matches = @($selectedTests | Where-Object { $_ -match $gate.pattern })
        if ($matches.Count -eq 0) {
            $missing += $gate.name
        }
        $gateInventory.Add([ordered]@{
            name = $gate.name
            selectedCount = $matches.Count
            tests = $matches
        })
    }
    if ($missing.Count -ne 0) {
        throw "Required M2 gates are not discovered: $($missing -join ', ')"
    }
    $notBuilt = @($selectedTests | Where-Object { $_ -match "_NOT_BUILT$" })
    if ($notBuilt.Count -ne 0) {
        throw "M2 inventory contains unbuilt tests: $($notBuilt -join ', ')"
    }
}
catch {
    Write-ExitReport "failed" $_.Exception.Message 1
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
}

if ($ListOnly) {
    $selectedTests | Write-Output
    $gateResults.Add([ordered]@{ name = "inventory"; status = "listed"; testCount = $selectedTests.Count })
    Write-ExitReport "listed"
    exit 0
}

$junitPath = Join-Path $reportDirectory "M2RenderPolicyExit.junit.xml"
$ctestLogPath = Join-Path $reportDirectory "M2RenderPolicyExit.ctest.log"
$ctestExitCode = 1
try {
    $ctestOutput = & ctest --test-dir $buildPath -C $Configuration -R $allPattern --output-on-failure --output-junit $junitPath 2>&1
    $ctestExitCode = $LASTEXITCODE
    $ctestOutput | Set-Content -LiteralPath $ctestLogPath -Encoding utf8
    Add-Artifact $ctestLogPath "ctest-log"
    Add-Artifact $junitPath "ctest-junit"
    if ($ctestExitCode -ne 0) {
        throw "M2 CTest matrix failed with exit code $ctestExitCode"
    }
    $gateResults.Add([ordered]@{ name = "M2.CTestMatrix"; status = "passed"; testCount = $selectedTests.Count; ctestExitCode = $ctestExitCode })
    foreach ($gate in $gateInventory) {
        $gateResults.Add([ordered]@{
            name = $gate.name
            status = "passed"
            testCount = $gate.selectedCount
        })
    }

    Invoke-DX12QueueGate "ModelViewerGPUDrivenDisabledSmoke" "DX12.Direct.DebugQueue"
    Invoke-DX12QueueGate "ModelViewerGPUDrivenSmoke" "DX12.GPUDriven.DebugQueue"
    Invoke-DX12QueueGate "ModelViewerGPUDrivenGBVSmoke" "DX12.GBV.DebugQueue" $true
    Invoke-DX12NativeGate "DX12Validation.IndexedIndirectZeroCountExecutesNoDraw" "DX12.NativeIndexedIndirectZeroCount"
    Invoke-DX12NativeGate "DX12Validation.IndexedIndirectMaximumCountStaysWithinValidatedRanges" "DX12.NativeIndexedIndirectMaximumCount"
    Invoke-DX12NativeGate "DX12Validation.IndexedIndirectResourcesRetireAfterFenceCompletion" "DX12.NativeIndexedIndirectRetirement"
    Invoke-VulkanDirectGate "ModelViewerVulkanDirectSmoke"

    $measurementRoot = Join-Path $buildPath "M2Exit\$Configuration"
    Test-MeasurementArtifact (Join-Path $measurementRoot "ModelViewerGPUDrivenResize.measurement.json") "ModelViewerGPUDrivenResizeSmoke" $true
    Test-MeasurementArtifact (Join-Path $measurementRoot "ModelViewerGPUDrivenZeroVisible.measurement.json") "ModelViewerGPUDrivenZeroVisibleSmoke" $true
    Test-MeasurementArtifact (Join-Path $measurementRoot "ModelViewerVulkanDirect.measurement.json") "ModelViewerVulkanDirectSmoke"

    Write-ExitReport "passed" "" $ctestExitCode
    exit 0
}
catch {
    $failureCode = if ($null -eq $ctestExitCode) { 1 } else { $ctestExitCode }
    Write-ExitReport "failed" $_.Exception.Message $failureCode
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
}
