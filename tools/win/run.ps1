# Run kh2recomp with the project's game files. Extra arguments pass through, e.g.
#   tools\win\run.ps1 --headless --frames=600 --screenshot-every=60
$iso = Get-ChildItem Src -Filter *.iso | Select-Object -First 1
& build\win\kh2recomp.exe game\SLPM_666.75 "--iso=$($iso.FullName)" --cd-root=game @args
exit $LASTEXITCODE
