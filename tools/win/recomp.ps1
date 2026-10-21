# Regenerate all translated C++ (generated/) from game/ using the built ps2_recomp.
python tools\recomp.py --recompiler build\ps2recomp-tools\ps2xRecomp\ps2_recomp.exe @args
exit $LASTEXITCODE
