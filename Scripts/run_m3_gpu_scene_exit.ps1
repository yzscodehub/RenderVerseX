[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ModelViewer,
    [string]$Model = '',
    [string]$SourceRoot = '',
    [string]$OutputDirectory = '',
    [int[]]$Workloads = @(100, 1000, 10000, 50000),
    [ValidateRange(4, 120)]
    [int]$Frames = 12,
    [ValidateRange(1000, 300000)]
    [int]$FrameTimeoutMilliseconds = 120000
)

$ErrorActionPreference = 'Stop'
$directFrames = 4
$scriptDirectory = Split-Path -Parent $PSCommandPath
if ([string]::IsNullOrWhiteSpace($scriptDirectory)) {
    throw 'Could not determine the M3 runner script directory.'
}
if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = Join-Path $scriptDirectory '..'
}
if ([string]::IsNullOrWhiteSpace($Model)) {
    $Model = Join-Path $SourceRoot 'Tests\Fixtures\ModelViewer\R7Triangle.gltf'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $SourceRoot 'build\win_x64_debug\M3Exit\Debug\Task11E'
}

function Get-RVXFileHashRecord {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [ordered]@{ path = $Path; exists = $false }
    }

    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $item.FullName
        exists = $true
        bytes = [uint64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Read-RVXP6Screenshot {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$ExpectedWidth,
        [Parameter(Mandatory = $true)][int]$ExpectedHeight
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Name did not write a screenshot: $Path"
    }

    [byte[]]$bytes = [IO.File]::ReadAllBytes($Path)
    [int]$cursor = 0
    $tokens = [System.Collections.Generic.List[string]]::new()
    while ($tokens.Count -lt 4) {
        while ($cursor -lt $bytes.Length) {
            if ($bytes[$cursor] -eq 35) {
                while ($cursor -lt $bytes.Length -and
                       $bytes[$cursor] -ne 10 -and $bytes[$cursor] -ne 13) {
                    ++$cursor
                }
                continue
            }
            if ($bytes[$cursor] -eq 9 -or $bytes[$cursor] -eq 10 -or
                $bytes[$cursor] -eq 13 -or $bytes[$cursor] -eq 32) {
                ++$cursor
                continue
            }
            break
        }
        if ($cursor -ge $bytes.Length) {
            throw "$Name screenshot has an incomplete PPM header: $Path"
        }

        $tokenStart = $cursor
        while ($cursor -lt $bytes.Length -and
               $bytes[$cursor] -ne 9 -and $bytes[$cursor] -ne 10 -and
               $bytes[$cursor] -ne 13 -and $bytes[$cursor] -ne 32) {
            ++$cursor
        }
        $tokens.Add([Text.Encoding]::ASCII.GetString(
            $bytes, $tokenStart, $cursor - $tokenStart))
    }

    if ($tokens[0] -ne 'P6') {
        throw "$Name screenshot is not binary P6 PPM: $Path"
    }
    try {
        $width = [Convert]::ToInt32($tokens[1], [Globalization.CultureInfo]::InvariantCulture)
        $height = [Convert]::ToInt32($tokens[2], [Globalization.CultureInfo]::InvariantCulture)
        $maxValue = [Convert]::ToInt32($tokens[3], [Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        throw "$Name screenshot has invalid PPM dimensions: $Path"
    }
    if ($width -ne $ExpectedWidth -or $height -ne $ExpectedHeight -or
        $maxValue -ne 255) {
        throw "$Name screenshot must be P6 $ExpectedWidth x $ExpectedHeight / 255: $Path"
    }

    if ($cursor -ge $bytes.Length -or
        ($bytes[$cursor] -ne 9 -and $bytes[$cursor] -ne 10 -and
         $bytes[$cursor] -ne 13 -and $bytes[$cursor] -ne 32)) {
        throw "$Name screenshot PPM header is not terminated: $Path"
    }
    if ($bytes[$cursor] -eq 13 -and $cursor + 1 -lt $bytes.Length -and
        $bytes[$cursor + 1] -eq 10) {
        $cursor += 2
    }
    else {
        ++$cursor
    }

    [uint64]$pixelByteCount = [uint64]$width * [uint64]$height * 3
    if ($pixelByteCount -gt [uint64]::MaxValue - [uint64]$cursor -or
        [uint64]$bytes.Length -ne [uint64]$cursor + $pixelByteCount) {
        throw "$Name screenshot has an unexpected PPM pixel byte count: $Path"
    }

    return [pscustomobject]@{
        bytes = $bytes
        pixelOffset = [uint64]$cursor
        pixelByteCount = $pixelByteCount
        width = $width
        height = $height
        maxValue = $maxValue
        artifact = Get-RVXFileHashRecord $Path
    }
}

function Test-RVXExactPPMScreenshotParity {
    param(
        [Parameter(Mandatory = $true)][uint64]$Workload,
        [Parameter(Mandatory = $true)][string]$DirectPath,
        [Parameter(Mandatory = $true)][string]$GPUPath
    )

    $direct = Read-RVXP6Screenshot -Name "M3 Direct $Workload" -Path $DirectPath `
        -ExpectedWidth 320 -ExpectedHeight 180
    $gpu = Read-RVXP6Screenshot -Name "M3 GPU $Workload" -Path $GPUPath `
        -ExpectedWidth 320 -ExpectedHeight 180

    if ($direct.pixelByteCount -ne $gpu.pixelByteCount) {
        throw "M3 Direct/GPU screenshot pixel-size mismatch for workload $Workload"
    }

    [uint64]$differentPixelByteCount = 0
    for ([uint64]$index = 0; $index -lt $direct.pixelByteCount; ++$index) {
        if ($direct.bytes[$direct.pixelOffset + $index] -ne
            $gpu.bytes[$gpu.pixelOffset + $index]) {
            ++$differentPixelByteCount
        }
    }

    $sha256ExactMatch = $direct.artifact.sha256 -eq $gpu.artifact.sha256
    $exactPixelParity = $differentPixelByteCount -eq 0
    if (-not $sha256ExactMatch -and -not $exactPixelParity) {
        throw "M3 Direct/GPU screenshots differ for workload ${Workload}: $differentPixelByteCount pixel byte(s)"
    }

    return [pscustomobject]@{
        exactPixelParity = $exactPixelParity
        sha256ExactMatch = $sha256ExactMatch
        differentPixelByteCount = $differentPixelByteCount
        direct = $direct.artifact
        gpu = $gpu.artifact
    }
}

function Get-RVXRequiredProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "$Context is missing required field '$Name'"
    }
    return $property.Value
}

