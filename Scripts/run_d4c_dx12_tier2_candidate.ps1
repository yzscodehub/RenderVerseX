[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ModelViewer,

    [Parameter(Mandatory = $true)]
    [string]$VisualGoldenValidation,

    [Parameter(Mandatory = $true)]
    [string]$DX12Validation,

    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [string]$ArtifactRoot
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    throw "SourceRoot is not an existing directory: $SourceRoot"
}
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$ArtifactRoot = [System.IO.Path]::GetFullPath($ArtifactRoot)
if ((Split-Path -Leaf $ArtifactRoot) -ine 'Task11D-D4c') {
    throw "ArtifactRoot must name the dedicated Task11D-D4c output directory: $ArtifactRoot"
}

function Invoke-RVXProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$LogPath,

        [string]$DX12DebugQueueReportPath = ''
    )

    $oldQueueReport = $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH
    try {
        if (-not [string]::IsNullOrWhiteSpace($DX12DebugQueueReportPath)) {
            $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH = $DX12DebugQueueReportPath
        }
        & $Executable @Arguments 2>&1 | Tee-Object -FilePath $LogPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldQueueReport) {
            Remove-Item Env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH -ErrorAction SilentlyContinue
        } else {
            $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH = $oldQueueReport
        }
    }
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode (log: $LogPath)"
    }
}

function Get-RVXFileHashRecord {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return [ordered]@{ path = $Path; exists = $false }
    }

    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $item.FullName
        exists = $true
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Test-RVXLogContains {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    return [bool](Select-String -LiteralPath $Path -Pattern $Pattern -SimpleMatch -Quiet)
}

function Test-RVXDX12DebugQueue {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Name did not write a DX12 debug queue report: $Path"
    }
    $queue = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($queue.schema -ne 'RVX.DX12DebugQueue' -or $queue.schemaVersion -ne 1 -or
        -not $queue.debugLayerEnabled -or -not $queue.available -or
        -not $queue.readComplete -or -not $queue.passed -or
        $queue.errorCount -ne 0 -or $queue.corruptionCount -ne 0) {
        throw "$Name reported unavailable or non-clean DX12 InfoQueue evidence"
    }
    return [ordered]@{
        name = $Name
        debugLayerEnabled = [bool]$queue.debugLayerEnabled
        available = [bool]$queue.available
        readComplete = [bool]$queue.readComplete
        messageCount = [uint64]$queue.messageCount
        errorCount = [uint64]$queue.errorCount
        corruptionCount = [uint64]$queue.corruptionCount
        passed = [bool]$queue.passed
        report = Get-RVXFileHashRecord $Path
    }
}

