// Headless: set up the GBA memory map for Grand Theft Auto Advance, place the
// routines the game copies to IWRAM, seed functions and decompile everything
// to one C file. Import the code part of the ROM (make_seeds.py writes it)
// as raw binary at 0x08000000, ARM:LE:32:v4t.
import java.io.*;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.disassemble.ArmDisassembleCommand;

public class GtaDecompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String seedPath = args[0], outPath = args[1];
        Memory mem = currentProgram.getMemory();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        List<long[]> seeds = new ArrayList<>();
        List<long[]> copies = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(seedPath))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] p = line.trim().split(" ");
                if (p.length == 4 && p[0].equals("COPY"))
                    copies.add(new long[]{Long.parseLong(p[1], 16), Long.parseLong(p[2], 16), Long.parseLong(p[3], 16)});
                else if (p.length == 2) seeds.add(new long[]{Long.parseLong(p[0], 16), Long.parseLong(p[1])});
            }
        }
        copies.sort((x, y) -> Long.compare(x[0], y[0]));
        long at = 0x03000000L;
        int n = 0;
        for (long[] c : copies) {
            if (c[0] > at) mem.createUninitializedBlock("iwram_" + (n++), sp.getAddress(at), c[0] - at, false);
            byte[] img = new byte[(int) c[2]];
            mem.getBytes(sp.getAddress(c[1]), img);
            MemoryBlock b = mem.createInitializedBlock("iwram_code_" + (n++), sp.getAddress(c[0]),
                    new ByteArrayInputStream(img), c[2], monitor, false);
            b.setExecute(true); b.setWrite(true);
            at = c[0] + c[2];
        }
        mem.createUninitializedBlock("iwram_" + (n++), sp.getAddress(at), 0x03008000L - at, false);
        mem.createUninitializedBlock("ewram", sp.getAddress(0x02000000L), 0x40000, false);
        mem.createUninitializedBlock("io", sp.getAddress(0x04000000L), 0x400, false);
        mem.createUninitializedBlock("pal", sp.getAddress(0x05000000L), 0x400, false);
        mem.createUninitializedBlock("vram", sp.getAddress(0x06000000L), 0x18000, false);
        mem.createUninitializedBlock("oam", sp.getAddress(0x07000000L), 0x400, false);
        mem.createUninitializedBlock("romdata", sp.getAddress(0x08072000L), 0x01000000L - 0x72000L, false);

        for (long[] s : seeds)
            new ArmDisassembleCommand(sp.getAddress(s[0]), null, s[1] == 1).applyTo(currentProgram, monitor);
        int made = 0;
        for (long[] s : seeds) {
            Address a = sp.getAddress(s[0]);
            if (getFunctionAt(a) == null && getInstructionAt(a) != null && createFunction(a, null) != null) made++;
        }
        println("seeds=" + seeds.size() + " functions created=" + made);
        analyzeAll(currentProgram);

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        int ok = 0, fail = 0;
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(outPath)))) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                DecompileResults res = dec.decompileFunction(f, 60, monitor);
                w.println("// ===== " + f.getEntryPoint() + " " + f.getName() + " size=" + f.getBody().getNumAddresses());
                if (res != null && res.decompileCompleted()) { w.println(res.getDecompiledFunction().getC()); ok++; }
                else { w.println("// decompile failed"); fail++; }
            }
        }
        println("decompiled=" + ok + " failed=" + fail);
    }
}
