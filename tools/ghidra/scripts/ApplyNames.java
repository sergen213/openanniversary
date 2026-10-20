// Apply hand-curated names from a CSV: address,name,confidence,evidence (header line, '#' comments).
// Code addresses get a function (created if missing) renamed to <name>; data addresses get a label.
// Every applied address gets a plate comment "OU name [confidence]: evidence". Idempotent.
// Arg 0: CSV path.
// @category OpenUnderworld
import java.io.File;
import java.nio.file.Files;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class ApplyNames extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File csv = new File(getScriptArgs()[0]);
        int funcs = 0, labels = 0, bad = 0;
        boolean header = true;
        for (String line : Files.readAllLines(csv.toPath())) {
            if (header) {
                header = false;
                continue;
            }
            if (line.isBlank() || line.startsWith("#")) {
                continue;
            }
            String[] f = line.split(",", 4);
            if (f.length < 4) {
                printerr("ApplyNames: bad line: " + line);
                bad++;
                continue;
            }
            Address a = toAddr(Long.parseLong(f[0].trim().replaceFirst("^0x", ""), 16));
            String name = f[1].trim();
            String note = "OU " + name + " [" + f[2].trim() + "]: " + f[3].trim().replaceAll("^\"|\"$", "");
            MemoryBlock b = currentProgram.getMemory().getBlock(a);
            if (b == null) {
                printerr("ApplyNames: unmapped " + a);
                bad++;
                continue;
            }
            if (b.isExecute()) {
                Function fn = getFunctionAt(a);
                if (fn == null) {
                    if (getInstructionAt(a) == null) {
                        new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                    }
                    fn = createFunction(a, null);
                }
                if (fn == null) {
                    printerr("ApplyNames: cannot create function at " + a);
                    bad++;
                    continue;
                }
                fn.setName(name, SourceType.USER_DEFINED);
                funcs++;
            }
            else {
                currentProgram.getSymbolTable().createLabel(a, name, SourceType.USER_DEFINED).setPrimary();
                labels++;
            }
            currentProgram.getListing().setComment(a, CodeUnit.PLATE_COMMENT, note);
        }
        println("ApplyNames: " + funcs + " functions, " + labels + " labels, " + bad + " errors from " + csv);
    }
}
