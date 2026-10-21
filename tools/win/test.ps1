# Headless test run with a hard wall-clock kill, for unattended use.
#   tools\win\test.ps1 -Name boot -KillAfter 180 -- --frames=600 --screenshot-every=60
# Output: logs\run_<Name>.log, screenshots in shots\<Name>\. Prints the log tail
# and exits with kh2recomp's exit code (or 124 if it had to be killed).
param([string]$Name = 'run', [int]$KillAfter = 300, [Parameter(ValueFromRemainingArguments)]$Rest)
New-Item -ItemType Directory -Force logs, "shots\$Name" | Out-Null
$iso = (Get-ChildItem Src -Filter *.iso | Select-Object -First 1).FullName
$log = "logs\run_$Name.log"
$argsList = @('game\SLPM_666.75', "--iso=$iso", '--cd-root=game', '--headless', '--no-settings', "--shots=shots\$Name") + @($Rest | Where-Object { $_ -ne '--' })
# Start-Process joins arguments with spaces and does not quote them.
$argsList = $argsList | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
$p = Start-Process -FilePath 'build\win\kh2recomp.exe' -ArgumentList $argsList -NoNewWindow -PassThru `
     -RedirectStandardOutput $log -RedirectStandardError "logs\run_$Name.err.log"
$null = $p.Handle
if (-not $p.WaitForExit($KillAfter * 1000)) { $p.Kill(); $p.WaitForExit(); $code = 124; "[test] killed after $KillAfter s" }
else { $code = $p.ExitCode }
"== stdout (last 40 lines)"; Get-Content $log -Tail 40
"== stderr (last 25 lines)"; Get-Content "logs\run_$Name.err.log" -Tail 25
"== screenshots: $((Get-ChildItem "shots\$Name" -Filter *.png -ErrorAction SilentlyContinue).Count)"
"== exit code $code"
exit $code
