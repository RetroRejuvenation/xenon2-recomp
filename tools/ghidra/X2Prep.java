// Xenon 2 recomp: Ghidra headless pre-script.
// Maps Amiga chip RAM / custom chips / CIAs as memory blocks, adds extra image blocks,
// labels hardware registers and seeds known entry points before auto-analysis.
// Usage (analyzeHeadless): -preScript X2Prep.java <config.txt>
// Config lines (hex addresses, '#' comments):
//   extra <file> <base> <name>   extra initialized block loaded from file
//   hw <addr> <name>             hardware register label
//   seed func|code <addr> <name> entry point (function or plain code label)
//@category Xenon2
import java.io.*;
import java.nio.file.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.SourceType;

public class X2Prep extends GhidraScript {

	private Address a(long v) {
		return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
	}

	private void ensureRam(Memory mem, long lo, long hi, String name) throws Exception {
		// fill unmapped holes in [lo,hi) with uninitialized RAM blocks
		long p = lo;
		int n = 0;
		while (p < hi) {
			MemoryBlock b = mem.getBlock(a(p));
			if (b != null) {
				p = b.getEnd().getOffset() + 1;
				continue;
			}
			long q = p;
			while (q < hi && mem.getBlock(a(q)) == null) {
				q++;
			}
			MemoryBlock nb = mem.createUninitializedBlock(name + "_" + (n++), a(p), q - p, false);
			nb.setRead(true);
			nb.setWrite(true);
			nb.setExecute(false);
			p = q;
		}
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		List<String> lines = Files.readAllLines(Paths.get(args[0]));
		Memory mem = currentProgram.getMemory();
		for (String raw : lines) {
			String[] f = raw.trim().split("\\s+");
			if (f.length >= 4 && f[0].equals("extra")) {
				File file = new File(f[1]);
				try (InputStream is = new FileInputStream(file)) {
					MemoryBlock b = mem.createInitializedBlock(f[3], a(Long.parseLong(f[2], 16)), is,
						file.length(), monitor, false);
					b.setRead(true);
					b.setWrite(true);
					b.setExecute(true);
				}
			}
		}
		for (MemoryBlock b : mem.getBlocks()) {
			b.setWrite(true);
		}
		ensureRam(mem, 0x0, 0x80000, "chipram");
		MemoryBlock custom = mem.createUninitializedBlock("CUSTOM", a(0xDFF000), 0x200, false);
		custom.setVolatile(true);
		custom.setWrite(true);
		MemoryBlock ciaa = mem.createUninitializedBlock("CIAA", a(0xBFE000), 0x1000, false);
		ciaa.setVolatile(true);
		ciaa.setWrite(true);
		MemoryBlock ciab = mem.createUninitializedBlock("CIAB", a(0xBFD000), 0x1000, false);
		ciab.setVolatile(true);
		ciab.setWrite(true);

		// Ghidra's address-table guesser turns graphics/music data into pointer tables (and into
		// data over real code, e.g. $9818); dispatch tables are supplied explicitly instead.
		setAnalysisOption(currentProgram, "Create Address Tables", "false");

		int seeds = 0;
		for (String raw : lines) {
			String[] f = raw.trim().split("\\s+");
			if (f.length >= 3 && f[0].equals("hw")) {
				createLabel(a(Long.parseLong(f[1], 16)), f[2], true, SourceType.IMPORTED);
			}
		}
		for (String raw : lines) {
			String[] f = raw.trim().split("\\s+");
			if (f.length < 4 || !f[0].equals("seed")) {
				continue;
			}
			Address ad = a(Long.parseLong(f[2], 16));
			createLabel(ad, f[3], true, SourceType.USER_DEFINED);
			currentProgram.getSymbolTable().addExternalEntryPoint(ad);
			disassemble(ad);
			if (f[1].equals("func") && getFunctionAt(ad) == null) {
				createFunction(ad, f[3]);
			}
			seeds++;
		}
		println("X2Prep: seeded " + seeds + " entry points");
	}
}
