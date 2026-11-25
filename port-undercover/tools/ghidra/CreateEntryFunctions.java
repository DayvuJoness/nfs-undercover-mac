// Before auto-analysis: make a function at each address given as a script argument (hex), so Ghidra's own
// analysis follows its body and resolves its switch tables. tools/analyze.py passes game.toml's
// [translate] entry_points (or a module's): functions reached only through pointers or PUSHed addresses,
// which Ghidra never makes by itself (NFS Undercover's state handlers such as 0048ad20, 2026-11-13).
// @category PopMetal
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class CreateEntryFunctions extends GhidraScript {
    public void run() throws Exception {
        int made = 0;
        for (String arg : getScriptArgs()) {
            Address a = toAddr(Long.parseLong(arg.replaceFirst("^0x", ""), 16));
            if (getFunctionAt(a) != null) continue;
            disassemble(a);
            if (createFunction(a, null) != null) made++;
            else println("CreateEntryFunctions: no function at " + a);
        }
        println("CreateEntryFunctions: made " + made + " of " + getScriptArgs().length);
    }
}
