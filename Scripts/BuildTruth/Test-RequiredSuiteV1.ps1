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

    $junitPath = Join-Path $buildRoot "required-suite.junit.xml"
    @"
<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="1"><testsuite name="required" tests="1"><testcase name="one"/></testsuite></testsuites>
"@ | Set-Content -LiteralPath $junitPath -Encoding utf8
    $executionJUnitEvidence = Get-BuildTruthReceiptFileEvidence `
        -RepositoryRoot $temporaryRoot `
        -Path $junitPath
    $executionJUnitOutcome = Get-BuildTruthJUnitOutcome -Path $junitPath
    Assert-BuildTruthJUnitOutcome `
        -Outcome $executionJUnitOutcome `
        -ExpectedTestCount 1 `
        -Context "required suite execution self-test"
    @"
<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="1"><testsuite name="required" tests="1"><testcase name="one"><failure/></testcase></testsuite></testsuites>
"@ | Set-Content -LiteralPath $junitPath -Encoding utf8
    Assert-RequiredSuiteFailure -Category "ReceiptTampered" -Action {
        Assert-BuildTruthReceiptFileEvidence `
            -RepositoryRoot $temporaryRoot `
            -Evidence $executionJUnitEvidence `
            -Context "Required Suite execution JUnit"
    }
    $tamperedJUnitOutcome = Get-BuildTruthJUnitOutcome -Path $junitPath
    Assert-RequiredSuiteFailure -Category "ReceiptTampered" -Action {
        Assert-BuildTruthJUnitOutcomeMatches `
            -Expected $executionJUnitOutcome `
            -Actual $tamperedJUnitOutcome `
            -Context "Required Suite execution JUnit"
    }

    $receiptRepository = Join-Path $temporaryRoot "receipt-repository"
    New-Item -ItemType Directory -Path $receiptRepository | Out-Null
    $receiptRunId = "fedcba9876543210fedcba9876543210"
    $receiptValue = [ordered]@{
        schema = "RVX.AgentHarness.RequiredSuiteReceipt"
        schemaVersion = 1
        runId = $receiptRunId
        immutableReceiptPath = ".agent-harness/receipts/RequiredSuite.$receiptRunId.json"
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        request = [ordered]@{}
        source = [ordered]@{}
        report = [ordered]@{}
        junit = [ordered]@{}
        requiredLabel = "suite.pr-smoke"
        selectedTests = @("Smoke.Producer", "Smoke.Validator")
        semanticArtifacts = @()
    }
    $requiredSuiteRunnerSource = Get-Content `
        -LiteralPath (Join-Path $PSScriptRoot "..\run_required_suite.ps1") `
        -Raw
    if ($requiredSuiteRunnerSource -notmatch 'skipConfigure\s*=\s*\$SkipConfigure')
    {
        throw "Required Suite receipt publisher does not bind skipConfigure."
    }
    $receipt = Write-BuildTruthImmutableReceipt `
        -RepositoryRoot $receiptRepository `
        -ReceiptKind "RequiredSuite" `
        -RunId $receiptRunId `
        -Value $receiptValue `
        -Status "passed"
    if ($receipt.immutableReceiptPath -ne ".agent-harness/receipts/RequiredSuite.$receiptRunId.json" -or
        $receipt.sha256 -notmatch "^[0-9a-f]{64}$")
    {
        throw "Required suite immutable receipt is incomplete."
    }
    Assert-RequiredSuiteFailure -Category "ReceiptCollision" -Action {
        Write-BuildTruthImmutableReceipt `
            -RepositoryRoot $receiptRepository `
            -ReceiptKind "RequiredSuite" `
            -RunId $receiptRunId `
            -Value $receiptValue `
            -Status "passed" | Out-Null
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
