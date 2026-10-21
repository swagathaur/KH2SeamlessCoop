# Start the game for playing (used by "Play KH2.bat"). Output goes to
# logs\play.log (the previous session's is kept as logs\play.prev.log); if the
# game crashes, crashes\ has a report. Extra arguments pass through.
Set-Location (Split-Path $PSScriptRoot -Parent | Split-Path -Parent)
New-Item -ItemType Directory -Force logs | Out-Null
if (Test-Path logs\play.log) { Move-Item -Force logs\play.log logs\play.prev.log }
if (Test-Path logs\play.err.log) { Move-Item -Force logs\play.err.log logs\play.prev.err.log }
# First run: starting settings (the in-game menu, F1, changes and saves them).
if (-not (Test-Path build\win\kh2recomp.ini)) {
    "ssaa = 4`r`npatch = widescreen`r`n" | Set-Content -NoNewline -Encoding ascii build\win\kh2recomp.ini
}
$iso = Get-ChildItem Src -Filter *.iso | Select-Object -First 1
$argv = @('game\SLPM_666.75', "`"--iso=$($iso.FullName)`"", '--cd-root=game') + $args
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