function Test-RVXGPUSceneTier2CandidateEvidenceDocument {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][psobject]$Evidence,
        [Parameter(Mandatory = $true)][uint32]$ExpectedResizeFrame
    )

    if ($Evidence.schema -ne 'RVX.GPUSceneTier2CandidateEvidence' -or
        $Evidence.schemaVersion -ne 3) {
        throw "$Name has an unsupported GPU-scene candidate evidence schema"
    }
    if (-not [bool]$Evidence.candidateSatisfied) {
        throw "$Name did not satisfy the GPU-scene Tier 2 candidate contract: $($Evidence.candidateReason)"
    }
    if ([uint32]$Evidence.resizeFrame -ne $ExpectedResizeFrame) {
        throw "$Name reported resize frame $($Evidence.resizeFrame), expected $ExpectedResizeFrame"
    }

    $frames = @($Evidence.frames)
    $previousEvidenceOrdinal = [uint64]0
    foreach ($frame in $frames) {
        $ordinal = [uint64]$frame.smokeFrameOrdinal
        if ($ordinal -eq 0 -or $ordinal -le $previousEvidenceOrdinal) {
            throw "$Name has missing, duplicate, or out-of-order smoke-frame ordinals"
        }
        $previousEvidenceOrdinal = $ordinal

        if ($frame.accepted -ne $true -and
            ($frame.selectedTier -eq 'GPUResidentScene' -or
             $frame.executedTier -eq 'GPUResidentScene')) {
            throw "$Name has an unaccepted GPUResidentScene evidence frame"
        }
    }

    $acceptedFrames = @($frames | Where-Object { $_.accepted -eq $true })
    if ([uint32]$Evidence.acceptedFrameCount -ne $acceptedFrames.Count) {
        throw "$Name reported an accepted frame count that does not match its evidence frames"
    }
    $previousSequence = [uint64]0
    $preResizePhase = 'AwaitCold'
    $postResizePhase = 'AwaitCold'
    foreach ($frame in $acceptedFrames) {
        $ordinal = [uint64]$frame.smokeFrameOrdinal
        $sequence = [uint64]$frame.frameSequence
        if ($sequence -eq 0 -or $sequence -le $previousSequence) {
            throw "$Name has missing, duplicate, or out-of-order accepted engine-frame sequences"
        }
        $previousSequence = $sequence

        if ($frame.executionStatus -ne 'Completed' -or
            $frame.selectedTier -ne $frame.executedTier -or
            $frame.tierFallbackReason -ne 'None') {
            throw "$Name has an accepted incomplete execution report or unexpected tier fallback"
        }

        $isPostResize = $ExpectedResizeFrame -gt 0 -and $ordinal -ge $ExpectedResizeFrame
        $phase = if ($isPostResize) { $postResizePhase } else { $preResizePhase }
        if ($frame.executedTier -eq 'IndirectGrouped') {
            if ([uint64]$frame.residentVersion -ne 0 -or [uint64]$frame.leaseVersion -ne 0) {
                throw "$Name has a cold/prewarm Tier 1 frame with a resident or lease version"
            }
            if ($phase -eq 'Warm') {
                throw "$Name has an unexpected Tier 1 fallback after Tier 2 became warm"
            }
            $phase = 'AwaitWarm'
        }
        elseif ($frame.executedTier -eq 'GPUResidentScene') {
            $required = [uint64]$frame.requiredResidentVersion
            if ($required -eq 0 -or $required -ne [uint64]$frame.residentVersion -or
                $required -ne [uint64]$frame.leaseVersion) {
                throw "$Name has mismatched exact Tier 2 resident/lease versions"
            }
            if ($phase -eq 'AwaitCold') {
                if ($isPostResize) {
                    throw "$Name executed Tier 2 before an accepted post-resize cold/prewarm Tier 1 frame"
                }
                throw "$Name executed Tier 2 before an accepted initial cold/prewarm Tier 1 frame"
            }
            $phase = 'Warm'
        }
        else {
            throw "$Name has an accepted unexpected non-GPU-driven tier"
        }

        if ($isPostResize) {
            $postResizePhase = $phase
        }
        else {
            $preResizePhase = $phase
        }
    }

    if ($preResizePhase -ne 'Warm') {
        throw "$Name did not prove the initial accepted cold/prewarm Tier 1 to warm Tier 2 transition"
    }
    if ($ExpectedResizeFrame -gt 0 -and $postResizePhase -ne 'Warm') {
        throw "$Name did not prove the post-resize cold/prewarm Tier 1 to warm Tier 2 transition"
    }
    if ($acceptedFrames.Count -lt 10) {
        throw "$Name reported fewer than ten accepted GPU-scene evidence frames"
    }

    return $Evidence
}

