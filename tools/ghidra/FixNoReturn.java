// One-off project repair (Q22): Ghidra's analysis marked soft-float helpers such as __aeabi_fsub as
// non-returning, so every function was cut off after its first call to one of them. This clears the
// flag on all __aeabi_* functions (and thunks/externals named like them), disassembles the code that
// follows each call, and re-computes the bodies of the affected functions. Run once, NOT read-only:
//   analyzeHeadless analysis\ghidra Gangstar -process libGangster2.so -noanalysis
//       -scriptPath tools\ghidra -postScript FixNoReturn.java
//@category Gangstar
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;

import java.util.LinkedHashSet;
import java.util.Set;

public class FixNoReturn extends GhidraScript {
    @Override
    public void run() throws Exception {
        Set<Function> helpers = new LinkedHashSet<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getName().startsWith("__aeabi_") && f.hasNoReturn()) helpers.add(f);
        }
        for (Function f : currentProgram.getFunctionManager().getExternalFunctions()) {
            if (f.getName().startsWith("__aeabi_") && f.hasNoReturn()) helpers.add(f);
        }
        println("non-returning __aeabi_* functions: " + helpers.size());
        Set<Function> callers = new LinkedHashSet<>();
        int fixed = 0, disasm = 0;
        for (Function h : helpers) {
            h.setNoReturn(false);
            ++fixed;
            for (Reference r : getReferencesTo(h.getEntryPoint())) {
                Address from = r.getFromAddress();
                Instruction ins = getInstructionAt(from);
                if (ins == null || !ins.getFlowType().isCall()) continue;
                // the call may have been marked as a terminal call-return override: clear it
                ins.setFlowOverride(ghidra.program.model.listing.FlowOverride.NONE);
                Address next = ins.getFallThrough();
                if (next == null) next = from.add(ins.getLength());
                if (getInstructionAt(next) == null) {
                    DisassembleCommand cmd = new DisassembleCommand(next, null, true);
                    cmd.applyTo(currentProgram, monitor);
                    ++disasm;
                }
                Function caller = getFunctionContaining(from);
                if (caller != null) callers.add(caller);
            }
        }
        int bodies = 0;
        for (Function c : callers) {
            if (CreateFunctionCmd.fixupFunctionBody(currentProgram, c, monitor)) ++bodies;
        }
        println("cleared no-return on " + fixed + " helpers, disassembled " + disasm + " fall-throughs, re-computed "
                + bodies + " of " + callers.size() + " caller bodies");
    }
}
