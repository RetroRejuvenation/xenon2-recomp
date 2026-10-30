// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Xenon 2 recomp: Ghidra headless post-script.
// 1. records what plain auto-analysis found from the seeds,
// 2. applies known jump tables, classifies heuristic candidate entry points
//    (PseudoDisassembler + overlap checks) and adds the plausible ones,
// 3. re-analyses and exports functions / indirect sites / hardware access /
//    code writes / anti-trace xrefs as JSON.
// Usage: -postScript X2Export.java <config.txt> <out.json>
// Config lines used here (hex):
//   range <lo> <hi>                   export window [lo,hi)
//   tableref <site> <target> jump|call  known dispatch table target
//   cand <addr> <kind> <name>         heuristic entry point to classify
//   name <addr> <name>                rename function/label after analysis
//   watch <lo> <hi> <tag>             report all xrefs into [lo,hi) from outside
//@category Xenon2
import java.io.*;
import java.nio.file.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class X2Export extends GhidraScript {

	private Address a(long v) {
		return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
	}

	private static String h(long v) {
		return String.format("\"0x%x\"", v);
	}

	private static String q(String s) {
		return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
	}

	private boolean isHw(long v) {
		v &= 0xFFFFFFL;
		return (v >= 0xDFF000 && v < 0xDFF200) || (v >= 0xBFD000 && v < 0xBFF000);
	}

	private String hwName(long v) {
		Symbol s = getSymbolAt(a(v & 0xFFFFFFL));
		return s != null ? s.getName() : String.format("hw_%06x", v & 0xFFFFFFL);
	}

	private static final Set<String> STRONG = Set.of("immediate", "immediate-ptr", "level-call",
		"call-target/branch", "call-branch", "table");

	private boolean decodes(PseudoDisassembler pd, Address ad) {
		try {
			return pd.disassemble(ad) != null;
		}
		catch (Exception e) {
			return false;
		}
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		List<String[]> cfg = new ArrayList<>();
		for (String raw : Files.readAllLines(Paths.get(args[0]))) {
			String t = raw.trim();
			if (!t.isEmpty() && !t.startsWith("#")) {
				cfg.add(t.split("\\s+"));
			}
		}
		long lo = 0, hi = 0x1000000;
		for (String[] f : cfg) {
			if (f[0].equals("range")) {
				lo = Long.parseLong(f[1], 16);
				hi = Long.parseLong(f[2], 16);
			}
		}
		AddressSet win = new AddressSet(a(lo), a(hi - 1));
		Listing listing = currentProgram.getListing();
		ReferenceManager rm = currentProgram.getReferenceManager();
		FunctionManager fm = currentProgram.getFunctionManager();

		// ---- phase 1: what plain analysis found
		Set<Long> pureFuncs = new TreeSet<>();
		for (Function f : fm.getFunctions(win, true)) {
			pureFuncs.add(f.getEntryPoint().getOffset());
		}
		long pureInsnBytes = 0;
		for (Instruction in : listing.getInstructions(win, true)) {
			pureInsnBytes += in.getLength();
		}

		// ---- phase 2a: known tables
		int tableRefs = 0;
		StringBuilder tableRejects = new StringBuilder();
		for (String[] f : cfg) {
			if (!f[0].equals("tableref")) {
				continue;
			}
			Address site = a(Long.parseLong(f[1], 16));
			Address tgt = a(Long.parseLong(f[2], 16));
			boolean call = f[3].equals("call");
			Data dd = listing.getDefinedDataContaining(tgt);
			if (dd != null && getInstructionAt(tgt) == null) {
				// analyzer-guessed pointer/variable over a known table target: the table wins
				clearListing(dd.getAddress(), dd.getMaxAddress());
			}
			if (getInstructionAt(tgt) == null) {
				PseudoDisassembler tpd = new PseudoDisassembler(currentProgram);
				boolean inWin = win.contains(tgt);
				boolean ok = !inWin || (listing.getInstructionContaining(tgt) == null &&
					(tpd.isValidCode(tgt) || tpd.checkValidSubroutine(tgt, true, false, false)));
				if (!ok) {
					tableRejects.append(tableRejects.length() > 0 ? ", " : "").append("{\"site\": ")
							.append(h(site.getOffset())).append(", \"target\": ").append(h(tgt.getOffset())).append("}");
					continue;
				}
			}
			disassemble(tgt);
			if (call && getFunctionAt(tgt) == null) {
				createFunction(tgt, null);
			}
			if (getInstructionAt(site) != null) {
				rm.addMemoryReference(site, tgt, call ? RefType.COMPUTED_CALL : RefType.COMPUTED_JUMP,
					SourceType.USER_DEFINED, 0);
				tableRefs++;
			}
		}
		analyzeChanges(currentProgram);

		// ---- phase 2b: classify heuristic candidates
		PseudoDisassembler pd = new PseudoDisassembler(currentProgram);
		StringBuilder cands = new StringBuilder();
		List<Address> toAdd = new ArrayList<>();
		Map<Long, Integer> kindRank = new HashMap<>();
		List<String> rankOrder = List.of("named-root", "level-call", "call-target/branch", "call-branch",
			"immediate-ptr", "immediate", "table-ptr", "table", "gap-sweep");
		for (String[] f : cfg) {
			if (!f[0].equals("cand")) {
				continue;
			}
			long v = Long.parseLong(f[1], 16);
			Address ad = a(v);
			kindRank.put(v, rankOrder.indexOf(f[2]) < 0 ? 8 : rankOrder.indexOf(f[2]));
			Instruction at = getInstructionAt(ad);
			Instruction cont = listing.getInstructionContaining(ad);
			Data data = listing.getDefinedDataContaining(ad);
			int nrefs = 0;
			for (Reference r : rm.getReferencesTo(ad)) {
				nrefs++;
			}
			boolean valid = false;
			int nins = 0;
			String verdict;
			boolean dead = false;
			for (String[] g : cfg) {
				if (g[0].equals("deadrange") && v >= Long.parseLong(g[1], 16) && v < Long.parseLong(g[2], 16)) {
					dead = true;
				}
			}
			if ((v & 1) != 0) {
				verdict = "data:odd-address";
			}
			else if (dead && at == null) {
				verdict = "dead:inside-deadrange-not-reached";
			}
			else if (at != null) {
				verdict = "code:reached-by-ghidra";
			}
			else if (cont != null) {
				verdict = "data:overlaps-ghidra-instruction@" + cont.getAddress();
			}
			else {
				valid = pd.checkValidSubroutine(ad, true, true, false);
				nins = pd.getLastCheckValidInstructionCount();
				boolean evidence = !f[2].equals("gap-sweep") || (f.length > 4 && !f[4].equals("-"));
				if (valid && evidence) {
					verdict = "code:valid-subroutine";
					toAdd.add(ad);
				}
				else if (valid) {
					verdict = "unknown:valid-subroutine-no-pointer-evidence";
				}
				else if (nrefs > 0 && STRONG.contains(f[2]) && data == null && decodes(pd, ad)) {
					// referenced as an immediate/call target but the pseudo flow check failed
					// (e.g. tail-jumps into the resident API block): trust the reference
					verdict = "code:referenced-pseudo-check-failed";
					toAdd.add(ad);
				}
				else {
					verdict = "data:not-valid-subroutine";
				}
			}
			Function fn = getFunctionAt(ad);
			if (cands.length() > 0) {
				cands.append(",\n");
			}
			cands.append("  {\"addr\": ").append(h(v)).append(", \"final_state\": \"@@").append(Long.toHexString(v)).append("@@\"").append(", \"heuristic_kind\": ").append(q(f[2]))
					.append(", \"verdict\": ").append(q(verdict))
					.append(", \"pointer_evidence\": ").append(q(f.length > 4 ? f[4] : "-"))
					.append(", \"ghidra_refs_to\": ").append(nrefs)
					.append(", \"ghidra_function_at\": ").append(fn != null)
					.append(", \"in_defined_data\": ").append(data != null)
					.append(", \"pseudo_insns\": ").append(nins).append("}");
		}
		// strongest evidence first so weak (gap-sweep) candidates cannot pre-empt real code
		toAdd.sort(Comparator.comparingInt(ad -> kindRank.getOrDefault(ad.getOffset(), 9)));
		for (Address ad : toAdd) {
			if (listing.getInstructionContaining(ad) != null && getInstructionAt(ad) == null) {
				continue;
			}
			disassemble(ad);
			if (getFunctionAt(ad) == null) {
				createFunction(ad, null);
			}
		}
		analyzeChanges(currentProgram);

		// ---- phase 2c: code Ghidra disassembled (address tables, computed jumps) but left outside
		// any function: make a function at the start of each such run that has an incoming reference
		Set<Long> orphanEntries = new TreeSet<>();
		for (int pass = 0; pass < 5; pass++) {
			List<Address> starts = new ArrayList<>();
			for (Instruction in : listing.getInstructions(win, true)) {
				if (fm.getFunctionContaining(in.getAddress()) != null) {
					continue;
				}
				Instruction prev = listing.getInstructionBefore(in.getAddress());
				boolean fallIn = prev != null && prev.getFallThrough() != null &&
					prev.getFallThrough().equals(in.getAddress());
				if (!fallIn && rm.getReferenceCountTo(in.getAddress()) > 0) {
					starts.add(in.getAddress());
				}
			}
			if (starts.isEmpty()) {
				break;
			}
			for (Address ad : starts) {
				if (fm.getFunctionContaining(ad) == null && createFunction(ad, null) != null) {
					orphanEntries.add(ad.getOffset());
				}
			}
			analyzeChanges(currentProgram);
		}

		// ---- phase 3: names
		for (String[] f : cfg) {
			if (f[0].equals("name")) {
				Address ad = a(Long.parseLong(f[1], 16));
				Function fn = getFunctionAt(ad);
				if (fn != null) {
					fn.setName(f[2], SourceType.USER_DEFINED);
				}
				else {
					createLabel(ad, f[2], true, SourceType.USER_DEFINED);
				}
			}
		}

		// ---- export
		StringBuilder o = new StringBuilder();
		o.append("{\n\"program\": ").append(q(currentProgram.getName()))
				.append(",\n\"language\": ").append(q(currentProgram.getLanguageID().toString()))
				.append(",\n\"window\": [").append(h(lo)).append(", ").append(h(hi)).append("]")
				.append(",\n\"pure_analysis_function_count\": ").append(pureFuncs.size())
				.append(",\n\"pure_analysis_insn_bytes\": ").append(pureInsnBytes)
				.append(",\n\"table_refs_added\": ").append(tableRefs)
				.append(",\n\"table_rejects\": [").append(tableRejects).append("]")
				.append(",\n\"pure_analysis_functions\": [");
		boolean first = true;
		for (long v : pureFuncs) {
			o.append(first ? "" : ", ").append(h(v));
			first = false;
		}
		String candText = cands.toString();
		java.util.regex.Matcher m = java.util.regex.Pattern.compile("@@([0-9a-f]+)@@").matcher(candText);
		StringBuffer cb = new StringBuffer();
		while (m.find()) {
			Address ad = a(Long.parseLong(m.group(1), 16));
			String st = getFunctionAt(ad) != null ? "function-entry"
					: getInstructionAt(ad) != null ? "instruction-start"
					: listing.getInstructionContaining(ad) != null ? "mid-instruction"
					: listing.getDefinedDataContaining(ad) != null ? "defined-data" : "undefined";
			m.appendReplacement(cb, st);
		}
		m.appendTail(cb);
		o.append("],\n\"candidates\": [\n").append(cb).append("\n],\n\"functions\": [\n");

		StringBuilder ind = new StringBuilder();
		StringBuilder hw = new StringBuilder();
		StringBuilder cw = new StringBuilder();
		first = true;
		for (Function fn : fm.getFunctions(win, true)) {
			AddressSetView body = fn.getBody();
			int nIns = 0;
			Map<String, Set<String>> regs = new TreeMap<>(); // reg -> {R,W}
			Map<Register, Long> base = new HashMap<>();
			for (Instruction in : listing.getInstructions(body, true)) {
				nIns++;
				// hardware via absolute/analysed refs
				for (Reference r : in.getReferencesFrom()) {
					long t = r.getToAddress().getOffset();
					if (r.getToAddress().isMemoryAddress() && isHw(t)) {
						String rw = r.getReferenceType().isWrite() ? "W" : (r.getReferenceType().isRead() ? "R" : "A");
						regs.computeIfAbsent(hwName(t), k -> new TreeSet<>()).add(rw);
					}
				}
				// hardware via base register (lea $dff000,An ... d16(An))
				String mn = in.getMnemonicString().toLowerCase();
				for (int i = 0; i < in.getNumOperands(); i++) {
					Object[] objs = in.getOpObjects(i);
					Register an = null;
					long disp = 0;
					for (Object ob : objs) {
						if (ob instanceof Register && ((Register) ob).getName().matches("[aA][0-6]") && an == null) {
							an = (Register) ob;
						}
						else if (ob instanceof Scalar) {
							disp = ((Scalar) ob).getSignedValue();
						}
					}
					if (an != null && base.containsKey(an) && objs.length > 1 &&
						in.getDefaultOperandRepresentation(i).contains("(")) {
						long t = base.get(an) + disp;
						boolean dst = (i == in.getNumOperands() - 1) && in.getNumOperands() > 1;
						regs.computeIfAbsent(hwName(t), k -> new TreeSet<>()).add(dst ? "W*" : "R*");
					}
				}
				Long newBase = null;
				if ((mn.startsWith("lea") || mn.startsWith("movea")) && in.getNumOperands() == 2) {
					for (Object ob : in.getOpObjects(0)) {
						long v = ob instanceof Scalar ? ((Scalar) ob).getUnsignedValue()
								: ob instanceof Address ? ((Address) ob).getOffset() : -1;
						if (v >= 0 && isHw(v)) {
							newBase = v & 0xFFFFFFL;
						}
					}
				}
				for (Object ob : in.getResultObjects()) {
					if (ob instanceof Register) {
						base.remove(ob);
						if (newBase != null && ((Register) ob).getName().matches("[aA][0-6]")) {
							base.put((Register) ob, newBase);
						}
					}
				}
				// indirect control flow
				FlowType ft = in.getFlowType();
				if (ft.isComputed()) {
					StringBuilder tg = new StringBuilder();
					for (Reference r : in.getReferencesFrom()) {
						if (r.getReferenceType().isFlow()) {
							tg.append(tg.length() > 0 ? ", " : "").append(h(r.getToAddress().getOffset()));
						}
					}
					ind.append(ind.length() > 0 ? ",\n" : "").append("  {\"site\": ").append(h(in.getAddress().getOffset()))
							.append(", \"insn\": ").append(q(in.toString()))
							.append(", \"function\": ").append(q(fn.getName()))
							.append(", \"flow\": ").append(q(ft.toString()))
							.append(", \"ghidra_targets\": [").append(tg).append("]}");
				}
				// writes into code
				for (Reference r : in.getReferencesFrom()) {
					if (r.getReferenceType().isWrite() && r.getToAddress().isMemoryAddress()) {
						Instruction tgt = listing.getInstructionContaining(r.getToAddress());
						if (tgt != null) {
							cw.append(cw.length() > 0 ? ",\n" : "").append("  {\"writer\": ").append(h(in.getAddress().getOffset()))
									.append(", \"target\": ").append(h(r.getToAddress().getOffset()))
									.append(", \"target_insn\": ").append(h(tgt.getAddress().getOffset()))
									.append(", \"insn\": ").append(q(in.toString()))
									.append(", \"target_text\": ").append(q(tgt.toString())).append("}");
						}
					}
				}
			}
			if (!regs.isEmpty()) {
				hw.append(hw.length() > 0 ? ",\n" : "").append("  {\"function\": ").append(h(fn.getEntryPoint().getOffset()))
						.append(", \"name\": ").append(q(fn.getName())).append(", \"regs\": {");
				boolean f2 = true;
				for (Map.Entry<String, Set<String>> e : regs.entrySet()) {
					hw.append(f2 ? "" : ", ").append(q(e.getKey())).append(": ").append(q(String.join("", e.getValue())));
					f2 = false;
				}
				hw.append("}}");
			}
			StringBuilder callers = new StringBuilder();
			Set<String> kinds = new TreeSet<>();
			int nc = 0;
			for (Reference r : rm.getReferencesTo(fn.getEntryPoint())) {
				kinds.add(r.getReferenceType().toString());
				if (nc++ < 40) {
					callers.append(callers.length() > 0 ? ", " : "").append(h(r.getFromAddress().getOffset()));
				}
			}
			StringBuilder kj = new StringBuilder();
			for (String k : kinds) {
				kj.append(kj.length() > 0 ? ", " : "").append(q(k));
			}
			o.append(first ? "" : ",\n").append("  {\"start\": ").append(h(fn.getEntryPoint().getOffset()))
					.append(", \"end\": ").append(h(body.getMaxAddress().getOffset() + 1))
					.append(", \"name\": ").append(q(fn.getName()))
					.append(", \"source\": ").append(q(fn.getSymbol().getSource().toString()))
					.append(", \"insns\": ").append(nIns)
					.append(", \"pure\": ").append(pureFuncs.contains(fn.getEntryPoint().getOffset()))
					.append(", \"orphan_entry\": ").append(orphanEntries.contains(fn.getEntryPoint().getOffset()))
					.append(", \"ref_kinds\": [").append(kj).append("]")
					.append(", \"refs_to\": [").append(callers).append("]}");
			first = false;
		}
		o.append("\n],\n\"indirect_sites\": [\n").append(ind).append("\n],\n\"hw_access\": [\n").append(hw)
				.append("\n],\n\"code_writes\": [\n").append(cw).append("\n],\n");

		// all instructions outside any function (orphans) and total coverage
		long orphan = 0, total = 0;
		StringBuilder orph = new StringBuilder();
		Address prevEnd = null;
		long runStart = -1;
		for (Instruction in : listing.getInstructions(win, true)) {
			total += in.getLength();
			if (fm.getFunctionContaining(in.getAddress()) == null) {
				orphan += in.getLength();
				if (prevEnd == null || !prevEnd.equals(in.getAddress())) {
					if (runStart >= 0) {
						orph.append(orph.length() > 0 ? ", " : "").append("[").append(h(runStart)).append(", ").append(h(prevEnd.getOffset())).append("]");
					}
					runStart = in.getAddress().getOffset();
				}
				prevEnd = in.getMaxAddress().add(1);
			}
		}
		if (runStart >= 0) {
			orph.append(orph.length() > 0 ? ", " : "").append("[").append(h(runStart)).append(", ").append(h(prevEnd.getOffset())).append("]");
		}
		o.append("\"insn_bytes\": ").append(total).append(",\n\"orphan_insn_bytes\": ").append(orphan)
				.append(",\n\"orphan_runs\": [").append(orph).append("],\n");

		// control flow leaving the window (level module -> resident game routines)
		Map<Long, Set<String>> ext = new TreeMap<>();
		for (Instruction in : listing.getInstructions(win, true)) {
			for (Reference r : in.getReferencesFrom()) {
				Address to = r.getToAddress();
				if (r.getReferenceType().isFlow() && to.isMemoryAddress() && !win.contains(to)) {
					ext.computeIfAbsent(to.getOffset(), k -> new TreeSet<>())
							.add(String.format("%x:%s", in.getAddress().getOffset(), r.getReferenceType()));
				}
			}
		}
		o.append("\"external_flow\": [");
		first = true;
		for (Map.Entry<Long, Set<String>> e : ext.entrySet()) {
			StringBuilder fr = new StringBuilder();
			for (String x : e.getValue()) {
				fr.append(fr.length() > 0 ? ", " : "").append(q(x));
			}
			o.append(first ? "" : ",\n  ").append("{\"to\": ").append(h(e.getKey())).append(", \"from\": [").append(fr).append("]}");
			first = false;
		}
		o.append("],\n");

		// watched ranges
		o.append("\"watch\": [\n");
		first = true;
		for (String[] f : cfg) {
			if (!f[0].equals("watch")) {
				continue;
			}
			long wlo = Long.parseLong(f[1], 16), whi = Long.parseLong(f[2], 16);
			AddressSet ws = new AddressSet(a(wlo), a(whi - 1));
			StringBuilder refs = new StringBuilder();
			for (Address to : rm.getReferenceDestinationIterator(ws, true)) {
				for (Reference r : rm.getReferencesTo(to)) {
					long fr = r.getFromAddress().getOffset();
					if (fr >= wlo && fr < whi) {
						continue;
					}
					refs.append(refs.length() > 0 ? ", " : "").append("{\"from\": ").append(h(fr)).append(", \"to\": ")
							.append(h(to.getOffset())).append(", \"type\": ").append(q(r.getReferenceType().toString())).append("}");
				}
			}
			long wins = 0;
			for (Instruction in : listing.getInstructions(ws, true)) {
				wins += in.getLength();
			}
			o.append(first ? "" : ",\n").append("  {\"tag\": ").append(q(f[3])).append(", \"lo\": ").append(h(wlo))
					.append(", \"hi\": ").append(h(whi)).append(", \"insn_bytes_inside\": ").append(wins)
					.append(", \"external_refs\": [").append(refs).append("]}");
			first = false;
		}
		o.append("\n],\n\"bad_bookmarks\": [");
		first = true;
		Iterator<Bookmark> bi = currentProgram.getBookmarkManager().getBookmarksIterator(BookmarkType.ERROR);
		while (bi.hasNext()) {
			Bookmark b = bi.next();
			if (!win.contains(b.getAddress())) {
				continue;
			}
			o.append(first ? "" : ", ").append("{\"addr\": ").append(h(b.getAddress().getOffset()))
					.append(", \"msg\": ").append(q(b.getComment())).append("}");
			first = false;
		}
		o.append("]\n}\n");
		Files.writeString(Paths.get(args[1]), o.toString());

		// plain listing of instructions/defined data (for reading and diffing)
		if (args.length > 2) {
			try (PrintWriter lw = new PrintWriter(new FileWriter(args[2]))) {
				for (CodeUnit cu : listing.getCodeUnits(win, true)) {
					if (cu instanceof Data && !((Data) cu).isDefined()) {
						continue;
					}
					Address ad = cu.getAddress();
					Function fn = getFunctionAt(ad);
					if (fn != null) {
						lw.printf("%n; ---- function %s%n", fn.getName());
					}
					Symbol s = getSymbolAt(ad);
					String lab = s != null ? s.getName() + ":" : "";
					StringBuilder refs = new StringBuilder();
					for (Reference r : cu.getReferencesFrom()) {
						if (r.getReferenceType().isFlow() && r.getReferenceType().isComputed()) {
							refs.append(" ->").append(r.getToAddress());
						}
					}
					lw.printf("%06x %-24s %s%s%n", ad.getOffset(), lab, cu.toString(), refs);
				}
			}
		}
		println("X2Export: wrote " + args[1]);
	}
}
