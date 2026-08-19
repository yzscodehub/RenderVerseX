param(
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [string]$ReportPath = "",
    [switch]$ListOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot "BuildTruth\BuildTruthV3.psm1") -Force

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
if (-not (Test-Path -LiteralPath $buildPath -PathType Container))
{
    throw "Build directory '$buildPath' does not exist. Configure this worktree first."
}

if ([string]::IsNullOrWhiteSpace($ReportPath))
{
    $ReportPath = Join-Path $buildPath "BuildTruth\ArchitectureBaseline.json"
}
$reportDirectory = Split-Path -Parent $ReportPath
$junitPath = Join-Path $reportDirectory "ArchitectureBaseline.junit.xml"
New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null
$gitSafeDirectory = $repoRoot.Replace(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar)
$sourceCommit = (& git -c "safe.directory=$gitSafeDirectory" -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0)
{
    throw "Unable to resolve the source commit for the architecture lane"
}

$inventory = Get-BuildTruthCTestInventory -BuildPath $buildPath -Configuration $Configuration
Assert-BuildTruthInventoryLanes -Inventory $inventory -Context "full inventory"
$architectureTests = @(
    $inventory.tests | Where-Object {
        (Get-BuildTruthTestLanes -Test $_) -contains "lane.architecture"
    }
)
if ($architectureTests.Count -eq 0)
{
    Throw-BuildTruthFailure -Category "ZeroTests" -Message "Architecture lane selects zero tests"
}

if ($ListOnly)
{
    $architectureTests.name | Write-Output
    Write-BuildTruthJson -Value ([ordered]@{
        schema = "RVX.BuildTruth.ArchitectureLane"
        schemaVersion = 3
        sourceCommit = $sourceCommit
        buildDir = $buildPath
        configuration = $Configuration
        selectedTestCount = $architectureTests.Count
        selectedTests = @($architectureTests.name)
        status = "listed"
    }) -Path $ReportPath
    Write-Host "Selected architecture tests: $($architectureTests.Count)"
    exit 0
}

if (Test-Path -LiteralPath $junitPath)
{
    Remove-Item -LiteralPath $junitPath -Force
}
$started = Get-Date
& ctest --test-dir $buildPath -C $Configuration -L "^lane\.architecture$" `
    --output-on-failure --output-junit $junitPath
$ctestExitCode = $LASTEXITCODE
$outcome = Get-BuildTruthJUnitOutcome -Path $junitPath
Assert-BuildTruthJUnitOutcome `
    -Outcome $outcome `
    -ExpectedTestCount $architectureTests.Count `
    -Context "architecture lane"
if ($ctestExitCode -ne 0)
{
    Throw-BuildTruthFailure -Category "TestCommand" -Message (
        "Architecture lane failed with exit code $ctestExitCode")
}

Write-BuildTruthJson -Value ([ordered]@{
    schema = "RVX.BuildTruth.ArchitectureLane"
    schemaVersion = 3
    sourceCommit = $sourceCommit
    buildDir = $buildPath
    configuration = $Configuration
    selectedTestCount = $architectureTests.Count
    selectedTests = @($architectureTests.name)
    junitPath = $junitPath
    outcome = $outcome
    durationMs = [int]((Get-Date) - $started).TotalMilliseconds
    status = "passed"
}) -Path $ReportPath

Write-Host "Architecture lane passed. Report: $ReportPath"
