[CmdletBinding()]
param(
    [string]$RepositoryRoot = ''
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RepositoryRoot))
{
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$assetId = '1E79ks2jbMt5iIrI8Ag9lRgA0VRnfU4pk'
$expectedSha256 = '87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a'
$expectedBytes = 2058883L
$assetUrl = "https://drive.usercontent.google.com/download?id=$assetId&export=download&confirm=t"
$destination = Join-Path $RepositoryRoot 'Samples\RenderVerseSamples\Assets\models\casual-female\Casual_Female.gltf'

if (Test-Path -LiteralPath $destination)
{
    $existing = Get-Item -LiteralPath $destination
    $existingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash.ToLowerInvariant()
    if ($existing.Length -eq $expectedBytes -and $existingHash -eq $expectedSha256)
    {
        Write-Output "Pinned Casual_Female.gltf is already present: $destination"
        exit 0
    }
    throw "Refusing to overwrite a non-matching asset: $destination"
}

$destinationDirectory = Split-Path -Parent $destination
New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) ("rvx-casual-female-{0}.gltf" -f [Guid]::NewGuid().ToString('N'))
try
{
    Invoke-WebRequest -Uri $assetUrl -OutFile $temporary -MaximumRedirection 5 -UseBasicParsing
    $download = Get-Item -LiteralPath $temporary
    $downloadHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash.ToLowerInvariant()
    if ($download.Length -ne $expectedBytes -or $downloadHash -ne $expectedSha256)
    {
        throw "Pinned Casual Female identity mismatch: bytes=$($download.Length), sha256=$downloadHash"
    }

    $document = Get-Content -LiteralPath $temporary -Raw | ConvertFrom-Json
    $skins = @($document.skins)
    $clipNames = @($document.animations | ForEach-Object { $_.name })
    if ($skins.Count -ne 1 -or @($skins[0].joints).Count -ne 23)
    {
        throw 'Pinned Casual Female must contain exactly one 23-joint skin.'
    }
    foreach ($requiredClip in @('Idle', 'Walk', 'Run'))
    {
        if ($clipNames -notcontains $requiredClip)
        {
            throw "Pinned Casual Female is missing required clip: $requiredClip"
        }
    }

    Move-Item -LiteralPath $temporary -Destination $destination
    Write-Output "Installed verified Casual_Female.gltf: $destination"
}
finally
{
    if (Test-Path -LiteralPath $temporary)
    {
        Remove-Item -LiteralPath $temporary -Force
    }
}
