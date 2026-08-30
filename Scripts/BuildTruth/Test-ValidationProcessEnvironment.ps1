Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot "ValidationProcessEnvironment.psm1") -Force

$initSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot "..\..\init.ps1") -Raw
if ($initSource -notmatch 'ValidationProcessEnvironment\.psm1' -or
    $initSource -notmatch '"-OutputFormat",\s*\$OutputFormat')
{
    throw "The harness bootstrap does not forward its arguments through the validation process launcher."
}

function Assert-Equal
{
    param([object]$Actual, [object]$Expected, [string]$Message)

    if ($Actual -ne $Expected)
    {
        throw "$Message. Expected '$Expected', got '$Actual'."
    }
}

function Assert-Throws
{
    param([string]$Message, [scriptblock]$Action)

    try
    {
        & $Action
    }
    catch
    {
        if ($_.Exception.Message -notmatch $Message)
        {
            throw "Expected error matching '$Message', got '$($_.Exception.Message)'."
        }
        return
    }
    throw "Expected error matching '$Message', but the action completed."
}

$effectivePath = [Environment]::GetEnvironmentVariable(
    "PATH", [EnvironmentVariableTarget]::Process)
if ([string]::IsNullOrWhiteSpace($effectivePath))
{
    throw "The self-test host has no usable effective PATH."
}

$rawHostEnvironment = @(Get-ValidationProcessEnvironmentEntries)
$childEnvironment = @($rawHostEnvironment | Where-Object {
    $_ -notmatch "^(?i:PATH)=" -and
    $_ -notmatch "^(?i:RVX_VALIDATION_PATH_CANONICALIZED)="
})
$duplicatePathEnvironment = @(
    $childEnvironment +
    "PATH=C:\\inherited-bin" +
    "Path=$effectivePath"
)

$plan = Get-ValidationPathCanonicalizationPlan `
    -EnvironmentEntries $duplicatePathEnvironment `
    -IsWindows $true `
    -GuardValue $null `
    -EffectivePath $effectivePath
Assert-Equal -Actual $plan.shouldRelaunch -Expected $true `
    -Message "duplicate PATH entries require one clean child"
Assert-Equal -Actual $plan.pathEntryCount -Expected 2 `
    -Message "duplicate PATH entry count"
