# Configure and build kh2recomp.exe into build\win (Release, MSVC + Ninja).
# Full output goes to logs\build.log (UTF-8); the last lines are printed.
# Room first: a first build writes about 4 GB (objects, precompiled headers);
# one that runs out stops with "No space left on device" deep in the log.
$root = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
$need = if (Test-Path (Join-Path $root 'build\win\kh2recomp.exe')) { 2GB } else { 6GB }
$drive = Get-PSDrive -Name (Split-Path -Qualifier $root).TrimEnd(':') -ErrorAction SilentlyContinue
if ($drive -and $drive.Free -lt $need) {
    Write-Host ("Not enough free space on {0}: ({1:N1} GB free, the build needs about {2:N0} GB). Free some space and run it again." -f $drive.Root, ($drive.Free / 1GB), ($need / 1GB)) -ForegroundColor Red
    exit 1
}
. "$PSScriptRoot\vsenv.ps1"
New-Item -ItemType Directory -Force logs | Out-Null
cmake -S . -B build\win -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | Select-String -Pattern 'kh2:|Error|error|Generating done' | Out-Host
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# Compiling: ninja prints "[done/total] ..." per step. The full output goes to
# logs\build.log; here a progress bar and a line every 5% (a first build has
# about 20,000 steps and takes a while, so it should not look stuck).
Write-Host 'Compiling (full output: logs\build.log) ...'
$writer = New-Object System.IO.StreamWriter((Join-Path (Get-Location) 'logs\build.log'), $false, [Text.UTF8Encoding]::new($false))
$lastPct = -1; $lastShown = -5; $started = Get-Date
cmake --build build\win --target kh2recomp coop_net_test key_bindings_test --parallel 2>&1 | ForEach-Object {
    $line = "$_"
    $writer.WriteLine($line)
    if ($line -match '^\[(\d+)/(\d+)\]') {
        $pct = [int](100 * [int]$Matches[1] / [math]::Max(1, [int]$Matches[2]))
        if ($pct -ne $lastPct) {
            $lastPct = $pct
            Write-Progress -Activity 'Building' -Status "$pct% ($($Matches[1]) of $($Matches[2]) steps)" -PercentComplete $pct
            if ($pct -ge $lastShown + 5 -and [int]$Matches[2] -gt 50) {
                $lastShown = $pct - ($pct % 5)
                Write-Host ("  {0,3}%  ({1} of {2} steps, {3:N0} min so far)" -f $pct, $Matches[1], $Matches[2], ((Get-Date) - $started).TotalMinutes)
            }
        }
    }
}
$code = $LASTEXITCODE
$writer.Close()
Write-Progress -Activity 'Building' -Completed
Get-Content logs\build.log -Tail 25
exit $code
