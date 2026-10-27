#!/usr/bin/env python3
"""Translate all 23k functions to C, split them into shards, generate the function table."""
import sys, os, struct, collections, time, multiprocessing as mp
sys.path.insert(0,'tools')
import pefile
from capstone import *
from capstone.x86 import *
from lift import Lifter, Unsupported, _lregs, _FTOL2_VA

EXE = sys.argv[1]
NSHARD = int(sys.argv[2]) if len(sys.argv)>2 else 128
# GENOUT: output directory (default gen/all); COMMENTS=1: every x86 instruction is
# written as a comment (same code; for instruction-level profiles, see tools/lift_profile.py)
OUT = os.environ.get('GENOUT', os.path.join(os.environ.get('GEN_DIR', 'gen'), 'all'))
os.makedirs(OUT, exist_ok=True)
# Second guest modules (server.dll, tools/prepare_module.py): RELOCS = the
# module's fixup slots, SEEDS = extra entry points (exports), SYMPFX = prefix
# of the generated tables (g_<pfx>fn_count/addr/ptr), so a module's fntab.c
# links next to the exe's.  Defaults reproduce the exe's output unchanged.
# GEN_DIR and OUT_DIR (gen.sh, from games/<id>/game.conf) put the exe's
# default paths under the game's folders (UG2: gen/all, out/relocs.txt).
RELOCS = os.environ.get('RELOCS', os.path.join(os.environ.get('OUT_DIR', 'out'), 'relocs.txt'))
SEEDS = os.environ.get('SEEDS', '')
SYMPFX = os.environ.get('SYMPFX', '')

# ---------- Phase 1: closed function set ----------
def discover_all():
    L=Lifter(EXE); pe=pefile.PE(EXE); IB=pe.OPTIONAL_HEADER.ImageBase
    EP=IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint
    def rd32(a):
        for nm,va,vs,d in L.secs:
            if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
    funcs={EP}
    if SEEDS:
        funcs |= {a for a in (int(x,16) for x in open(SEEDS) if x.strip()) if L.in_text(a)}
    fixed=set(funcs)                       # entries no jump table explains
    relref=collections.defaultdict(list)   # pointer -> the fixup slots holding it
    for a in (int(x,16) for x in open(RELOCS)):
        v=rd32(a)
        if v is not None and L.in_text(v): funcs.add(v); relref[v].append(a)
    md=L.md; cov=bytearray(L.TSZ); q=collections.deque(funcs); seen=set(funcs)
    while q:
        va=q.popleft()
        while True:
            if not L.in_text(va): break
            off=va-L.TVA
            if off>=L.TSZ or cov[off]: break
            try: i=next(md.disasm(L.TDATA[off:off+16],va))
            except StopIteration: break
            for k in range(i.size):
                if off+k<L.TSZ: cov[off+k]=1
            m=i.mnemonic; o=i.operands[0] if i.operands else None
            if m=='call':
                if o is not None and o.type==X86_OP_IMM and L.in_text(o.imm):
                    fixed.add(o.imm)
                    if o.imm not in seen: seen.add(o.imm); funcs.add(o.imm); q.append(o.imm)
                va=i.address+i.size; continue
            if m=='jmp':
                if o is not None and o.type==X86_OP_IMM: va=o.imm; continue
                break
            if m.startswith('j'):
                if o is not None and o.type==X86_OP_IMM and o.imm not in seen:
                    seen.add(o.imm); q.append(o.imm)
                va=i.address+i.size; continue
            if m in ('ret','retf','int3','hlt','ud2'): break
            va=i.address+i.size
    return sorted(funcs), fixed, relref

# JTSWITCH=1: a pointer whose every fixup slot lies in a jump table the lifter
# turns into a switch (lift.py) is a block of the functions that contain the
# switch, not an entry of its own.  Without this, every table target became a
# function: a copy of the rest of its parent's body that only the switch's
# dispatcher tail call reached.  The entry point, the seeds, call targets and
# the runtime's overrides stay entries whatever points at them.
def _jt_left(L, blocks):
    """tables of the 'jmp [r*4 + table]'s in blocks that L's last lift left to the dispatcher"""
    left = []
    for ins in blocks.values():
        if not ins: continue
        i = ins[-1]
        if i.mnemonic == 'jmp' and i.operands and i.operands[0].type == X86_OP_MEM and i.address not in L.jtab:
            m = i.operands[0].mem
            if m.index and m.scale == 4 and not m.base: left.append(m.disp & 0xffffffff)
    return left

