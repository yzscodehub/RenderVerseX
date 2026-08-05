[CmdletBinding()]
param(
    [switch]$ListOnly,

    [string]$BuildDir = '',

    [string]$Configuration = 'Debug',

    [string]$ReportDirectory = '',

    [string]$ModelViewer = '',

    [string]$GPUDrivenValidation = ''
)

$ErrorActionPreference = 'Stop'

# Task12 is a backend qualification runner, rather than a golden-image test.
# It owns only the explicitly named files below in ReportDirectory.  In
# particular, it never recursively clears a user-selected directory.
$SourceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $SourceRoot 'build/win_x64_debug'
}
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

if ([string]::IsNullOrWhiteSpace($ReportDirectory)) {
    $ReportDirectory = Join-Path $BuildDir (Join-Path 'Task12Exit' $Configuration)
}
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)

function Get-RVXExecutablePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativeStem
    )

    $candidate = Join-Path $Root $RelativeStem
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    if ($IsWindows -or $env:OS -eq 'Windows_NT') {
        $windowsCandidate = "$candidate.exe"
        if (Test-Path -LiteralPath $windowsCandidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $windowsCandidate).Path
        }
    }

    return $candidate
}

if ([string]::IsNullOrWhiteSpace($ModelViewer)) {
    $ModelViewer = Get-RVXExecutablePath -Root $BuildDir -RelativeStem "Samples/Showcase/ModelViewer/$Configuration/ModelViewer"
} else {
    $ModelViewer = [IO.Path]::GetFullPath($ModelViewer)
}

if ([string]::IsNullOrWhiteSpace($GPUDrivenValidation)) {
    $GPUDrivenValidation = Get-RVXExecutablePath -Root $BuildDir -RelativeStem "Tests/$Configuration/GPUDrivenValidation"
} else {
    $GPUDrivenValidation = [IO.Path]::GetFullPath($GPUDrivenValidation)
}

$Fixture = Join-Path $SourceRoot 'Tests/Fixtures/ModelViewer/R7Triangle.gltf'
$Width = 320
$Height = 180
$ResizeWidth = 192
$ResizeHeight = 108
$Frames = 8
$ResizeFrame = 4

$DirectCapture = Join-Path $ReportDirectory 'Direct.ppm'
$DirectResizeCapture = Join-Path $ReportDirectory 'DirectResize.ppm'
$GPUCapture = Join-Path $ReportDirectory 'GPU.ppm'
$GPURepeatCapture = Join-Path $ReportDirectory 'GPURepeat.ppm'
$GPUResizeCapture = Join-Path $ReportDirectory 'GPUResize.ppm'
$GPUZeroVisibleCapture = Join-Path $ReportDirectory 'GPUZeroVisible.ppm'
$DirectReport = Join-Path $ReportDirectory 'Direct.render-policy.json'
$DirectResizeReport = Join-Path $ReportDirectory 'DirectResize.render-policy.json'
$GPUReport = Join-Path $ReportDirectory 'GPU.render-policy.json'
$GPURepeatReport = Join-Path $ReportDirectory 'GPURepeat.render-policy.json'
$GPUResizeReport = Join-Path $ReportDirectory 'GPUResize.render-policy.json'
$GPUZeroVisibleReport = Join-Path $ReportDirectory 'GPUZeroVisible.render-policy.json'
$QualificationLog = Join-Path $ReportDirectory 'VulkanQualification.log'
$DirectLog = Join-Path $ReportDirectory 'Direct.log'
$DirectResizeLog = Join-Path $ReportDirectory 'DirectResize.log'
$GPULog = Join-Path $ReportDirectory 'GPU.log'
$GPURepeatLog = Join-Path $ReportDirectory 'GPURepeat.log'
$GPUResizeLog = Join-Path $ReportDirectory 'GPUResize.log'
$GPUZeroVisibleLog = Join-Path $ReportDirectory 'GPUZeroVisible.log'
$SummaryPath = Join-Path $ReportDirectory 'Task12VulkanCandidate.summary.json'
$FailurePath = Join-Path $ReportDirectory 'Task12VulkanCandidate.failure.json'

