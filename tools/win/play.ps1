# Start the game for playing (used by "Play KH2.bat"). Output goes to
# logs\play.log (the previous session's is kept as logs\play.prev.log); if the
# game crashes, crashes\ has a report. Extra arguments pass through.
Set-Location (Split-Path $PSScriptRoot -Parent | Split-Path -Parent)
New-Item -ItemType Directory -Force logs | Out-Null
if (Test-Path logs\play.log) { Move-Item -Force logs\play.log logs\play.prev.log }
if (Test-Path logs\play.err.log) { Move-Item -Force logs\play.err.log logs\play.prev.err.log }

# The PC release (KINGDOM HEARTS HD 1.5+2.5 ReMIX, Steam or Epic), for the
# English text and the HD textures: game\pc-release.txt may name its
# "Image\dt" folder; otherwise the usual install places are searched.
function Find-PCRelease {
    if (Test-Path game\pc-release.txt) {
        $p = (Get-Content game\pc-release.txt -TotalCount 1).Trim()
        if ($p -and (Test-Path (Join-Path $p 'kh2_first.hed'))) { return $p }
    }
    $candidates = @()
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steam) {
            $libs = @($steam)
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path $vdf) {
                foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $libs += $m.Groups[1].Value -replace '\\\\', '\'
                }
            }
            foreach ($lib in $libs) {
                $candidates += Join-Path $lib 'steamapps\common\KINGDOM HEARTS -HD 1.5+2.5 ReMIX-\Image\dt'
            }
        }
    } catch { }
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($root) { $candidates += Join-Path $root 'Epic Games\KH_1.5_2.5\Image\dt' }
    }
    foreach ($d in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        $candidates += Join-Path $d.Root 'Epic Games\KH_1.5_2.5\Image\dt'
        $candidates += Join-Path $d.Root 'SteamLibrary\steamapps\common\KINGDOM HEARTS -HD 1.5+2.5 ReMIX-\Image\dt'
    }
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'kh2_first.hed')) { return $c }
        if (Test-Path (Join-Path $c 'backup\kh2_first.hed')) { return $c }
    }
    return $null
}

$pc = Find-PCRelease
$python = Get-Command python -ErrorAction SilentlyContinue
if ($pc -and $python) {
    # English text: extracted once.
    if (-not (Test-Path game\files\english\msg)) {
        "Extracting the English text from $pc (once) ..."
        & python tools\egs_extract.py $pc game\files\english --english | Select-Object -Last 1
        & python tools\egs_extract.py $pc game\files\english --english-images | Select-Object -Last 1
    }
    # HD texture index: built once, and again when the tools find more.
    $format = 0
    if (Test-Path game\files\hd\manifest.txt) {
        $first = Get-Content game\files\hd\manifest.txt -TotalCount 1
        if ($first -match '^#format (\d+)') { $format = [int]$Matches[1] }
    }
    $want = 0
    $line = Select-String -Path tools\egs_hd_extract.py -Pattern '^MANIFEST_FORMAT = (\d+)' | Select-Object -First 1
    if ($line) { $want = [int]$line.Matches[0].Groups[1].Value }
    if ($format -lt $want) {
        "Indexing the HD textures in $pc (a minute or two, once) ..."
        & python tools\egs_hd_extract.py $pc game\files\hd | Select-Object -Last 1
    }
} elseif ($pc) {
    "Found the PC release at $pc, but not Python: install Python 3 to use its English text and HD textures."
}

# First run: starting settings (the in-game menu, F1, changes and saves them).
if (-not (Test-Path build\win\kh2recomp.ini)) {
    $ini = "ssaa = 4`r`npatch = widescreen`r`n"
    if (Test-Path game\files\english\msg) { $ini += "english = 1`r`n" }
    if (Test-Path game\files\hd\manifest.txt) { $ini += "hd-textures = 1`r`n" }
    $ini | Set-Content -NoNewline -Encoding ascii build\win\kh2recomp.ini
}
$iso = Get-ChildItem Src -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
$isoPath = if ($iso) { $iso.FullName } elseif ($env:KH2_ISO) { $env:KH2_ISO } else { $null } # KH2_ISO: a disc image elsewhere
if (-not $isoPath -or -not (Test-Path $isoPath)) { "No disc image: put it in Src\ (or set KH2_ISO)."; Read-Host "Press Enter to close"; exit 1 }
$argv = @('game\SLPM_666.75', "`"--iso=$isoPath`"", '--cd-root=game') + $args
"Kingdom Hearts II FM (native). This window closes when the game does; log: logs\play.log"
$p = Start-Process -FilePath 'build\win\kh2recomp.exe' -ArgumentList $argv -PassThru -NoNewWindow -Wait `
    -RedirectStandardOutput logs\play.log -RedirectStandardError logs\play.err.log
$code = $p.ExitCode
if ($code -ne 0) {
    "The game stopped with code $code."
    Get-Content logs\play.err.log -Tail 15
    $crash = Get-ChildItem crashes\*.txt -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($crash -and $crash.LastWriteTime -gt (Get-Date).AddMinutes(-2)) { "Crash report: $($crash.FullName)" }
    Read-Host "Press Enter to close"
}
exit $code