Assert-Equal -Actual $plan.canonicalPath -Expected $effectivePath `
    -Message "effective PowerShell PATH is canonicalized"
Assert-Throws -Message "does not exactly match" -Action {
    Get-ValidationPathCanonicalizationPlan `
        -EnvironmentEntries $duplicatePathEnvironment `
        -IsWindows $true `
        -GuardValue $null `
        -EffectivePath "C:\\not-present" | Out-Null
}
Assert-Throws -Message "canonicalization guard is set" -Action {
    Get-ValidationPathCanonicalizationPlan `
        -EnvironmentEntries $duplicatePathEnvironment `
        -IsWindows $true `
        -GuardValue "1" `
        -EffectivePath $effectivePath | Out-Null
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    "RenderVerseX-ValidationProcessEnvironment-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
try
{
    $childScriptPath = Join-Path $temporaryRoot "binding-child.ps1"
    $childResultPath = Join-Path $temporaryRoot "binding-result.json"
    @'
param(
    [Parameter(Mandatory)][string]$OutputPath,
    [string[]]$BuildTarget = @(),
    [string[]]$RequiredEvidence = @(),
    [switch]$Fresh,
    [switch]$SkipConfigure,
    [string]$Mode = "",
    [string]$Preset = "",
    [string]$TestPreset = "",
    [string]$BuildDir = "",
    [string]$Configuration = "",
    [string]$BaseRef = "",
    [switch]$AllowConfigure,
    [string]$OutputFormat = ""
)

$pathEntryCount = @(
    [Environment]::GetEnvironmentVariables([EnvironmentVariableTarget]::Process).Keys |
        Where-Object { [string]::Equals($_, "PATH", [StringComparison]::OrdinalIgnoreCase) }
).Count
$record = [ordered]@{
    buildTargetSerialized = @($BuildTarget)
    buildTargets = @($BuildTarget | ForEach-Object { $_ -split "," })
    requiredEvidenceSerialized = @($RequiredEvidence)
    requiredEvidence = @($RequiredEvidence | ForEach-Object { $_ -split ";" })
    fresh = [bool]$Fresh
    skipConfigure = [bool]$SkipConfigure
    mode = $Mode
    preset = $Preset
    testPreset = $TestPreset
    buildDir = $BuildDir
    configuration = $Configuration
    baseRef = $BaseRef
    allowConfigure = [bool]$AllowConfigure
    outputFormat = $OutputFormat
    pathEntryCount = $pathEntryCount
    canonicalizationGuard = $env:RVX_VALIDATION_PATH_CANONICALIZED
}
[IO.File]::WriteAllText(
    $OutputPath,
    ($record | ConvertTo-Json -Compress),
    [Text.UTF8Encoding]::new($false))
exit 37
'@ | Set-Content -LiteralPath $childScriptPath -Encoding utf8

    $unicodeEvidence = "evidence-" + [char]0x4E8C + ".json"
    $arguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $childScriptPath,
        "-BuildTarget", "target.one,target two",
        "-RequiredEvidence", ("evidence one.json;" + $unicodeEvidence),
        "-Fresh", "-SkipConfigure",
        "-OutputPath", $childResultPath
    )
    $childResult = Invoke-ValidationProcessEnvironmentCanonicalization `
        -WorkingDirectory $temporaryRoot `
        -InvocationArguments $arguments `
        -EnvironmentEntries $duplicatePathEnvironment `
        -GuardValue $null `
        -EffectivePath $effectivePath `
        -IsWindows $true
    Assert-Equal -Actual $childResult.relaunched -Expected $true `
        -Message "duplicate PATH starts exactly one child"
    Assert-Equal -Actual $childResult.exitCode -Expected 37 `
        -Message "child exit code is preserved"
    if (-not (Test-Path -LiteralPath $childResultPath -PathType Leaf))
    {
        throw "The actual child PowerShell did not publish its binding result."
    }
    $binding = Get-Content -LiteralPath $childResultPath -Raw | ConvertFrom-Json
    Assert-Equal -Actual @($binding.buildTargetSerialized).Count -Expected 1 `
        -Message "child binds the serialized BuildTarget token"
    Assert-Equal -Actual $binding.buildTargetSerialized[0] -Expected "target.one,target two" `
        -Message "child preserves BuildTarget token including spaces"
    Assert-Equal -Actual (@($binding.buildTargets) -join "|") -Expected "target.one|target two" `
        -Message "child round-trips two BuildTarget values"
    Assert-Equal -Actual @($binding.requiredEvidenceSerialized).Count -Expected 1 `
        -Message "child binds the serialized RequiredEvidence token"
    Assert-Equal -Actual $binding.requiredEvidenceSerialized[0] -Expected ("evidence one.json;" + $unicodeEvidence) `
        -Message "child preserves RequiredEvidence token including Unicode"
    Assert-Equal -Actual (@($binding.requiredEvidence) -join "|") `
        -Expected ("evidence one.json|" + $unicodeEvidence) `
        -Message "child round-trips two RequiredEvidence values"
    Assert-Equal -Actual $binding.fresh -Expected $true `
        -Message "child binds Fresh switch"
    Assert-Equal -Actual $binding.skipConfigure -Expected $true `
        -Message "child binds SkipConfigure switch"
    Assert-Equal -Actual $binding.pathEntryCount -Expected 1 `
        -Message "actual child sees one PATH entry"
    Assert-Equal -Actual $binding.canonicalizationGuard -Expected "1" `
        -Message "actual child receives canonicalization guard"

    $initResultPath = Join-Path $temporaryRoot "init-binding-result.json"
    $initArguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $childScriptPath,
        "-Mode", "Verify",
        "-Preset", "win preset",
        "-TestPreset", "win preset architecture",
        "-BuildDir", "build\\with spaces",
        "-Configuration", "Release",
        "-BaseRef", "origin/main",
        "-AllowConfigure",
        "-OutputFormat", "Json",
        "-OutputPath", $initResultPath
    )
    $initChildResult = Invoke-ValidationProcessEnvironmentCanonicalization `
        -WorkingDirectory $temporaryRoot `
        -InvocationArguments $initArguments `
        -EnvironmentEntries $duplicatePathEnvironment `
        -GuardValue $null `
        -EffectivePath $effectivePath `
        -IsWindows $true
    Assert-Equal -Actual $initChildResult.relaunched -Expected $true `
        -Message "init arguments start one clean child"
    Assert-Equal -Actual $initChildResult.exitCode -Expected 37 `
        -Message "init child failure exit code is preserved"
    $initBinding = Get-Content -LiteralPath $initResultPath -Raw | ConvertFrom-Json
    Assert-Equal -Actual $initBinding.mode -Expected "Verify" `
        -Message "init child binds Mode"
    Assert-Equal -Actual $initBinding.preset -Expected "win preset" `
        -Message "init child binds Preset including spaces"
    Assert-Equal -Actual $initBinding.testPreset -Expected "win preset architecture" `
        -Message "init child binds TestPreset including spaces"
    Assert-Equal -Actual $initBinding.buildDir -Expected "build\\with spaces" `
        -Message "init child binds BuildDir including spaces"
    Assert-Equal -Actual $initBinding.configuration -Expected "Release" `
        -Message "init child binds Configuration"
    Assert-Equal -Actual $initBinding.baseRef -Expected "origin/main" `
        -Message "init child binds BaseRef"
    Assert-Equal -Actual $initBinding.allowConfigure -Expected $true `
        -Message "init child binds AllowConfigure switch"
    Assert-Equal -Actual $initBinding.outputFormat -Expected "Json" `
        -Message "init child binds OutputFormat"

    $normalResult = Invoke-ValidationProcessEnvironmentCanonicalization `
        -WorkingDirectory $temporaryRoot `
        -InvocationArguments @("-NoProfile", "-Command", "exit 1") `
        -EnvironmentEntries @("Path=$effectivePath", "TEMP=C:\\temp") `
        -GuardValue $null `
        -EffectivePath $effectivePath `
        -IsWindows $true `
        -StartChild { throw "normal environment must not start a child" }
    Assert-Equal -Actual $normalResult.relaunched -Expected $false `
        -Message "normal environment does not relaunch"
}
finally
{
    if (Test-Path -LiteralPath $temporaryRoot)
    {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host "Validation process environment self-test passed."
