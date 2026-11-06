// Decompiles functions to a text file.
// Headless args: <output file> <address> [address...]   (addresses as 0x hex; any address inside a function works)
// @category PS2Port

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.io.File;
import java.io.PrintWriter;

public class DecompileFunctions extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        try (PrintWriter w = new PrintWriter(out)) {
            for (int i = 1; i < args.length; i++) {
                Function f = getFunctionContaining(toAddr(Long.decode(args[i])));
                if (f == null) {
                    w.printf("// %s: no function%n%n", args[i]);
                    continue;
                }
                DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                w.printf("// ===== %s @ %s =====%n", f.getName(), f.getEntryPoint());
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                w.println();
            }
        }
        decomp.dispose();
        println("DecompileFunctions: wrote " + out.getAbsolutePath());
    }
}
