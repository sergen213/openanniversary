// Export all functions as TSV: entry, name, size, callers, callees, thunk, signature.
// Arg 0: output directory. Writes functions.tsv.
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File dir = new File(getScriptArgs().length > 0 ? getScriptArgs()[0] : ".");
        dir.mkdirs();
        File out = new File(dir, "functions.tsv");
        int n = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("entry\tname\tsize\tcallers\tcallees\tthunk\tsignature");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                monitor.checkCancelled();
                Function f = it.next();
                w.printf("%s\t%s\t%d\t%d\t%d\t%s\t%s%n",
                    f.getEntryPoint(), f.getName(true), f.getBody().getNumAddresses(),
                    f.getCallingFunctions(monitor).size(), f.getCalledFunctions(monitor).size(),
                    f.isThunk() ? "1" : "0",
                    clean(f.getPrototypeString(false, false)));
                n++;
            }
        }
        println("ExportFunctions: " + n + " functions -> " + out);
    }

    static String clean(String s) {
        return s == null ? "" : s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }
}
