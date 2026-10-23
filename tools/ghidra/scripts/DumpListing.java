// Dump raw disassembly for address ranges: <outfile> <start> <end> [<start> <end>]...
// Addresses are hex, no 0x prefix needed.
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class DumpListing extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 4) {
            printerr("usage: DumpListing <outfile> <start> <end>...");
            return;
        }
        java.io.File f = new java.io.File(args[0]);
        PrintWriter t = new PrintWriter(f.toString(), "UTF-8");
        List<String> ranges = new ArrayList<String>();
        for (int i = 1; i + 1 < args.length; i += 2) ranges.add(args[i] + "-" + args[i + 1]);
        int count = 0;
        for (String r : ranges) {
            String[] p = r.split("-");
            long s = Long.parseLong(p[0].replaceFirst("^0x", ""), 16);
            long e = Long.parseLong(p[1].replaceFirst("^0x", ""), 16);
            for (long a = s; a <= e;) {
                Instruction in = getInstructionAt(toAddr(a));
                if (in == null) {
                    a++;
                    continue;
                }
                    String ops = "";
                    for (int i = 0; i < in.getNumOperands(); ++i) {
                        ops += in.getSeparator(i) + in.getDefaultOperandRepresentation(i);
                    }
                    t.println(in.getAddress() + ": " + in.getMnemonicString() + " " + ops);
                count++;
                a = in.getAddress().add(in.getLength()).getOffset();
            }
        }
        t.close();
        System.out.println("wrote " + count + " instructions to " + f);
    }
}
