Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Import-Module (Join-Path $PSScriptRoot "BuildTruthV3.psm1") -Force

function Assert-RequiredSuiteFailure
{
    param([string]$Category, [scriptblock]$Action)

    try
    {
        & $Action
    }
    catch
    {
        if ($_.Exception.Message -notmatch "^BT3\.${Category}:")
        {
            throw "Expected BT3.${Category} failure, got '$($_.Exception.Message)'"
        }
        return
    }
    throw "Expected BT3.${Category} failure, but the action completed."
}

function New-RequiredSuiteTest
{
    param([string]$Name, [string[]]$Labels)

    return [pscustomobject]@{
        name = $Name
        properties = @(
            [pscustomobject]@{
                name = "LABELS"
                value = @($Labels)
            }
        )
    }
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    "RenderVerseX-RequiredSuiteV1-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null

try
{
    $validInventory = [pscustomobject]@{
        tests = @(
            (New-RequiredSuiteTest -Name "Smoke.Producer" `
                -Labels @("lane.qualification", "suite.pr-smoke")),
            (New-RequiredSuiteTest -Name "Smoke.Validator" `
                -Labels @("lane.qualification", "suite.pr-smoke"))
        )
    }
    Assert-BuildTruthRequiredSelection `
        -Inventory $validInventory `
        -RequiredLabel "suite.pr-smoke" `
        -ExpectedTestCount 2 `
        -Context "self-test"

    Assert-RequiredSuiteFailure -Category "ZeroTests" -Action {
        Assert-BuildTruthRequiredSelection `
            -Inventory ([pscustomobject]@{ tests = @() }) `
            -RequiredLabel "suite.pr-smoke"
    }
    Assert-RequiredSuiteFailure -Category "RequiredLabel" -Action {
        Assert-BuildTruthRequiredSelection `
            -Inventory $validInventory `
            -RequiredLabel "product.samples"
    }
    Assert-RequiredSuiteFailure -Category "RequiredLabel" -Action {
        $wrongLabel = [pscustomobject]@{
            tests = @(
                (New-RequiredSuiteTest -Name "Wrong" `
                    -Labels @("lane.qualification", "suite.other"))
            )
        }
        Assert-BuildTruthRequiredSelection `
            -Inventory $wrongLabel `
            -RequiredLabel "suite.pr-smoke"
    }
    Assert-RequiredSuiteFailure -Category "NotRun" -Action {
        Assert-BuildTruthRequiredSelection `
            -Inventory $validInventory `
            -RequiredLabel "suite.pr-smoke" `
            -ExpectedTestCount 3
    }

    $buildRoot = Join-Path $temporaryRoot "build"
    New-Item -ItemType Directory -Path $buildRoot | Out-Null
    Assert-RequiredSuiteFailure -Category "PathEscape" -Action {
        Resolve-BuildTruthContainedPath `
            -Root $buildRoot `
            -Path "..\outside.json" `
            -AllowMissing | Out-Null
    }
    Assert-RequiredSuiteFailure -Category "MissingArtifact" -Action {
        Get-BuildTruthFileEvidence `
            -BuildRoot $buildRoot `
            -Paths @("missing.json") | Out-Null
    }

    $evidencePath = Join-Path $buildRoot "evidence.json"
    "{`"status`":`"passed`"}" |
        Set-Content -LiteralPath $evidencePath -Encoding utf8
    $evidence = @(Get-BuildTruthFileEvidence `
        -BuildRoot $buildRoot `
        -Paths @($evidencePath))
    if ($evidence.Count -ne 1 -or
        $evidence[0].size -le 0 -or
        $evidence[0].sha256 -notmatch "^[0-9a-f]{64}$")
    {
        throw "Required evidence fingerprint is incomplete."
    }

    Write-Host "Required Suite v1 self-test passed."
}
finally
{
    if (Test-Path -LiteralPath $temporaryRoot)
    {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