def _jt_tables(a):
    """(tables the lift turns into switches, tables it leaves to the dispatcher)"""
    try:
        blocks, _ = _L.discover(a)
    except Exception:
        return [], []
    return [(d, tuple(ents)) for (_r, d, ents) in _L.jtab.values()], _jt_left(_L, blocks)

def drop_table_targets(funcs, fixed, relref):
    # a table counts only if EVERY function that jumps through it switches: one
    # that dispatches instead still needs the targets as functions
    tabs = set(); left = set()
    with mp.Pool(mp.cpu_count(), initializer=_init) as pool:
        for sw, lf in pool.imap_unordered(_jt_tables, funcs, chunksize=64): tabs.update(sw); left.update(lf)
    tabs = {d: ents for d, ents in tabs if d not in left}
    slots = set()
    for d, ents in tabs.items(): slots.update(range(d, d + 4 * len(ents), 4))
    drop = {v for v in funcs if v not in fixed and v not in OVERRIDES and relref.get(v) and all(a in slots for a in relref[v])}
    print(f"  [jtswitch] {len(tabs)} jump tables; {len(drop)} entries reached only through them are not functions")
    return [v for v in funcs if v not in drop], tabs

# ---------- Phase 2: parallel generation ----------
_L=None
# INLINE=<n>: "leaf" functions that make no calls and have at most n x86
# instructions go into every shard as static inline (always_inline) copies, and
# direct calls are redirected to them.  The fixed cost per call (saving registers
# on entry/exit, writing c-> fields to memory and reading them back) is as large
# as the work itself in leaf calls.  The original sub_X definitions stay as they
# are (fntab, indirect calls).
INLINE_MAX = int(os.environ.get('INLINE','0') or 0)
_INL = {}
import re as _re
_INL_CALL = _re.compile(r'\bsub_([0-9a-f]{8})\(c\)')
_INL_DENY = ('dispatch_indirect', 'DISPATCH_CALL', 'DISPATCH_TAIL', 'unsupported_insn', 'cpuid_shim', 'guest_hook', 'rdtsc_shim', 'cov_hit')
# INLINE_LIST=<file>: these functions (hex addresses; # comments) are inlined
# the same way at their direct call sites, whatever their size, calls
# included (an inline copy calls its callees as usual): the hot callees of a
# profile (tools/inline_pick.py).  Not inlined: a tail jump (it would leave
# the caller), a run-time helper call (hooks, traps, CPUID), an override, a
# function that calls itself.
INLINE_LIST = set()
if os.environ.get('INLINE_LIST'):
    INLINE_LIST = {int(x, 16) for ln in open(os.environ['INLINE_LIST']) for x in ln.split('#')[0].replace(',', ' ').split()}
_INL_LIST_DENY = ('DISPATCH_TAIL', 'unsupported_insn', 'cpuid_shim', 'guest_hook', 'rdtsc_shim', 'cov_hit')
OVERRIDES = {int(x,16) for x in os.environ.get('OVERRIDE','').replace('0x','').split(',') if x.strip()}
# OPTNONE=<hex addresses> (gen.sh, a module's game.conf list such as
# D3DX_OPTNONE): these functions are compiled without optimisation
# (optnone, never inlined).  MW's D3DX oracle has one of 2,568 blocks that
# clang had not compiled after 10 minutes at -O3; unoptimised, its whole shard
# takes 11 s.  An address that is not a function of the lift is refused.
# Empty: every function as before.
OPTNONE = {int(x,16) for x in os.environ.get('OPTNONE','').replace('0x','').split(',') if x.strip()}
LREGS = os.environ.get('LREGS', '') == '1'      # lift.py runs tools/lregs.py on every function

