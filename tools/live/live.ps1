# Live headless sessions driven by control files (main.cpp --control).
#   live.ps1 solo            one game:  scratch\ctl-solo.txt,  logs\live-solo.log,  shots\live-solo
#   live.ps1 client|p3|host   one of them again (rejoining a running session)
#   live.ps1 duo             a host and a client joined to it (ctl-host / ctl-client, live-host / live-client)
# Env LIVE_EXTRA: more arguments for every game (space-separated).
# Started without waiting: the job runner stays free. Saves: copies of game\mc0 (env LIVE_CLIENT_SAVE for the client's).
param([string]$mode = 'solo', [int]$port = 27600)
# trio: duo, and a third game (p3, ctl-p3 / live-p3) joined to the host
Set-Location "$PSScriptRoot\..\.."
$iso = (Get-ChildItem Src -Filter *.iso | Select-Object -First 1).FullName
function Start-Live([string]$name, [string]$save, [string[]]$extra) {
  $dir = "scratch\live-$name"
  Remove-Item $dir -Recurse -ErrorAction SilentlyContinue
  New-Item -ItemType Directory $dir | Out-Null
  Copy-Item game\SLPM_666.75 $dir\
  Copy-Item $save "$dir\mc0" -Recurse
  Remove-Item "shots\live-$name" -Recurse -ErrorAction SilentlyContinue
  Set-Content -Path "scratch\ctl-$name.txt" -Value '' -NoNewline
  $a = @("$dir\SLPM_666.75", "`"--iso=$iso`"", '--cd-root=game', '--headless', '--no-settings','--english','--renderer=gpu','--frames=0','--timeout=14400','--status-every=30','--mute','--speed=1','--skip-movies','--no-upnp','--watchdog=0',"--shots=shots\live-$name","--control=scratch\ctl-$name.txt") + $extra
  if ($env:LIVE_EXTRA) { $a += $env:LIVE_EXTRA -split ' ' }
  $p = Start-Process -FilePath 'build\win\kh2recomp.exe' -ArgumentList $a -PassThru -NoNewWindow -RedirectStandardOutput "logs\live-$name.log" -RedirectStandardError "logs\live-$name.err.log"
  "$name started: $($p.Id)"
}
$clientSave = if ($env:LIVE_CLIENT_SAVE) { $env:LIVE_CLIENT_SAVE } else { 'game\mc0' }
if ($mode -eq 'solo') { Start-Live 'solo' 'game\mc0' @('--name=Solo') }
elseif ($mode -eq 'client') { Start-Live 'client' $clientSave @("--join=127.0.0.1:$port", '--join-at=2700', '--name=Client') }
elseif ($mode -eq 'p3') { Start-Live 'p3' $clientSave @("--join=127.0.0.1:$port", '--join-at=2800', '--name=Third') }
elseif ($mode -eq 'host') { Start-Live 'host' 'game\mc0' @("--host=$port", '--name=Host') }
else {
  Start-Live 'host' 'game\mc0' @("--host=$port", '--name=Host')
  Start-Sleep 3
  Start-Live 'client' $clientSave @("--join=127.0.0.1:$port", '--join-at=2700', '--name=Client')
  if ($mode -eq 'trio') {
    Start-Sleep 3
    Start-Live 'p3' $clientSave @("--join=127.0.0.1:$port", '--join-at=2800', '--name=Third')
  }
}
