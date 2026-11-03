#!/usr/bin/env python3
"""Map top flat IPs from perf report using nm; dump nearby guest symbols + source if any."""
import re, subprocess
from collections import defaultdict
from pathlib import Path

OUT = Path.home() / "nfsu2-linux-build"
BIN = OUT / "nfsu2_bench"
REPORT = OUT / "perf" / "race.report.txt"

nm = subprocess.check_output(["nm", "-n", str(BIN)], text=True, errors="replace").splitlines()
syms = []
for line in nm:
    parts = line.split()
    if len(parts) < 3:
        continue
    try:
        a = int(parts[0], 16)
    except ValueError:
        continue
    syms.append((a, parts[1], parts[2]))

def lookup(addr: int):
    best = None
    for i, (sa, typ, sn) in enumerate(syms):
        if sa <= addr:
            best = i
        else:
            break
    if best is None:
        return None
    sa, typ, sn = syms[best]
    nxt = syms[best + 1][0] if best + 1 < len(syms) else sa + 1
    return sn, addr - sa, nxt - sa, typ

# Parse flat overhead lines: "     2.84%         10838  nfsu2_bench  nfsu2_bench           [.] 0x0000000001ea364f"
pat = re.compile(
    r"^\s*([0-9.]+)%\s+(\d+)\s+\S+\s+(\S+)\s+\[\.\]\s+0x([0-9a-fA-F]+)"
)
rows = []
for line in REPORT.read_text(errors="replace").splitlines():
    m = pat.match(line)
    if not m:
        continue
    pct, samples, dso, hx = m.groups()
    addr = int(hx, 16)
    info = lookup(addr)
    rows.append((float(pct), int(samples), dso, addr, info))

# Aggregate by function
by_fn = defaultdict(lambda: [0.0, 0])
for pct, samples, dso, addr, info in rows:
    name = info[0] if info else f"0x{addr:x}"
    by_fn[name][0] += pct
    by_fn[name][1] += samples

print("=== top by function (sum of flat % from report lines) ===")
for name, (pct, samples) in sorted(by_fn.items(), key=lambda kv: -kv[1][0])[:30]:
    print(f"{pct:6.2f}%  samples~{samples:6d}  {name}")

print("\n=== top IPs ===")
for pct, samples, dso, addr, info in rows[:25]:
    if info:
        sn, off, size, typ = info
        print(f"{pct:5.2f}%  {samples:6d}  0x{addr:x}  {sn}+0x{off:x}  size~{size}  {dso}")
    else:
        print(f"{pct:5.2f}%  {samples:6d}  0x{addr:x}  ?  {dso}")

# addr2line for the hottest guest subs
hots = [name for name, _ in sorted(by_fn.items(), key=lambda kv: -kv[1][0])[:15]]
print("\n=== addr2line ===")
for name in hots:
    # find symbol address
    for sa, typ, sn in syms:
        if sn == name:
            out = subprocess.check_output(
                ["addr2line", "-e", str(BIN), "-f", "-C", "-p", hex(sa)],
                text=True, errors="replace",
            ).strip()
            print(f"{name} @ {hex(sa)}: {out}")
            break