function Get-RVXUInt64 {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Context
    )

    try {
        return [Convert]::ToUInt64($Value, [Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        throw "$Context must be an unsigned integer"
    }
}

function Test-RVXDX12DebugQueue {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Name did not write a DX12 debug queue report: $Path"
    }
    $queue = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ((Get-RVXRequiredProperty $queue 'schema' "$Name DX12 debug queue") -ne 'RVX.DX12DebugQueue' -or
        (Get-RVXUInt64 (Get-RVXRequiredProperty $queue 'schemaVersion' "$Name DX12 debug queue") "$Name DX12 schemaVersion") -ne 1 -or
        -not [bool](Get-RVXRequiredProperty $queue 'debugLayerEnabled' "$Name DX12 debug queue") -or
        -not [bool](Get-RVXRequiredProperty $queue 'available' "$Name DX12 debug queue") -or
        -not [bool](Get-RVXRequiredProperty $queue 'readComplete' "$Name DX12 debug queue") -or
        -not [bool](Get-RVXRequiredProperty $queue 'passed' "$Name DX12 debug queue") -or
        (Get-RVXUInt64 (Get-RVXRequiredProperty $queue 'errorCount' "$Name DX12 debug queue") "$Name DX12 errorCount") -ne 0 -or
        (Get-RVXUInt64 (Get-RVXRequiredProperty $queue 'corruptionCount' "$Name DX12 debug queue") "$Name DX12 corruptionCount") -ne 0) {
        throw "$Name reported unavailable or non-clean DX12 InfoQueue evidence"
    }

    return [pscustomobject]@{
        name = $Name
        debugLayerEnabled = $true
        available = $true
        readComplete = $true
        messageCount = Get-RVXUInt64 (Get-RVXRequiredProperty $queue 'messageCount' "$Name DX12 debug queue") "$Name DX12 messageCount"
        errorCount = [uint64]0
        corruptionCount = [uint64]0
        passed = $true
        report = Get-RVXFileHashRecord $Path
    }
}

function Assert-RVXNoEngineErrors {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        throw "$Name did not write a process log: $LogPath"
    }
    $matches = @(Select-String -LiteralPath $LogPath -Pattern '\[(CORE|RHI|RENDER|SCENE)\] \[error\]')
    if ($matches.Count -ne 0) {
        throw "$Name log contains $($matches.Count) engine error line(s): $LogPath"
    }
}

function Invoke-RVXModelViewer {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [Parameter(Mandatory = $true)][string]$DX12DebugQueueReportPath
    )

    $oldQueueReport = $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH
    try {
        $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH = $DX12DebugQueueReportPath
        & $Executable @Arguments 2>&1 | Tee-Object -LiteralPath $LogPath
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldQueueReport) {
            Remove-Item Env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH -ErrorAction SilentlyContinue
        }
        else {
            $env:RVX_DX12_DEBUG_QUEUE_REPORT_PATH = $oldQueueReport
        }
    }
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode (log: $LogPath)"
    }
    Assert-RVXNoEngineErrors -Name $Name -LogPath $LogPath
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
        # A vendor query is allowed only when it names the same tested adapter.
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

