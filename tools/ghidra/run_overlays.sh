#!/bin/bash
# Build function maps for the five KH2 overlays with headless Ghidra.
#   GHIDRA_HOME=/path/to/ghidra tools/ghidra/run_overlays.sh
# Requires the ghidra-emotionengine-reloaded extension and the overlay ELFs
# from tools/overlay_to_elf.py in build/overlays/.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${GHIDRA_HOME:?set GHIDRA_HOME}"
mkdir -p build/ghidra logs
for o in ${OVERLAYS:-title movie shop gumimenu gumibattle}; do
  "$GHIDRA_HOME/support/analyzeHeadless" build/ghidra kh2ovl -import "build/overlays/$o.elf" -overwrite \
    -processor r5900:LE:32:default -scriptPath tools/ghidra \
    -postScript SeedPrologues.java \
    -postScript ExportFunctionCsv.java "$PWD/config/functions_$o.csv" > "logs/ghidra_$o.log" 2>&1
  python3 tools/prefix_overlay_names.py "$o" "config/functions_$o.csv"
  python3 tools/clamp_function_maps.py "config/functions_$o.csv"
  python3 tools/fix_overlay_maps.py "$o"
  echo "$o: $(grep -o 'SeedPrologues: [0-9]* functions added' "logs/ghidra_$o.log"), $(($(wc -l < "config/functions_$o.csv") - 1)) total"
done