$OwnedArtifacts = @(
    $DirectCapture, $DirectResizeCapture, $GPUCapture, $GPURepeatCapture,
    $GPUResizeCapture, $GPUZeroVisibleCapture, $DirectReport,
    $DirectResizeReport, $GPUReport, $GPURepeatReport, $GPUResizeReport,
    $GPUZeroVisibleReport, $QualificationLog, $DirectLog, $DirectResizeLog,
    $GPULog, $GPURepeatLog, $GPUResizeLog, $GPUZeroVisibleLog,
    $SummaryPath, $FailurePath
)

function Get-RVXFileRecord {
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

function Get-RVXGitProvenance {
    $safeRoot = $SourceRoot.Replace('\', '/')
    $commit = @(& git -c "safe.directory=$safeRoot" -C $SourceRoot rev-parse HEAD)
    if ($LASTEXITCODE -ne 0 -or $commit.Count -eq 0) {
        throw "Could not resolve source commit for provenance: $SourceRoot"
    }
    $status = @(& git -c "safe.directory=$safeRoot" -C $SourceRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not resolve source worktree status for provenance: $SourceRoot"
    }
    return [ordered]@{
        sourceRoot = $SourceRoot
        commit = $commit[-1].Trim()
        treeDirty = [bool]($status.Count -gt 0)
        runner = Get-RVXFileRecord -Path $PSCommandPath
    }
}

function Get-RVXProcessIds {
    param([Parameter(Mandatory = $true)][string]$Executable)

    $name = [IO.Path]::GetFileNameWithoutExtension($Executable)
    $ids = @()
    foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
        try {
            $processPath = $process.Path
            if (-not [string]::IsNullOrWhiteSpace($processPath) -and
                ([IO.Path]::GetFullPath($processPath) -ieq [IO.Path]::GetFullPath($Executable))) {
                $ids += [uint32]$process.Id
            }
        } catch {
            # Access to another process's image path is restricted on some
            # Windows configurations.  The child process is still checked by
            # the exit code and this runner does not terminate unknown PIDs.
        }
    }
    return @($ids | Sort-Object -Unique)
}

function Get-RVXValidationScan {
    param([Parameter(Mandatory = $true)][string]$Path)

    $validationMessages = @()
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $lines = @(Get-Content -LiteralPath $Path)
        foreach ($line in $lines) {
            if ($line -match '(?i)Vulkan\s+Validation\s*:|validation.*\b(error|warning)\b|\b(error|warning)\b.*validation') {
                $validationMessages += [string]$line
            }
        }
    }
    $errors = @($validationMessages | Where-Object { $_ -match '(?i)\berror\b' })
    $warnings = @($validationMessages | Where-Object { $_ -match '(?i)\bwarning\b' })
    return [ordered]@{
        messageCount = [uint64]$validationMessages.Count
        errorCount = [uint64]$errors.Count
        warningCount = [uint64]$warnings.Count
        clean = ($validationMessages.Count -eq 0)
        messages = @($validationMessages)
    }
}

function Get-RVXReportProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    foreach ($name in $Names) {
        if ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$name]) {
            $value = $Object.PSObject.Properties[$name].Value
            if ($null -ne $value) {
                return $value
            }
        }
    }
    return $null
}

function Get-RVXNestedProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $current = $Object
    foreach ($part in $Path.Split('.')) {
        if ($null -eq $current -or $null -eq $current.PSObject.Properties[$part]) {
            return $null
        }
        $current = $current.PSObject.Properties[$part].Value
    }
    return $current
}

