// Decompile functions to a text file. Headless usage (see tools/decompile.ps1):
//   analyzeHeadless <proj> Gangstar -process libGangster2.so -noanalysis -readOnly
//       -scriptPath tools/ghidra -postScript DecompileFunctions.java <out.c> <names.txt>
// names.txt: one query per line:
//   AnalogStick::processTouch   full C++ name (all overloads match)
//   appOnTouch                  plain symbol name
//   @0x2adeb1                   function containing this address (Ghidra address = lib offset + 0x10000)
//   callers:NAME                every function that references/calls NAME
//   strref:TEXT                 every function referencing a string that contains TEXT
//   dataref:NAME                every function reading/writing data symbol NAME, directly or
//                               through a pointer to it (e.g. its GOT slot, as PIC code does)
//@category Gangstar
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class DecompileFunctions extends GhidraScript {
    private List<Function> byName(String q) {
        List<Function> hits = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getName(true).equals(q) || f.getName().equals(q)) { hits.add(f); continue; }
            for (Symbol s : currentProgram.getSymbolTable().getSymbols(f.getEntryPoint())) {
                if (s.getName().equals(q)) { hits.add(f); break; }
            }
        }
        return hits;
    }

    private Set<Function> referencing(Address target) {
        Set<Function> out = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(target)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) out.add(f);
        }
        return out;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        List<String> queries = Files.readAllLines(new File(args[1]).toPath());
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Set<Function> done = new LinkedHashSet<>();
        try (PrintWriter pw = new PrintWriter(out, "UTF-8")) {
            for (String q : queries) {
                q = q.replace("﻿", "").trim();
                if (q.isEmpty() || q.startsWith("#")) continue;
                List<Function> hits = new ArrayList<>();
                if (q.startsWith("@")) {
                    Function f = getFunctionContaining(toAddr(Long.decode(q.substring(1)) & ~1L));
                    if (f != null) hits.add(f);
                } else if (q.startsWith("callers:")) {
                    for (Function t : byName(q.substring(8))) hits.addAll(referencing(t.getEntryPoint()));
                } else if (q.startsWith("dataref:")) {
                    for (Symbol sym : currentProgram.getSymbolTable().getSymbols(q.substring(8))) {
                        Address a = sym.getAddress();
                        hits.addAll(referencing(a));
                        for (Reference r : getReferencesTo(a)) {          // pointers to it (GOT slots)
                            if (getFunctionContaining(r.getFromAddress()) == null)
                                hits.addAll(referencing(r.getFromAddress()));
                        }
                    }
                } else if (q.startsWith("strref:")) {
                    String text = q.substring(7);
                    DataIterator it = currentProgram.getListing().getDefinedData(true);
                    while (it.hasNext() && !monitor.isCancelled()) {
                        Data d = it.next();
                        Object v = d.getValue();
                        if (v instanceof String && ((String) v).contains(text)) {
                            pw.println("// string @ " + d.getAddress() + ": \"" + v + "\"");
                            hits.addAll(referencing(d.getAddress()));
                        }
                    }
                } else {
                    hits.addAll(byName(q));
                }
                if (hits.isEmpty()) {
                    pw.println("// ==== " + q + ": NOT FOUND\n");
                    continue;
                }
                for (Function f : hits) {
                    pw.println("// ==== [" + q + "] " + f.getName(true) + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses() + " bytes)");
                    if (!done.add(f)) { pw.println("// (already printed above)\n"); continue; }
                    DecompileResults r = di.decompileFunction(f, 180, monitor);
                    pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                    pw.println();
                }
            }
        }
        println("wrote " + out.getAbsolutePath());
    }
}
