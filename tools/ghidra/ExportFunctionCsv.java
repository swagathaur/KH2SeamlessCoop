// Write every function as Name,Start,End,Size (exclusive end) for ps2_recomp.
// Headless usage: -postScript ExportFunctionCsv.java <out.csv>
// @category KH2Coop
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class ExportFunctionCsv extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        int n = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            w.println("Name,Start,End,Size");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isExternal() || f.isThunk() && f.getBody().getNumAddresses() == 0) continue;
                long start = f.getEntryPoint().getOffset();
                // Use only the contiguous range holding the entry point: Ghidra
                // can attach far-away blocks to a body, which inflated one
                // function to 400 KB. clamp_function_maps.py also clamps ends
                // to the next function's start.
                AddressRange entryRange = f.getBody().getRangeContaining(f.getEntryPoint());
                long end = entryRange != null ? entryRange.getMaxAddress().getOffset() + 1 : start + 4;
                String name = f.getName().replaceAll("[^A-Za-z0-9_]", "_");
                w.printf("%s,0x%08X,0x%08X,%d%n", name, start, end, end - start);
                n++;
            }
        }
        println("Exported " + n + " functions to " + out);
    }
}
