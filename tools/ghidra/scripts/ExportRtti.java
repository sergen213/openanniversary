// Recover MSVC (x86) RTTI by scanning memory directly (independent of Ghidra's RTTI analyzer):
// TypeDescriptor (".?AV"/".?AU" names) -> CompleteObjectLocator -> vftable (slot -1 = COL).
// Writes rtti.tsv: vftable, class, mangled, col, offset, cd_offset, nslots, bases, slots.
//   offset  = offset of this vfptr inside the complete object (non-zero = secondary base vtable)
//   bases   = ClassHierarchyDescriptor base list (excluding the class itself), ';'-separated
//   slots   = virtual function addresses, ';'-separated
// Arg 0: output directory. Optional arg 1 "apply": label vftables as <Class>::vftable[_off],
// plate-comment them and create functions at slot targets Ghidra missed (needs a writable program).
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import mdemangler.MDMangGhidra;

public class ExportRtti extends GhidraScript {
    private Memory mem;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args.length > 0 ? args[0] : ".");
        boolean apply = args.length > 1 && args[1].equals("apply");
        dir.mkdirs();
        mem = currentProgram.getMemory();

        // 1. TypeDescriptors: {pVFTable(type_info), spare, char name[]}; name starts ".?A".
        Map<Long, String> td = new HashMap<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b.isExecute()) {
                continue;
            }
            Address a = b.getStart();
            while (a != null && a.compareTo(b.getEnd()) < 0) {
                monitor.checkCancelled();
                Address hit = mem.findBytes(a, b.getEnd(), ".?A".getBytes(), null, true, monitor);
                if (hit == null) {
                    break;
                }
                String name = cstr(hit, 512);
                if (name != null && name.endsWith("@@")) {
                    td.put(hit.getOffset() - 8, name);
                }
                a = hit.add(3);
            }
        }

        // 2. COLs and 3. vftables: scan all initialized, non-executable dwords.
        Map<Long, long[]> col = new HashMap<>(); // col -> {offset, cdOffset, td, chd}
        List<long[]> dwords = new ArrayList<>(); // (address, value) for the vftable pass
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b.isExecute()) {
                continue;
            }
            byte[] data = new byte[(int) b.getSize()];
            b.getBytes(b.getStart(), data);
            long base = b.getStart().getOffset();
            for (int i = 0; i + 20 <= data.length; i += 4) {
                long v0 = u32(data, i);
                long v3 = u32(data, i + 12);
                if (v0 == 0 && td.containsKey(v3)) {
                    col.put(base + i, new long[] { u32(data, i + 4), u32(data, i + 8), v3, u32(data, i + 16) });
                }
            }
            for (int i = 0; i + 4 <= data.length; i += 4) {
                dwords.add(new long[] { base + i, u32(data, i) });
            }
        }

        TreeMap<Long, Long> vft = new TreeMap<>(); // vftable -> col
        for (long[] d : dwords) {
            if (col.containsKey(d[1]) && isCode(d[0] + 4)) {
                vft.put(d[0] + 4, d[1]);
            }
        }

        File out = new File(dir, "rtti.tsv");
        int labeled = 0, created = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("vftable\tclass\tmangled\tcol\toffset\tcd_offset\tnslots\tbases\tslots");
            for (Map.Entry<Long, Long> e : vft.entrySet()) {
                monitor.checkCancelled();
                long v = e.getKey();
                long[] c = col.get(e.getValue());
                String mangled = td.get(c[2]);
                String cls = demangle(mangled);
                List<String> slots = new ArrayList<>();
                for (long s = v;; s += 4) {
                    if (s != v && (vft.containsKey(s + 4) || col.containsKey(s))) {
                        break; // next vftable's COL pointer starts here
                    }
                    Long p = dword(s);
                    if (p == null || !isCode(s)) {
                        break;
                    }
                    slots.add(String.format("%08x", p));
                }
                w.printf("%08x\t%s\t%s\t%08x\t%d\t%d\t%d\t%s\t%s%n", v, cls, mangled, e.getValue(), c[0], c[1],
                    slots.size(), String.join(";", bases(c[3])), String.join(";", slots));
                if (apply) {
                    labeled += label(v, cls, c[0]);
                    for (String sl : slots) {
                        Address fa = toAddr(Long.parseLong(sl, 16));
                        if (getFunctionContaining(fa) == null) {
                            if (getInstructionAt(fa) == null) {
                                new DisassembleCommand(fa, null, true).applyTo(currentProgram, monitor);
                            }
                            if (createFunction(fa, null) != null) {
                                created++;
                            }
                        }
                    }
                }
            }
        }
        println("ExportRtti: " + td.size() + " type descriptors, " + col.size() + " COLs, " + vft.size() +
            " vftables -> " + out + (apply ? " (" + labeled + " labels, " + created + " slot functions created)" : ""));
    }

    private int label(long v, String cls, long offset) throws Exception {
        Address a = toAddr(v);
        String name = offset == 0 ? "vftable" : "vftable_" + offset;
        Namespace ns = currentProgram.getSymbolTable().getOrCreateNameSpace(currentProgram.getGlobalNamespace(),
            cls.replace("::", "__").replace(' ', '_'), SourceType.ANALYSIS);
        currentProgram.getSymbolTable().createLabel(a, name, ns, SourceType.ANALYSIS).setPrimary();
        currentProgram.getListing().setComment(a, CodeUnit.PLATE_COMMENT, "RTTI vftable " + cls +
            (offset == 0 ? "" : " (vfptr at +" + offset + ")"));
        return 1;
    }

    private List<String> bases(long chd) throws Exception {
        List<String> r = new ArrayList<>();
        Long n = dword(chd + 8), arr = dword(chd + 12);
        if (n == null || arr == null || n > 64) {
            return r;
        }
        for (int i = 1; i < n; i++) {
            Long bcd = dword(arr + 4L * i);
            Long tdp = bcd == null ? null : dword(bcd);
            String nm = tdp == null ? null : cstr(toAddr(tdp + 8), 512);
            r.add(nm == null ? "?" : demangle(nm));
        }
        return r;
    }

    /** Pointer at addr targets an executable block. */
    private boolean isCode(long addr) throws Exception {
        Long p = dword(addr);
        if (p == null) {
            return false;
        }
        MemoryBlock b = mem.getBlock(toAddr(p));
        return b != null && b.isExecute();
    }

    private Long dword(long addr) {
        try {
            return mem.getInt(toAddr(addr)) & 0xffffffffL;
        }
        catch (Exception e) {
            return null;
        }
    }

    private String cstr(Address a, int max) {
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < max; i++) {
                byte c = mem.getByte(a.add(i));
                if (c == 0) {
                    return sb.toString();
                }
                if (c < 0x20 || c > 0x7e) {
                    return null;
                }
                sb.append((char) c);
            }
        }
        catch (Exception e) {
            return null;
        }
        return null;
    }

    static String demangle(String mangled) {
        try {
            MDMangGhidra md = new MDMangGhidra();
            md.setMangledSymbol(mangled.substring(1)); // drop leading '.'
            md.setErrorOnRemainingChars(false);
            String s = md.demangleType().toString().trim();
            for (String p : new String[] { "class ", "struct ", "union " }) {
                if (s.startsWith(p)) {
                    s = s.substring(p.length());
                }
            }
            if (!s.isEmpty()) {
                return s;
            }
        }
        catch (Exception e) {
            // fall through
        }
        return mangled;
    }

    static long u32(byte[] d, int i) {
        return (d[i] & 0xffL) | (d[i + 1] & 0xffL) << 8 | (d[i + 2] & 0xffL) << 16 | (d[i + 3] & 0xffL) << 24;
    }
}
