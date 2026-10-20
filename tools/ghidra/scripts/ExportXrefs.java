// Dump incoming references to explicit addresses, reusing the already-analyzed program (-process -noanalysis):
//   1) ReferenceManager.getReferencesTo - whatever Ghidra recovered, and
//   2) a little-endian dword scan of every initialized non-executable block for the target value, which catches pointer
//      tables and immediate operands (tra.exe keeps its strings in .rsrc next to code, where Ghidra leaves many uses
//      without a reference).
// Writes xrefs.tsv: target kind site instruction function. Arg 0: output directory; remaining args: hex addresses.
// @category OpenAnniversary
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class ExportXrefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args.length > 0 ? args[0] : ".");
        dir.mkdirs();
        PrintWriter tsv = new PrintWriter(new File(dir, "xrefs.tsv"));
        tsv.println("target\tkind\tsite\tinstruction\tfunction");

        for (int i = 1; i < args.length; ++i) {
            long v = Long.parseUnsignedLong(args[i].replaceAll("^0[xX]", ""), 16);
            Address a = toAddr(v);
            int recovered = 0, stored = 0;
            for (Reference r : currentProgram.getReferenceManager().getReferencesTo(a)) {
                CodeUnit cu = currentProgram.getListing().getCodeUnitAt(r.getFromAddress());
                Function f = getFunctionContaining(r.getFromAddress());
                tsv.println(hex(v) + "\tref\t" + hex(r.getFromAddress().getOffset()) + "\t"
                        + (cu == null ? "?" : cu.toString()) + "\t" + (f == null ? "" : f.getName()));
                ++recovered;
            }
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                monitor.checkCancelled();
                // code blocks too: tra.exe reaches its strings through disp32 operands Ghidra never turned into references
                if (!b.isInitialized()) {
                    continue;
                }
                byte[] data = new byte[(int) b.getSize()];
                b.getBytes(b.getStart(), data);
                long base = b.getStart().getOffset();
                for (int k = 0; k + 4 <= data.length; k += 4) {
                    if (u32(data, k) == v) {
                        tsv.println(hex(v) + "\tvalue\t" + hex(base + k) + "\t\t");
                        ++stored;
                    }
                }
            }
            println("target " + hex(v) + ": " + recovered + " reference(s), " + stored + " stored dword(s)");
        }
        tsv.close();
    }

    private static String hex(long v) { return String.format("%08x", v); }

    private static long u32(byte[] d, int i) {
        return (d[i] & 0xffL) | (d[i + 1] & 0xffL) << 8 | (d[i + 2] & 0xffL) << 16 | (d[i + 3] & 0xffL) << 24;
    }
}
