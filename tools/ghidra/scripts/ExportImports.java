// Export imported (external) symbols with cross references as TSV:
// library, name, IAT slot(s), thunk(s), call/ref count, referencing functions (name@entry;...).
// Arg 0: output directory. Writes imports.tsv.
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ExportImports extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File dir = new File(getScriptArgs().length > 0 ? getScriptArgs()[0] : ".");
        dir.mkdirs();
        File out = new File(dir, "imports.tsv");
        ReferenceManager rm = currentProgram.getReferenceManager();
        int n = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("library\tname\tiat\tthunks\trefs\tfunctions");
            SymbolIterator it = currentProgram.getSymbolTable().getExternalSymbols();
            while (it.hasNext()) {
                monitor.checkCancelled();
                Symbol sym = it.next();
                Set<String> iat = new LinkedHashSet<>();
                Set<String> thunks = new LinkedHashSet<>();
                Set<String> funcs = new LinkedHashSet<>();
                // Targets whose incoming references count as uses of the import.
                List<Address> targets = new ArrayList<>();
                for (Reference r : rm.getReferencesTo(sym.getAddress())) {
                    Address from = r.getFromAddress();
                    if (!from.isMemoryAddress()) {
                        continue;
                    }
                    if (getFunctionContaining(from) == null && getInstructionAt(from) == null) {
                        iat.add(from.toString());
                        targets.add(from);
                    }
                    else {
                        addUse(funcs, from);
                    }
                }
                if (sym.getObject() instanceof Function ef) {
                    Address[] th = ef.getFunctionThunkAddresses(true);
                    if (th != null) {
                        for (Address a : th) {
                            thunks.add(a.toString());
                            targets.add(a);
                        }
                    }
                }
                int refs = 0;
                for (Address t : targets) {
                    for (Reference r : rm.getReferencesTo(t)) {
                        refs++;
                        addUse(funcs, r.getFromAddress());
                    }
                }
                w.printf("%s\t%s\t%s\t%s\t%d\t%s%n", sym.getParentNamespace().getName(), sym.getName(),
                    String.join(";", iat), String.join(";", thunks), refs, String.join(";", funcs));
                n++;
            }
        }
        println("ExportImports: " + n + " imports -> " + out);
    }

    private void addUse(Set<String> funcs, Address from) {
        Function f = getFunctionContaining(from);
        if (f != null) {
            funcs.add(f.getName(true) + "@" + f.getEntryPoint());
        }
        else {
            funcs.add("?@" + from);
        }
    }
}
