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
            for (String arg : args) {
                Address addr = currentProgram.getAddressFactory()
                        .getDefaultAddressSpace().getAddress(arg);
                Function fn = getFunctionContaining(addr);
                if (fn == null) {
                    println("=== " + arg + ": no function");
                    continue;
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