function Get-RVXPolicyFromLog {
    param(
        [Parameter(Mandatory = $true)][string]$LogPath,
        [Parameter(Mandatory = $true)][ValidateSet('Direct', 'GPU')][string]$ExpectedRoute
    )

    $text = if (Test-Path -LiteralPath $LogPath -PathType Leaf) {
        Get-Content -LiteralPath $LogPath -Raw
    } else { '' }
    $backend = $null
    $adapter = $null
    $mode = $null
    $reason = $null
    $qualification = $null
    $executed = $null
    $policySource = 'log'

    $backendMatch = [regex]::Match($text, '(?im)Smoke mode:\s*backend=(?<value>[^,\s]+)')
    if ($backendMatch.Success) { $backend = $backendMatch.Groups['value'].Value.Trim() }
    $adapterMatch = [regex]::Match($text, '(?im)(?:Adapter|adapter):\s*(?<value>[^\r\n]+)')
    if ($adapterMatch.Success) { $adapter = $adapterMatch.Groups['value'].Value.Trim() }

    $policyPatterns = @(
        '(?im)GPUDriven:\s*mode=(?<mode>[^,\s]+),\s*policyReason=(?<reason>[^,\s]+),\s*qualification=(?<qualification>[^,\s]+)',
        '(?im)policyMode=(?<mode>[^,\s]+),\s*policyReason=(?<reason>[^,\s]+),\s*qualification=(?<qualification>[^,\s]+)'
    )
    foreach ($pattern in $policyPatterns) {
        $policyMatch = [regex]::Match($text, $pattern)
        if ($policyMatch.Success) {
            $mode = $policyMatch.Groups['mode'].Value.Trim()
            $reason = $policyMatch.Groups['reason'].Value.Trim()
            $qualification = $policyMatch.Groups['qualification'].Value.Trim()
            break
        }
    }

    if ($ExpectedRoute -eq 'Direct' -and
        $text -match '(?im)GPU-driven\s+direct-draw\s+fallback\s+ready') {
        $executed = 'Direct'
    }
    elseif ($ExpectedRoute -eq 'GPU' -and
            ($text -match '(?im)GPU-driven\s+culling\s+ready' -or
             $text -match '(?im)zero-visible\s+GPU-driven\s+path\s+recorded')) {
        $executed = 'GPUDriven'
    }

    return [ordered]@{
        backend = $backend
        adapter = $adapter
        requestedMode = $mode
        policyReason = $reason
        qualification = $qualification
        executedMode = $executed
        source = $policySource
        rawEvidence = @($text -split "`r?`n" | Where-Object {
            $_ -match '(?i)Smoke mode:|Adapter:|GPUDriven:|policyMode=|GPU-driven .*ready'
        })
    }
}

