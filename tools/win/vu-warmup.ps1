# Record the VU1 programs of the first room of every world, unattended, so the
# next build compiles them in (3D at full speed from the first play; see
# docs/playing.md "Speed"). Runs a few headless sessions at once, each warping
# from a new game into one world; takes about 5-10 minutes. Recordings are
# game code and stay in build\vu-profile (never committed).
#   tools\win\vu-warmup.ps1 [-Parallel 4]
param([int]$Parallel = 4, [int]$KillAfter = 240)
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent | Split-Path -Parent)
$exe = 'build\win\kh2recomp.exe'
if (-not (Test-Path $exe)) { Write-Host 'Build the game first.' -ForegroundColor Red; exit 1 }
$iso = Get-ChildItem Src -Filter *.iso -ErrorAction SilentlyContinue | Select-Object -First 1
$isoPath = if ($iso) { $iso.FullName } elseif ($env:KH2_ISO) { $env:KH2_ISO } else { $null }
if (-not $isoPath) { Write-Host 'No disc image in Src\.' -ForegroundColor Red; exit 1 }
New-Item -ItemType Directory -Force logs, build\vu-profile | Out-Null
$profileDir = (Resolve-Path build\vu-profile).Path
$work = Join-Path (Resolve-Path build).Path 'vu-warmup'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
function Count-Entries { (Get-Content "$profileDir\entries.txt" -ErrorAction SilentlyContinue | Sort-Object -Unique | Measure-Object -Line).Lines }
$before = Count-Entries

# Twilight Town, Hollow Bastion, Beast's Castle, Olympus Coliseum, Agrabah, the
# Land of Dragons, 100 Acre Wood, the Pride Lands, Disney Castle, Timeless
# River, Halloween Town, Port Royal, Space Paranoids, The World That Never Was.
$worlds = @('02', '04', '05', '06', '07', '08', '09', '0A', '0C', '0D', '0E', '10', '11', '12')
$queue = [System.Collections.Generic.Queue[string]]::new([string[]]$worlds)
$running = @()
$done = 0
while ($queue.Count -or $running.Count) {
    while ($queue.Count -and $running.Count -lt $Parallel) {
        $w = $queue.Dequeue()
        $dir = Join-Path $work $w
        New-Item -ItemType Directory -Force $dir | Out-Null
        $env:PS2_VU_PROGRAM_PROFILE = $dir
        $env:KH2_VU_HOT = '0' # record only; the build compiles them
        $a = @('game\SLPM_666.75', "`"--iso=$isoPath`"", '--cd-root=game', '--headless', '--no-settings', '--renderer=gpu',
            '--frames=3600', "--timeout=$KillAfter", '--mute', '--skip-movies', '--skip-cutscenes', '--mash=CIRCLE:20',
            '--mash-until=2200', "--warp-test=2300:$($w):00", '--no-upnp')
        $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru -NoNewWindow `
            -RedirectStandardOutput "logs\vu-warmup-$w.log" -RedirectStandardError "logs\vu-warmup-$w.err.log"
        $null = $p.Handle
        $running += [pscustomobject]@{ World = $w; Proc = $p; Start = Get-Date }
    }
    Start-Sleep -Milliseconds 500
    foreach ($r in @($running)) {
        if ($r.Proc.HasExited -or ((Get-Date) - $r.Start).TotalSeconds -gt $KillAfter + 20) {
            if (-not $r.Proc.HasExited) { $r.Proc.Kill() }
            $running = @($running | Where-Object { $_ -ne $r })
            $done++
            Write-Progress -Activity 'Recording 3D programs' -Status "$done of $($worlds.Count) worlds" -PercentComplete (100 * $done / $worlds.Count)
        }
    }
}
Write-Progress -Activity 'Recording 3D programs' -Completed
$env:PS2_VU_PROGRAM_PROFILE = ''
$env:KH2_VU_HOT = ''

# Merge: microcode images by name, entries without repeats.
$lines = @(Get-Content "$profileDir\entries.txt" -ErrorAction SilentlyContinue)
foreach ($d in Get-ChildItem $work -Directory) {
    Get-ChildItem $d.FullName -Filter *.code | ForEach-Object {
        $to = Join-Path $profileDir $_.Name
        if (-not (Test-Path $to)) { Copy-Item $_.FullName $to }
    }
    $lines += @(Get-Content (Join-Path $d.FullName 'entries.txt') -ErrorAction SilentlyContinue)
}
$unique = $lines | Where-Object { $_ } | Select-Object -Unique
if ($unique.Count -ne $before) { $unique | Set-Content -Encoding ascii "$profileDir\entries.txt" }
Remove-Item -Recurse -Force $work
$after = Count-Entries
"3D programs recorded: $after ($($after - $before) new)."
exit 0
