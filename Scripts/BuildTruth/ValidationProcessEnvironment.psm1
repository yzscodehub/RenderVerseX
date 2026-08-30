Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:ValidationPathCanonicalizationGuard =
    "RVX_VALIDATION_PATH_CANONICALIZED"

function ConvertFrom-ValidationEnvironmentEntry
{
    param([Parameter(Mandatory)][string]$Entry)

    $separatorIndex = if ($Entry.StartsWith("=", [StringComparison]::Ordinal))
    {
        $Entry.IndexOf("=", 1)
    }
    else
    {
        $Entry.IndexOf("=")
    }
    if ($separatorIndex -le 0)
    {
        throw "Validation process environment contains an invalid entry."
    }

    return [pscustomobject]@{
        name = $Entry.Substring(0, $separatorIndex)
        value = $Entry.Substring($separatorIndex + 1)
    }
}

function Get-ValidationProcessEnvironmentEntries
{
    if (-not [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [Runtime.InteropServices.OSPlatform]::Windows))
    {
        return @()
    }

    $nativeEnvironmentType = "RVX.Validation.NativeEnvironment" -as [type]
    if ($null -eq $nativeEnvironmentType)
    {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace RVX.Validation
{
    public static class NativeEnvironment
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr GetEnvironmentStringsW();

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FreeEnvironmentStringsW(IntPtr environmentBlock);

        public static string[] GetEntries()
        {
            IntPtr environmentBlock = GetEnvironmentStringsW();
            if (environmentBlock == IntPtr.Zero)
            {
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            }

            try
            {
                var entries = new List<string>();
                IntPtr cursor = environmentBlock;
                while (true)
                {
                    string entry = Marshal.PtrToStringUni(cursor);
                    if (String.IsNullOrEmpty(entry))
                    {
                        break;
                    }
                    entries.Add(entry);
                    cursor = IntPtr.Add(cursor, checked((entry.Length + 1) * sizeof(char)));
                }
                return entries.ToArray();
            }
            finally
            {
                if (!FreeEnvironmentStringsW(environmentBlock))
                {
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                }
            }
        }
    }
}
'@
    }

    return @([RVX.Validation.NativeEnvironment]::GetEntries())
}

function Get-ValidationPathCanonicalizationPlan
{
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$EnvironmentEntries,
        [Parameter(Mandatory)][bool]$IsWindows,
        [AllowNull()][string]$GuardValue,
        [AllowNull()][string]$EffectivePath
    )

    if (-not $IsWindows)
    {
        return [pscustomobject]@{
            shouldRelaunch = $false
            canonicalPath = $null
            pathEntryCount = 0
        }
    }

    $pathEntries = [System.Collections.Generic.List[object]]::new()
    foreach ($rawEntry in $EnvironmentEntries)
    {
        $entry = ConvertFrom-ValidationEnvironmentEntry -Entry $rawEntry
        if ([string]::Equals($entry.name, "PATH", [StringComparison]::OrdinalIgnoreCase))
        {
            $pathEntries.Add($entry)
        }
    }

    if ($pathEntries.Count -le 1)
    {
        return [pscustomobject]@{
            shouldRelaunch = $false
            canonicalPath = if ($pathEntries.Count -eq 1) { $pathEntries[0].value } else { $null }
            pathEntryCount = $pathEntries.Count
        }
    }
    if ($GuardValue -eq "1")
    {
        throw (
            "Validation process environment canonicalization guard is set, " +
            "but $($pathEntries.Count) PATH entries remain.")
    }
    if ([string]::IsNullOrWhiteSpace($EffectivePath))
    {
        throw "Validation process environment has no usable effective PATH value."
    }

    $effectivePathMatches = @($pathEntries | Where-Object {
        [string]::Equals($_.value, $EffectivePath, [StringComparison]::Ordinal)
    })
    if ($effectivePathMatches.Count -eq 0)
    {
        throw (
            "Validation process effective PATH does not exactly match any raw " +
            "PATH/Path environment entry.")
    }

    return [pscustomobject]@{
        shouldRelaunch = $true
        canonicalPath = $EffectivePath
        pathEntryCount = $pathEntries.Count
    }
}

