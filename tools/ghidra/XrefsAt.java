// Print references to each supplied address in the current Ghidra program.
// @category TaikoRecomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;

public class XrefsAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String argument : getScriptArgs()) {
            Address address = toAddr(Long.parseUnsignedLong(argument, 16));
            println("=== xrefs to " + address + " ===");
            for (Reference reference : getReferencesTo(address)) {
                println(reference.getFromAddress() + " " +
                        reference.getReferenceType() + " " +
                        reference.getSource());
            }
        }
    }
}
