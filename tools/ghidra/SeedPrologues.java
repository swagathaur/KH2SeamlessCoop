// Create functions at stack-frame prologues Ghidra's reference-following missed.
// Overlay entry points are reached only through pointers held by the main
// executable, so auto-analysis cannot discover them. This scans the code
// region (start of .text up to the last function Ghidra did find) for
// `addiu $sp, $sp, -N` words that sit outside every function, plus the
// `j target` trampoline at +0x14, creates functions there and re-runs analysis
// until nothing new appears.
// @category KH2Coop
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.MemoryBlock;

public class SeedPrologues extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        FunctionManager fm = currentProgram.getFunctionManager();
        Address base = text.getStart();
        int total = 0;
        for (int pass = 0; pass < 8; pass++) {
            long codeEnd = base.getOffset();
            for (Function f : fm.getFunctions(text.getStart(), true)) {
                if (!text.contains(f.getEntryPoint())) break;
                codeEnd = Math.max(codeEnd, f.getBody().getMaxAddress().getOffset() + 1);
            }
            int created = 0;
            for (long off = base.getOffset(); off + 4 <= codeEnd; off += 4) {
                Address a = base.getNewAddress(off);
                if (fm.getFunctionContaining(a) != null) continue;
                int w = currentProgram.getMemory().getInt(a);
                boolean prologue = (w >>> 16) == 0x27BD && (short) (w & 0xFFFF) < 0;
                boolean trampoline = off == base.getOffset() + 0x14 && (w >>> 26) == 2;
                // Note: seeding "first word after a function" was tried and split
                // real functions wherever Ghidra's body skipped blocks; only
                // unambiguous prologues are used.
                if (!prologue && !trampoline) continue;
                disassemble(a);
                if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) created++;
            }
            analyzeChanges(currentProgram);
            total += created;
            println("pass " + pass + ": created " + created + " functions (code end 0x"
                    + Long.toHexString(codeEnd) + ")");
            if (created == 0) break;
        }
        println("SeedPrologues: " + total + " functions added");
    }
}
