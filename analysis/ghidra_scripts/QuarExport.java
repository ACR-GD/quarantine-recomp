// Seeds functions + labels, then exports a function table and decompiled C for SKIPIT.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import java.io.*; import java.nio.file.*;
public class QuarExport extends GhidraScript {
  public void run() throws Exception {
    String dir = getScriptArgs()[0];
    int made = 0;
    for (String l : Files.readAllLines(Paths.get(dir, "seeds.txt"))) {
      Address a = toAddr(Long.parseLong(l.trim(), 16));
      if (getFunctionAt(a) == null) { disassemble(a); if (createFunction(a, null) != null) made++; }
    }
    for (String l : Files.readAllLines(Paths.get(dir, "labels.txt"))) {
      String[] p = l.trim().split("\\s+"); if (p.length < 2) continue;
      Address a = toAddr(Long.parseLong(p[0], 16));
      Function f = getFunctionAt(a);
      if (f == null) { disassemble(a); f = createFunction(a, p[1]); }
      if (f != null) f.setName(p[1], SourceType.USER_DEFINED);
    }
    analyzeAll(currentProgram);
    println("seeded functions: " + made);
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    PrintWriter tsv = new PrintWriter(new File(dir, "functions.tsv"));
    PrintWriter c = new PrintWriter(new File(dir, "skipit_decomp.c"));
    tsv.println("addr\tsize\tname\tcallers\tcallees\tdecomp_ok");
    int n = 0, ok = 0;
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      n++;
      DecompileResults r = di.decompileFunction(f, 60, monitor);
      boolean good = r != null && r.decompileCompleted();
      if (good) { ok++; c.println(r.getDecompiledFunction().getC()); }
      else c.println("/* FAILED " + f.getEntryPoint() + " */");
      tsv.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName() + "\t"
        + f.getCallingFunctions(monitor).size() + "\t" + f.getCalledFunctions(monitor).size() + "\t" + good);
    }
    tsv.close(); c.close();
    println("functions: " + n + " decompiled ok: " + ok);
  }
}
