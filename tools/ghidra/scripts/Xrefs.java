// Print references to targets, for quick headless investigation.
// Arg 0: output file. Remaining args: hex address, symbol/function/import name, or "str:<substring>"
// (every defined string containing the substring). Output TSV: target, from, ref type, containing function.
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class Xrefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter w = new PrintWriter(new File(args[0]), "UTF-8")) {
            w.println("target\tfrom\ttype\tfunction");
            for (int i = 1; i < args.length; i++) {
                for (Address t : targets(args[i])) {
                    List<Address> chain = new ArrayList<>(List.of(t));
                    // Imports are reached through IAT slots / thunks: follow one level.
                    for (Reference r : getReferencesTo(t)) {
                        if (getFunctionContaining(r.getFromAddress()) == null && t.isExternalAddress()) {
                            chain.add(r.getFromAddress());
                        }
                    }
                    Function ext = getFunctionAt(t);
                    if (ext != null && ext.getFunctionThunkAddresses(true) != null) {
                        chain.addAll(List.of(ext.getFunctionThunkAddresses(true)));
                    }
                    for (Address c : chain) {
                        for (Reference r : getReferencesTo(c)) {
                            Function f = getFunctionContaining(r.getFromAddress());
                            w.printf("%s(%s)\t%s\t%s\t%s%n", args[i], c, r.getFromAddress(), r.getReferenceType(),
                                f == null ? "?" : f.getName(true) + "@" + f.getEntryPoint());
                        }
                    }
                }
            }
        }
    }

    private Set<Address> targets(String s) {
        Set<Address> r = new LinkedHashSet<>(); // getSymbols() also returns the external symbol
        if (s.startsWith("str:")) {
            for (Data d : currentProgram.getListing().getDefinedData(true)) {
                if (StringDataInstance.isString(d)) {
                    Object v = d.getValue();
                    if (v != null && v.toString().contains(s.substring(4))) {
                        r.add(d.getAddress());
                    }
                }
            }
        }
        else if (s.matches("(0x)?[0-9a-fA-F]{6,8}")) {
            r.add(toAddr(Long.parseLong(s.replaceFirst("^0x", ""), 16)));
        }
        else {
            for (Symbol sym : currentProgram.getSymbolTable().getSymbols(s)) {
                r.add(sym.getAddress());
            }
            for (Symbol sym : currentProgram.getSymbolTable().getExternalSymbols(s)) {
                r.add(sym.getAddress());
            }
        }
        return r;
    }
}