def _init(inl=None):
    global _L, _INL
    if inl: _INL = inl
    hooks = [int(x,16) for x in os.environ.get('HOOKS','').replace('0x','').split(',') if x.strip()]
    watch = os.environ.get('WATCH','') == '1'
    cov   = os.environ.get('COVERAGE','') == '1'
    _L = Lifter(EXE, comments=os.environ.get('COMMENTS','') == '1', trap=True, hooks=hooks, watch=watch)
    _L.cov = cov

def _inlprobe(a):
    """leaf + small + not an override -> (address, code) or (address, None)"""
    if a in OVERRIDES or a in OPTNONE: return a, None
    _L.traps = 0
    try:
        code, tg = _L.lift(a)
    except Exception:
        return a, None
    if a in INLINE_LIST:
        if any(y in code for y in _INL_LIST_DENY) or _L.traps or f"sub_{a:08x}(c)" in code.split('{', 1)[1]: return a, None
        return a, code
    m = _re.search(r'\((\d+) insns,', code)
    if not m or int(m.group(1)) > INLINE_MAX: return a, None
    if _INL_CALL.search(code.split('{', 1)[1]) or any(y in code for y in _INL_DENY): return a, None
    if _L.traps: return a, None
    return a, code

def _shard(args):
    idx, addrs = args
    L=_L; parts=[]; defined=[]; refs=set(); traps=0; trunc=0; kinds=collections.Counter(); used=set(); left=set()
    for a in addrs:
        L.traps=0; L.truncated=0
        try:
            code,tg = L.lift(a)
            left.update(_jt_left(L, L.last_blocks))
        except Exception as e:
            stub = f"void sub_{a:08x}(CPU *c){{ unsupported_insn(c,0x{a:x}u,\"lift-error\"); POP32(c->eip); }}"
            parts.append(_lregs().transform(stub) if LREGS else stub)
            defined.append(a); kinds[f"<lift:{type(e).__name__}>"]+=1; continue
        if a in OVERRIDES:
            code = code.replace(f"void sub_{a:08x}(CPU *restrict c) {{",
                                f"void sub_{a:08x}_orig(CPU *restrict c) {{")
        if a in OPTNONE:
            code = code.replace(f"void sub_{a:08x}(CPU *restrict c) {{",
                                f"__attribute__((optnone, noinline)) void sub_{a:08x}(CPU *restrict c) {{")
        if _INL:
            hdr, body = code.split('{', 1)
            body = _INL_CALL.sub(lambda m: (used.add(int(m.group(1), 16)) or f"inl_{m.group(1)}(c)")
                                 if int(m.group(1), 16) in _INL else m.group(0), body)
            code = hdr + '{' + body
        parts.append(code); defined.append(a)
        refs |= {t for t in tg if L.in_text(t)}
        traps += L.traps; trunc += L.truncated
        kinds.update(L.trap_kinds); L.trap_kinds.clear()
    path=f"{OUT}/shard_{idx:04d}.c"
    with open(path,'w') as f:
        f.write((_lregs().SHARD_DEFINE if LREGS else '') + '#include "cpu.h"\n#include "decls.h"\n#include <math.h>\n\n')
        for x in sorted(used):
            f.write(_INL[x].replace(f"void sub_{x:08x}(CPU *restrict c) {{",
                    f"static inline __attribute__((always_inline)) void inl_{x:08x}(CPU *restrict c) {{") + "\n")
        f.write("\n".join(parts))
        f.write("\n")
    return idx, defined, sorted(refs), traps, trunc, dict(kinds), os.path.getsize(path), sorted(left)

