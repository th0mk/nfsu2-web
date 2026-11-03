#!/usr/bin/env python3
"""Resolve hot addresses from perf report / race.summary into function names."""
import re, subprocess, sys
from collections import defaultdict
from pathlib import Path

OUT = Path.home() / "nfsu2-linux-build"
BIN = OUT / "nfsu2_bench"
REPORT = OUT / "perf" / "race.report.txt"
DATA = OUT / "perf" / "race.perf.data"

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
    syms.append((a, parts[2]))

def lookup(addr: int):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    if best is None:
        return ("?", 0, 0)
    sa, sn = syms[best]
    nxt = syms[best + 1][0] if best + 1 < len(syms) else sa + 1
    return (sn, addr - sa, nxt - sa)

# Prefer aggregating via perf script (more accurate than parsing report).
agg = defaultdict(float)
samples = 0
try:
    proc = subprocess.Popen(
        ["perf", "script", "-i", str(DATA), "--fields", "ip,sym,dso"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace",
    )
    for line in proc.stdout:
        # formats vary; take first hex token as IP
        m = re.search(r"\b([0-9a-fA-F]{6,})\b", line)
        if not m:
            continue
        ip = int(m.group(1), 16)
        name, off, size = lookup(ip)
        # Fold huge generated shards by base name only.
        agg[name] += 1.0
        samples += 1
        if samples >= 250000:  # cap for speed
            break
    proc.kill()
except Exception as e:
    print("perf script failed:", e, file=sys.stderr)

# Also resolve the flat-report top addresses as a fallback.
addrs = []
if REPORT.exists():
    for line in REPORT.read_text(errors="replace").splitlines():
        m = re.search(r"\[\.\]\s+0x([0-9a-fA-F]+)", line)
        if m:
            addrs.append(int(m.group(1), 16))

print(f"samples_aggregated={samples}")
print("=== top functions (aggregated) ===")
total = sum(agg.values()) or 1.0
for name, n in sorted(agg.items(), key=lambda kv: -kv[1])[:40]:
    print(f"{100.0 * n / total:6.2f}%  {int(n):7d}  {name}")

print("=== report address resolve ===")
seen = set()
for a in addrs:
    name, off, size = lookup(a)
    key = name
    if key in seen:
        continue
    seen.add(key)
    print(f"0x{a:x} -> {name}+0x{off:x} (fn~{size})")
    if len(seen) >= 25:
        break
