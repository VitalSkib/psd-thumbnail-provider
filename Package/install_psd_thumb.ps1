[CmdletBinding()]
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath))
    if ($Uninstall) { $arguments += '-Uninstall' }
    $elevated = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $elevated.ExitCode
}

$root = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
$programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$folder = Join-Path $programFiles 'PSD PSB Thumbnail Provider'
$dll = Join-Path $folder 'PsdThumbnailProvider.dll'
$backup = Join-Path $folder 'registration.json'
$source = Join-Path $PSScriptRoot 'PsdThumbnailProvider.dll'
$clsid = '{5BB45D32-AF01-414F-B60A-5E999B986681}'
$thumbnailSlot = '{e357fccd-a995-4576-b01f-234630154e96}'
$class = "SOFTWARE\Classes\CLSID\$clsid"
$approved = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved'

$entries = @(
    @{ Path = $class; Name = ''; Value = 'PSD and PSB Thumbnail Provider' }
    @{ Path = "$class\InprocServer32"; Name = ''; Value = $dll }
    @{ Path = "$class\InprocServer32"; Name = 'ThreadingModel'; Value = 'Apartment' }
    @{ Path = "SOFTWARE\Classes\.psd\ShellEx\$thumbnailSlot"; Name = ''; Value = $clsid }
    @{ Path = "SOFTWARE\Classes\.psb\ShellEx\$thumbnailSlot"; Name = ''; Value = $clsid }
    @{ Path = $approved; Name = $clsid; Value = 'PSD and PSB Thumbnail Provider' }
)

function Restore-Registration($snapshot) {
    foreach ($entry in $snapshot) {
        $key = $root.OpenSubKey($entry.Path, $true)
        if (!$key) { continue }
        try {
            if ($key.GetValue($entry.Name, $null, 'DoNotExpandEnvironmentNames') -cne $entry.Value) { continue }
            if ($entry.Existed) {
                $key.SetValue($entry.Name, $entry.Previous, $entry.Kind)
            } else {
                $key.DeleteValue($entry.Name, $false)
            }
        } finally {
            $key.Dispose()
        }
    }
}

try {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class PsdRegistryNotice {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(uint id, uint flags, IntPtr first, IntPtr second);
}
"@

    if ($Uninstall) {
        if (!(Test-Path -LiteralPath $backup -PathType Leaf)) { throw 'Installation backup was not found.' }
        $snapshot = ConvertFrom-Json -InputObject (Get-Content -LiteralPath $backup -Raw)
        Restore-Registration $snapshot
        [PsdRegistryNotice]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
        if (Test-Path -LiteralPath $dll) {
            try { Remove-Item -LiteralPath $dll -Force }
            catch { throw 'Registration removed. Windows is still using the DLL. Restart Windows and run uninstall again to remove the file.' }
        }
        Remove-Item -LiteralPath $backup
        Write-Host 'PSD and PSB thumbnail provider uninstalled.'
        exit 0
    }

    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw 'PsdThumbnailProvider.dll is missing from the extracted package.' }
    if ((Test-Path -LiteralPath $backup) -or (Test-Path -LiteralPath $dll)) { throw 'Uninstall the existing package before installing.' }
    $existingClass = $root.OpenSubKey("$class\InprocServer32")
    if ($existingClass) {
        try {
            if ($existingClass.GetValue('')) { throw 'Another version of this provider is already registered. Uninstall it first.' }
        } finally { $existingClass.Dispose() }
    }

    $snapshot = foreach ($entry in $entries) {
        $key = $root.OpenSubKey($entry.Path)
        try {
            $exists = $key -and $key.GetValueNames() -contains $entry.Name
            $previous = $null
            $kind = $null
            if ($exists) {
                $kind = $key.GetValueKind($entry.Name).ToString()
                if ($kind -notin 'String', 'ExpandString') { throw "Unexpected registry value type at $($entry.Path)." }
                $previous = $key.GetValue($entry.Name, $null, 'DoNotExpandEnvironmentNames')
            }
            [pscustomobject]@{ Path=$entry.Path; Name=$entry.Name; Value=$entry.Value; Existed=$exists; Previous=$previous; Kind=$kind }
        } finally {
            if ($key) { $key.Dispose() }
        }
    }

    New-Item -Path $folder -ItemType Directory -Force | Out-Null
    $snapshot | ConvertTo-Json | Set-Content -LiteralPath $backup -Encoding UTF8
    try {
        Copy-Item -LiteralPath $source -Destination $dll
        foreach ($entry in $entries) {
            $key = $root.CreateSubKey($entry.Path)
            try { $key.SetValue($entry.Name, $entry.Value, 'String') }
            finally { $key.Dispose() }
        }
    } catch {
        Restore-Registration $snapshot
        throw
    }
    [PsdRegistryNotice]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Host 'PSD and PSB thumbnail provider installed.'
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    $root.Dispose()
}