function Test-RVXPolicyReport {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$ReportPath,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [Parameter(Mandatory = $true)][ValidateSet('Direct', 'GPU')][string]$ExpectedRoute,
        [Parameter(Mandatory = $true)][string]$ExpectedMode,
        [Parameter(Mandatory = $true)][object]$QualificationGate
    )

    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
        throw "$Name did not write render-policy report: $ReportPath"
    }
    try {
        $report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
    } catch {
        throw "$Name wrote invalid render-policy JSON: $($_.Exception.Message)"
    }
    if ($report.schema -ne 'RVX.RenderPolicyMeasurement' -or
        [uint32]$report.schemaVersion -ne 1) {
        throw "$Name wrote unsupported render-policy schema"
    }
    foreach ($field in @('frameSequence', 'planCpuNanoseconds', 'submissionCpuNanoseconds',
                         'candidatePacketCount', 'drawGroupCount')) {
        if ($null -eq $report.PSObject.Properties[$field]) {
            throw "$Name render-policy report omits required measurement field '$field'"
        }
    }
    if (-not [bool]$report.nonGating -or [bool]$report.usedForAutoDecision) {
        throw "$Name render-policy report violates non-gating/Auto isolation"
    }

    $logPolicy = Get-RVXPolicyFromLog -LogPath $LogPath -ExpectedRoute $ExpectedRoute
    $backend = Get-RVXReportProperty -Object $report -Names @('backend')
    $adapter = Get-RVXReportProperty -Object $report -Names @('adapterName', 'adapter')
    $requested = Get-RVXReportProperty -Object $report -Names @('requestedMode', 'policyMode')
    $executed = Get-RVXReportProperty -Object $report -Names @('executedMode', 'executedTier')
    $qualification = Get-RVXReportProperty -Object $report -Names @('qualification', 'qualificationLevel')
    $dispatch = Get-RVXReportProperty -Object $report -Names @('dispatch', 'indexedIndirectDispatch')
    if ($null -eq $backend) { $backend = $logPolicy.backend }
    if ($null -eq $adapter) { $adapter = $logPolicy.adapter }
    # The runner's command line is the authoritative request for this gate.
    # The existing v1 measurement report deliberately has no policy fields.
    if ($null -eq $requested) { $requested = $ExpectedMode }
    if ($null -eq $executed) { $executed = $logPolicy.executedMode }
    if ($null -eq $qualification) { $qualification = $logPolicy.qualification }
    if ($null -eq $dispatch) { $dispatch = 'UnavailableInRenderPolicyReport' }

    if ([string]$backend -ine 'Vulkan') {
        throw "$Name did not prove realized Vulkan backend (observed '$backend')"
    }
    if ([string]$requested -ne $ExpectedMode) {
        throw "$Name requested mode mismatch: expected '$ExpectedMode', observed '$requested'"
    }
    if ([string]$ExpectedRoute -eq 'Direct' -and [string]$executed -notin @('Direct', 'DirectDraw')) {
        throw "$Name did not prove direct execution (observed '$executed')"
    }
    if ([string]$ExpectedRoute -eq 'GPU' -and [string]$executed -notmatch '(?i)GPU|Indirect') {
        throw "$Name did not prove GPU/indirect execution (observed '$executed')"
    }
    if ($null -eq $QualificationGate -or -not $QualificationGate.passed) {
        throw "$Name cannot use a failed Vulkan Candidate manifest gate"
    }
    if ([string]$QualificationGate.qualification -ne 'Candidate') {
        throw "$Name requires Vulkan qualification Candidate, observed '$($QualificationGate.qualification)'"
    }
    if ([string]$requested -match '(?i)Auto' -or
        (($logPolicy.rawEvidence -join "`n") -match '(?i)Auto policy ready')) {
        throw "$Name unexpectedly involved Auto policy"
    }

    return [ordered]@{
        schema = [string]$report.schema
        schemaVersion = [uint32]$report.schemaVersion
        frameSequence = [uint64]$report.frameSequence
        backend = [string]$backend
        backendSource = if ($null -ne $report.PSObject.Properties['backend']) { 'report' } else { 'log' }
        adapter = [string]$adapter
        adapterSource = if ($null -ne $report.PSObject.Properties['adapterName'] -or $null -ne $report.PSObject.Properties['adapter']) { 'report' } else { 'log' }
        requestedMode = [string]$requested
        requestedModeSource = if ($null -ne $report.PSObject.Properties['requestedMode'] -or $null -ne $report.PSObject.Properties['policyMode']) { 'report' } else { 'runner-arguments' }
        executedMode = [string]$executed
        executedModeSource = if ($null -ne $report.PSObject.Properties['executedMode'] -or $null -ne $report.PSObject.Properties['executedTier']) { 'report' } else { 'readiness-log' }
        qualification = [string]$QualificationGate.qualification
        qualificationSource = 'GPUDrivenValidationFixture.VulkanQualificationIsCandidateAndAutoRemainsDirect'
        dispatch = [string]$dispatch
        dispatchSource = if ($null -ne $report.PSObject.Properties['dispatch'] -or $null -ne $report.PSObject.Properties['indexedIndirectDispatch']) { 'report' } else { 'raw-log-evidence-only' }
        autoInvolved = $false
        measurement = Get-RVXFileRecord -Path $ReportPath
        rawLogEvidence = $logPolicy.rawEvidence
    }
}

function Get-RVXPPMHeader {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing PPM capture: $Path"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 16) { throw "PPM capture is too small: $Path" }
    $prefix = [Text.Encoding]::ASCII.GetString($bytes, 0, [Math]::Min($bytes.Length, 256))
    $match = [regex]::Match($prefix, '^P6\s+(?<width>\d+)\s+(?<height>\d+)\s+255\s')
    if (-not $match.Success) { throw "Unsupported PPM header: $Path" }
    return [ordered]@{
        width = [uint32]$match.Groups['width'].Value
        height = [uint32]$match.Groups['height'].Value
        file = Get-RVXFileRecord -Path $Path
    }
}