function Test-RVXGPUSceneTier2CandidateEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][uint32]$ExpectedResizeFrame
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Name did not write GPU-scene Tier 2 candidate evidence: $Path"
    }
    $evidence = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    return Test-RVXGPUSceneTier2CandidateEvidenceDocument `
        -Name $Name -Evidence $evidence -ExpectedResizeFrame $ExpectedResizeFrame
}

function Assert-RVXGPUSceneTier2NegativeOrderRegression {
    $tierTwoBeforeCold = [pscustomobject]@{
        schema = 'RVX.GPUSceneTier2CandidateEvidence'
        schemaVersion = 3
        candidateSatisfied = $true
        candidateReason = ''
        resizeFrame = 0
        acceptedFrameCount = 1
        frames = @(
            [pscustomobject]@{
                smokeFrameOrdinal = 1
                frameSequence = 1
                accepted = $true
                selectedTier = 'GPUResidentScene'
                executedTier = 'GPUResidentScene'
                executionStatus = 'Completed'
                tierFallbackReason = 'None'
                requiredResidentVersion = 1
                residentVersion = 1
                leaseVersion = 1
            }
        )
    }

    $initialRejected = $false
    try {
        $null = Test-RVXGPUSceneTier2CandidateEvidenceDocument `
            -Name 'negative-order regression' -Evidence $tierTwoBeforeCold -ExpectedResizeFrame 0
    }
    catch {
        if ($_.Exception.Message -like '*before an accepted initial cold/prewarm Tier 1 frame*') {
            $initialRejected = $true
        }
        else {
            throw
        }
    }
    if (-not $initialRejected) {
        throw 'initial negative-order regression was incorrectly accepted'
    }

    $postResizeTierTwoBeforeCold = [pscustomobject]@{
        schema = 'RVX.GPUSceneTier2CandidateEvidence'
        schemaVersion = 3
        candidateSatisfied = $true
        candidateReason = ''
        resizeFrame = 3
        acceptedFrameCount = 3
        frames = @(
            [pscustomobject]@{
                smokeFrameOrdinal = 1; frameSequence = 1; accepted = $true
                selectedTier = 'IndirectGrouped'; executedTier = 'IndirectGrouped'
                executionStatus = 'Completed'; tierFallbackReason = 'None'
                requiredResidentVersion = 1; residentVersion = 0; leaseVersion = 0
            },
            [pscustomobject]@{
                smokeFrameOrdinal = 2; frameSequence = 2; accepted = $true
                selectedTier = 'GPUResidentScene'; executedTier = 'GPUResidentScene'
                executionStatus = 'Completed'; tierFallbackReason = 'None'
                requiredResidentVersion = 1; residentVersion = 1; leaseVersion = 1
            },
            [pscustomobject]@{
                smokeFrameOrdinal = 3; frameSequence = 3; accepted = $true
                selectedTier = 'GPUResidentScene'; executedTier = 'GPUResidentScene'
                executionStatus = 'Completed'; tierFallbackReason = 'None'
                requiredResidentVersion = 2; residentVersion = 2; leaseVersion = 2
            }
        )
    }

    $postResizeRejected = $false
    try {
        $null = Test-RVXGPUSceneTier2CandidateEvidenceDocument `
            -Name 'post-resize negative-order regression' `
            -Evidence $postResizeTierTwoBeforeCold -ExpectedResizeFrame 3
    }
    catch {
        if ($_.Exception.Message -like '*before an accepted post-resize cold/prewarm Tier 1 frame*') {
            $postResizeRejected = $true
        }
        else {
            throw
        }
    }
    if (-not $postResizeRejected) {
        throw 'post-resize negative-order regression was incorrectly accepted'
    }

    $unacceptedTierTwoBetweenColdAndWarm = [pscustomobject]@{
        schema = 'RVX.GPUSceneTier2CandidateEvidence'
        schemaVersion = 3
        candidateSatisfied = $true
        candidateReason = ''
        resizeFrame = 0
        acceptedFrameCount = 2
        frames = @(
            [pscustomobject]@{
                smokeFrameOrdinal = 1; frameSequence = 1; accepted = $true
                selectedTier = 'IndirectGrouped'; executedTier = 'IndirectGrouped'
                executionStatus = 'Completed'; tierFallbackReason = 'None'
                requiredResidentVersion = 1; residentVersion = 0; leaseVersion = 0
            },
            [pscustomobject]@{
                smokeFrameOrdinal = 2; frameSequence = 2; accepted = $false
                selectedTier = 'GPUResidentScene'; executedTier = 'GPUResidentScene'
                executionStatus = 'NotAttempted'; tierFallbackReason = 'None'
                requiredResidentVersion = 2; residentVersion = 2; leaseVersion = 2
            },
            [pscustomobject]@{
                smokeFrameOrdinal = 3; frameSequence = 3; accepted = $true
                selectedTier = 'GPUResidentScene'; executedTier = 'GPUResidentScene'
                executionStatus = 'Completed'; tierFallbackReason = 'None'
                requiredResidentVersion = 2; residentVersion = 2; leaseVersion = 2
            }
        )
    }

    $unacceptedTierTwoRejected = $false
    try {
        $null = Test-RVXGPUSceneTier2CandidateEvidenceDocument `
            -Name 'unaccepted Tier 2 regression' `
            -Evidence $unacceptedTierTwoBetweenColdAndWarm -ExpectedResizeFrame 0
    }
    catch {
        if ($_.Exception.Message -like '*unaccepted GPUResidentScene evidence frame*') {
            $unacceptedTierTwoRejected = $true
        }
        else {
            throw
        }
    }
    if (-not $unacceptedTierTwoRejected) {
        throw 'unaccepted Tier 2 regression was incorrectly accepted'
    }
}

