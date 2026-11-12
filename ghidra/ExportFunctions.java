import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            w.println("{\"functions\":[");
            boolean first = true;
            List<String> indirect = new ArrayList<>();
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                AddressSetView body = f.getBody();
                TreeSet<Long> calls = new TreeSet<>();
                long insns = 0;
                for (Instruction i : currentProgram.getListing().getInstructions(body, true)) {
                    insns++;
                    String m = i.getMnemonicString();
                    if (m.startsWith("bctr") || m.equals("bctrl") || m.equals("bcctr")) indirect.add(String.format("\"%08x\"", i.getAddress().getOffset()));
                    for (Reference r : i.getReferencesFrom())
                        if (r.getReferenceType().isCall() && r.getToAddress().isMemoryAddress()) calls.add(r.getToAddress().getOffset());
                }
                StringBuilder sb = new StringBuilder();
                for (long c : calls) { if (sb.length()>0) sb.append(','); sb.append(String.format("\"%08x\"", c)); }
                if (!first) w.println(","); first = false;
                w.printf("{\"entry\":\"%08x\",\"min\":\"%08x\",\"max\":\"%08x\",\"insns\":%d,\"name\":\"%s\",\"calls\":[%s]}",
                    f.getEntryPoint().getOffset(), body.getMinAddress().getOffset(), body.getMaxAddress().getOffset(), insns, f.getName(), sb);
            }
            w.println("],\n\"indirect_branches\":[" + String.join(",", indirect) + "]}");
        }
        println("EXPORTED " + out);
    }
}
