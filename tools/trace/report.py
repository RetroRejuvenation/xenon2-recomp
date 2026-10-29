#!/usr/bin/env python3
"""Compare merged trace coverage with the Ghidra code map.

    python3 -I tools/trace/report.py [--trace generated/trace] [--lane generated/lane-d2] [--md FILE]

Reads coverage.json / indirect_targets.json (tools/trace/merge_traces.py) and the Ghidra outputs
(functions.json, indirect_sites.json, entrypoints_v2.json, ghidra_<module>.lst). Writes TRACE/report.json and a
Markdown summary (stdout, or --md):
  - per module: Ghidra instructions reached (count and %), functions reached / never executed;
  - executed instructions Ghidra did not decode, and executed code outside every known function;
  - per indirect site: traced targets, which of them static analysis already had, and new ones;
  - heuristic "unknown" entry points that turned out to execute;
  - writes into executed code and opcode changes (self-modifying code check).
"""
import argparse
import json
import os
import re

DATA_MNEMONICS = {"undefined1", "undefined2", "undefined4", "addr", "ds", "ushort"}
MODULE_WINDOWS = {"game": (0x0, 0x54E00)}
for _i in range(1, 7):
    MODULE_WINDOWS[f"level{_i}"] = (0x54E00, 0x80000)


def ghidra_insns(lst_path, lo, hi):
    """Instruction addresses (with byte lengths up to the next listing line) from a Ghidra listing."""
    rows = []
    for line in open(lst_path):
        m = re.match(r"^([0-9a-f]{6}) (.*)$", line.rstrip("\n"))
        if not m:
            continue
        t = m.group(2).split()
        if t and t[0].endswith(":"):
            t = t[1:]
        rows.append((int(m.group(1), 16), bool(t) and t[0] not in DATA_MNEMONICS))
    out = {}
    for i, (a, is_insn) in enumerate(rows):
        if is_insn and lo <= a < hi:
            nxt = rows[i + 1][0] if i + 1 < len(rows) else a + 2
            out[a] = max(2, min(10, nxt - a))
    return out


