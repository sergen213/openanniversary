// Export defined strings with cross references as TSV:
// address, type, xref_count, referencing functions (name@entry;...), ref sites, escaped value.
// Arg 0: output directory. Writes strings.tsv.
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class ExportStrings extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File dir = new File(getScriptArgs().length > 0 ? getScriptArgs()[0] : ".");
        dir.mkdirs();
        File out = new File(dir, "strings.tsv");
        ReferenceManager rm = currentProgram.getReferenceManager();
        int n = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("address\ttype\txrefs\tfunctions\tref_sites\tvalue");
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext()) {
                monitor.checkCancelled();
                Data d = it.next();
                if (!d.hasStringValue()) {
                    continue;
                }
                Object v = d.getValue();
                String s = v == null ? "" : v.toString();
                Set<String> funcs = new LinkedHashSet<>();
                Set<String> sites = new LinkedHashSet<>();
                int count = 0;
                for (Reference r : rm.getReferencesTo(d.getAddress())) {
                    count++;
                    sites.add(r.getFromAddress().toString());
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) {
                        funcs.add(f.getName(true) + "@" + f.getEntryPoint());
                    }
                }
                w.printf("%s\t%s\t%d\t%s\t%s\t%s%n", d.getAddress(), d.getDataType().getName(),
                    count, String.join(";", funcs), String.join(";", sites), escape(s));
                n++;
            }
        }
        println("ExportStrings: " + n + " strings -> " + out);
    }

    static String escape(String s) {
        StringBuilder b = new StringBuilder(s.length());
        for (char c : s.toCharArray()) {
            switch (c) {
                case '\\': b.append("\\\\"); break;
                case '\t': b.append("\\t"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                default:
                    if (c < 0x20) {
                        b.append(String.format("\\x%02x", (int) c));
                    }
                    else {
                        b.append(c);
                    }
            }
        }
        return b.toString();
    }
}
