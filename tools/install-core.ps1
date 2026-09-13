<#
.SYNOPSIS
  Installs Proteus Retune as a wrapper around an existing RetroArch core.

.EXAMPLE
  .\tools\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Core snes9x
  Creates cores\proteus_snes9x_libretro.dll and info\proteus_snes9x_libretro.info,
  which RetroArch lists as "Proteus Retune (Snes9x)".
#>
param(
    [Parameter(Mandatory)] [string] $RetroArch,
    [Parameter(Mandatory)] [string] $Core,
    [string] $CoresDir = (Join-Path $RetroArch 'cores'),
    [string] $InfoDir = (Join-Path $RetroArch 'info'),
    [string] $Dll = (Join-Path $PSScriptRoot '..\build\proteus_libretro.dll')
)

$ErrorActionPreference = 'Stop'
$Core = $Core -replace '_libretro(\.dll)?$', ''

if (-not (Test-Path $Dll)) { throw "Build Proteus first: $Dll not found" }
$inner = Join-Path $CoresDir "${Core}_libretro.dll"
if (-not (Test-Path $inner)) { throw "Core not installed: $inner" }

$target = Join-Path $CoresDir "proteus_${Core}_libretro.dll"
Copy-Item $Dll $target -Force
Write-Output "installed $target"

$innerInfo = Join-Path $InfoDir "${Core}_libretro.info"
if (Test-Path $innerInfo) {
    # Reuse the inner core's metadata (extensions, firmware, database) under a new name.
    $lines = Get-Content -LiteralPath $innerInfo -Encoding UTF8 | ForEach-Object {
        if ($_ -match '^\s*(display_name|corename)\s*=\s*"(.*)"\s*$') {
            "$($Matches[1]) = `"Proteus Retune ($($Matches[2]))`""
        } else { $_ }
    }
    $infoTarget = Join-Path $InfoDir "proteus_${Core}_libretro.info"
    [System.IO.File]::WriteAllLines($infoTarget, [string[]]$lines, (New-Object System.Text.UTF8Encoding $false))
    Write-Output "installed $infoTarget"
} else {
    Write-Warning "No $innerInfo; RetroArch will list the core by file name."
}
