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
cmake --build build\win --target kh2recomp coop_net_test key_bindings_test --parallel 2>&1 | Out-File -Encoding utf8 logs\build.log
$code = $LASTEXITCODE
Get-Content logs\build.log -Tail 25
exit $code