def runs(addrs, gap=12):
    """Group sorted addresses into [first, last] runs."""
    out = []
    for a in sorted(addrs):
        if out and a - out[-1][1] <= gap:
            out[-1][1] = a
        else:
            out.append([a, a])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--trace", default="generated/trace")
    ap.add_argument("--lane", default="generated/lane-d2")
    ap.add_argument("--md")
    a = ap.parse_args()
    cov = json.load(open(os.path.join(a.trace, "coverage.json")))
    ind = json.load(open(os.path.join(a.trace, "indirect_targets.json")))
    funcs = json.load(open(os.path.join(a.lane, "functions.json")))
    isites = json.load(open(os.path.join(a.lane, "indirect_sites.json")))
    ep = json.load(open(os.path.join(a.lane, "entrypoints_v2.json")))
    replays = cov["replays"]

    report = {"replays": replays, "modules": {}, "indirect": [], "unknown_entries": [], "smc": {}}
    md = []
    md.append(f"Replays merged ({len(replays)}): {', '.join(replays)}\n")
    md.append("| Module | Ghidra insns | Reached | % | Ghidra bytes reached | Executed insns | Not decoded by Ghidra | "
              "Outside known functions | Functions reached | Never executed |")
    md.append("|---|---|---|---|---|---|---|---|---|---|")
    details = []
    total_g = total_r = 0
    for mod, (lo, hi) in MODULE_WINDOWS.items():
        lst = os.path.join(a.lane, f"ghidra_{mod}.lst")
        g = ghidra_insns(lst, lo, hi) if os.path.exists(lst) else {}
        m = cov["modules"].get(mod, {"insns": [], "replays": []})
        ex = {e[0]: e for e in m["insns"]}
        reached = [pc for pc in g if pc in ex]
        not_ghidra = sorted(pc for pc in ex if pc not in g)
        fl = [f for f in funcs if f["module"] == mod]
        franges = [(int(f["start"], 16), int(f["end"], 16), f) for f in fl]
        outside_fn = sorted(pc for pc in ex if not any(s <= pc < e for s, e, _ in franges))
        f_reached, f_never = [], []
        for s, e, f in franges:
            (f_reached if any(s <= pc < e for pc in ex) else f_never).append(f)
        gbytes = sum(g.values())
        rbytes = sum(g[pc] for pc in reached)
        pct = 100.0 * len(reached) / len(g) if g else 0.0
        total_g += len(g)
        total_r += len(reached)
        md.append(f"| {mod} | {len(g)} | {len(reached)} | {pct:.1f} | {rbytes}/{gbytes} | {len(ex)} | {len(not_ghidra)} | "
                  f"{len(outside_fn)} | {len(f_reached)}/{len(fl)} | {len(f_never)} |")
        report["modules"][mod] = {
            "ghidra_insns": len(g), "ghidra_bytes": gbytes, "reached_insns": len(reached), "reached_bytes": rbytes,
            "reached_pct": round(pct, 2), "executed_insns": len(ex), "replays": m["replays"],
            "executed_not_decoded_by_ghidra": [[hex(x), hex(y)] for x, y in runs(not_ghidra)],
            "executed_outside_functions": [[hex(x), hex(y)] for x, y in runs(outside_fn)],
            "functions_reached": len(f_reached), "functions_total": len(fl),
            "functions_never_executed": [{"start": f["start"], "end": f["end"], "name": f["name"], "insns": f["insns"],
                                          "source": f["source"]} for f in f_never]}
        if not_ghidra or outside_fn:
            details.append(f"- **{mod}** executed but not decoded by Ghidra: "
                           + (", ".join(f"${x:X}-${y:X}" for x, y in runs(not_ghidra)) or "none")
                           + "; executed outside known functions: "
                           + (", ".join(f"${x:X}-${y:X}" for x, y in runs(outside_fn)) or "none"))
    pct = 100.0 * total_r / total_g if total_g else 0
    md.append(f"| **all** | {total_g} | {total_r} | {pct:.1f} | | | | | | |")
    report["total"] = {"ghidra_insns": total_g, "reached_insns": total_r, "reached_pct": round(pct, 2)}
    for extra in sorted(set(cov["modules"]) - set(MODULE_WINDOWS)):
        m = cov["modules"][extra]
        md.append(f"\nExtra trace context `{extra}` ({m['base']}-{m['end']}): {m['executed_insns']} instructions executed "
                  f"(not compared: no Ghidra program for it).")
    md.append("")
    md += details

    # Indirect sites.
    static = {}
    for s in isites["sites"]:
        static[(s["module"], int(s["site"], 16))] = s
    imm = set()
    for k, v in isites["handler_sets"].items():
        imm |= {int(x, 16) for x in v}
    md.append("\n### Indirect sites\n")
    md.append("| Module | Site | Instruction | Static resolution | Traced targets | Already known | New |")
    md.append("|---|---|---|---|---|---|---|")
    traced_sites = set()
    for s in ind["sites"]:
        site = int(s["site"], 16)
        st = static.get((s["module"], site)) or (static.get((s["module"], site - 4)) if s["rts_jump"] else None)
        traced_sites.add((s["module"], site))
        known = {int(t, 16) for t in st["targets"]} if st else set()
        tg = [int(t["pc"], 16) for t in s["targets"]]
        new = [t for t in tg if t not in known and t not in imm]
        res = st["resolution"] if st else "not in indirect_sites.json"
        report["indirect"].append({"module": s["module"], "site": s["site"], "insn": s["insn"], "rts_jump": s["rts_jump"],
                                   "static_resolution": res, "targets": [t["pc"] for t in s["targets"]],
                                   "known_static": [hex(t) for t in tg if t in known],
                                   "known_immediate_only": [hex(t) for t in tg if t not in known and t in imm],
                                   "new": [hex(t) for t in new]})
        md.append(f"| {s['module']} | ${site:X} | `{s['insn']}`{' (rts as jump)' if s['rts_jump'] else ''} | {res} | "
                  f"{len(tg)} | {len(tg) - len(new)} | {', '.join(f'${t:X}' for t in new) or '-'} |")
    never = [s for k, s in static.items() if k not in traced_sites and not (s["module"], k[1] + 4) in traced_sites]
    report["static_sites_never_executed"] = [{"module": s["module"], "site": s["site"], "insn": s["insn"]} for s in never]
    if never:
        md.append("\nStatic indirect sites never executed: " + ", ".join(f"{s['module']} {s['site']}" for s in never))

    # Heuristic "unknown" entry points (ghidra-code-map.md section 7).
    unknown = [int(e["addr"], 16) for e in ep["heuristic_reconciliation"]["resident"] if e["status"].startswith("unknown")]
    gx = {e[0]: e for e in cov["modules"].get("game", {"insns": []})["insns"]}
    hit = [u for u in unknown if u in gx]
    report["unknown_entries"] = [{"addr": hex(u), "executed": u in gx} for u in unknown]
    md.append(f"\nHeuristic 'unknown' resident entry points executed: {len(hit)}/{len(unknown)}"
              + (": " + ", ".join(f"${u:X}" for u in hit) if hit else ""))

    # Self-modifying code.
    report["smc"] = {"code_writes": cov["code_writes"], "code_changes": cov["code_changes"]}
    md.append(f"\nWrites into executed code: {len(cov['code_writes'])}; opcode changes at executed PCs: "
              f"{len(cov['code_changes'])}")
    for w in cov["code_writes"][:20]:
        md.append(f"- {w['module']} {w['addr']} written by {w['writer_module']} {w['writer']} x{w['count']}")
    md.append(f"\nInterrupt entries: " + ", ".join(f"L{i['level']} {i['pc']} x{i['count']}" for i in cov["interrupts"]))
    md.append("HLE traps (body replaced by the runtime): " + ", ".join(f"{h['pc']} x{h['count']}" for h in cov["hle_traps"]))
    if cov["outside"]:
        md.append("Executed outside chip windows: " + ", ".join(o["pc"] for o in cov["outside"]))

    with open(os.path.join(a.trace, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
        f.write("\n")
    text = "\n".join(md) + "\n"
    if a.md:
        open(a.md, "w").write(text)
    else:
        print(text)


if __name__ == "__main__":
    main()