function New-ValidationCanonicalProcessStartInfo
{
    param(
        [Parameter(Mandatory)][string]$PowerShellExecutable,
        [Parameter(Mandatory)][string]$WorkingDirectory,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$EnvironmentEntries,
        [Parameter(Mandatory)][string]$CanonicalPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Arguments
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $PowerShellExecutable
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.Environment.Clear()

    foreach ($rawEntry in $EnvironmentEntries)
    {
        $entry = ConvertFrom-ValidationEnvironmentEntry -Entry $rawEntry
        if ([string]::Equals($entry.name, "PATH", [StringComparison]::OrdinalIgnoreCase) -or
            [string]::Equals($entry.name, $script:ValidationPathCanonicalizationGuard,
                [StringComparison]::OrdinalIgnoreCase))
        {
            continue
        }
        # Per-drive current-directory entries (for example, '=C:=C:\work') are
        # Windows shell implementation details. WorkingDirectory is explicit for
        # the child and ProcessStartInfo rejects those pseudo-variable names.
        if ($entry.name.StartsWith("=", [StringComparison]::Ordinal))
        {
            continue
        }
        $startInfo.Environment[$entry.name] = $entry.value
    }
    $startInfo.Environment["PATH"] = $CanonicalPath
    $startInfo.Environment[$script:ValidationPathCanonicalizationGuard] = "1"

    foreach ($argument in $Arguments)
    {
        $startInfo.ArgumentList.Add($argument)
    }
    return $startInfo
}

function Invoke-ValidationProcessEnvironmentCanonicalization
{
    param(
        [Parameter(Mandatory)][string]$WorkingDirectory,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$InvocationArguments,
        [AllowEmptyCollection()][string[]]$EnvironmentEntries = @(),
        [AllowNull()][string]$GuardValue = $env:RVX_VALIDATION_PATH_CANONICALIZED,
        [AllowNull()][string]$EffectivePath = [Environment]::GetEnvironmentVariable(
            "PATH", [EnvironmentVariableTarget]::Process),
        [bool]$IsWindows = [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [Runtime.InteropServices.OSPlatform]::Windows),
        [scriptblock]$StartChild
    )

    if ($EnvironmentEntries.Count -eq 0 -and $IsWindows)
    {
        $EnvironmentEntries = Get-ValidationProcessEnvironmentEntries
    }
    $plan = Get-ValidationPathCanonicalizationPlan `
        -EnvironmentEntries $EnvironmentEntries `
        -IsWindows $IsWindows `
        -GuardValue $GuardValue `
        -EffectivePath $EffectivePath
    if (-not $plan.shouldRelaunch)
    {
        return [pscustomobject]@{
            relaunched = $false
            exitCode = 0
        }
    }

    $powerShellFileName = if ($IsWindows) { "pwsh.exe" } else { "pwsh" }
    $powerShellExecutable = Join-Path $PSHOME $powerShellFileName
    if (-not (Test-Path -LiteralPath $powerShellExecutable -PathType Leaf))
    {
        throw "Validation process environment canonicalization requires PowerShell 7."
    }
    $startInfo = New-ValidationCanonicalProcessStartInfo `
        -PowerShellExecutable $powerShellExecutable `
        -WorkingDirectory $WorkingDirectory `
        -EnvironmentEntries $EnvironmentEntries `
        -CanonicalPath $plan.canonicalPath `
        -Arguments $InvocationArguments

    if ($null -ne $StartChild)
    {
        $exitCode = & $StartChild $startInfo
        return [pscustomobject]@{
            relaunched = $true
            exitCode = [int]$exitCode
        }
    }

    $child = [System.Diagnostics.Process]::Start($startInfo)
    if ($null -eq $child)
    {
        throw "Validation process environment canonicalization could not start PowerShell 7."
    }
    $child.WaitForExit()
    return [pscustomobject]@{
        relaunched = $true
        exitCode = $child.ExitCode
    }
}

Export-ModuleMember -Function @(
    "Get-ValidationPathCanonicalizationPlan",
    "Get-ValidationProcessEnvironmentEntries",
    "Invoke-ValidationProcessEnvironmentCanonicalization",
    "New-ValidationCanonicalProcessStartInfo"
)