function Test-RVXExactPPMParity {
    param(
        [Parameter(Mandatory = $true)][string]$LeftPath,
        [Parameter(Mandatory = $true)][string]$RightPath
    )
    $left = Get-RVXPPMHeader -Path $LeftPath
    $right = Get-RVXPPMHeader -Path $RightPath
    if ($left.width -ne $right.width -or $left.height -ne $right.height) {
        throw "Parity captures have different dimensions ($($left.width)x$($left.height) vs $($right.width)x$($right.height))"
    }
    if ($left.file.sha256 -ne $right.file.sha256 -or $left.file.bytes -ne $right.file.bytes) {
        throw "Direct/GPU captures are not byte-exact"
    }
    return [ordered]@{
        passed = $true
        dimensions = "$($left.width)x$($left.height)"
        direct = $left.file
        gpu = $right.file
    }
}

function Invoke-RVXGate {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [string]$CapturePath = '',
        [string]$ReportPath = ''
    )

    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "$Name executable was not found: $Executable"
    }
    $beforePids = @(Get-RVXProcessIds -Executable $Executable)
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $LogPath
    $exitCode = [int]$LASTEXITCODE
    $afterPids = @(Get-RVXProcessIds -Executable $Executable)
    $orphanPids = @($afterPids | Where-Object { $_ -notin $beforePids })
    $validation = Get-RVXValidationScan -Path $LogPath
    $record = [ordered]@{
        name = $Name
        executable = Get-RVXFileRecord -Path $Executable
        arguments = @($Arguments)
        exitCode = $exitCode
        orphanProcessIds = @($orphanPids)
        log = Get-RVXFileRecord -Path $LogPath
        report = if ([string]::IsNullOrWhiteSpace($ReportPath)) { $null } else { Get-RVXFileRecord -Path $ReportPath }
        capture = if ([string]::IsNullOrWhiteSpace($CapturePath)) { $null } else { Get-RVXFileRecord -Path $CapturePath }
        validation = $validation
    }
    $script:GateRecords += [pscustomobject]$record
    if ($exitCode -ne 0) { throw "$Name failed with exit code $exitCode" }
    if ($orphanPids.Count -ne 0) { throw "$Name left orphan ModelViewer process IDs: $($orphanPids -join ',')" }
    if (-not [bool]$validation.clean) { throw "$Name emitted Vulkan validation diagnostics" }
    return $record
}

function Invoke-RVXQualificationGate {
    param([Parameter(Mandatory = $true)][string]$LogPath)

    $filter = 'GPUDrivenValidationFixture.VulkanQualificationIsCandidateAndAutoRemainsDirect'
    $record = Invoke-RVXGate -Name 'Vulkan qualification manifest' `
        -Executable $GPUDrivenValidation -LogPath $LogPath `
        -Arguments @("--gtest_filter=$filter")
    $text = Get-Content -LiteralPath $LogPath -Raw
    if ($text -notmatch '(?im)\[\s*OK\s*\].*VulkanQualificationIsCandidateAndAutoRemainsDirect' -and
        $text -notmatch '(?im)PASSED.*VulkanQualificationIsCandidateAndAutoRemainsDirect') {
        throw 'Vulkan qualification manifest did not report the exact Candidate test as passed'
    }
    return [ordered]@{
        passed = $true
        qualification = 'Candidate'
        test = $filter
        log = $record.log
    }
}