function Test-RVXGPUSceneM3Tables {
    param(
        [Parameter(Mandatory = $true)][object]$GPUScene,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $tables = @(Get-RVXRequiredProperty $GPUScene 'tables' "$Name gpuScene")
    if ($tables.Count -ne 6) {
        throw "$Name must report exactly six GPU-scene tables; observed $($tables.Count)"
    }

    [uint64]$cpuPayloadBytes = 0
    [uint64]$cpuReservedBytes = 0
    [uint64]$residentBytes = 0
    [uint64]$frameUploadBytes = 0
    [uint64]$cumulativeUploadBytes = 0
    [uint64]$peakFrameUploadBytes = 0
    [uint64]$frameUploadRanges = 0
    [uint64]$cumulativeUploadRanges = 0
    for ($index = 0; $index -lt $tables.Count; ++$index) {
        $table = $tables[$index]
        $context = "$Name table[$index]"
        $payload = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'payloadRowCount' $context) "$context payloadRowCount"
        $cpuCapacity = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'cpuRowCapacity' $context) "$context cpuRowCapacity"
        $residentCapacity = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'residentCapacity' $context) "$context residentCapacity"
        $stride = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'stride' $context) "$context stride"
        $cpuPayload = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'cpuPayloadBytes' $context) "$context cpuPayloadBytes"
        $cpuReserved = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'cpuReservedBytes' $context) "$context cpuReservedBytes"
        $resident = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'residentBytes' $context) "$context residentBytes"
        $frameUpload = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'frameUploadBytes' $context) "$context frameUploadBytes"
        $cumulativeUpload = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'cumulativeUploadBytes' $context) "$context cumulativeUploadBytes"
        $peakFrameUpload = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'peakFrameUploadBytes' $context) "$context peakFrameUploadBytes"
        $frameRanges = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'frameUploadRangeCount' $context) "$context frameUploadRangeCount"
        $cumulativeRanges = Get-RVXUInt64 (Get-RVXRequiredProperty $table 'cumulativeUploadRangeCount' $context) "$context cumulativeUploadRangeCount"

        if ($payload -eq 0 -or $cpuCapacity -lt $payload -or $residentCapacity -lt $payload -or
            $stride -eq 0 -or $cpuPayload -ne ($payload * $stride) -or
            $cpuReserved -lt $cpuPayload -or $resident -ne ($residentCapacity * $stride) -or
            -not [bool](Get-RVXRequiredProperty $table 'resident' $context) -or
            [bool](Get-RVXRequiredProperty $table 'fullUpload' $context) -or
            $frameUpload -ne 0 -or $frameRanges -ne 0 -or $cumulativeUpload -eq 0 -or
            $peakFrameUpload -eq 0 -or $cumulativeUpload -lt $peakFrameUpload -or
            $cumulativeRanges -eq 0) {
            throw "$context does not describe a resident warm-static GPU-scene table"
        }

        $cpuPayloadBytes += $cpuPayload
        $cpuReservedBytes += $cpuReserved
        $residentBytes += $resident
        $frameUploadBytes += $frameUpload
        $cumulativeUploadBytes += $cumulativeUpload
        $peakFrameUploadBytes += $peakFrameUpload
        $frameUploadRanges += $frameRanges
        $cumulativeUploadRanges += $cumulativeRanges
    }

    $aggregateValues = [ordered]@{
        cpuPayloadBytes = $cpuPayloadBytes
        cpuReservedBytes = $cpuReservedBytes
        frameUploadBytes = $frameUploadBytes
        cumulativeUploadBytes = $cumulativeUploadBytes
        peakFrameUploadBytes = $peakFrameUploadBytes
        frameUploadRangeCount = $frameUploadRanges
        cumulativeUploadRangeCount = $cumulativeUploadRanges
    }
    foreach ($entry in $aggregateValues.GetEnumerator()) {
        $actual = Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene $entry.Key "$Name gpuScene") "$Name gpuScene.$($entry.Key)"
        if ($actual -ne $entry.Value) {
            throw "$Name gpuScene.$($entry.Key) does not equal the exact sum of its six tables"
        }
    }

    $gpuAllocation = Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene 'gpuAllocationBytes' "$Name gpuScene") "$Name gpuScene.gpuAllocationBytes"
    if ($gpuAllocation -lt $residentBytes -or
        [bool](Get-RVXRequiredProperty $GPUScene 'fullUpload' "$Name gpuScene")) {
        throw "$Name GPU allocation/full-upload aggregate is inconsistent with its resident tables"
    }
}