function Resolve-RVXAdapterDriverVersion {
    param([Parameter(Mandatory = $true)][string]$AdapterName)

    try {
        $controllers = @(Get-CimInstance Win32_VideoController -ErrorAction Stop)
        $controller = $controllers | Where-Object {
            -not [string]::IsNullOrWhiteSpace([string]$_.Name) -and
            ([string]$_.Name).Trim() -ieq $AdapterName.Trim()
        } | Select-Object -First 1
        if ($null -ne $controller -and
            -not [string]::IsNullOrWhiteSpace([string]$controller.DriverVersion)) {
            return [ordered]@{
                version = ([string]$controller.DriverVersion).Trim()
                source = 'Win32_VideoController'
            }
        }
    }
    catch {
        # Some constrained Windows sessions deny CIM queries. A vendor query is
        # permitted only when it names the same adapter and returns a version.
    }

    if ($AdapterName -match 'NVIDIA') {
        try {
            $nvidiaLines = @(& nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>$null)
            if ($LASTEXITCODE -eq 0) {
                foreach ($line in $nvidiaLines) {
                    $parts = ([string]$line).Split(',', 2)
                    if ($parts.Count -eq 2 -and $parts[0].Trim() -ieq $AdapterName.Trim() -and
                        -not [string]::IsNullOrWhiteSpace($parts[1])) {
                        return [ordered]@{
                            version = $parts[1].Trim()
                            source = 'NVIDIA-SMI'
                        }
                    }
                }
            }
        }
        catch {
            # Fall through to the fail-closed error below.
        }
    }

    throw "No nonempty driver version is available for the tested adapter '$AdapterName'"
}

New-Item -ItemType Directory -Force -Path $ArtifactRoot | Out-Null
$fixture = Join-Path $SourceRoot 'Tests/Fixtures/ModelViewer/R7Triangle.gltf'
$gpuCapture = Join-Path $ArtifactRoot 'GPUResidentScene.ppm'
$directCapture = Join-Path $ArtifactRoot 'Direct.ppm'
$parityDiff = Join-Path $ArtifactRoot 'GPUResidentScene_vs_Direct.diff.ppm'
$parityReport = Join-Path $ArtifactRoot 'GPUResidentScene_vs_Direct.parity.json'
$gpuEvidence = Join-Path $ArtifactRoot 'GPUResidentScene.evidence.json'
$gbvEvidence = Join-Path $ArtifactRoot 'GPUResidentScene.gbv.evidence.json'
$failureReport = Join-Path $ArtifactRoot 'failure.json'
$provenanceReport = Join-Path $ArtifactRoot 'provenance.json'
$gpuLog = Join-Path $ArtifactRoot 'GPUResidentScene.log'
$directLog = Join-Path $ArtifactRoot 'Direct.log'
$parityLog = Join-Path $ArtifactRoot 'Parity.log'
$gbvLog = Join-Path $ArtifactRoot 'GPUResidentScene.gbv.log'
$leaseLog = Join-Path $ArtifactRoot 'GPUSceneExactLeaseRetirement.log'
$debugQueueReport = Join-Path $ArtifactRoot 'DX12DebugQueue.json'
$gpuQueueReport = Join-Path $ArtifactRoot 'GPUResidentScene.dx12-debug-queue.json'
$directQueueReport = Join-Path $ArtifactRoot 'Direct.dx12-debug-queue.json'
$gbvQueueReport = Join-Path $ArtifactRoot 'GPUResidentScene.gbv.dx12-debug-queue.json'
$leaseQueueReport = Join-Path $ArtifactRoot 'GPUSceneExactLeaseRetirement.dx12-debug-queue.json'

# The runner owns only these exact D4c output files. Clearing them one-by-one
# prevents a succeeding run from being mixed with a stale failure marker or a
# previous machine's evidence, without ever recursively deleting a computed path.
foreach ($outputPath in @(
    $gpuCapture, $directCapture, $parityDiff, $parityReport,
    $gpuEvidence, $gbvEvidence, $failureReport, $provenanceReport,
    $gpuLog, $directLog, $parityLog, $gbvLog, $leaseLog, $debugQueueReport,
    $gpuQueueReport, $directQueueReport, $gbvQueueReport, $leaseQueueReport
)) {
    Remove-Item -LiteralPath $outputPath -Force -ErrorAction SilentlyContinue
}

