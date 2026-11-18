// Decompile functions to a text file. Headless usage (see tools/decompile.ps1):
//   analyzeHeadless <proj> Gangstar -process libGangster2.so -noanalysis -readOnly
//       -scriptPath tools/ghidra -postScript DecompileFunctions.java <out.c> <names.txt>
// names.txt: one query per line: a full C++ name ("AnalogStick::processTouch"),
// a plain symbol name ("appOnTouch"), or an address ("@0x2adeb1"). All overloads match.
//@category Gangstar
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

public class DecompileFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        List<String> queries = Files.readAllLines(new File(args[1]).toPath());
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter pw = new PrintWriter(out, "UTF-8")) {
            for (String q : queries) {
                q = q.trim();
                if (q.isEmpty() || q.startsWith("#")) continue;
                List<Function> hits = new ArrayList<>();
                if (q.startsWith("@")) {
                    Address a = toAddr(Long.decode(q.substring(1)) & ~1L);
                    Function f = getFunctionContaining(a);
                    if (f != null) hits.add(f);
                } else {
                    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                        if (f.getName(true).equals(q) || f.getName().equals(q)) { hits.add(f); continue; }
                        for (Symbol s : currentProgram.getSymbolTable().getSymbols(f.getEntryPoint())) {
                            if (s.getName().equals(q)) { hits.add(f); break; }
                        }
                    }
                }
                if (hits.isEmpty()) {
                    pw.println("// ==== " + q + ": NOT FOUND\n");
                    continue;
                }
                for (Function f : hits) {
                    DecompileResults r = di.decompileFunction(f, 120, monitor);
                    pw.println("// ==== " + f.getName(true) + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses() + " bytes)");
                    pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                    pw.println();
                }
            }
        }
        println("wrote " + out.getAbsolutePath());
    }
}
