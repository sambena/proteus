# SPDX-License-Identifier: LGPL-2.1-or-later
<#
.SYNOPSIS
  Installs Proteus Retune into a RetroArch folder.

.DESCRIPTION
  -Core installs the wrapper core around an existing core (per-song muting,
  core options). -Dsp installs the audio DSP plugin, which works with any core
  unchanged. Both can be given at once.

.EXAMPLE
  .\tools\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Core snes9x
  Creates cores\proteus_snes9x_libretro.dll and its .info file, listed as
  "Nintendo - SNES / SFC (Proteus Retune + Snes9x)".

.EXAMPLE
  .\tools\install-core.ps1 -RetroArch C:\RetroArch-Win64 -Dsp
  Copies the plugin to filters\audio and adds Proteus.dsp, selectable under
  Settings > Audio > DSP Plugin.
#>
param(
    [Parameter(Mandatory)] [string] $RetroArch,
    [string] $Core,
    [switch] $Dsp,
    [string] $CoresDir = (Join-Path $RetroArch 'cores'),
    [string] $InfoDir = (Join-Path $RetroArch 'info'),
    [string] $FiltersDir = (Join-Path $RetroArch 'filters\audio'),
    [string] $Build
)

$ErrorActionPreference = 'Stop'
# $PSScriptRoot is empty inside param() defaults on Windows PowerShell 5.1.
if (-not $Build) { $Build = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\build' }
if (-not $Core -and -not $Dsp) { throw 'Pass -Core <name>, -Dsp, or both.' }

if ($Core) {
    $Core = $Core -replace '_libretro(\.dll)?$', ''
    $dll = Join-Path $Build 'proteus_libretro.dll'
    if (-not (Test-Path $dll)) { throw "Build Proteus first: $dll not found" }
    $inner = Join-Path $CoresDir "${Core}_libretro.dll"
    if (-not (Test-Path $inner)) { throw "Core not installed: $inner" }

    $target = Join-Path $CoresDir "proteus_${Core}_libretro.dll"
    Copy-Item $dll $target -Force
    Write-Output "installed $target"

    $innerInfo = Join-Path $InfoDir "${Core}_libretro.info"
    if (Test-Path $innerInfo) {
        # Reuse the inner core's metadata (extensions, firmware, database) under a new name:
        # "Nintendo - SNES / SFC (Snes9x)" -> "Nintendo - SNES / SFC (Proteus Retune + Snes9x)"
        $lines = Get-Content -LiteralPath $innerInfo -Encoding UTF8 | ForEach-Object {
            if ($_ -match '^\s*display_name\s*=\s*"(.*)\((.+)\)"\s*$') {
                "display_name = `"$($Matches[1])(Proteus Retune + $($Matches[2]))`""
            } elseif ($_ -match '^\s*(display_name|corename)\s*=\s*"(.*)"\s*$') {
                "$($Matches[1]) = `"Proteus Retune ($($Matches[2]))`""
            } else { $_ }
        }
        $infoTarget = Join-Path $InfoDir "proteus_${Core}_libretro.info"
        [System.IO.File]::WriteAllLines($infoTarget, [string[]]$lines, (New-Object System.Text.UTF8Encoding $false))
        Write-Output "installed $infoTarget"
    } else {
        Write-Warning "No $innerInfo; RetroArch will list the core by file name."
    }
}

if ($Dsp) {
    $plugin = Join-Path $Build 'proteus_dsp.dll'
    if (-not (Test-Path $plugin)) { throw "Build Proteus first: $plugin not found" }
    if (-not (Test-Path $FiltersDir)) { throw "No audio filter folder: $FiltersDir" }

    Copy-Item $plugin (Join-Path $FiltersDir 'proteus_dsp.dll') -Force
    $preset = @'
filters = 1
filter0 = proteus

# Proteus Retune: swaps game music using profiles, with any core.
# Profiles are read from <ROM name>.proteus.ini next to the ROM, or from
# <system folder>/proteus/<ROM name>.ini. Song changes and errors go to
# <log folder>/proteus.log.
#
# The plugin cannot change the core's options while a game runs, so it saves
# the profile's [mute] options as the game's core options
# (config/<core>/<game>.opt); they apply from the next time the game is loaded.
# Proteus Studio's Generate INI writes them before the first run.
#
# Folders are found from retroarch.cfg; override them here if needed:
# proteus_system_dir = "C:\RetroArch\system"
# proteus_history = "C:\RetroArch\playlists\builtin\content_history.lpl"
# proteus_log = "C:\RetroArch\logs\proteus.log"
'@
    [System.IO.File]::WriteAllText((Join-Path $FiltersDir 'Proteus.dsp'), $preset, (New-Object System.Text.UTF8Encoding $false))
    Write-Output "installed $(Join-Path $FiltersDir 'proteus_dsp.dll') and Proteus.dsp"
}
