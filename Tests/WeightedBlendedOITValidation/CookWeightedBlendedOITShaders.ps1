[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Cooker,
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Catalog,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][ValidateSet("DX12", "Vulkan")][string]$Backend
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-ExistingFile([string]$Path, [string]$Description)
{
    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf))
    {
        throw "$Description does not name an existing file: $resolved"
    }
    return $resolved
}

function Resolve-RepositoryOutput([string]$Path, [string]$RepositoryRoot)
{
    $resolved = [System.IO.Path]::GetFullPath($Path)
    $rootPrefix = $RepositoryRoot.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith(
            $rootPrefix,
            [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "Refusing to replace GFX22 fixture output outside the repository: $resolved"
    }
    return $resolved
}

function Get-OutputFingerprint([string]$Root)
{
    return @(Get-ChildItem -LiteralPath $Root -File -Recurse |
        ForEach-Object {
            [pscustomobject]@{
                RelativePath = $_.FullName.Substring($Root.Length + 1).Replace('\', '/')
                Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        } | Sort-Object RelativePath)
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Get-Location).Path)
$cookerPath = Resolve-ExistingFile $Cooker "RVXCook"
$catalogPath = Resolve-ExistingFile $Catalog "Weighted-blended OIT shader catalog"
$sourcePath = [System.IO.Path]::GetFullPath($Source)
if (-not (Test-Path -LiteralPath $sourcePath -PathType Container))
{
    throw "Weighted-blended OIT shader source root does not exist: $sourcePath"
}

$primaryOutput = Resolve-RepositoryOutput $Output $repositoryRoot
$comparisonOutput = Resolve-RepositoryOutput ($primaryOutput + ".determinism-check") $repositoryRoot

# This script only removes the two exact build-tree destinations passed by
# CMake. It never cleans the repository, shared cooked roots, or user data.
foreach ($path in @($primaryOutput, $comparisonOutput))
{
    if (Test-Path -LiteralPath $path)
    {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}

function Invoke-Cook([string]$Destination)
{
    & $cookerPath `
        --source $sourcePath `
        --output $Destination `
        --shader-catalog $catalogPath `
        --shader-backend $Backend `
        --fail-on-errors
    if ($LASTEXITCODE -ne 0)
    {
        throw "RVXCook failed for $Backend weighted-blended OIT fixture output '$Destination'"
    }
}

Invoke-Cook $primaryOutput
Invoke-Cook $comparisonOutput

$primaryFiles = Get-OutputFingerprint $primaryOutput
$comparisonFiles = Get-OutputFingerprint $comparisonOutput
if ($primaryFiles.Count -eq 0)
{
    throw "$Backend weighted-blended OIT fixture cook produced no cooked artifacts"
}
if ($primaryFiles.Count -ne $comparisonFiles.Count)
{
    throw "$Backend weighted-blended OIT fixture cook produced a different artifact count"
}
for ($index = 0; $index -lt $primaryFiles.Count; ++$index)
{
    if ($primaryFiles[$index].RelativePath -ne $comparisonFiles[$index].RelativePath -or
        $primaryFiles[$index].Hash -ne $comparisonFiles[$index].Hash)
    {
        throw "$Backend weighted-blended OIT fixture cook is not byte deterministic at '$($primaryFiles[$index].RelativePath)'"
    }
}

Remove-Item -LiteralPath $comparisonOutput -Recurse -Force