try {
    # Keep an executable negative regression for the independent evidence
    # parser: a warm record cannot satisfy the contract before its cold phase.
    Assert-RVXGPUSceneTier2NegativeOrderRegression

    # This one process intentionally begins cold, resizes once, and must later
    # execute Tier 2.  It is not allowed to pass by observing only Tier 1.
    Invoke-RVXProcess -Executable $ModelViewer -Name 'DX12 GPU-scene candidate' -Arguments @(
        '--smoke', '--model', $fixture, '--backend', 'dx12',
        '--width', '320', '--height', '180', '--frames', '16',
        '--screenshot', $gpuCapture, '--no-ibl',
        '--gpu-driven-culling-test-scene', '--gpu-driven', 'on',
        '--resize-frame', '6', '--resize-width', '192', '--resize-height', '108',
        '--expect-gpu-scene-tier2-candidate',
        '--gpu-scene-tier2-evidence-report', $gpuEvidence,
        '--validation'
    ) -LogPath $gpuLog -DX12DebugQueueReportPath $gpuQueueReport

    # A separate direct process uses the identical fixture, camera defaults,
    # frame count, and resize timeline as the parity reference.
    Invoke-RVXProcess -Executable $ModelViewer -Name 'DX12 direct parity reference' -Arguments @(
        '--smoke', '--model', $fixture, '--backend', 'dx12',
        '--width', '320', '--height', '180', '--frames', '16',
        '--screenshot', $directCapture, '--no-ibl',
        '--gpu-driven-culling-test-scene', '--gpu-driven', 'off',
        '--resize-frame', '6', '--resize-width', '192', '--resize-height', '108',
        '--expect-gpu-driven-direct-ready', '--validation'
    ) -LogPath $directLog -DX12DebugQueueReportPath $directQueueReport

    Invoke-RVXProcess -Executable $VisualGoldenValidation -Name 'DX12 Tier 2 direct parity' -Arguments @(
        '--expected', $directCapture, '--actual', $gpuCapture,
        '--diff', $parityDiff, '--report', $parityReport,
        '--tolerance', '0.0', '--max-different-pixels', '0'
    ) -LogPath $parityLog

    Invoke-RVXProcess -Executable $DX12Validation -Name 'DX12 exact GPU-scene lease retirement' -Arguments @(
        "--gtest_filter=DX12Validation.GPUSceneExactLeaseRetirementAndFailureClosure"
    ) -LogPath $leaseLog -DX12DebugQueueReportPath $leaseQueueReport

    # GBV is deliberately bounded but still exercises the same cold-to-warm
    # contract rather than a one-frame Tier 1-only smoke invocation.
    Invoke-RVXProcess -Executable $ModelViewer -Name 'DX12 GPU-scene GBV candidate' -Arguments @(
        '--smoke', '--model', $fixture, '--backend', 'dx12',
        '--width', '320', '--height', '180', '--frames', '10',
        '--no-ibl', '--gpu-driven-culling-test-scene', '--gpu-driven', 'on',
        '--expect-gpu-scene-tier2-candidate',
        '--gpu-scene-tier2-evidence-report', $gbvEvidence,
        '--validation', '--gpu-validation'
    ) -LogPath $gbvLog -DX12DebugQueueReportPath $gbvQueueReport

    $gpuEvidenceDocument = Test-RVXGPUSceneTier2CandidateEvidence `
        -Name 'DX12 GPU-scene candidate' -Path $gpuEvidence -ExpectedResizeFrame 6
    $gbvEvidenceDocument = Test-RVXGPUSceneTier2CandidateEvidence `
        -Name 'DX12 GPU-scene GBV candidate' -Path $gbvEvidence -ExpectedResizeFrame 0
    if ([string]::IsNullOrWhiteSpace([string]$gpuEvidenceDocument.adapterName)) {
        throw 'GPU-scene candidate evidence omitted the tested DX12 adapter name'
    }
    $driverEvidence = if (-not [string]::IsNullOrWhiteSpace(
        [string]$gpuEvidenceDocument.driverVersion)) {
        [ordered]@{
            version = ([string]$gpuEvidenceDocument.driverVersion).Trim()
            source = 'RenderDiagnostics'
        }
    } else {
        Resolve-RVXAdapterDriverVersion -AdapterName ([string]$gpuEvidenceDocument.adapterName)
    }
    $gpuQueueGate = Test-RVXDX12DebugQueue -Name 'DX12 GPU-scene candidate' -Path $gpuQueueReport
    $directQueueGate = Test-RVXDX12DebugQueue -Name 'DX12 direct parity reference' -Path $directQueueReport
    $gbvQueueGate = Test-RVXDX12DebugQueue -Name 'DX12 GPU-scene GBV candidate' -Path $gbvQueueReport
    $leaseQueueGate = Test-RVXDX12DebugQueue -Name 'DX12 exact GPU-scene lease retirement' -Path $leaseQueueReport
    $debugLayerObserved = $gpuQueueGate.debugLayerEnabled -and
        $directQueueGate.debugLayerEnabled -and $gbvQueueGate.debugLayerEnabled
    $gpuValidationObserved = Test-RVXLogContains -Path $gbvLog -Pattern 'DX12 GPU-based validation enabled'
    if (-not $debugLayerObserved -or -not $gpuValidationObserved) {
        throw 'DX12 debug-layer or bounded GPU-based-validation enablement was not observed in its process log'
    }
    $reportedErrorLines = @(
        Select-String -LiteralPath @($gpuLog, $directLog, $gbvLog, $leaseLog) `
            -Pattern '\[(CORE|RHI)\] \[error\]' |
            ForEach-Object {
                [ordered]@{
                    path = $_.Path
                    line = $_.LineNumber
                    text = $_.Line
                }
            }
    )
    if ($reportedErrorLines.Count -ne 0) {
        throw "DX12 candidate logs reported $($reportedErrorLines.Count) engine error line(s)"
    }
    [ordered]@{
        schema = 'RVX.Task11D.D4c.DX12DebugQueue'
        schemaVersion = 1
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        debugLayerObserved = $debugLayerObserved
        gpuBasedValidationObserved = $gpuValidationObserved
        queueGates = @($gpuQueueGate, $directQueueGate, $gbvQueueGate, $leaseQueueGate)
        engineErrorLines = $reportedErrorLines
        processLogs = @(
            Get-RVXFileHashRecord $gpuLog
            Get-RVXFileHashRecord $directLog
            Get-RVXFileHashRecord $gbvLog
        )
    } | ConvertTo-Json -Depth 7 | Set-Content -Path $debugQueueReport -Encoding utf8

    $gitSafeDirectory = $SourceRoot.Replace('\', '/')
    $sourceCommitLines = @(& git -c "safe.directory=$gitSafeDirectory" -C $SourceRoot rev-parse HEAD)
    if ($LASTEXITCODE -ne 0 -or $sourceCommitLines.Count -eq 0) {
        throw "Could not resolve the source commit for provenance: $SourceRoot"
    }
    $sourceCommit = $sourceCommitLines[-1].Trim()
    $sourceStatusLines = @(& git -c "safe.directory=$gitSafeDirectory" -C $SourceRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not resolve source worktree status for provenance: $SourceRoot"
    }
    $sourceDirty = [bool]($sourceStatusLines.Count -gt 0)
    $artifactHashes = @()
    foreach ($artifactPath in @(
        $gpuEvidence, $gbvEvidence, $gpuCapture, $directCapture,
        $parityDiff, $parityReport, $gpuLog, $directLog, $parityLog,
        $gbvLog, $leaseLog, $gpuQueueReport, $directQueueReport,
        $gbvQueueReport, $leaseQueueReport, $debugQueueReport
    )) {
        $artifactHashes += Get-RVXFileHashRecord $artifactPath
    }

    $provenance = [ordered]@{
        schema = 'RVX.Task11D.D4c.Provenance'
        schemaVersion = 2
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        source = [ordered]@{
            root = $SourceRoot
            commit = $sourceCommit
            dirty = $sourceDirty
            runner = Get-RVXFileHashRecord $PSCommandPath
        }
        executables = [ordered]@{
            modelViewer = Get-RVXFileHashRecord $ModelViewer
            visualGoldenValidation = Get-RVXFileHashRecord $VisualGoldenValidation
            dx12Validation = Get-RVXFileHashRecord $DX12Validation
        }
        hardware = [ordered]@{
            backend = $gpuEvidenceDocument.backend
            adapterName = $gpuEvidenceDocument.adapterName
            driverVersion = $driverEvidence.version
            driverVersionSource = $driverEvidence.source
        }
        candidateLevel = 'Candidate'
        autoPolicyChanged = $false
        exactParity = [ordered]@{ tolerance = 0.0; maxDifferentPixels = 0 }
        debugQueueReport = Get-RVXFileHashRecord $debugQueueReport
        artifacts = $artifactHashes
    }
    $provenance | ConvertTo-Json -Depth 5 | Set-Content -Path $provenanceReport -Encoding utf8
}
catch {
    [ordered]@{
        schema = 'RVX.Task11D.D4c.Failure'
        schemaVersion = 1
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        message = $_.Exception.Message
        logs = @($gpuLog, $directLog, $parityLog, $gbvLog, $leaseLog)
    } | ConvertTo-Json | Set-Content -Path $failureReport -Encoding utf8
    throw
}
