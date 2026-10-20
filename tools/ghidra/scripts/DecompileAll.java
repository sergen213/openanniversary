// Decompile every non-thunk, non-external function into one C file (slow: ~16k functions).
// Arg 0: output file.
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileAll extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File out = new File(getScriptArgs().length > 0 ? getScriptArgs()[0] : "decomp_all.c");
        if (out.getParentFile() != null) {
            out.getParentFile().mkdirs();
        }
        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        ifc.setOptions(opts);
        ifc.openProgram(currentProgram);
        int ok = 0, total = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                monitor.checkCancelled();
                if (f.isThunk() || f.isExternal()) {
                    continue;
                }
                total++;
                w.println("/* ==== " + f.getEntryPoint() + " " + f.getName(true) + " ==== */");
                DecompileResults r = ifc.decompileFunction(f, 60, monitor);
                if (r.decompileCompleted()) {
                    w.println(r.getDecompiledFunction().getC());
                    ok++;
                }
                else {
                    w.println("/* decompile failed: " + r.getErrorMessage() + " */\n");
                }
            }
        }
        ifc.dispose();
        println("DecompileAll: " + ok + "/" + total + " -> " + out);
    }
}
