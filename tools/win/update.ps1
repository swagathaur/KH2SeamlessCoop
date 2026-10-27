# Update and rebuild (used by "Update and rebuild.bat"): gets the latest version of this
# project, brings the libraries to the versions it pins, and builds what
# changed (build.ps1; it also translates the game again when the recompiler
# changed). Run "First-time setup.bat" instead on a new PC.
param([switch]$NoPause) # -NoPause: no prompts (testing)
$ErrorActionPreference = 'Continue'
$Root = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
Set-Location $Root
New-Item -ItemType Directory -Force logs | Out-Null
function Say([string]$t, [string]$c = 'Cyan') { Write-Host $t -ForegroundColor $c }
function Fail([string]$t) { Write-Host $t -ForegroundColor Red; if (-not $NoPause) { Read-Host 'Press Enter to close' | Out-Null }; exit 1 }

if (-not (Test-Path .git) -or -not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail 'This folder is not a git checkout, or Git is missing: run "First-time setup.bat".'
}
if (-not (Test-Path build\win\kh2recomp.exe) -or -not (Test-Path generated\main)) {
    Fail 'The game was never built here: run "First-time setup.bat" first.'
}
while (Get-Process kh2recomp -ErrorAction SilentlyContinue) {
    Read-Host 'The game is running: close it, then press Enter' | Out-Null
}

# ---- 1. this project ------------------------------------------------------
Say '== Getting the latest version ...' 'White'
$pinRecomp = (& git rev-parse HEAD:third_party/PS2Recomp 2>$null)
$pinPgs = (& git rev-parse HEAD:third_party/parallel-gs 2>$null)
$pgsDiffBefore = (& git rev-parse HEAD:patches/parallel-gs/tree.diff 2>$null)
$graniteDiffBefore = (& git rev-parse HEAD:patches/parallel-gs/granite.diff 2>$null)
$old = (& git rev-parse HEAD)
& git pull --ff-only
if ($LASTEXITCODE) {
    Fail ("Could not update (see above). If it says your local changes would be overwritten, " +
          "those files were changed here: 'git stash' keeps them aside, then run this again.")
}
$new = (& git rev-parse HEAD)
if ($old -eq $new) { Say '  Already up to date.' 'Green' }
else { & git log --oneline "$old..$new" | ForEach-Object { "  $_" } }

# ---- 2. libraries (git submodules) ----------------------------------------
# Both carry this project's changes in their working trees: the recompiler's
# are reapplied by the build (patches\ps2recomp, recorded in a stamp), the
# renderer's from patches\parallel-gs\tree.diff.
$pgs = 'third_party\parallel-gs'
$pgsDiff = (Resolve-Path patches\parallel-gs\tree.diff -ErrorAction SilentlyContinue).Path
if ((& git rev-parse HEAD:third_party/PS2Recomp) -ne $pinRecomp) {
    Say '  The recompiler library moved to a new version ...'
    Remove-Item -Force third_party\PS2Recomp\.kh2-applied-patches -ErrorAction SilentlyContinue
    & git -C third_party\PS2Recomp reset -q --hard
    & git -c core.longpaths=true submodule update --init --recursive --force third_party/PS2Recomp
    if ($LASTEXITCODE) { Fail 'Could not update third_party\PS2Recomp (see above).' }
}
$pgsMoved = (& git rev-parse HEAD:third_party/parallel-gs) -ne $pinPgs
$pgsDiffChanged = ((& git rev-parse HEAD:patches/parallel-gs/tree.diff 2>$null) -ne $pgsDiffBefore) -or
                  ((& git rev-parse HEAD:patches/parallel-gs/granite.diff 2>$null) -ne $graniteDiffBefore)
if ($pgsMoved -or $pgsDiffChanged) {
    Say '  The renderer library or its changes moved ...'
    & git -C $pgs reset -q --hard
    & git -c core.longpaths=true submodule update --init --recursive --force third_party/parallel-gs
    if ($LASTEXITCODE) { Fail "Could not update $pgs (see above)." }
}
& git -c core.longpaths=true submodule update --init --recursive --jobs 8 2>&1 | Out-Null
if ($pgsDiff) {
    & git -C $pgs apply --check --reverse --whitespace=nowarn $pgsDiff 2>$null
    if ($LASTEXITCODE) {
        & git -C $pgs apply --whitespace=nowarn $pgsDiff
        if ($LASTEXITCODE) { Fail "Could not apply $pgsDiff to $pgs." }
    }
}
$graniteDiff = (Resolve-Path patches\parallel-gs\granite.diff -ErrorAction SilentlyContinue).Path
if ($graniteDiff) {
    & git -C "$pgs\Granite" apply --check --reverse --whitespace=nowarn $graniteDiff 2>$null
    if ($LASTEXITCODE) {
        & git -C "$pgs\Granite" apply --whitespace=nowarn $graniteDiff
        if ($LASTEXITCODE) { Fail "Could not apply $graniteDiff to $pgs\Granite." }
    }
}

# ---- 3. build ---------------------------------------------------------------
Say '== Building what changed ...' 'White'
$start = Get-Date
& powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\build.ps1 | Out-Host
if ($LASTEXITCODE) { Fail 'The build failed (logs\build.log).' }
Say ("Up to date and built ({0:N0} min). Start the game with 'Play KH2.bat'." -f ((Get-Date) - $start).TotalMinutes) 'Green'
if (-not $NoPause) { Read-Host 'Press Enter to close' | Out-Null }
