// Disassemble and decompile named target addresses; follow leading JMPs (Denuvo moved the bodies).
// Args: <output file> name:0xADDR [name:0xADDR ...]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompTargets extends GhidraScript {
    private DecompInterface decomp;
    private PrintWriter out;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        out = new PrintWriter(new FileWriter(args[0]));
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            String[] kv = args[i].split(":");   // name:0xADDR ('=' is an argument separator for cmd)
            handle(kv[0], toAddr(kv[1]), 0);
            out.flush();
        }
        out.close();
    }

    private void handle(String name, Address a, int depth) throws Exception {
        disassemble(a);
        Instruction ins = getInstructionAt(a);
        out.println("==== " + name + " @ " + a + (ins == null ? "  (no instruction)" : ""));
        if (ins == null) return;
        Instruction it = ins;
        for (int k = 0; k < 40 && it != null; k++) {
            out.println("  " + it.getAddress() + "  " + it);
            if (it.getFlowType().isTerminal() || it.getFlowType().isJump()) break;
            it = it.getNext();
        }
        if (ins.getMnemonicString().equalsIgnoreCase("JMP") && depth < 4) {
            Address[] flows = ins.getFlows();
            if (flows.length > 0) { handle(name + " -> jmp", flows[0], depth + 1); return; }
        }
        Function f = getFunctionAt(a);
        if (f == null) f = createFunction(a, name.replaceAll("[^A-Za-z0-9_]", "_"));
        if (f == null) { out.println("  (could not create a function here)"); return; }
        DecompileResults r = decomp.decompileFunction(f, 300, monitor);
        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "  decompile failed: " + r.getErrorMessage());
    }
}
