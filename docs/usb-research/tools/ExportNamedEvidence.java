// Reproducible static evidence export. Does not execute target code.
//@category Octopus
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
public class ExportNamedEvidence extends GhidraScript {
 public void run() throws Exception {
  String[] a=getScriptArgs(); File dir=new File(a[0],currentProgram.getName());dir.mkdirs();
  String regex=a.length>1?a[1]:".*";
  DecompInterface d=new DecompInterface();d.openProgram(currentProgram);
  try(PrintWriter index=new PrintWriter(new File(dir,"functions.tsv"));PrintWriter out=new PrintWriter(new File(dir,"selected-functions.txt"));PrintWriter syms=new PrintWriter(new File(dir,"symbols.tsv"))) {
   SymbolIterator si=currentProgram.getSymbolTable().getAllSymbols(true);
   while(si.hasNext()){Symbol s=si.next();syms.println(s.getAddress()+"\t"+s.getName(true)+"\t"+s.getSymbolType());}
   FunctionIterator fs=currentProgram.getFunctionManager().getFunctions(true);
   while(fs.hasNext()&&!monitor.isCancelled()) {Function f=fs.next();String name=f.getName(true);
    index.println(f.getEntryPoint()+"\t"+f.getBody().getNumAddresses()+"\t"+name);
    if(!name.matches(regex))continue;
    out.println("\n===== "+f.getEntryPoint()+" "+name+" =====");
    ReferenceIterator refs=currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());while(refs.hasNext()){Reference r=refs.next();out.println("XREF "+r.getFromAddress()+" "+r.getReferenceType()+" "+getFunctionContaining(r.getFromAddress()));}
    InstructionIterator ins=currentProgram.getListing().getInstructions(f.getBody(),true);while(ins.hasNext()){Instruction i=ins.next();out.println(i.getAddress()+" "+i.toString());}
    DecompileResults r=d.decompileFunction(f,45,monitor);if(r.decompileCompleted())out.println(r.getDecompiledFunction().getC());else out.println("DECOMPILE ERROR "+r.getErrorMessage());
   }
  } finally {d.dispose();}
 }
}