if __name__=='__main__':
    t0=time.time()
    print("Phase 1: discovering the function set...")
    funcs, _fixed, _relref = discover_all()
    jt_tabs = {}
    if os.environ.get('JTSWITCH', '') == '1': funcs, jt_tabs = drop_table_targets(funcs, _fixed, _relref)
    print(f"  {len(funcs)} functions  ({time.time()-t0:.0f}s)")

    # extra shards / stubs.c left over from earlier runs must not clash with the new output
    import glob as _glob
    for _p in _glob.glob(f"{OUT}/shard_*.c"):
        if int(os.path.basename(_p)[6:10]) >= NSHARD: os.remove(_p)
    if os.path.exists(f"{OUT}/stubs.c"): os.remove(f"{OUT}/stubs.c")
    chunks=[(i, funcs[i::NSHARD]) for i in range(NSHARD)]
    hk=os.environ.get('HOOKS',''); wt=os.environ.get('WATCH','')
    if OVERRIDES: print(f"  [override: {[hex(a) for a in sorted(OVERRIDES)]}]")
    if OPTNONE: print(f"  [optnone: {[hex(a) for a in sorted(OPTNONE)]}]")
    print(f"\nPhase 2: generating {NSHARD} shards on {mp.cpu_count()} cores..."
          + (f"  [hooks: {hk}]" if hk else "")
          + ("  [COVERAGE measurement]" if os.environ.get('COVERAGE','')=='1' else "") + ("  [WATCHPOINT instrumentation]" if wt=='1' else ""))
    t1=time.time()
    inl = {}
    if INLINE_MAX or INLINE_LIST:
        with mp.Pool(mp.cpu_count(), initializer=_init) as pool:
            for a, code in pool.imap_unordered(_inlprobe, funcs if INLINE_MAX else [f for f in funcs if f in INLINE_LIST], chunksize=64):
                if code: inl[a] = code
        print(f"  [inline: {len(inl)} functions: {len(INLINE_LIST & set(inl))} of the {len(INLINE_LIST)} in INLINE_LIST"
              + (f", leaf functions <= {INLINE_MAX} instructions" if INLINE_MAX else "") + f"]  ({time.time()-t1:.0f}s)")
    defined=set(); refs=set(); traps=0; trunc=0; kinds=collections.Counter(); bytes_=0; jt_left=set()
    with mp.Pool(mp.cpu_count(), initializer=_init, initargs=(inl,)) as pool:
        for n,(idx,d,r,tp,tr,kk,sz,lf) in enumerate(pool.imap_unordered(_shard, chunks)):
            defined |= set(d); refs |= set(r); traps+=tp; trunc+=tr; jt_left |= set(lf)
            kinds.update(kk); bytes_+=sz
            if (n+1)%16==0: print(f"  {n+1}/{NSHARD} shards  ({time.time()-t1:.0f}s)")
    print(f"  generation done ({time.time()-t1:.0f}s)")

    # JTSWITCH: a table whose targets were dropped but that a generated function
    # (a closure function, say) still dispatches through needs its targets back
    jt_need = lambda: {t for d in jt_left if d in jt_tabs for t in jt_tabs[d]}
    missing = sorted((refs | jt_need()) - defined)
    print(f"\n  defined: {len(defined)}   referenced: {len(refs)}   MISSING: {len(missing)}")
    # Closure (v12na port): Phase 1 shares ONE coverage map across all
    # functions, so a misaligned decode can mark the bytes of a real function
    # as covered and its start is never queued.  The per-function CFG of the
    # lifter still sees the direct call, which used to end up as a trapping
    # stub (calls silently did nothing).  Lift those transitively into extra
    # shards (shard_<NSHARD+k>.c); only what fails to lift stays a stub.
    k = 0
    if missing and os.environ.get('NOCLOSURE', '') != '1':
        _init(inl)
        while missing and k < 16:
            idx, d, r, tp, tr, kk, sz, lf = _shard((NSHARD + k, missing))
            defined |= set(d); refs |= set(r); traps += tp; trunc += tr; jt_left |= set(lf)
            kinds.update(kk); bytes_ += sz; k += 1
            print(f"  [closure] shard_{idx:04d}.c: {len(d)} missed functions lifted: "
                  + " ".join(f"{a:x}" for a in d[:12]))
            missing = sorted((refs | jt_need()) - defined)
    # an OPTNONE address that names no lifted function marks nothing: a stale list
    if OPTNONE - defined:
        sys.exit("OPTNONE: " + ", ".join(f"{a:#x}" for a in sorted(OPTNONE - defined)) + ": not a function of this lift")
    if missing:
        with open(f"{OUT}/stubs.c",'w') as f:
            f.write('#include "cpu.h"\n#include "decls.h"\n\n')
            for a in missing:
                f.write(f"void sub_{a:08x}(CPU *c){{ unsupported_insn(c,0x{a:x}u,\"stub\"); POP32(c->eip); }}\n")
        print(f"  -> stubs.c ({len(missing)} stubs)")

    allf = sorted(defined | set(missing))
    with open(f"{OUT}/decls.h",'w') as f:
        f.write("#ifndef DECLS_H\n#define DECLS_H\n#include \"cpu.h\"\n")
        f.write("void dispatch_indirect(CPU*,uint32_t);\nvoid unsupported_insn(CPU*,uint32_t,const char*);\n")
        f.write("void cpuid_shim(CPU*);\nuint64_t rdtsc_shim(void);\n")
        for a in allf: f.write(f"void sub_{a:08x}(CPU*);\n")
        # only this module's overrides (gen.sh passes one list to every module)
        _Lm = Lifter(EXE)
        for a in sorted(OVERRIDES):
            if _Lm.in_text(a): f.write(f"void sub_{a:08x}_orig(CPU*);\n")
        f.write("#endif\n")
    with open(f"{OUT}/fntab.c",'w') as f:
        f.write('#include "cpu.h"\n#include "decls.h"\ntypedef void(*guest_fn)(CPU*);\n')
        f.write(f"const uint32_t g_{SYMPFX}fn_count = {len(allf)};\n")
        f.write(f"const uint32_t g_{SYMPFX}fn_addr[] = {{\n")
        for i in range(0,len(allf),8): f.write("  "+",".join(f"0x{a:08x}u" for a in allf[i:i+8])+",\n")
        f.write(f"}};\nconst guest_fn g_{SYMPFX}fn_ptr[] = {{\n")
        for i in range(0,len(allf),4): f.write("  "+",".join(f"sub_{a:08x}" for a in allf[i:i+4])+",\n")
        f.write("};\n")
    # gameinfo.h: the exe's values the runtime is built with (runtime/cpu.h
    # includes it): its image, the .text the dispatch table covers, the entry
    # point, the C runtime's _ftol2.  Not for a second module (SYMPFX).
    if not SYMPFX:
        oh = pefile.PE(EXE, fast_load=True).OPTIONAL_HEADER; ep = oh.ImageBase + oh.AddressOfEntryPoint
        with open(f"{OUT}/gameinfo.h",'w') as f:
            f.write("/* The lifted exe, as the runtime is built for it: written by tools/genall.py\n"
                    " * with the shards, included by runtime/cpu.h (without this file it takes\n"
                    " * SPEED2.EXE's values from runtime/gameinfo_ug2.h). */\n"
                    "#ifndef GAMEINFO_H\n#define GAMEINFO_H\n"
                    f"#define GAMEINFO_IMAGE_BASE 0x{oh.ImageBase:08x}u   /* ImageBase */\n"
                    f"#define GAMEINFO_IMAGE_SIZE 0x{oh.SizeOfImage:08x}u   /* SizeOfImage: the image ends at 0x{oh.ImageBase + oh.SizeOfImage:08x} */\n"
                    f"#define GAMEINFO_TEXT_BASE  0x{_Lm.TVA:08x}u   /* .text: one dispatch table slot per byte */\n"
                    f"#define GAMEINFO_TEXT_SIZE  0x{_Lm.TSZ:08x}u\n"
                    f"#define GAMEINFO_ENTRY      0x{ep:08x}u   /* the entry point, lifted as GAMEINFO_ENTRY_FN */\n"
                    f"#define GAMEINFO_ENTRY_FN   sub_{ep:08x}\n"
                    f"#define GAMEINFO_FTOL2      0x{_FTOL2_VA:08x}u   /* the C runtime's _ftol2 (FTOL2_VA) */\n"
                    "#endif\n")
    print(f"\n=== GENERATION SUMMARY ({time.time()-t0:.0f}s) ===")
    print(f"  functions        : {len(allf)}")
    print(f"  C source size    : {bytes_/1048576:.1f} MB ({NSHARD} shards)")
    print(f"  traps (unsupported instructions): {traps}")
    print(f"  truncated branch targets    : {trunc}")
    print("\n  most common trap reasons:")
    for k,v in collections.Counter(kinds).most_common(12): print(f"    {k:16s} {v}")
