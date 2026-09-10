// Print direct references to each address, including one extra hop through
// function descriptors and vtable/data slots. Useful for PS3 ELF code where
// virtual calls reference an .opd descriptor rather than the code entry.
//
//   analyzeHeadless <project-dir> <project-name> -process <program> -noanalysis \
//       -scriptPath tools/ghidra -postScript XrefsAt.java 0x000B309C
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.util.HashSet;
import java.util.Set;

public class XrefsAt extends GhidraScript {
    private void printReferences(Address target, String indent, Set<Address> expanded)
            throws Exception {
        Reference[] refs = getReferencesTo(target);
        println(indent + target + " refs=" + refs.length);
        for (Reference ref : refs) {
            Address from = ref.getFromAddress();
            Function owner = getFunctionContaining(from);
            String ownerText = owner == null
                    ? "<data>"
                    : owner.getName() + " @ " + owner.getEntryPoint();
            println(indent + "  " + from + " " + ref.getReferenceType() + " " + ownerText);

            // A data reference is commonly the descriptor or vtable slot that
            // actual callers reference. Expand it once so those owners show up.
            if (owner == null && expanded.add(from)) {
                printReferences(from, indent + "    ", expanded);
            }
        }
    }

    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address addr = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(arg);
            Function fn = getFunctionContaining(addr);
            println("=== " + arg + (fn == null
                    ? ""
                    : " -> " + fn.getName() + " @ " + fn.getEntryPoint()));
            HashSet<Address> expanded = new HashSet<>();
            expanded.add(addr);
            printReferences(addr, "", expanded);
        }
    }
}
