# Play (or run a headless script) while recording the VU1 microprograms the
# game uses into build\vu-profile. The next tools\win\build.ps1 compiles them to
# native code (see CMakeLists.txt), which is what makes 3D scenes run at full
# speed. Recordings add up across runs: play through new areas to cover their
# programs. They are game code and stay local.
#   tools\win\vu-record.ps1                 # play in a window
#   tools\win\vu-record.ps1 --headless --frames=20000 --speed=1 --mash=CIRCLE:40
New-Item -ItemType Directory -Force build\vu-profile | Out-Null
$env:PS2_VU_PROGRAM_PROFILE = (Resolve-Path build\vu-profile).Path
& "$PSScriptRoot\run.ps1" @args
$code = $LASTEXITCODE
$entries = (Get-Content build\vu-profile\entries.txt -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
"VU1 program entries recorded: $entries (rebuild to compile them)"
exit $code
