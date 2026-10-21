# Configure and build kh2recomp.exe into build\win (Release, MSVC + Ninja).
# Full output goes to logs\build.log (UTF-8); the last lines are printed.
. "$PSScriptRoot\vsenv.ps1"
New-Item -ItemType Directory -Force logs | Out-Null
cmake -S . -B build\win -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | Select-String -Pattern 'kh2:|Error|error|Generating done' | Out-Host
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build build\win --target kh2recomp coop_net_test --parallel 2>&1 | Out-File -Encoding utf8 logs\build.log
$code = $LASTEXITCODE
Get-Content logs\build.log -Tail 25
exit $code
