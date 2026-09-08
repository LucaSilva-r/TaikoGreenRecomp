// Decompile the function containing each address given as a script argument.
//
//   analyzeHeadless <proj> taiko_headless -process "EBOOT GREEN.elf" -noanalysis \
//       -scriptPath tools/ghidra -postScript DecompAt.java 0x0056195C [0x...]
//
// Prints the C for each, so a guest address seen at runtime can be read as
// source instead of as lifted PPC.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("DecompAt: no addresses given");
            return;
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try {
            for (String spec : args) {
                // Optional ADDRESS:TOC for this multi-TOC PS3 image. A stale
                // default r2 can resolve unrelated strings and jump tables.
                String[] parts = spec.split(":", -1);
                if (parts.length > 2) {
                    throw new IllegalArgumentException("Expected ADDRESS[:TOC]: " + spec);
                }
                String arg = parts[0];
                Address addr = currentProgram.getAddressFactory()
                        .getDefaultAddressSpace().getAddress(arg);
                Function fn = getFunctionContaining(addr);
                if (fn == null) {
                    println("=== " + arg + ": no function");
                    continue;
                }
                if (parts.length == 2) {
                    currentProgram.getProgramContext().setValue(
                            currentProgram.getRegister("r2"), fn.getEntryPoint(),
                            fn.getBody().getMaxAddress(),
                            new java.math.BigInteger(parts[1].replaceFirst("^0[xX]", ""), 16));
                    decomp.flushCache();
                }
                println("=== " + arg + " -> " + fn.getName() + " @ " + fn.getEntryPoint());
                DecompileResults res = decomp.decompileFunction(fn, 120, monitor);
                if (res.decompileCompleted()) {
                    println(res.getDecompiledFunction().getC());
                } else {
                    println("decompile failed: " + res.getErrorMessage());
                }
            }
        } finally {
            decomp.dispose();
        }
    }
}