function Test-RVXGPUSceneM3Lifecycle {
    param(
        [Parameter(Mandatory = $true)][object]$GPUScene,
        [Parameter(Mandatory = $true)][uint64]$Workload,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (-not [bool](Get-RVXRequiredProperty $GPUScene 'available' "$Name gpuScene") -or
        -not [bool](Get-RVXRequiredProperty $GPUScene 'informationalOnly' "$Name gpuScene") -or
        -not [bool](Get-RVXRequiredProperty $GPUScene 'publicationAttempted' "$Name gpuScene") -or
        -not [bool](Get-RVXRequiredProperty $GPUScene 'publicationCandidate' "$Name gpuScene") -or
        -not [bool](Get-RVXRequiredProperty $GPUScene 'publicationPublished' "$Name gpuScene") -or
        [bool](Get-RVXRequiredProperty $GPUScene 'publicationFailed' "$Name gpuScene") -or
        -not [bool](Get-RVXRequiredProperty $GPUScene 'publicationComplete' "$Name gpuScene") -or
        (Get-RVXRequiredProperty $GPUScene 'publicationFailureReason' "$Name gpuScene") -ne 'None' -or
        (Get-RVXRequiredProperty $GPUScene 'uploadFailureReason' "$Name gpuScene") -ne 'None') {
        throw "$Name GPU-scene publication/upload did not complete cleanly"
    }

    foreach ($field in @(
        'committedVersion', 'residentVersion', 'safeReclaimVersion',
        'requiredResidentVersion', 'leaseVersion', 'currentBufferSetCount',
        'peakBufferSetCount', 'pendingBufferSetCount', 'inFlightBufferSetCount',
        'unusableBufferSetCount')) {
        $null = Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene $field "$Name gpuScene") "$Name gpuScene.$field"
    }
    if ((Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene 'committedVersion' "$Name gpuScene") "$Name committedVersion") -eq 0 -or
        (Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene 'residentVersion' "$Name gpuScene") "$Name residentVersion") -eq 0 -or
        (Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene 'currentBufferSetCount' "$Name gpuScene") "$Name currentBufferSetCount") -eq 0 -or
        (Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene 'peakBufferSetCount' "$Name gpuScene") "$Name peakBufferSetCount") -lt
        (Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene 'currentBufferSetCount' "$Name gpuScene") "$Name currentBufferSetCount") -or
        (Get-RVXUInt64 (Get-RVXRequiredProperty $GPUScene 'unusableBufferSetCount' "$Name gpuScene") "$Name unusableBufferSetCount") -ne 0) {
        throw "$Name GPU-scene buffer-set lifecycle is incomplete or unhealthy"
    }

    $slots = Get-RVXRequiredProperty $GPUScene 'slots' "$Name gpuScene"
    $expectedSlots = $Workload * 6
    $expectedDrawBlocks = $Workload
    $expected = [ordered]@{
        liveSlotCount = $expectedSlots
        freeSlotCount = [uint64]0
        retiredSlotCount = [uint64]0
        permanentlyRetiredSlotCount = [uint64]0
        liveDrawBlockCount = $expectedDrawBlocks
        freeDrawBlockCount = [uint64]0
        retiredDrawBlockCount = [uint64]0
        reclaimedSlotCount = [uint64]0
        reclaimedDrawBlockCount = [uint64]0
        conservativeReusePressureCount = [uint64]0
    }
    foreach ($entry in $expected.GetEnumerator()) {
        $actual = Get-RVXUInt64 (Get-RVXRequiredProperty $slots $entry.Key "$Name slots") "$Name slots.$($entry.Key)"
        if ($actual -ne $entry.Value) {
            throw "$Name slots.$($entry.Key) expected $($entry.Value), observed $actual"
        }
    }
}