function Write-RVXSummary {
    param(
        [Parameter(Mandatory = $true)][bool]$Passed,
        [string]$FailureMessage = '',
        [object]$QualificationGate = $null,
        [object]$Parity = $null,
        [object]$Resize = $null,
        [object]$ZeroVisible = $null
    )
    $provenance = Get-RVXGitProvenance
    $summary = [ordered]@{
        schema = 'RVX.Task12VulkanCandidate'
        schemaVersion = 1
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        overall = if ($Passed) { 'pass' } else { 'fail' }
        failureMessage = if ([string]::IsNullOrWhiteSpace($FailureMessage)) { $null } else { $FailureMessage }
        provenance = $provenance
        configuration = [ordered]@{
            backend = 'Vulkan'
            model = Get-RVXFileRecord -Path $Fixture
            width = $Width
            height = $Height
            resizeFrame = $ResizeFrame
            resizeWidth = $ResizeWidth
            resizeHeight = $ResizeHeight
            frames = $Frames
            buildDirectory = $BuildDir
            configuration = $Configuration
            reportDirectory = $ReportDirectory
        }
        qualification = $QualificationGate
        gates = @($script:GateRecords)
        parity = $Parity
        resize = $Resize
        zeroVisible = $ZeroVisible
        validationCounts = [ordered]@{
            messageCount = [uint64](@($script:GateRecords | ForEach-Object { $_.validation.messageCount } | Measure-Object -Sum).Sum)
            errorCount = [uint64](@($script:GateRecords | ForEach-Object { $_.validation.errorCount } | Measure-Object -Sum).Sum)
            warningCount = [uint64](@($script:GateRecords | ForEach-Object { $_.validation.warningCount } | Measure-Object -Sum).Sum)
        }
    }
    $summary | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath $SummaryPath -Encoding utf8
    return $summary
}

