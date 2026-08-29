[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Cooker,
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Catalog,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][ValidateSet("DX12", "Vulkan")][string]$Backend
)

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

function Assert-RepositoryOutput([string]$Path, [string]$RepositoryRoot)
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
        throw "Refusing to replace hybrid RT fixture output outside the repository: $resolved"
    }
    return $resolved
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Get-Location).Path)
$cookerPath = Resolve-ExistingFile $Cooker "RVXCook"
$catalogPath = Resolve-ExistingFile $Catalog "Hybrid RT shader catalog"
$sourcePath = [System.IO.Path]::GetFullPath($Source)
if (-not (Test-Path -LiteralPath $sourcePath -PathType Container))
{
    throw "Hybrid RT shader source root does not exist: $sourcePath"
}

$primaryOutput = Assert-RepositoryOutput $Output $repositoryRoot
$comparisonOutput = Assert-RepositoryOutput ($primaryOutput + ".determinism-check") $repositoryRoot

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
        throw "RVXCook failed for $Backend hybrid RT fixture output '$Destination'"
    }
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

Invoke-Cook $primaryOutput
Invoke-Cook $comparisonOutput

$primaryFiles = Get-OutputFingerprint $primaryOutput
$comparisonFiles = Get-OutputFingerprint $comparisonOutput
if ($primaryFiles.Count -eq 0)
{
    throw "$Backend hybrid RT fixture cook produced no cooked artifacts"
}
if ($primaryFiles.Count -ne $comparisonFiles.Count)
{
    throw "$Backend hybrid RT fixture cook produced a different artifact count"
}
for ($index = 0; $index -lt $primaryFiles.Count; ++$index)
{
    if ($primaryFiles[$index].RelativePath -ne $comparisonFiles[$index].RelativePath -or
        $primaryFiles[$index].Hash -ne $comparisonFiles[$index].Hash)
    {
        throw "$Backend hybrid RT fixture cook is not byte deterministic at '$($primaryFiles[$index].RelativePath)'"
    }
}

Remove-Item -LiteralPath $comparisonOutput -Recurse -Force
