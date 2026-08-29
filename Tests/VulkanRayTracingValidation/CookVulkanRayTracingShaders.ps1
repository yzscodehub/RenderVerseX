[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Cooker,
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Catalog,
    [Parameter(Mandatory)][string]$Output
)

$ErrorActionPreference = "Stop"
$primaryOutput = [System.IO.Path]::GetFullPath($Output)
$comparisonOutput = $primaryOutput + ".determinism-check"
$repositoryRoot = [System.IO.Path]::GetFullPath((Get-Location).Path)
$rootPrefix = $repositoryRoot.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar) +
    [System.IO.Path]::DirectorySeparatorChar

foreach ($path in @($primaryOutput, $comparisonOutput))
{
    if (-not $path.StartsWith(
            $rootPrefix,
            [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "Refusing to replace Vulkan RT fixture output outside the repository: $path"
    }
    if (Test-Path -LiteralPath $path)
    {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}

function Invoke-Cook([string]$Destination)
{
    & $Cooker `
        --source $Source `
        --output $Destination `
        --shader-catalog $Catalog `
        --shader-backend Vulkan `
        --fail-on-errors
    if ($LASTEXITCODE -ne 0)
    {
        throw "RVXCook failed for Vulkan RT fixture output '$Destination'"
    }
}

Invoke-Cook $primaryOutput
Invoke-Cook $comparisonOutput

$primaryFiles = @(Get-ChildItem -LiteralPath $primaryOutput -File -Recurse |
    ForEach-Object {
        [pscustomobject]@{
            RelativePath = $_.FullName.Substring(
                $primaryOutput.Length + 1).Replace('\', '/')
            Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    } | Sort-Object RelativePath)
$comparisonFiles = @(Get-ChildItem -LiteralPath $comparisonOutput -File -Recurse |
    ForEach-Object {
        [pscustomobject]@{
            RelativePath = $_.FullName.Substring(
                $comparisonOutput.Length + 1).Replace('\', '/')
            Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    } | Sort-Object RelativePath)

if ($primaryFiles.Count -eq 0)
{
    throw "Vulkan RT fixture cook produced no files"
}
if ($primaryFiles.Count -ne $comparisonFiles.Count)
{
    throw "Vulkan RT fixture cook produced a different file count"
}
for ($index = 0; $index -lt $primaryFiles.Count; ++$index)
{
    if ($primaryFiles[$index].RelativePath -ne
            $comparisonFiles[$index].RelativePath -or
        $primaryFiles[$index].Hash -ne $comparisonFiles[$index].Hash)
    {
        throw "Vulkan RT fixture cook is not byte deterministic at '$($primaryFiles[$index].RelativePath)'"
    }
}

Remove-Item -LiteralPath $comparisonOutput -Recurse -Force
