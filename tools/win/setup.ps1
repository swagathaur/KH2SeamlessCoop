# First-time setup (double-click "First-time setup.bat"). Safe to run again:
# every step checks what is already there and skips it.
#
#   1. Tools: Git, Python 3.11+, Visual Studio 2022's C++ build tools with its
#      CMake and Ninja. Missing ones are installed with winget (one Windows
#      "allow changes" prompt); an existing Visual Studio 2022 without the C++
#      CMake tools gets them added.
#   2. The libraries this project builds on (git submodules).
#   3. The PC release (KINGDOM HEARTS HD 1.5+2.5 ReMIX, Steam or Epic), if you
#      have it: found and remembered in game\pc-release.txt, for the English
#      text and the HD textures.
#   4. Your disc image: Kingdom Hearts II Final Mix, PS2, Japan (SLPM-66675),
#      placed in Src\ and checked; its files are unpacked into game\.
#   5. Optionally, the build: recompiler, translation, game (tools\win\build.ps1).
#
# Options: -Yes answers yes to every question (unattended), -NoBuild stops
# before step 5, -NoInstall only reports missing tools, -NoWarmup skips
# recording the 3D programs after the first build (tools\win\vu-warmup.ps1).
param([switch]$Yes, [switch]$NoBuild, [switch]$NoInstall, [switch]$NoWarmup)

$ErrorActionPreference = 'Continue'
$Root = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
Set-Location $Root
New-Item -ItemType Directory -Force logs, Src, game | Out-Null
try { Start-Transcript -Path logs\setup.log -Force | Out-Null } catch { }

function Say([string]$t, [string]$c = 'Cyan') { Write-Host $t -ForegroundColor $c }
function Step([string]$t) { Write-Host ''; Write-Host "== $t" -ForegroundColor White }
function Done { try { Stop-Transcript | Out-Null } catch { }; if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null } }
function Fail([string]$t) { Write-Host $t -ForegroundColor Red; Write-Host 'The full log is logs\setup.log.' -ForegroundColor Yellow; Done; exit 1 }
function Ask([string]$q, [bool]$default = $true) {
    if ($Yes) { return $default }
    $hint = if ($default) { '[Y/n]' } else { '[y/N]' }
    while ($true) {
        $a = (Read-Host "$q $hint").Trim().ToLower()
        if (-not $a) { return $default }
        if ($a -in 'y', 'yes') { return $true }
        if ($a -in 'n', 'no') { return $false }
    }
}
function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}

