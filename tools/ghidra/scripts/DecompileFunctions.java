// Decompile selected functions, one C file per function: <outdir>/<entry>_<name>.c
// Arg 0: output directory. Remaining args: function selectors, each one of
//   hex address (0x5dfbe0 / 005dfbe0; any address inside a function works),
//   function name (exact, e.g. GAMELOOP_Process), or @listfile (one selector per line, '#' comments).
// Each function is preceded by a header comment with entry, name, signature, callers and callees.
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: DecompileFunctions <outdir> <selector|@listfile>...");
            return;
        }
        List<String> sel = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            if (args[i].startsWith("@")) {
                for (String line : Files.readAllLines(new File(args[i].substring(1)).toPath())) {
                    line = line.replaceAll("#.*", "").trim();
                    if (!line.isEmpty()) {
                        sel.add(line.split("[\\s,]+")[0]);
                    }
                }
            }
            else {
                sel.add(args[i]);
            }
        }

        Set<Function> funcs = new LinkedHashSet<>();
        for (String s : sel) {
            Function f = resolve(s);
            if (f == null) {
                printerr("DecompileFunctions: no function for '" + s + "'");
            }
            else {
                funcs.add(f);
            }
        }

        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        ifc.setOptions(opts);
        ifc.openProgram(currentProgram);
        File dir = new File(args[0]);
        dir.mkdirs();
        int ok = 0;
        for (Function f : funcs) {
            monitor.checkCancelled();
            File out = new File(dir, f.getEntryPoint() + "_" + f.getName(true).replaceAll("[^A-Za-z0-9_.-]", "_") + ".c");
            try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
                w.println("/* ==== " + f.getEntryPoint() + " " + f.getName(true) + " ====");
                w.println(" * " + f.getPrototypeString(true, false));
                w.println(" * callers: " + names(f.getCallingFunctions(monitor)));
                w.println(" * callees: " + names(f.getCalledFunctions(monitor)));
                w.println(" */");
                DecompileResults r = ifc.decompileFunction(f, 120, monitor);
                if (r.decompileCompleted()) {
                    w.println(r.getDecompiledFunction().getC());
                    ok++;
                }
                else {
                    w.println("/* decompile failed: " + r.getErrorMessage() + " */");
                }
            }
        }
        ifc.dispose();
        println("DecompileFunctions: " + ok + "/" + funcs.size() + " -> " + dir);
    }

    private Function resolve(String s) {
        if (s.matches("(0x)?[0-9a-fA-F]{6,8}")) {
            Address a = toAddr(Long.parseLong(s.replaceFirst("^0x", ""), 16));
            Function f = getFunctionAt(a);
            return f != null ? f : getFunctionContaining(a);
        }
        List<Function> fs = getGlobalFunctions(s);
        if (!fs.isEmpty()) {
            return fs.get(0);
        }
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getName(true).equals(s)) {
                return f;
            }
        }
        return null;
    }

    private static String names(Set<Function> fs) {
        List<String> r = new ArrayList<>();
        for (Function f : fs) {
            r.add(f.getName(true) + "@" + f.getEntryPoint());
        }
        return String.join(", ", r);
    }
}
