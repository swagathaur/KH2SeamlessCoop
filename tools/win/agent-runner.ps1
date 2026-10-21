# KH2 Co-op agent runner
# Runs PowerShell job scripts that appear in <project>\.agent\jobs, natively on this PC.
# For each job NAME.ps1 it writes NAME.started, NAME.out.log, NAME.err.log and NAME.exit.
# Creating NAME.cancel while a job runs kills its whole process tree (exit -2).
# Only runs while this window is open. Close the window to stop it.

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$jobs = Join-Path $root '.agent\jobs'
New-Item -ItemType Directory -Force -Path $jobs | Out-Null
$Host.UI.RawUI.WindowTitle = 'KH2 Co-op agent runner (close to stop)'
Write-Host "KH2 Co-op agent runner"
Write-Host "Project : $root"
Write-Host "Watching: $jobs"
Write-Host "Close this window at any time to stop."
Write-Host ""

while ($true) {
    $pending = Get-ChildItem -Path $jobs -Filter '*.ps1' -File |
        Where-Object { -not (Test-Path ($_.FullName -replace '\.ps1$', '.started')) } |
        Sort-Object Name
    foreach ($job in $pending) {
        $base = $job.FullName -replace '\.ps1$', ''
        (Get-Date -Format o) | Set-Content -Path "$base.started" -Encoding ASCII
        Write-Host ("[{0}] running {1}" -f (Get-Date -Format 'HH:mm:ss'), $job.Name)
        # Note: no -Wait. Start-Process -Wait also waits for detached helpers such as
        # MSVC's mspdbsrv.exe, which outlive builds and would hang the runner.
        $p = Start-Process -FilePath 'powershell.exe' `
            -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($job.FullName)`"") `
            -WorkingDirectory $root -NoNewWindow -PassThru `
            -RedirectStandardOutput "$base.out.log" -RedirectStandardError "$base.err.log"
        $null = $p.Handle   # keep a handle so ExitCode is available after exit
        $cancelled = $false
        while (-not $p.WaitForExit(1000)) {
            if (Test-Path "$base.cancel") {
                & taskkill.exe /PID $p.Id /T /F | Out-Null
                $p.WaitForExit()
                $cancelled = $true
                break
            }
        }
        $code = if ($cancelled) { -2 } else { $p.ExitCode }
        "$code" | Set-Content -Path "$base.exit" -Encoding ASCII
        Write-Host ("[{0}] finished {1} (exit {2})" -f (Get-Date -Format 'HH:mm:ss'), $job.Name, $code)
    }
    Start-Sleep -Milliseconds 750
}