function Test-RVXGPUSceneM3Timing {
    param(
        [Parameter(Mandatory = $true)][object]$Evidence,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $timing = Get-RVXRequiredProperty $Evidence 'timing' $Name
    if (-not [bool](Get-RVXRequiredProperty $timing 'nonGating' "$Name timing") -or
        [bool](Get-RVXRequiredProperty $timing 'usedForAutoDecision' "$Name timing")) {
        throw "$Name timing violates the non-gating M3 contract"
    }
    foreach ($field in @('cpuPlanTimingAvailable', 'cpuSubmissionTimingAvailable')) {
        if (-not [bool](Get-RVXRequiredProperty $timing $field "$Name timing")) {
            throw "$Name timing.$field must be true for M3 evidence"
        }
    }
    foreach ($field in @('cpuPlanMilliseconds', 'cpuSubmissionMilliseconds')) {
        try {
            $milliseconds = [Convert]::ToDouble(
                (Get-RVXRequiredProperty $timing $field "$Name timing"),
                [Globalization.CultureInfo]::InvariantCulture)
        }
        catch {
            throw "$Name timing.$field is not numeric"
        }
        if ([double]::IsNaN($milliseconds) -or
            [double]::IsInfinity($milliseconds) -or
            $milliseconds -lt 0.0) {
            throw "$Name timing.$field must be finite and nonnegative"
        }
    }

    $delayedGpuAvailable = [bool](Get-RVXRequiredProperty $timing 'delayedGpuTimingAvailable' "$Name timing")
    try {
        $delayedGpuMilliseconds = [Convert]::ToDouble(
            (Get-RVXRequiredProperty $timing 'delayedGpuMilliseconds' "$Name timing"),
            [Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        throw "$Name timing.delayedGpuMilliseconds is not numeric"
    }
    if ([double]::IsNaN($delayedGpuMilliseconds) -or
        [double]::IsInfinity($delayedGpuMilliseconds)) {
        throw "$Name timing.delayedGpuMilliseconds must be finite"
    }
    if ($delayedGpuAvailable) {
        if ($delayedGpuMilliseconds -lt 0.0) {
            throw "$Name timing.delayedGpuMilliseconds must be nonnegative when available"
        }
    }
    elseif ($delayedGpuMilliseconds -ne 0.0) {
        throw "$Name timing.delayedGpuMilliseconds must be zero when unavailable"
    }
}

function Test-RVXGPUSceneM3Frames {
    param(
        [Parameter(Mandatory = $true)][object]$Evidence,
        [Parameter(Mandatory = $true)][ValidateSet('Direct', 'GPU')][string]$Mode,
        [Parameter(Mandatory = $true)][int]$ExpectedFrameCount,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $frames = @(Get-RVXRequiredProperty $Evidence 'frames' $Name)
    if ($frames.Count -ne $ExpectedFrameCount) {
        throw "$Name must report $ExpectedFrameCount frames; observed $($frames.Count)"
    }
    [uint64]$previousOrdinal = 0
    [uint64]$previousSequence = 0
    $coldObserved = $false
    $warmObserved = $false
    foreach ($frame in $frames) {
        $context = "$Name frame"
        $ordinal = Get-RVXUInt64 (Get-RVXRequiredProperty $frame 'smokeFrameOrdinal' $context) "$context smokeFrameOrdinal"
        $sequence = Get-RVXUInt64 (Get-RVXRequiredProperty $frame 'frameSequence' $context) "$context frameSequence"
        if ($ordinal -eq 0 -or $ordinal -le $previousOrdinal -or $sequence -eq 0 -or $sequence -le $previousSequence) {
            throw "$Name has missing, duplicate, or out-of-order frame evidence"
        }
        $previousOrdinal = $ordinal
        $previousSequence = $sequence
        $selected = [string](Get-RVXRequiredProperty $frame 'selectedTier' $context)
        $executed = [string](Get-RVXRequiredProperty $frame 'executedTier' $context)
        $accepted = [bool](Get-RVXRequiredProperty $frame 'accepted' $context)

        if ($Mode -eq 'Direct') {
            if (-not $accepted -or $selected -ne 'Direct' -or $executed -ne 'Direct' -or
                (Get-RVXRequiredProperty $frame 'executionStatus' $context) -ne 'Completed') {
                throw "$Name requires every evidence frame to complete using Direct"
            }
            continue
        }

        if (-not $accepted) {
            if ($selected -eq 'GPUResidentScene' -or $executed -eq 'GPUResidentScene') {
                throw "$Name has an unaccepted Tier 2 frame"
            }
            throw "$Name has an unaccepted GPU-driven frame"
        }
        if ($selected -ne $executed -or
            (Get-RVXRequiredProperty $frame 'executionStatus' $context) -ne 'Completed' -or
            (Get-RVXRequiredProperty $frame 'tierFallbackReason' $context) -ne 'None') {
            throw "$Name has an incomplete or fallback GPU-driven execution"
        }
        if ($executed -eq 'IndirectGrouped') {
            if ($warmObserved -or
                (Get-RVXUInt64 (Get-RVXRequiredProperty $frame 'residentVersion' $context) "$context residentVersion") -ne 0 -or
                (Get-RVXUInt64 (Get-RVXRequiredProperty $frame 'leaseVersion' $context) "$context leaseVersion") -ne 0) {
                throw "$Name has an invalid cold/prewarm Tier 1 frame"
            }
            $coldObserved = $true
        }
        elseif ($executed -eq 'GPUResidentScene') {
            $required = Get-RVXUInt64 (Get-RVXRequiredProperty $frame 'requiredResidentVersion' $context) "$context requiredResidentVersion"
            $resident = Get-RVXUInt64 (Get-RVXRequiredProperty $frame 'residentVersion' $context) "$context residentVersion"
            $lease = Get-RVXUInt64 (Get-RVXRequiredProperty $frame 'leaseVersion' $context) "$context leaseVersion"
            if (-not $coldObserved -or $required -eq 0 -or $required -ne $resident -or $required -ne $lease) {
                throw "$Name has an invalid warm Tier 2 resident/lease version"
            }
            $warmObserved = $true
        }
        else {
            throw "$Name executed an unexpected GPU-driven tier '$executed'"
        }
    }
    if ($Mode -eq 'GPU' -and (-not $coldObserved -or -not $warmObserved)) {
        throw "$Name did not prove cold/prewarm Tier 1 followed by warm Tier 2"
    }

    return [pscustomobject]@{
        frameCount = $frames.Count
        coldTier1Observed = $coldObserved
        warmTier2Observed = $warmObserved
    }
}

function Test-RVXGPUSceneM3Evidence {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][uint64]$Workload,
        [Parameter(Mandatory = $true)][ValidateSet('Direct', 'GPU')][string]$Mode,
        [Parameter(Mandatory = $true)][int]$ExpectedFrameCount
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Name did not write M3 evidence: $Path"
    }
    $evidence = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ((Get-RVXRequiredProperty $evidence 'schema' $Name) -ne 'RVX.GPUSceneM3Evidence' -or
        (Get-RVXUInt64 (Get-RVXRequiredProperty $evidence 'schemaVersion' $Name) "$Name schemaVersion") -ne 1 -or
        -not [bool](Get-RVXRequiredProperty $evidence 'nonGating' $Name) -or
        [bool](Get-RVXRequiredProperty $evidence 'usedForAutoDecision' $Name) -or
        (Get-RVXRequiredProperty $evidence 'backend' $Name) -ne 'DirectX 12') {
        throw "$Name has an unsupported or gating M3 evidence document"
    }

    $adapterName = ([string](Get-RVXRequiredProperty $evidence 'adapterName' $Name)).Trim()
    if ([string]::IsNullOrWhiteSpace($adapterName)) {
        throw "$Name omitted the tested DX12 adapter name"
    }
    $workloadRecord = Get-RVXRequiredProperty $evidence 'workload' $Name
    foreach ($field in @(
        'requestedPrimitiveCount', 'sourceStaticMeshPrimitiveCount',
        'sourceSceneObjectCount', 'publishedObjectCount')) {
        $actual = Get-RVXUInt64 (Get-RVXRequiredProperty $workloadRecord $field "$Name workload") "$Name workload.$field"
        if ($actual -ne $Workload) {
            throw "$Name workload.$field expected $Workload, observed $actual"
        }
    }
    $packetCount = Get-RVXUInt64 (Get-RVXRequiredProperty $workloadRecord 'passPacketCount' "$Name workload") "$Name workload.passPacketCount"
    if ($packetCount -lt $Workload) {
        throw "$Name reported fewer pass packets than published source primitives"
    }

    Test-RVXGPUSceneM3Timing -Evidence $evidence -Name $Name
    $gpuScene = Get-RVXRequiredProperty $evidence 'gpuScene' $Name
    Test-RVXGPUSceneM3Tables -GPUScene $gpuScene -Name $Name
    Test-RVXGPUSceneM3Lifecycle -GPUScene $gpuScene -Workload $Workload -Name $Name
    $tierEvidence = Test-RVXGPUSceneM3Frames -Evidence $evidence -Mode $Mode -ExpectedFrameCount $ExpectedFrameCount -Name $Name

    return [pscustomobject]@{
        evidence = $evidence
        adapterName = $adapterName
        driverVersion = ([string](Get-RVXRequiredProperty $evidence 'driverVersion' $Name)).Trim()
        tierEvidence = $tierEvidence
        artifact = Get-RVXFileHashRecord $Path
    }
}

if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    throw "SourceRoot is not an existing directory: $SourceRoot"
}
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
if (-not (Test-Path -LiteralPath $ModelViewer -PathType Leaf)) {
    throw "ModelViewer executable was not found: $ModelViewer"
}
if (-not (Test-Path -LiteralPath $Model -PathType Leaf)) {
    throw "GPU-scene workload fixture was not found: $Model"
}
$ModelViewer = (Resolve-Path -LiteralPath $ModelViewer).Path
$Model = (Resolve-Path -LiteralPath $Model).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$seenWorkloads = @{}
foreach ($workload in $Workloads) {
    if ($workload -notin @(100, 1000, 10000, 50000)) {
        throw "Unsupported workload '$workload'. Supported workloads are 100, 1000, 10000, 50000."
    }
    if ($seenWorkloads.ContainsKey($workload)) {
        throw "Workload '$workload' was specified more than once."
    }
    $seenWorkloads[$workload] = $true
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$summaryPath = Join-Path $OutputDirectory 'GPUSceneM3Exit.summary.json'
$failurePath = Join-Path $OutputDirectory 'GPUSceneM3Exit.failure.json'
$knownArtifacts = @($summaryPath, $failurePath)
foreach ($workload in $Workloads) {
    $knownArtifacts += @(
        (Join-Path $OutputDirectory "GPUSceneM3.Direct.$workload.json"),
        (Join-Path $OutputDirectory "GPUSceneM3.GPU.$workload.json"),
        (Join-Path $OutputDirectory "GPUSceneM3.Direct.$workload.log"),
        (Join-Path $OutputDirectory "GPUSceneM3.GPU.$workload.log"),
        (Join-Path $OutputDirectory "GPUSceneM3.Direct.$workload.ppm"),
        (Join-Path $OutputDirectory "GPUSceneM3.GPU.$workload.ppm"),
        (Join-Path $OutputDirectory "GPUSceneM3.Direct.$workload.dx12-debug-queue.json"),
        (Join-Path $OutputDirectory "GPUSceneM3.GPU.$workload.dx12-debug-queue.json")
    )
}
# The runner owns only these known paths.  Clearing them individually prevents
# stale evidence from satisfying a later run without deleting arbitrary output.
foreach ($artifact in $knownArtifacts) {
    Remove-Item -LiteralPath $artifact -Force -ErrorAction SilentlyContinue
}

$rows = @()
$queueGates = @()
$processLogs = @()
try {
    foreach ($workload in $Workloads) {
        $directEvidencePath = Join-Path $OutputDirectory "GPUSceneM3.Direct.$workload.json"
        $gpuEvidencePath = Join-Path $OutputDirectory "GPUSceneM3.GPU.$workload.json"
        $directLogPath = Join-Path $OutputDirectory "GPUSceneM3.Direct.$workload.log"
        $gpuLogPath = Join-Path $OutputDirectory "GPUSceneM3.GPU.$workload.log"
        $directScreenshotPath = Join-Path $OutputDirectory "GPUSceneM3.Direct.$workload.ppm"
        $gpuScreenshotPath = Join-Path $OutputDirectory "GPUSceneM3.GPU.$workload.ppm"
        $directQueuePath = Join-Path $OutputDirectory "GPUSceneM3.Direct.$workload.dx12-debug-queue.json"
        $gpuQueuePath = Join-Path $OutputDirectory "GPUSceneM3.GPU.$workload.dx12-debug-queue.json"

        Invoke-RVXModelViewer -Name "M3 Direct $workload" -Executable $ModelViewer -Arguments @(
            '--smoke', '--model', $Model, '--backend', 'dx12', '--width', '320', '--height', '180',
            '--frames', "$directFrames", '--no-ibl', '--gpu-driven', 'off',
            '--smoke-frame-timeout-ms', "$FrameTimeoutMilliseconds",
            '--expect-gpu-driven-direct-ready', '--gpu-scene-workload-primitives', "$workload",
            '--gpu-scene-m3-evidence-report', $directEvidencePath,
            '--screenshot', $directScreenshotPath, '--validation'
        ) -LogPath $directLogPath -DX12DebugQueueReportPath $directQueuePath
        Invoke-RVXModelViewer -Name "M3 GPU $workload" -Executable $ModelViewer -Arguments @(
            '--smoke', '--model', $Model, '--backend', 'dx12', '--width', '320', '--height', '180',
            '--frames', "$Frames", '--no-ibl', '--gpu-driven', 'on',
            '--smoke-frame-timeout-ms', "$FrameTimeoutMilliseconds",
            '--gpu-scene-workload-primitives', "$workload",
            '--gpu-scene-m3-evidence-report', $gpuEvidencePath,
            '--screenshot', $gpuScreenshotPath, '--validation'
        ) -LogPath $gpuLogPath -DX12DebugQueueReportPath $gpuQueuePath

        $direct = Test-RVXGPUSceneM3Evidence -Name "M3 Direct $workload" -Path $directEvidencePath -Workload $workload -Mode Direct -ExpectedFrameCount $directFrames
        $gpu = Test-RVXGPUSceneM3Evidence -Name "M3 GPU $workload" -Path $gpuEvidencePath -Workload $workload -Mode GPU -ExpectedFrameCount $Frames
        if ($direct.adapterName -ine $gpu.adapterName) {
            throw "M3 Direct/GPU adapter mismatch for workload $workload"
        }
        $screenshotParity = Test-RVXExactPPMScreenshotParity -Workload $workload `
            -DirectPath $directScreenshotPath -GPUPath $gpuScreenshotPath

        $queueGates += Test-RVXDX12DebugQueue -Name "M3 Direct $workload" -Path $directQueuePath
        $queueGates += Test-RVXDX12DebugQueue -Name "M3 GPU $workload" -Path $gpuQueuePath
        $processLogs += Get-RVXFileHashRecord $directLogPath
        $processLogs += Get-RVXFileHashRecord $gpuLogPath
        $rows += [pscustomobject]@{
            workload = [uint64]$workload
            adapterName = $gpu.adapterName
            direct = [pscustomobject]@{
                evidence = $direct.artifact
                log = Get-RVXFileHashRecord $directLogPath
                screenshot = $screenshotParity.direct
                debugQueue = Get-RVXFileHashRecord $directQueuePath
                frameCount = $direct.tierEvidence.frameCount
            }
            gpu = [pscustomobject]@{
                evidence = $gpu.artifact
                log = Get-RVXFileHashRecord $gpuLogPath
                screenshot = $screenshotParity.gpu
                debugQueue = Get-RVXFileHashRecord $gpuQueuePath
                frameCount = $gpu.tierEvidence.frameCount
                coldTier1Observed = $gpu.tierEvidence.coldTier1Observed
                warmTier2Observed = $gpu.tierEvidence.warmTier2Observed
            }
            screenshotParity = $screenshotParity
        }
    }

    if ($rows.Count -eq 0) {
        throw 'No M3 workloads were selected.'
    }
    $firstEvidencePath = Join-Path $OutputDirectory "GPUSceneM3.GPU.$($Workloads[0]).json"
    $firstEvidence = Get-Content -LiteralPath $firstEvidencePath -Raw | ConvertFrom-Json
    $adapterName = ([string]$firstEvidence.adapterName).Trim()
    $embeddedDriverVersion = ([string]$firstEvidence.driverVersion).Trim()
    $driverEvidence = if (-not [string]::IsNullOrWhiteSpace($embeddedDriverVersion)) {
        [ordered]@{ version = $embeddedDriverVersion; source = 'RenderDiagnostics' }
    }
    else {
        Resolve-RVXAdapterDriverVersion -AdapterName $adapterName
    }

    $gitSafeDirectory = $SourceRoot.Replace('\', '/')
    $commitLines = @(& git -c "safe.directory=$gitSafeDirectory" -C $SourceRoot rev-parse HEAD)
    if ($LASTEXITCODE -ne 0 -or $commitLines.Count -eq 0) {
        throw "Could not resolve source commit for provenance: $SourceRoot"
    }
    $statusLines = @(& git -c "safe.directory=$gitSafeDirectory" -C $SourceRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not resolve source worktree status for provenance: $SourceRoot"
    }

    [ordered]@{
        schema = 'RVX.GPUSceneM3Exit'
        schemaVersion = 2
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        nonGating = $true
        usedForAutoDecision = $false
        backend = 'DX12'
        adapterName = $adapterName
        driverVersion = $driverEvidence.version
        driverVersionSource = $driverEvidence.source
        configuration = [ordered]@{
            directFrames = $directFrames
            gpuFrames = $Frames
            frameTimeoutMilliseconds = $FrameTimeoutMilliseconds
            workloads = @($Workloads)
        }
        provenance = [ordered]@{
            sourceRoot = $SourceRoot
            commit = $commitLines[-1].Trim()
            treeDirty = [bool]($statusLines.Count -gt 0)
            runner = Get-RVXFileHashRecord $PSCommandPath
            modelViewer = Get-RVXFileHashRecord $ModelViewer
            model = Get-RVXFileHashRecord $Model
            screenshotArtifacts = @(
                $rows | ForEach-Object {
                    $_.direct.screenshot
                    $_.gpu.screenshot
                }
            )
        }
        debugQueueGates = $queueGates
        processLogs = $processLogs
        workloads = $rows
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding utf8
    Write-Host "GPU-scene M3 evidence summary: $summaryPath"
}
catch {
    [ordered]@{
        schema = 'RVX.GPUSceneM3ExitFailure'
        schemaVersion = 1
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        message = $_.Exception.Message
        summaryPath = $summaryPath
        logs = @(
            $knownArtifacts |
                Where-Object { $_.EndsWith('.log', [StringComparison]::OrdinalIgnoreCase) } |
                ForEach-Object { Get-RVXFileHashRecord $_ }
        )
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $failurePath -Encoding utf8
    throw
}
