# Make a portable copy of the game (for a USB drive): everything it needs to
# run on another Windows PC, without the build tree, the PC release or
# Python there.
#
#   powershell -ExecutionPolicy Bypass -File tools\win\make-portable.ps1 -Dest E:\KH2
#
# What goes in (about 10 GB with the HD textures):
#   Play KH2.bat, tools\win\play.ps1      start it (double-click the .bat)
#   build\win\kh2recomp.exe and its DLLs  the game, plus the Visual C++ runtime
#   build\win\kh2recomp.ini               your settings (the copy does not ask the
#                                         friend's router to open a port: no-upnp)
#   game\                                 the game files, English text, your saves
#   game\files\hd\                        the HD textures it uses, copied out of the
#                                         PC release (-NoHd to leave them out)
#   Src\*.iso                             the disc image
# The drive must be exFAT or NTFS: the disc image is over 4 GB (FAT32's limit).
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [switch]$NoHd,          # leave the HD textures out (saves about 5 GB)
    [switch]$KeepRouter,    # keep asking routers to open the port (online play from there)
    [switch]$NoSaveCopies   # do not give players 2 and 3 a copy of your saves
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
Set-Location $root

$iso = Get-ChildItem Src -Filter *.iso | Select-Object -First 1
if (-not $iso) { throw 'No disc image in Src\' }
$hdBytes = 0
if (-not $NoHd) {
    if (-not (Test-Path game\files\hd\manifest.txt)) { "No HD texture index here: leaving the HD textures out."; $NoHd = $true }
    elseif (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw 'The HD textures need Python here to copy them (or use -NoHd).' }
    else { $hdBytes = 5.4e9 }
}

# Room on the drive, and its file system.
New-Item -ItemType Directory -Force $Dest | Out-Null
$Dest = (Resolve-Path $Dest).Path
$drive = Get-PSDrive -Name ($Dest.Substring(0, 1))
$need = $iso.Length + 0.3e9 + 0.2e9 + $hdBytes
"Copying to $Dest (needs about {0:N1} GB; {1:N1} GB free)" -f ($need / 1e9), ($drive.Free / 1e9)
if ($drive.Free -lt $need) { throw 'Not enough room on that drive.' }
try {
    $fs = (Get-Volume -DriveLetter $Dest.Substring(0, 1) -ErrorAction Stop).FileSystem
    if ($fs -eq 'FAT32') { throw "The drive is FAT32, which cannot hold the 4.3 GB disc image: format it as exFAT (or NTFS)." }
} catch [Microsoft.Management.Infrastructure.CimException] { }

# The game and its libraries.
New-Item -ItemType Directory -Force "$Dest\build\win", "$Dest\tools\win", "$Dest\Src", "$Dest\logs" | Out-Null
Copy-Item 'Play KH2.bat' $Dest
Copy-Item tools\win\play.ps1 "$Dest\tools\win\"
Copy-Item build\win\kh2recomp.exe "$Dest\build\win\"
Get-ChildItem build\win -Filter *.dll | Copy-Item -Destination "$Dest\build\win\"
Copy-Item build\win\gamecontrollerdb.txt "$Dest\build\win\" -ErrorAction SilentlyContinue # controller mappings
foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    $sys = Join-Path $env:SystemRoot "System32\$dll"
    if (Test-Path $sys) { Copy-Item $sys "$Dest\build\win\" }
}
$ini = @()
if (Test-Path build\win\kh2recomp.ini) { $ini = @(Get-Content build\win\kh2recomp.ini | Where-Object { $_ -notmatch '^\s*no-upnp' }) }
if (-not $KeepRouter) { $ini += 'no-upnp = 1' }
$ini | Set-Content -Encoding ascii "$Dest\build\win\kh2recomp.ini"
if (Test-Path build\win\kh2recomp-windows.ini) { Copy-Item build\win\kh2recomp-windows.ini "$Dest\build\win\" }
if (Test-Path docs\playing.md) { Copy-Item docs\playing.md "$Dest\PLAYING.md" }

# Game files (all but the HD index, which is made below), saves included.
"Copying the game files ..."
robocopy game "$Dest\game" /E /XD "$root\game\files\hd" /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying game\ failed (robocopy $LASTEXITCODE)" }
if (-not $NoSaveCopies) {
    # Players 2 and 3 start with a copy of your memory card (their own from then on).
    foreach ($p in 'mc0-p2', 'mc0-p3') {
        $d = "$Dest\game\$p"
        if (-not (Test-Path $d) -or -not (Get-ChildItem $d -Recurse -File -ErrorAction SilentlyContinue)) {
            robocopy game\mc0 $d /E /NFL /NDL /NJH /NJS /NP | Out-Null
        }
    }
}

"Copying the disc image ({0:N1} GB) ..." -f ($iso.Length / 1e9)
Copy-Item $iso.FullName "$Dest\Src\"

if (-not $NoHd) {
    "Copying the HD textures out of the PC release (about 5 GB) ..."
    & python tools\hd_portable.py game\files\hd "$Dest\game\files\hd"
    if ($LASTEXITCODE -ne 0) { throw 'Copying the HD textures failed.' }
} else {
    # Without them: no index, so the copy starts with the PS2 textures.
    $iniPath = "$Dest\build\win\kh2recomp.ini"
    (Get-Content $iniPath | Where-Object { $_ -notmatch '^\s*hd-textures' }) | Set-Content -Encoding ascii $iniPath
}

@"
Kingdom Hearts II Final Mix (native) - portable copy

Start: double-click "Play KH2.bat" (Windows 10/11, a graphics card with
Vulkan 1.3). Settings: F1 in the game (or Select+Start on a controller).

Local multiplayer (one PC, a window per player):
 1. Plug in a controller per player before starting.
 2. Start the game, press F1 -> Local players -> Add player 2 (and 3).
    Or start it with: "Play KH2.bat" --local-players=2   (or 3)
 3. Each window is one player: player N uses the Nth controller (each
    window's menu can change it); the keyboard goes to the active window.
 4. Arrange windows (F1 -> Local players) tiles them; each remembers where
    you put it. Turn up the sound of whichever window should play it.
 5. Player 1 runs the story; players 2 and 3 load a save (they start with a
    copy of player 1's) or start a new game, and join player 1's story.
More: PLAYING.md (Co-op and Local multiplayer).
"@ | Set-Content -Encoding ascii "$Dest\README.txt"
"Done: $Dest"