# ---- 1. tools ---------------------------------------------------------------------
Step '1/5  Build tools'
function Find-Python {
    foreach ($cmd in 'python', 'py') {
        $c = Get-Command $cmd -ErrorAction SilentlyContinue
        if (-not $c -or $c.Source -like '*WindowsApps*') { continue } # the Store's placeholder
        $ok = & $cmd -c 'import sys; print(1 if sys.version_info >= (3, 11) else 0)' 2>$null
        if ($ok -eq '1') { return $cmd }
    }
    return $null
}
function Find-VS {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    # Visual Studio 2022 (17.x) with the x64 C++ compiler, any edition.
    $p = & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($p) { return ($p | Select-Object -First 1) }
    return $null
}
function Find-AnyVS2022 {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -version '[17.0,18.0)' -products * -property installationPath
    if ($p) { return ($p | Select-Object -First 1) }
    return $null
}
function Has-VSCMake($vs) { $vs -and (Test-Path (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')) }

Refresh-Path
$git = [bool](Get-Command git -ErrorAction SilentlyContinue)
$py = Find-Python
$vs = Find-VS
$vsAny = Find-AnyVS2022
$cmakeOk = Has-VSCMake $vs
Say ("  Git:                      " + $(if ($git) { 'found' } else { 'missing' }))
Say ("  Python 3.11+:             " + $(if ($py) { 'found' } else { 'missing' }))
Say ("  VS 2022 C++ build tools:  " + $(if ($vs) { "found ($vs)" } elseif ($vsAny) { "Visual Studio 2022 without C++ ($vsAny)" } else { 'missing' }))
Say ("  CMake + Ninja (from VS):  " + $(if ($cmakeOk) { 'found' } else { 'missing' }))

$components = @('Microsoft.VisualStudio.Workload.VCTools', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    'Microsoft.VisualStudio.Component.VC.CMake.Project', 'Microsoft.VisualStudio.Component.Windows11SDK.22621')
$adds = ($components | ForEach-Object { "--add $_" }) -join ' '

if (-not ($git -and $py -and $vs -and $cmakeOk)) {
    if ($NoInstall) { Fail 'Missing tools (above). Run without -NoInstall to install them.' }
    if (-not (Ask 'Install the missing tools now? (Windows will ask to allow changes; 20-40 minutes for Visual Studio)')) {
        Fail 'Install them yourself (see README "Setup"), then run this again.'
    }
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Start-Process 'ms-windows-store://pdp/?ProductId=9NBLGGH4NNS1'
        Fail 'winget (Windows Package Manager) is missing: the Microsoft Store has opened "App Installer". Install or update it, then run this again.'
    }
    $common = @('--exact', '--source', 'winget', '--accept-package-agreements', '--accept-source-agreements')
    if (-not $git) { Say '  Installing Git ...'; winget install --id Git.Git @common --silent | Out-Host }
    if (-not $py) { Say '  Installing Python 3.12 ...'; winget install --id Python.Python.3.12 @common --silent --scope user | Out-Host }
    $setupExe = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    if ($vsAny -and (Test-Path $setupExe)) {
        if (-not $vs -or -not $cmakeOk) {
            # Visual Studio 2022 is there: add the C++ pieces to it.
            Say "  Adding the C++ build tools and CMake to $vsAny ..."
            Start-Process -Wait -Verb RunAs $setupExe -ArgumentList "modify --installPath `"$vsAny`" $adds --passive --norestart"
        }
    } else {
        Say '  Installing the Visual Studio 2022 Build Tools (C++, CMake, Windows SDK) ...'
        winget install --id Microsoft.VisualStudio.2022.BuildTools @common --override "--wait --passive --norestart $adds" | Out-Host
    }
    Refresh-Path
    $git = [bool](Get-Command git -ErrorAction SilentlyContinue)
    $py = Find-Python
    $vs = Find-VS
    if (-not $git) { Fail 'Git is still missing. Restart the PC and run this again.' }
    if (-not $py) { Fail 'Python is still missing. Restart the PC and run this again.' }
    if (-not (Has-VSCMake $vs)) { Fail 'The Visual Studio C++ build tools (with CMake) are still missing. Restart the PC and run this again.' }
    Say '  Tools installed.' 'Green'
}
if ($py -eq 'py') {
    # The project's scripts call "python".
    $real = & py -3 -c 'import sys; print(sys.executable)'
    $env:Path = (Split-Path $real) + ';' + $env:Path
}

# ---- 2. libraries -------------------------------------------------------------------
Step '2/5  Libraries (git submodules)'
if (-not (Test-Path .git)) { Fail 'This folder is not a git checkout: get it with "git clone" (see README).' }
$missing = (& git submodule status --recursive) | Where-Object { $_ -match '^-' }
$pgs = 'third_party\parallel-gs'
if ($missing -or -not (Test-Path "$pgs\gs")) {
    Say '  Downloading them (a few minutes) ...'
    & git -c core.longpaths=true submodule update --init --recursive --jobs 8
    if ($LASTEXITCODE) { Fail 'Could not download the libraries (see above).' }
} else {
    Say '  Present.' 'Green'
}
# The GPU renderer's edits for this project, on top of its pinned version.
$diff = (Resolve-Path patches\parallel-gs\tree.diff -ErrorAction SilentlyContinue).Path
if ($diff) {
    & git -C $pgs apply --check --reverse --whitespace=nowarn $diff 2>$null
    if ($LASTEXITCODE) {
        & git -C $pgs apply --whitespace=nowarn $diff
        if ($LASTEXITCODE) { Fail "Could not apply $diff to $pgs (it has other local changes?)." }
        Say '  Applied the renderer changes.' 'Green'
    }
}

# ---- 3. PC release --------------------------------------------------------------------
Step '3/5  KINGDOM HEARTS HD 1.5+2.5 ReMIX (Steam or Epic), for English text and HD textures'
. "$PSScriptRoot\pc-release.ps1"
function Test-Dt($p) { $p -and ((Test-Path (Join-Path $p 'kh2_first.hed')) -or (Test-Path (Join-Path $p 'backup\kh2_first.hed'))) }
function Pick-Folder([string]$title) {
    Add-Type -AssemblyName System.Windows.Forms
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = $title
    if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    return $null
}
$pc = Find-PCRelease
if ($pc) {
    Say "  Found: $pc" 'Green'
    Set-Content -Encoding ascii game\pc-release.txt $pc
} elseif (Ask 'Not found automatically. Do you have it installed (Steam or Epic)?' $false) {
    Say '  Choose its install folder (the one with "Image" inside, or "Image\dt" itself).' 'Yellow'
    $pick = Pick-Folder 'KINGDOM HEARTS HD 1.5+2.5 ReMIX install folder'
    foreach ($c in @($pick, (Join-Path "$pick" 'Image\dt'), (Join-Path "$pick" 'dt'))) {
        if (Test-Dt $c) { $pc = (Resolve-Path $c).Path; break }
    }
    if ($pc) { Set-Content -Encoding ascii game\pc-release.txt $pc; Say "  Using $pc" 'Green' }
    else { Say '  No kh2_first.hed there: skipping (the game will be in Japanese, with PS2 textures). Run this again to retry.' 'Yellow' }
} else {
    Say '  Skipped: the game will be in Japanese, with PS2 textures. Run this again after installing it.' 'Yellow'
}
if ($pc) { Say '  The English text and HD texture index are built the first time you start "Play KH2.bat".' }

# ---- 4. disc image ----------------------------------------------------------------------
Step '4/5  Your disc image (Kingdom Hearts II Final Mix, PS2, Japan, SLPM-66675)'
$src = (Resolve-Path Src).Path
function Find-Iso { Get-ChildItem Src -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1 }
$iso = Find-Iso
while (-not $iso) {
    if ($Yes) { Fail "No .iso in $src." }
    Say "  Copy your .iso into this folder (it opens now): $src" 'Yellow'
    Start-Process explorer.exe $src
    Read-Host '  Press Enter when it is there' | Out-Null
    $iso = Find-Iso
}
Say "  $($iso.Name)"
if (-not (Test-Path game\SLPM_666.75) -or -not (Test-Path game\overlays)) {
    Say '  Checking it and unpacking the game files ...'
    & python tools\extract_game.py $iso.FullName game | Out-Host
    if ($LASTEXITCODE) { Fail 'That disc image did not check out (above). It has to be the Japanese PS2 Kingdom Hearts II Final Mix (SLPM-66675).' }
    Say '  Disc OK.' 'Green'
} else {
    Say '  Already unpacked.' 'Green'
}

# ---- 5. build -----------------------------------------------------------------------------
Step '5/5  Build'
if ($NoBuild -or -not (Ask 'Build the game now? (the first build takes 30-90 minutes)')) {
    Say '  Later: tools\win\recomp.ps1, then tools\win\build.ps1 (README "Setup"), or run this again.'
    Done; exit 0
}
. "$PSScriptRoot\vsenv.ps1"
Set-Location $Root
$recompiler = 'build\ps2recomp-tools\ps2xRecomp\ps2_recomp.exe'
if (-not (Test-Path $recompiler)) {
    Say '  The recompiler ...'
    cmake -S . -B build\patch-only -G Ninja -DKH2_PATCH_ONLY=ON 2>&1 | Out-File -Encoding utf8 logs\patch.log
    if ($LASTEXITCODE) { Get-Content logs\patch.log -Tail 15 | Out-Host; Fail 'Could not patch the runtime (logs\patch.log).' }
    cmake -S third_party\PS2Recomp -B build\ps2recomp-tools -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF 2>&1 | Out-File -Encoding utf8 logs\tools.log
    if (-not $LASTEXITCODE) { cmake --build build\ps2recomp-tools --parallel 2>&1 | Out-File -Append -Encoding utf8 logs\tools.log }
    if ($LASTEXITCODE -or -not (Test-Path $recompiler)) { Get-Content logs\tools.log -Tail 15 | Out-Host; Fail 'Could not build the recompiler (logs\tools.log).' }
}
if (-not (Test-Path generated\main)) {
    Say '  Translating the game to C++ (a few minutes) ...'
    & python tools\recomp.py --recompiler $recompiler | Out-Host
    if ($LASTEXITCODE) { Fail 'Translation failed (logs\recomp_*.log).' }
}
Say '  Building the game (first time: 30-90 minutes; later only what changed) ...'
$start = Get-Date
& powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\build.ps1 | Out-Host
if ($LASTEXITCODE) { Fail 'The build failed (logs\build.log).' }
Say ("  Built in {0:N0} min." -f ((Get-Date) - $start).TotalMinutes) 'Green'
# 3D at full speed from the first play: record the programs of every world's
# first room (unattended, 5-10 minutes) and build them in.
if (-not $NoWarmup -and -not (Test-Path build\vu-profile\entries.txt)) {
    Say '  Recording the 3D programs every world uses (5-10 minutes, nothing to do) ...'
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\vu-warmup.ps1 | Out-Host
    if (Test-Path build\vu-profile\entries.txt) {
        Say '  Building them in (a few minutes) ...'
        & powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\build.ps1 | Select-Object -Last 2 | Out-Host
        if ($LASTEXITCODE) { Fail 'The build failed (logs\build.log).' }
    }
}
Say ''
Say 'All set: double-click "Play KH2.bat" to play.' 'Green'
Done
exit 0
