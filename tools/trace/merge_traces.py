#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Merge x2recomp trace files (--trace-out, format x2-trace-1) into the recompiler work-list.

    python3 -I tools/trace/merge_traces.py [--out generated/trace] TRACE.json|DIR ...

Writes, deterministically (inputs are sorted by label):
  OUT/coverage.json          per module: every executed instruction [pc, len, count, flags, replay_mask], branch
                             statistics [site, target, taken, not_taken], module loads, interrupt entries, HLE
                             traps, writes into executed code and opcode changes.
  OUT/indirect_targets.json  every indirect jmp/jsr site (and rts used as a jump) with the union of observed
                             targets, their counts and the replays that produced them.

Flags (OR-ed over runs): 1 call target, 2 taken-branch target, 4 indirect target, 8 interrupt entry, 16 HLE trap
(the runtime replaces that routine; its body is not executed). replay_mask bit i = replays[i] executed it.
"""
import argparse
import glob
import json
import os
import sys


def load_traces(paths):
    files = []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, "*.json"))) if os.path.isdir(p) else [p]
    traces = []
    for f in files:
        with open(f) as fh:
            d = json.load(fh)
        if d.get("format") != "x2-trace-1":
            print(f"skipping {f}: not an x2-trace-1 file", file=sys.stderr)
            continue
        traces.append(d)
    traces.sort(key=lambda d: d["label"])
    labels = [d["label"] for d in traces]
    if len(set(labels)) != len(labels):
        sys.exit("duplicate trace labels: " + ", ".join(labels))
    return traces


def merge(traces):
    labels = [d["label"] for d in traces]
    mods = {}
    interrupts, hle, cw, changes, outside = {}, {}, {}, [], {}
    indirect = {}
    for i, d in enumerate(traces):
        bit = 1 << i
        for m in d["modules"]:
            mm = mods.setdefault(m["name"], {"base": m["base"], "end": m["end"], "keys": set(), "loads": [],
                                             "insns": {}, "branches": {}, "replays": set()})
            if m["key"] != "0000000000000000":
                mm["keys"].add(m["key"])
            for ld in m["loads"]:
                mm["loads"].append(dict(ld, replay=d["label"]))
            if m["insns"]:
                mm["replays"].add(d["label"])
            for pc, count, ln, flags in m["insns"]:
                e = mm["insns"].setdefault(pc, [pc, ln, 0, 0, 0])
                if e[1] != ln:
                    print(f"warning: {m['name']} ${pc:X}: length {e[1]} vs {ln} in {d['label']}", file=sys.stderr)
                e[2] += count
                e[3] |= flags
                e[4] |= bit
            for site, target, taken, not_taken in m["branches"]:
                b = mm["branches"].setdefault(site, [site, target, 0, 0])
                b[2] += taken
                b[3] += not_taken
        for it in d["interrupts"]:
            k = (it["level"], it["module"], it["pc"])
            interrupts[k] = interrupts.get(k, 0) + it["count"]
        for h in d["hle_traps"]:
            k = (h["module"], h["pc"])
            hle[k] = hle.get(k, 0) + h["count"]
        for w in d["code_writes"]:
            k = (w["module"], w["addr"], w["writer_module"], w["writer"])
            e = cw.setdefault(k, dict(w, count=0, replays=[]))
            e["count"] += w["count"]
            e["replays"].append(d["label"])
        for c in d["code_changes"]:
            changes.append(dict(c, replay=d["label"]))
        for o in d["outside"]:
            outside[o["pc"]] = outside.get(o["pc"], 0) + o["count"]
        for s in d["indirect"]:
            k = (s["module"], int(s["site"], 16))
            e = indirect.setdefault(k, {"module": s["module"], "site": s["site"], "insn": " ".join(s["insn"].split()),
                                        "op": s["op"], "rts_jump": s["rts_jump"], "targets": {}})
            for t in s["targets"]:
                tk = (t["module"], int(t["pc"], 16))
                te = e["targets"].setdefault(tk, {"module": t["module"], "pc": t["pc"], "count": 0, "replays": []})
                te["count"] += t["count"]
                te["replays"].append(d["label"])

    order = sorted(mods, key=lambda n: (n != "game", n))
    cov = {"format": "x2-coverage-1", "replays": labels,
           "insn_fields": ["pc", "len", "count", "flags", "replay_mask"],
           "branch_fields": ["site", "target", "taken", "not_taken"],
           "flag_bits": traces[0]["flag_bits"] if traces else {}, "modules": {}}
    for n in order:
        mm = mods[n]
        insns = [mm["insns"][pc] for pc in sorted(mm["insns"])]
        cov["modules"][n] = {
            "base": mm["base"], "end": mm["end"], "content_keys": sorted(mm["keys"]),
            "replays": sorted(mm["replays"]), "loads": mm["loads"],
            "executed_insns": len(insns), "executed_bytes": sum(e[1] for e in insns),
            "insns": insns, "branches": [mm["branches"][s] for s in sorted(mm["branches"])]}
    cov["interrupts"] = [{"level": k[0], "module": k[1], "pc": k[2], "count": v} for k, v in sorted(interrupts.items())]
    cov["hle_traps"] = [{"module": k[0], "pc": k[1], "count": v} for k, v in sorted(hle.items())]
    cov["code_writes"] = [cw[k] for k in sorted(cw)]
    cov["code_changes"] = changes
    cov["outside"] = [{"pc": k, "count": v} for k, v in sorted(outside.items())]

    sites = []
    for k in sorted(indirect, key=lambda k: (k[0] != "game", k[0], k[1])):
        e = indirect[k]
        e["targets"] = [e["targets"][t] for t in sorted(e["targets"], key=lambda t: (t[0] != "game", t[0], t[1]))]
        e["target_count"] = len(e["targets"])
        sites.append(e)
    ind = {"format": "x2-indirect-1", "replays": labels,
           "note": "observed targets of every register-indirect jmp/jsr, pc-relative indexed jmp and rts that did not "
                   "return to its caller's pushed address; module = overlay context of the site/target",
           "sites": sites}
    return cov, ind


def dump(obj, path):
    with open(path, "w") as f:
        json.dump(obj, f, separators=(",", ":"))
        f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="generated/trace")
    ap.add_argument("traces", nargs="+")
    a = ap.parse_args()
    traces = load_traces(a.traces)
    if not traces:
        sys.exit("no traces")
    cov, ind = merge(traces)
    os.makedirs(a.out, exist_ok=True)
    dump(cov, os.path.join(a.out, "coverage.json"))
    with open(os.path.join(a.out, "indirect_targets.json"), "w") as f:
        json.dump(ind, f, indent=1)
        f.write("\n")
    for n, m in cov["modules"].items():
        print(f"{n:18s} {m['executed_insns']:6d} insns {m['executed_bytes']:7d} bytes  replays: {len(m['replays'])}")
    print(f"{len(ind['sites'])} indirect sites, {sum(s['target_count'] for s in ind['sites'])} targets; "
          f"{len(cov['code_writes'])} code writes, {len(cov['code_changes'])} opcode changes -> {a.out}")


if __name__ == "__main__":
    main()
