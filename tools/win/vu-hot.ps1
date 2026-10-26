# Compile recorded VU1 programs into a DLL the running game loads (called by
# kh2recomp.exe, port/runtime/vu_hot.cpp; not meant to be run by hand).
#   -Job <dir>   holds profile\ (entries.txt and the vu1-*.code images);
#                hot.dll and log.txt are written there
#   -Root <dir>  the project
# Same generator and compiler flags as the build (compile_commands.json's
# entry for the runtime's ps2_vu1_program.cpp), linked against the game's
# import library (build\win\kh2recomp.lib).
param([Parameter(Mandatory = $true)][string]$Job, [Parameter(Mandatory = $true)][string]$Root,
      [int]$Shards = 0, [int]$MaxPairs = 128)
$ErrorActionPreference = 'Continue'
$log = Join-Path $Job 'log.txt'
function Log($m) { Add-Content -Encoding utf8 $log $m }
try {
    Set-Location $Root
    . (Join-Path $Root 'tools\win\vsenv.ps1')
    Set-Location $Root
    $gen = Join-Path $Job 'gen'
    New-Item -ItemType Directory -Force $gen | Out-Null
    # One file per core, compiled side by side (cl /MP), and short routine
    # chunks (MSVC slows down sharply on long functions): measured 528 s as one
    # file, 169 s like this, for 14 programs on 24 cores.
    if ($Shards -le 0) { $Shards = [Math]::Max(2, [int]$env:NUMBER_OF_PROCESSORS - 1) }
    $extra = @()
    if ($MaxPairs -gt 0) { $extra = @('--max-routine-pairs', "$MaxPairs") }
    $out = & python third_party\PS2Recomp\tools\compile_vu_programs.py --output "$gen\vu_programs.inc" --shards $Shards @extra (Join-Path $Job 'profile') 2>&1
    Log ($out | Out-String)
    if ($LASTEXITCODE) { throw 'compile_vu_programs.py failed' }

    @'
// A module of VU1 programs compiled while the game runs (tools/win/vu-hot.ps1).
#include "ps2_vu1_program.h"
#include "vu_programs_extern.inc"

namespace ps2_vu_program
{
namespace
{
const Entry kHotEntries[] = {
#include "vu_programs.inc"
};
}
}

extern "C" __declspec(dllexport) const ps2_vu_program::Entry *kh2_vu_hot_entries(size_t *count)
{
    *count = sizeof(ps2_vu_program::kHotEntries) / sizeof(ps2_vu_program::kHotEntries[0]);
    return ps2_vu_program::kHotEntries;
}
'@ | Set-Content -Encoding ascii "$gen\module.cpp"

    # The runtime's own compile line, for the same headers, defines and flags.
    $cc = Get-Content -Raw build\win\compile_commands.json | ConvertFrom-Json
    $ref = $cc | Where-Object { $_.file -like '*ps2_vu1_program.cpp' } | Select-Object -First 1
    if (-not $ref) { throw 'no compile line for ps2_vu1_program.cpp in build\win\compile_commands.json' }
    $cmd = $ref.command
    $cmd = $cmd -replace '^\S*cl\.exe\s+', ''
    $cmd = $cmd -replace '\s/Fo\S+', '' -replace '\s/Fd\S+', '' -replace '\s-c\s+.*$', '' -replace '\s/showIncludes', ''
    $cmd = $cmd -replace '\s/Zi\b', '' -replace '-DPS2X_VU_HOT_EXPORT=1', '-DPS2X_VU_HOT_IMPORT=1'
    $vuDir = Join-Path $Root 'third_party\PS2Recomp\ps2xRuntime\src\lib\vu'
    $flags = "$cmd /I`"$vuDir`" /I`"$gen`" /Fo`"$gen\\`""
    $sources = @(Get-ChildItem $gen -Filter 'vu_programs_*.cpp' | ForEach-Object { $_.FullName }) + @("$gen\module.cpp")
    $compile = "cl $flags /MP /c " + (($sources | ForEach-Object { "`"$_`"" }) -join ' ')
    Log $compile
    $o = cmd /c "$compile 2>&1"
    Log ($o | Out-String)
    if ($LASTEXITCODE) { throw 'compiling failed' }
    $objs = Get-ChildItem $gen -Filter *.obj | ForEach-Object { "`"$($_.FullName)`"" }
    $link = "link /nologo /DLL /OUT:`"$Job\hot.dll`" $($objs -join ' ') `"$Root\build\win\kh2recomp.lib`""
    Log $link
    $o = cmd /c "$link 2>&1"
    Log ($o | Out-String)
    if ($LASTEXITCODE) { throw 'linking failed' }
    exit 0
} catch {
    Log "FAILED: $_"
    exit 1
}
