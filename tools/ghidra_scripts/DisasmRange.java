// Disassemble from a function start (following flow) and print instructions in address order within [start, start+len).
// Args: <output file> name:0xSTART:0xLEN [name:0xSTART:0xLEN ...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DisasmRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        for (int i = 1; i < args.length; i++) {
            String[] p = args[i].split(":");
            Address start = toAddr(p[1]);
            long len = Long.decode(p[2]);
            disassemble(start);
            out.println("==== " + p[0] + " @ " + start + " (+" + p[2] + ")");
            Instruction it = getInstructionAt(start);
            if (it == null) it = getInstructionAfter(start);
            while (it != null && it.getAddress().subtract(start) < len) {
                out.println("  " + it.getAddress() + "  +" + Long.toHexString(it.getAddress().subtract(start)) + "  " + it);
                it = getInstructionAfter(it.getAddress());
            }
            out.flush();
        }
        out.close();
    }
}
