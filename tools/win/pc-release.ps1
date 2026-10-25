# Find the PC release (KINGDOM HEARTS HD 1.5+2.5 ReMIX, Steam or Epic): its
# "Image\dt" folder, for the English text and the HD textures. Dot-source it.
# game\pc-release.txt names the folder (setup.ps1 writes it); otherwise
# Steam's library folders and the usual Epic places are searched.
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
    # Epic: its install manifests say where each game went.
    $manifests = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    foreach ($m in (Get-ChildItem $manifests -Filter *.item -ErrorAction SilentlyContinue)) {
        try {
            $j = Get-Content $m.FullName -Raw | ConvertFrom-Json
            if ($j.DisplayName -match 'KINGDOM HEARTS.*1\.5.*2\.5' -and $j.InstallLocation) {
                $candidates += Join-Path $j.InstallLocation 'Image\dt'
            }
        } catch { }
    }
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