if ($ListOnly) {
    [ordered]@{
        script = $PSCommandPath
        sourceRoot = $SourceRoot
        buildDirectory = $BuildDir
        configuration = $Configuration
        reportDirectory = $ReportDirectory
        modelViewer = $ModelViewer
        gpuDrivenValidation = $GPUDrivenValidation
        fixture = $Fixture
        commands = @(
            [ordered]@{ name = 'Vulkan qualification manifest'; executable = $GPUDrivenValidation; arguments = @('--gtest_filter=GPUDrivenValidationFixture.VulkanQualificationIsCandidateAndAutoRemainsDirect') },
            [ordered]@{ name = 'Direct'; executable = $ModelViewer; arguments = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$DirectCapture,'--render-policy-report',$DirectReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','off','--expect-gpu-driven-direct-ready','--validation') },
            [ordered]@{ name = 'Direct resize'; executable = $ModelViewer; arguments = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$DirectResizeCapture,'--render-policy-report',$DirectResizeReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','off','--expect-gpu-driven-direct-ready','--resize-frame',"$ResizeFrame",'--resize-width',"$ResizeWidth",'--resize-height',"$ResizeHeight",'--validation') },
            [ordered]@{ name = 'GPU'; executable = $ModelViewer; arguments = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$GPUCapture,'--render-policy-report',$GPUReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','on','--expect-gpu-driven-culling-ready','--validation') },
            [ordered]@{ name = 'GPU repeat'; executable = $ModelViewer; arguments = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$GPURepeatCapture,'--render-policy-report',$GPURepeatReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','on','--expect-gpu-driven-culling-ready','--validation') },
            [ordered]@{ name = 'GPU resize'; executable = $ModelViewer; arguments = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$GPUResizeCapture,'--render-policy-report',$GPUResizeReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','on','--expect-gpu-driven-culling-ready','--resize-frame',"$ResizeFrame",'--resize-width',"$ResizeWidth",'--resize-height',"$ResizeHeight",'--validation') },
            [ordered]@{ name = 'GPU zero-visible'; executable = $ModelViewer; arguments = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$GPUZeroVisibleCapture,'--render-policy-report',$GPUZeroVisibleReport,'--no-ibl','--gpu-driven-zero-visible-scene','--gpu-driven','on','--expect-gpu-driven-zero-visible-ready','--validation') }
        )
    } | ConvertTo-Json -Depth 12
    exit 0
}

if (-not (Test-Path -LiteralPath $Fixture -PathType Leaf)) {
    throw "R7Triangle fixture was not found: $Fixture"
}
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
foreach ($artifact in $OwnedArtifacts) {
    Remove-Item -LiteralPath $artifact -Force -ErrorAction SilentlyContinue
}

$script:GateRecords = @()
$qualificationGate = $null
$parity = $null
$resizeEvidence = $null
$zeroVisibleEvidence = $null
$failure = ''
$passed = $false
try {
    $qualificationGate = Invoke-RVXQualificationGate -LogPath $QualificationLog

    $directArgs = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$DirectCapture,'--render-policy-report',$DirectReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','off','--expect-gpu-driven-direct-ready','--validation')
    $directResizeArgs = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$DirectResizeCapture,'--render-policy-report',$DirectResizeReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','off','--expect-gpu-driven-direct-ready','--resize-frame',"$ResizeFrame",'--resize-width',"$ResizeWidth",'--resize-height',"$ResizeHeight",'--validation')
    $gpuArgs = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$GPUCapture,'--render-policy-report',$GPUReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','on','--expect-gpu-driven-culling-ready','--validation')
    $gpuRepeatArgs = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$GPURepeatCapture,'--render-policy-report',$GPURepeatReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','on','--expect-gpu-driven-culling-ready','--validation')
    $gpuResizeArgs = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$GPUResizeCapture,'--render-policy-report',$GPUResizeReport,'--no-ibl','--gpu-driven-culling-test-scene','--gpu-driven','on','--expect-gpu-driven-culling-ready','--resize-frame',"$ResizeFrame",'--resize-width',"$ResizeWidth",'--resize-height',"$ResizeHeight",'--validation')
    $gpuZeroVisibleArgs = @('--smoke','--model',$Fixture,'--backend','vulkan','--width',"$Width",'--height',"$Height",'--frames',"$Frames",'--screenshot',$GPUZeroVisibleCapture,'--render-policy-report',$GPUZeroVisibleReport,'--no-ibl','--gpu-driven-zero-visible-scene','--gpu-driven','on','--expect-gpu-driven-zero-visible-ready','--validation')

    Invoke-RVXGate -Name 'Vulkan Direct' -Executable $ModelViewer -Arguments $directArgs -LogPath $DirectLog -CapturePath $DirectCapture -ReportPath $DirectReport | Out-Null
    $directPolicy = Test-RVXPolicyReport -Name 'Vulkan Direct' -ReportPath $DirectReport -LogPath $DirectLog -ExpectedRoute Direct -ExpectedMode ForceDisabled -QualificationGate $qualificationGate
    $script:GateRecords[-1] | Add-Member -NotePropertyName policy -NotePropertyValue $directPolicy

    Invoke-RVXGate -Name 'Vulkan Direct resize' -Executable $ModelViewer -Arguments $directResizeArgs -LogPath $DirectResizeLog -CapturePath $DirectResizeCapture -ReportPath $DirectResizeReport | Out-Null
    $directResizePolicy = Test-RVXPolicyReport -Name 'Vulkan Direct resize' -ReportPath $DirectResizeReport -LogPath $DirectResizeLog -ExpectedRoute Direct -ExpectedMode ForceDisabled -QualificationGate $qualificationGate
    $script:GateRecords[-1] | Add-Member -NotePropertyName policy -NotePropertyValue $directResizePolicy

    Invoke-RVXGate -Name 'Vulkan GPU' -Executable $ModelViewer -Arguments $gpuArgs -LogPath $GPULog -CapturePath $GPUCapture -ReportPath $GPUReport | Out-Null
    $gpuPolicy = Test-RVXPolicyReport -Name 'Vulkan GPU' -ReportPath $GPUReport -LogPath $GPULog -ExpectedRoute GPU -ExpectedMode ForceEnabled -QualificationGate $qualificationGate
    $script:GateRecords[-1] | Add-Member -NotePropertyName policy -NotePropertyValue $gpuPolicy

    Invoke-RVXGate -Name 'Vulkan GPU repeat' -Executable $ModelViewer -Arguments $gpuRepeatArgs -LogPath $GPURepeatLog -CapturePath $GPURepeatCapture -ReportPath $GPURepeatReport | Out-Null
    $gpuRepeatPolicy = Test-RVXPolicyReport -Name 'Vulkan GPU repeat' -ReportPath $GPURepeatReport -LogPath $GPURepeatLog -ExpectedRoute GPU -ExpectedMode ForceEnabled -QualificationGate $qualificationGate
    $script:GateRecords[-1] | Add-Member -NotePropertyName policy -NotePropertyValue $gpuRepeatPolicy

    $directGPUParity = Test-RVXExactPPMParity -LeftPath $DirectCapture -RightPath $GPUCapture
    $repeatParity = Test-RVXExactPPMParity -LeftPath $GPUCapture -RightPath $GPURepeatCapture
    $parity = [ordered]@{
        directGPU = $directGPUParity
        repeat = $repeatParity
    }

    Invoke-RVXGate -Name 'Vulkan GPU resize' -Executable $ModelViewer -Arguments $gpuResizeArgs -LogPath $GPUResizeLog -CapturePath $GPUResizeCapture -ReportPath $GPUResizeReport | Out-Null
    $resizePolicy = Test-RVXPolicyReport -Name 'Vulkan GPU resize' -ReportPath $GPUResizeReport -LogPath $GPUResizeLog -ExpectedRoute GPU -ExpectedMode ForceEnabled -QualificationGate $qualificationGate
    $script:GateRecords[-1] | Add-Member -NotePropertyName policy -NotePropertyValue $resizePolicy
    $resizeHeader = Get-RVXPPMHeader -Path $GPUResizeCapture
    $directResizeHeader = Get-RVXPPMHeader -Path $DirectResizeCapture
    if ($directResizeHeader.width -ne $ResizeWidth -or $directResizeHeader.height -ne $ResizeHeight) {
        throw "Vulkan Direct resize did not produce the requested final dimensions"
    }
    if ($resizeHeader.width -ne $ResizeWidth -or $resizeHeader.height -ne $ResizeHeight) {
        throw "Vulkan GPU resize did not produce the requested final dimensions"
    }
    $resizeParity = Test-RVXExactPPMParity -LeftPath $DirectResizeCapture -RightPath $GPUResizeCapture
    $resizeEvidence = [ordered]@{
        passed = $true
        frame = $ResizeFrame
        frameCount = $Frames
        finalDimensions = "$($resizeHeader.width)x$($resizeHeader.height)"
        directPolicy = $directResizePolicy
        gpuPolicy = $resizePolicy
        directGPUParity = $resizeParity
    }

    Invoke-RVXGate -Name 'Vulkan GPU zero-visible' -Executable $ModelViewer -Arguments $gpuZeroVisibleArgs -LogPath $GPUZeroVisibleLog -CapturePath $GPUZeroVisibleCapture -ReportPath $GPUZeroVisibleReport | Out-Null
    $zeroPolicy = Test-RVXPolicyReport -Name 'Vulkan GPU zero-visible' -ReportPath $GPUZeroVisibleReport -LogPath $GPUZeroVisibleLog -ExpectedRoute GPU -ExpectedMode ForceEnabled -QualificationGate $qualificationGate
    $script:GateRecords[-1] | Add-Member -NotePropertyName policy -NotePropertyValue $zeroPolicy
    $zeroVisibleEvidence = [ordered]@{
        passed = $true
        frameCount = $Frames
        policy = $zeroPolicy
    }
    $passed = $true
}
catch {
    $failure = $_.Exception.Message
}

try {
    $summary = Write-RVXSummary -Passed $passed -FailureMessage $failure `
        -QualificationGate $qualificationGate -Parity $parity -Resize $resizeEvidence -ZeroVisible $zeroVisibleEvidence
    Write-Host "Task12 Vulkan Candidate summary: $SummaryPath"
    if (-not $passed) {
        [ordered]@{
            schema = 'RVX.Task12VulkanCandidateFailure'
            schemaVersion = 1
            generatedUtc = [DateTime]::UtcNow.ToString('o')
            message = $failure
            summaryPath = $SummaryPath
            gates = @($script:GateRecords)
        } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $FailurePath -Encoding utf8
        throw $failure
    }
}
catch {
    if ($_.Exception.Message -eq $failure -and -not [string]::IsNullOrWhiteSpace($failure)) {
        Write-Error $_.Exception.Message
        exit 1
    }
    throw
}
