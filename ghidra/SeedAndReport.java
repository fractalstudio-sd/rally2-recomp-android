import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class SeedAndReport extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();   // seeds_calls seeds_bctr run.pc.txt
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        int created = 0, tried = 0;
        for (int k = 0; k < 2; k++) {
            for (String l : Files.readAllLines(Paths.get(a[k]))) {
                l = l.trim(); if (l.isEmpty()) continue;
                long v = Long.parseLong(l, 16);
                Address ad = sp.getAddress(v);
                tried++;
                disassemble(ad);
                if (getFunctionAt(ad) == null && createFunction(ad, null) != null) created++;
            }
        }
        println("SEED tried=" + tried + " created=" + created);
        analyzeChanges(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        println("FUNCTIONS_TOTAL=" + fm.getFunctionCount());
        long inFn = 0, total = 0;
        for (String l : (a[2].equals("/dev/null") ? new ArrayList<String>() : Files.readAllLines(Paths.get(a[2])))) {
            String[] p = l.split(" ");
            if (!p[1].equals("R")) continue;
            long v = Long.parseLong(p[0], 16);
            total++;
            if (fm.getFunctionContaining(sp.getAddress(v)) != null) inFn++;
        }
        println("EXECUTED_PCS=" + total + " IN_FUNCTIONS=" + inFn);
        long insn = 0;
        for (Instruction i : currentProgram.getListing().getInstructions(true)) insn++;
        println("INSTRUCTIONS_DISASSEMBLED=" + insn);
    }
}
