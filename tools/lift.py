#!/usr/bin/env python3
"""x86-32 -> C lifter.  Translates a function into C that runs on the CPU state."""
import sys, struct, collections
import pefile
from capstone import *
from capstone.x86 import *

R32 = {X86_REG_EAX:'eax',X86_REG_ECX:'ecx',X86_REG_EDX:'edx',X86_REG_EBX:'ebx',
       X86_REG_ESP:'esp',X86_REG_EBP:'ebp',X86_REG_ESI:'esi',X86_REG_EDI:'edi'}
R16 = {X86_REG_AX:'eax',X86_REG_CX:'ecx',X86_REG_DX:'edx',X86_REG_BX:'ebx',
       X86_REG_SP:'esp',X86_REG_BP:'ebp',X86_REG_SI:'esi',X86_REG_DI:'edi'}
R8L = {X86_REG_AL:'eax',X86_REG_CL:'ecx',X86_REG_DL:'edx',X86_REG_BL:'ebx'}
R8H = {X86_REG_AH:'eax',X86_REG_CH:'ecx',X86_REG_DH:'edx',X86_REG_BH:'ebx'}
CC  = {'o':'CC_O','no':'CC_NO','b':'CC_B','ae':'CC_AE','e':'CC_E','ne':'CC_NE',
       'be':'CC_BE','a':'CC_A','s':'CC_S','ns':'CC_NS','p':'CC_P','np':'CC_NP',
       'l':'CC_L','ge':'CC_GE','le':'CC_LE','g':'CC_G',
       'nae':'CC_B','nb':'CC_AE','c':'CC_B','nc':'CC_AE','z':'CC_E','nz':'CC_NE',
       'na':'CC_BE','nbe':'CC_A','pe':'CC_P','po':'CC_NP','nge':'CC_L','nl':'CC_GE',
       'ng':'CC_LE','nle':'CC_G'}


# ---- flag liveness ------------------------------------------------------
# Every arithmetic instruction used to compute all six flags up front; the next
# branch usually reads only one.  We solve liveness BACKWARDS over the function's
# control-flow graph and give every instruction the mask "which flags are read
# after this one"; since the mask is a compile-time constant, the unused
# branches are removed entirely.
F_CF,F_PF,F_AF,F_ZF,F_SF,F_OF = 0x01,0x02,0x04,0x08,0x10,0x20
F_ALL = 0x3F
# CAUTION: UNDEFINED_* do not count as "definitions".  If they did, we would take
# the flag as dead and skip the earlier computation; yet our lift may not write
# that flag -> stale value.
_EFTAB = [
    (F_CF, X86_EFLAGS_MODIFY_CF|X86_EFLAGS_RESET_CF|X86_EFLAGS_SET_CF,
           X86_EFLAGS_TEST_CF|X86_EFLAGS_PRIOR_CF),
    (F_PF, X86_EFLAGS_MODIFY_PF|X86_EFLAGS_RESET_PF|X86_EFLAGS_SET_PF,
           X86_EFLAGS_TEST_PF|X86_EFLAGS_PRIOR_PF),
    (F_AF, X86_EFLAGS_MODIFY_AF|X86_EFLAGS_RESET_AF|X86_EFLAGS_SET_AF,
           X86_EFLAGS_TEST_AF|X86_EFLAGS_PRIOR_AF),
    (F_ZF, X86_EFLAGS_MODIFY_ZF|X86_EFLAGS_RESET_ZF|X86_EFLAGS_SET_ZF,
           X86_EFLAGS_TEST_ZF|X86_EFLAGS_PRIOR_ZF),
    (F_SF, X86_EFLAGS_MODIFY_SF|X86_EFLAGS_RESET_SF|X86_EFLAGS_SET_SF,
           X86_EFLAGS_TEST_SF|X86_EFLAGS_PRIOR_SF),
    (F_OF, X86_EFLAGS_MODIFY_OF|X86_EFLAGS_RESET_OF|X86_EFLAGS_SET_OF,
           X86_EFLAGS_TEST_OF|X86_EFLAGS_PRIOR_OF),
]
# Call: the callee gets the CPU, it CAN READ and CLOBBER every flag.
# Indirect branch/ret/int are just as conservative.
# capstone NEVER reports "reads flags" for these instructions.  Left out, we
# would take the flag as dead and skip its computation -> the instruction
# reads garbage.
_EXTRA_USE = {
    'pushf': F_ALL, 'pushfd': F_ALL, 'pushfw': F_ALL,
    'lahf':  F_CF|F_PF|F_AF|F_ZF|F_SF,
    'salc':  F_CF, 'setalc': F_CF,
    'rcl':   F_CF, 'rcr': F_CF,
    'cmc':   F_CF,
    'daa': F_CF|F_AF, 'das': F_CF|F_AF,
    'aaa': F_CF|F_AF, 'aas': F_CF|F_AF,
    'into':  F_OF,
}
# Instructions where our lifter REALLY writes fewer flags than capstone
# claims.  A spurious "writes" means: we kill the flag above but do not write
# it here either -> stale value.  imul/mul write only CF+OF.
_DEF_CLAMP = {'mul': F_CF|F_OF, 'imul': F_CF|F_OF}

# x86: shifts/rotates do NOT TOUCH the flags when the COUNT is 0 (the lifted
# body is guarded by "if(_s){...}" too).  capstone still says "writes"; if we
# followed it and killed the flag above, a count of 0 would leave garbage.
_SHIFT = ('shl','sal','shr','sar')
_ROT   = ('rol','ror','rcl','rcr')
_DSH   = ('shld','shrd')

def _shift_count(i, base):
    """Constant shift count (0-31); None if it is variable (cl)."""
    o = i.operands
    if base in _DSH:
        return (o[2].imm & 31) if len(o) > 2 and o[2].type == X86_OP_IMM else None
    if len(o) > 1:
        return (o[1].imm & 31) if o[1].type == X86_OP_IMM else None
    return 1                      # one-operand form: the count is a constant 1

_OPAQUE = ('call','ret','retf','int','int3','iret','iretd',
           'sysenter','syscall','ud2','hlt','arpl')

# x87: capstone REUSES the `eflags` field for the FPU status bits (C0-C3)
# -- the same numeric values (fld -> 0xd002, fcom -> 0x2d).  Taken as they
# are, every fld counts as "wrote CF", every fcom as "wrote ZF/SF/AF/OF"; we
# would take those flags as dead above and skip the real computation -> stale value.
# Only the fcomi/fucomi family really writes EFLAGS (the FCOMI macro writes all 6).
_FCOMI = ('fcomi','fcomip','fucomi','fucomip')
# fcmovcc READS EFLAGS (v12na); it got lost in the 'f' branch.
_FCMOV_USE = {'fcmovb': F_CF, 'fcmovnb': F_CF, 'fcmove': F_ZF, 'fcmovne': F_ZF,
              'fcmovbe': F_CF|F_ZF, 'fcmovnbe': F_CF|F_ZF, 'fcmovu': F_PF, 'fcmovnu': F_PF}

def _flag_defuse(i):
    if i.mnemonic in _OPAQUE:
        return 0, F_ALL
    base = i.mnemonic.split()[-1]          # "repe cmpsb" -> "cmpsb"
    if base[:1] == 'f' or X86_GRP_FPU in i.groups:
        if base in _FCMOV_USE: return 0, _FCMOV_USE[base]
        return (F_ALL, 0) if base in _FCOMI else (0, 0)
    ef = getattr(i, 'eflags', 0) or 0
    d = u = 0
    for bit, dm, um in _EFTAB:
        if ef & dm: d |= bit
        if ef & um: u |= bit
    u |= _EXTRA_USE.get(base, 0)
    if base in _DEF_CLAMP: d &= _DEF_CLAMP[base]
    if base in _SHIFT or base in _ROT or base in _DSH:
        n = _shift_count(i, base)
        if not n:                 # count 0, or not known at compile time
            d = 0                 # -> may write no flag at all
        elif base in _ROT:
            # rol/ror with a constant count: CF always, OF only when count==1.
            # rcl/rcr never write OF in the lift.
            d &= F_CF if base in ('rcl','rcr') else (F_CF | (F_OF if n == 1 else 0))
    if i.mnemonic.split()[0] in ('rep','repe','repz','repne','repnz'):
        d = 0                     # if ECX==0 the body never runs, the flags are kept
    return d, u

# ---- FLAGABI: inter-procedural flag liveness (see tools/flagabi.py) ----
# On x86, reading a flag left "undefined" has no meaningful result: in ABI mode
# undefined flags count as DEFINED as well.  Otherwise and/test/xor never kill
# AF; AF spreads backwards from an indirect call / outer edge source and looks
# "live" at almost every call site (measured: 3171 bogus "flag-returning"
# functions, all of them AF only).
_EFUNDEF = [(F_CF, X86_EFLAGS_UNDEFINED_CF), (F_PF, X86_EFLAGS_UNDEFINED_PF), (F_AF, X86_EFLAGS_UNDEFINED_AF),
            (F_ZF, X86_EFLAGS_UNDEFINED_ZF), (F_SF, X86_EFLAGS_UNDEFINED_SF), (F_OF, X86_EFLAGS_UNDEFINED_OF)]
def _flag_defuse_abi(i):
    d, u = _flag_defuse(i)
    if i.mnemonic in _OPAQUE: return d, u
    base = i.mnemonic.split()[-1]
    if base[:1] == 'f' or X86_GRP_FPU in i.groups: return d, u
    if base in _SHIFT or base in _ROT or base in _DSH:
        return d, u                  # the count may be 0: do not kill the undefined ones
    if i.mnemonic.split()[0] in ('rep','repe','repz','repne','repnz'):
        return d, u
    ef = getattr(i, 'eflags', 0) or 0
    for bit, um in _EFUNDEF:
        if ef & um: d |= bit
    return d, u

def abi_cfg(blocks, in_text, jtab=None):
    """Compact control-flow graph for FLAGABI: block -> (successors, ext_kind, insns)
    ext_kind: 0 none, 1 unknown outer edge (all flags live), 2 indirect jmp
    (virtual tail call / switch table: dead under the ABI).
    insn: (address, def, use, kind, target)  kind: 0 normal, 1 direct call,
    2 indirect call, 3 ret."""
    g = {}
    for ba in blocks:
        ins = blocks[ba]
        if not ins:
            g[ba] = ([], 1, []); continue
        last = ins[-1]; m = last.mnemonic
        s = []; ext = 0
        cond = (m.startswith('j') and m[1:] in CC) or m in ('jecxz','jcxz','loop','loope','loopz','loopne','loopnz')
        if m == 'jmp' or cond:
            o = last.operands[0] if last.operands else None
            if o is not None and o.type == X86_OP_IMM and o.imm in blocks: s.append(o.imm)
            elif m == 'jmp' and (o is None or o.type != X86_OP_IMM):
                ext = 2
                if jtab and last.address in jtab: s += [t for t in dict.fromkeys(jtab[last.address][2]) if t in blocks]
            else: ext = 1
            if cond:
                nxt = last.address + last.size
                if nxt in blocks: s.append(nxt)
                else: ext = 1
        elif m in ('ret','retf','iret','iretd'):
            pass
        else:
            nxt = last.address + last.size
            if nxt in blocks: s.append(nxt)
            else: ext = 1
        kl = []
        for i in ins:
            mm = i.mnemonic
            if mm == 'call':
                o = i.operands[0] if i.operands else None
                if o is not None and o.type == X86_OP_IMM and in_text(o.imm):
                    kl.append((i.address, F_ALL, 0, 1, o.imm))
                else:
                    kl.append((i.address, F_ALL, 0, 2, 0))   # ABI: the callee does not read the caller's flags
            elif mm in ('ret','retn'):
                kl.append((i.address, 0, 0, 3, 0))
            else:
                d, u = _flag_defuse_abi(i)
                kl.append((i.address, d, u, 0, 0))
        g[ba] = (s, ext, kl)
    return g

def abi_liveness(g, ent, callee_in, returns_flags):
    """-> (live at entry, {insn: live after}, {call: (live after, kind, target)})
    call T: defines F_ALL, uses callee_in[T]; ret: F_ALL if returns_flags, else 0."""
    live_in = {ba: 0 for ba in g}
    after = {}; call_after = {}
    order = sorted(g, reverse=True)
    for _ in range(64):
        changed = False
        for ba in order:
            s, ext, kl = g[ba]
            cur = F_ALL if ext == 1 else 0
            for t in s: cur |= live_in.get(t, F_ALL)
            for (addr, d, u, kind, target) in reversed(kl):
                if kind == 3:
                    cur = F_ALL if returns_flags else 0
                after[addr] = cur
                if kind == 3: continue
                if kind in (1, 2):
                    call_after[addr] = (cur, kind, target)
                    if kind == 1: u = callee_in.get(target, 0)
                cur = (cur & ~d) | u
            if cur != live_in[ba]:
                live_in[ba] = cur; changed = True
        if not changed: break
    return live_in.get(ent, F_ALL), after, call_after

_FTOL2INL = __import__('os').environ.get('FTOL2INL', '') == '1'
# the CRT's _ftol2 that FTOL2INL inlines: FTOL2_VA from games/<id>/game.conf (gen.sh), UG2's by default
_FTOL2_VA = int(__import__('os').environ.get('FTOL2_VA', '0x0075BED4'), 16)
# LOCKSTRIP=1 (games/<id>/game.conf, gen.sh): a lock prefix on a read-modify-
# write of memory is lifted as the plain instruction.  capstone names it
# "lock xadd", which matched no handler and trapped (MW's PATH audio library).
# Guest threads run one at a time (the GIL changes hands only at blocking
# waits), so the plain read-modify-write is atomic for them.  A lock on any
# other instruction or on a register destination (#UD on the CPU) is not
# decoded by capstone at all, so discovery ends the block before it, switch
# on or off: no trap is lifted there.  Off for UG2, whose 4 `lock or` sites
# are data decoded as code and stay traps.
_LOCKSTRIP = __import__('os').environ.get('LOCKSTRIP', '') == '1'
_LOCKABLE = ('add', 'adc', 'and', 'btc', 'btr', 'bts', 'cmpxchg', 'cmpxchg8b', 'dec', 'inc', 'neg', 'not',
             'or', 'sbb', 'sub', 'xadd', 'xchg', 'xor')
def _retpast():
    """callees that can return past their caller: the game's list, RETPAST_LIST from
    games/<id>/game.conf (gen.sh; a path in the checkout), UG2's tools/retpast.txt by default"""
    import os as _os
    root = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
    p = _os.path.join(root, _os.environ.get('RETPAST_LIST') or 'tools/retpast.txt')
    return {int(x, 16) for ln in open(p) for x in ln.split('#')[0].replace(',', ' ').split()}
# X87ENV=1 (games/<id>/game.conf, gen.sh): fnstenv/fldenv and fnsave/frstor
# (the 32-bit 28- and 108-byte images) are lifted; without it they are traps.
# MW's x87 pow (0x7C464D, the CRT's special-value path) saves the FPU around
# its helper with fnsave/frstor.  Off for UG2, whose same CRT sites stay traps.
_X87ENV = __import__('os').environ.get('X87ENV', '') == '1'
# CODE_RVA=<lo>-<hi> (hex RVAs; gen.sh, from a module's game.conf range such as
# D3DX_CODE): only [lo, hi) of the module's .text is code.  d3dx9_26.dll's
# linker merged its read-only data into .text: the IAT, constants, strings and
# vtables before the code, the C++ EH tables and the import and export tables
# after it.  Its relocations point at that data thousands of times, and every
# such value would be seeded and lifted as a function (1.1 GB of C).  The rest
# of .text stays readable as data (secs).  Not set: the whole .text is code.
_CODE_RVA = __import__('os').environ.get('CODE_RVA', '')
_LREGS = None
def _lregs():
    """tools/lregs.py (LREGS=1), next to this file"""
    global _LREGS
    if _LREGS is None:
        import importlib.util as _iu, os as _os
        sp = _iu.spec_from_file_location('lregs', _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'lregs.py'))
        _LREGS = _iu.module_from_spec(sp); sp.loader.exec_module(_LREGS)
    return _LREGS

_ABI = None
def abi_table():
    """FLAGABI=<json> (output of tools/flagabi.py) -> (live_in, flags_out) or None"""
    global _ABI
    if _ABI is None:
        import os as _os, json as _json
        p = _os.environ.get('FLAGABI', '')
        if p:
            j = _json.load(open(p))
            _ABI = ({int(k, 16): v for k, v in j['live_in'].items()}, {int(a, 16) for a in j['flags_out']})
        else:
            _ABI = False
    return _ABI or None

def flag_liveness(blocks, addrs, in_text):
    """Instruction address -> the flags live AFTER that instruction."""
    succ = {}
    for ba in addrs:
        ins = blocks[ba]
        if not ins: succ[ba] = ([], True); continue
        last = ins[-1]; m = last.mnemonic
        s = []; ext = False
        cond = (m.startswith('j') and m[1:] in CC) or \
               m in ('jecxz','jcxz','loop','loope','loopz','loopne','loopnz')
        if m == 'jmp' or cond:
            o = last.operands[0] if last.operands else None
            if o is not None and o.type == X86_OP_IMM and o.imm in blocks: s.append(o.imm)
            else: ext = True
            if cond:
                nxt = last.address + last.size
                if nxt in blocks: s.append(nxt)
                else: ext = True
        elif m in ('ret','retf','iret','iretd'):
            # ret is in the _OPAQUE list: all flags count as live on return
            # (hand-written asm may use CF as a return channel).
            # Measured: even with this CONSERVATIVE setting flag writes drop by 53%,
            # so there is no need to loosen the safety.
            ext = True
        else:
            nxt = last.address + last.size
            if nxt in blocks: s.append(nxt)
            else: ext = True
        succ[ba] = (s, ext)
    live_in = {ba: 0 for ba in addrs}
    after = {}
    for _ in range(64):
        changed = False
        for ba in reversed(addrs):
            s, ext = succ[ba]
            lo = F_ALL if ext else 0
            for t in s: lo |= live_in.get(t, F_ALL)
            cur = lo; per = {}
            for i in reversed(blocks[ba]):
                per[i.address] = cur
                d, u = _flag_defuse(i)
                cur = (cur & ~d) | u
            if cur != live_in[ba]:
                live_in[ba] = cur; changed = True
            after.update(per)
        if not changed: break
    return after

class Unsupported(Exception): pass

import os as _os_pc
_X87PC = _os_pc.environ.get('X87PC', '') == '1'   # see "x87 FPU" in lift_insn

# ---- X87OPT: x87 register allocation within a block ----------------------
# ST(n) = c->st[(c->top+n)&7] is indexed at run time: clang can never keep these
# values in registers, every x87 operation is a top read + address + load/store.
# Within a basic block the stack depth is known at compile time: at the start of
# a region _T = c->top, ST(n) -> _x[off+n+8] (constant index -> ARM FP register),
# FPUSH/FPOP only change the static off.  A slot read before it is written is
# loaded once within the region; at the end of the region (branch, call, ret,
# an instruction touching c->st/c->top, end of block) only the modified slots
# still on the stack are written back and c->top is updated once.
import re as _re
_ST_RE = _re.compile(r'ST\((\d+)\)')
_STA_RE = _re.compile(r'ST\((\d+)\)\s*=(?!=)')
_BOUNDARY_RE = _re.compile(r'\b\w+\(c[,)]|\breturn\b|c->st|c->top|\bST\(|FPUSH|FPOP|fsw_get')

class X87Region:
    def __init__(self): self.reset(); self.used = False
    def reset(self):
        self.active = False; self.off = 0; self.wr = set(); self.ld = set()
    def _slot(self, k):
        if not -8 <= k <= 7: raise ValueError('x87 depth')
        return k + 8
    def translate(self, lines):
        """lines of an x87 instruction -> region lines, or None (leave them raw)"""
        text = '\n'.join(lines)
        if 'c->st' in text or 'c->top' in text:
            return None
        saved = (self.active, self.off, set(self.wr), set(self.ld))
        try:
            out = []
            if not self.active:
                out.append('_T = (uint32_t)c->top;'); self.active = True
            for line in lines:
                st = line.strip()
                if st == 'FPOP();':
                    self.off += 1; continue
                pre = []
                if st.startswith('FPUSH(') and st.endswith(');'):
                    expr = self._st(st[6:-2], pre)
                    k = self.off - 1; self._slot(k)
                    self.wr.add(k); self.off -= 1
                    out += pre; out.append(f'_x[{k + 8}] = (double)({expr});'); continue
                line = line.replace('fsw_get(c)', f'fsw_get_top(c, (_T + ({self.off})) & 7u)')
                out += []
                line = self._st(line, pre)
                out += pre; out.append(line)
            self.used = True
            return out
        except ValueError:
            self.active, self.off, self.wr, self.ld = saved
            return None
    def _st(self, line, pre):
        uses = {}; assigned = {}
        for m in _ST_RE.finditer(line): uses[int(m.group(1))] = uses.get(int(m.group(1)), 0) + 1
        for m in _STA_RE.finditer(line): assigned[int(m.group(1))] = assigned.get(int(m.group(1)), 0) + 1
        for n, cnt in uses.items():
            k = self.off + n; self._slot(k)
            if cnt > assigned.get(n, 0) and k not in self.wr and k not in self.ld:
                pre.append(f'_x[{k + 8}] = c->st[(_T + ({k})) & 7];'); self.ld.add(k)
        for n in assigned: self.wr.add(self.off + n)
        off = self.off
        return _ST_RE.sub(lambda m: f'_x[{off + int(m.group(1)) + 8}]', line)
    def close(self):
        if not self.active: return []
        out = [f'c->st[(_T + ({k})) & 7] = _x[{k + 8}];' for k in sorted(self.wr) if k >= self.off]
        if self.off: out.append(f'c->top = (int32_t)((_T + ({self.off})) & 7u);')
        self.reset()
        return out

class RawLifter:
    """Lift over a raw byte buffer: for the instruction semantics tests."""
    def __init__(self, data, base=0x00401000, comments=True):
        self.comments=comments; self.trap=False; self.traps=0
        self.trap_kinds=collections.Counter(); self.hooks=set(); self.watch=False
        self.cov=False; self.retpast=set()
        self.secs=[('.text', base, len(data), bytes(data))]
        self.TVA=base; self.TSZ=len(data); self.TDATA=bytes(data)
        self.md=Cs(CS_ARCH_X86,CS_MODE_32); self.md.detail=True
    in_text = lambda self,a: self.TVA<=a<self.TVA+self.TSZ

class Lifter:
    def __init__(self, pe_path, comments=True, trap=False, hooks=None, watch=False):
        self.comments=comments; self.trap=trap; self.traps=0; self.trap_kinds=collections.Counter()
        self.hooks=set(hooks or ()); self.watch=watch
        self.cov=False
        pe = pefile.PE(pe_path); self.IB = pe.OPTIONAL_HEADER.ImageBase
        self.secs=[]
        for s in pe.sections:
            nm=s.Name.rstrip(b'\x00').decode('latin1').strip() or '<none>'
            self.secs.append((nm,self.IB+s.VirtualAddress,s.Misc_VirtualSize,s.get_data()))
        self.TVA,self.TSZ,self.TDATA = next((va,vs,d) for nm,va,vs,d in self.secs if nm=='.text')
        if _CODE_RVA:
            lo, hi = (self.IB + int(x, 16) for x in _CODE_RVA.split('-'))
            if not self.TVA <= lo < hi <= self.TVA + self.TSZ:
                sys.exit(f"CODE_RVA={_CODE_RVA}: not a range inside .text (0x{self.TVA:08x}-0x{self.TVA + self.TSZ:08x})")
            self.TDATA = self.TDATA[lo - self.TVA:hi - self.TVA]; self.TVA, self.TSZ = lo, hi - lo
        self.md=Cs(CS_ARCH_X86,CS_MODE_32); self.md.detail=True
        # this module's callees from the game's retpast list (a call to one may not come back to the next instruction)
        self.retpast={a for a in _retpast() if self.in_text(a)}
    def in_text(self,a): return self.TVA<=a<self.TVA+self.TSZ

    # ---------- operand -> C expression ----------
    def ea(self, m, extra=0):
        """Effective address.  `extra`: a constant extra offset (the upper half in 128-bit operations)."""
        parts=[]
        # Win32 flat memory model: es/ds/ss/cs have base 0 -> ignored.
        # fs, however, points at the TEB (SEH chain fs:[0], TLS fs:[0x2c]) -> its base is added.
        if m.segment:
            sname = self.md.reg_name(m.segment) or ''
            if sname == 'fs':
                parts.append("c->fs_base")
            elif sname == 'gs':
                parts.append("c->gs_base")
        if m.base and m.base in R32: parts.append(f"c->{R32[m.base]}")
        elif m.base and m.base!=0: raise Unsupported(f"base register {m.base}")
        if m.index:
            if m.index not in R32: raise Unsupported("index register")
            parts.append(f"c->{R32[m.index]}*{m.scale}" if m.scale!=1 else f"c->{R32[m.index]}")
        d = (m.disp + extra) & 0xffffffff
        if d or not parts: parts.append(f"0x{d:x}u")
        return "(" + "+".join(parts) + ")"

    def rd(self, op):
        if op.type==X86_OP_IMM:  return f"0x{op.imm & 0xffffffff:x}u"
        if op.type==X86_OP_REG:
            r=op.reg
            if r in R32: return f"c->{R32[r]}"
            if r in R16: return f"(uint16_t)c->{R16[r]}"
            if r in R8L: return f"(uint8_t)c->{R8L[r]}"
            if r in R8H: return f"(uint8_t)(c->{R8H[r]}>>8)"
            SEG = {X86_REG_ES:0x23,X86_REG_CS:0x1b,X86_REG_SS:0x23,
                   X86_REG_DS:0x23,X86_REG_FS:0x3b,X86_REG_GS:0x00}
            if r in SEG: return f"0x{SEG[r]:x}u"   # flat model: constant selector
            raise Unsupported(f"register {r}")
        if op.type==X86_OP_MEM:
            return {1:'MR8',2:'MR16',4:'MR32',8:'MR64'}[op.size] + self.ea(op.mem)
        raise Unsupported("operand type")

    def wr(self, op, val):
        if op.type==X86_OP_REG:
            r=op.reg
            if r in R32: return f"c->{R32[r]} = (uint32_t)({val});"
            if r in R16: return f"c->{R16[r]} = (c->{R16[r]} & 0xffff0000u) | (uint32_t)(({val})&0xffffu);"
            if r in R8L: return f"c->{R8L[r]} = (c->{R8L[r]} & 0xffffff00u) | (uint32_t)(({val})&0xffu);"
            if r in R8H: return f"c->{R8H[r]} = (c->{R8H[r]} & 0xffff00ffu) | ((uint32_t)(({val})&0xffu)<<8);"
            if r in (X86_REG_ES,X86_REG_CS,X86_REG_SS,X86_REG_DS,X86_REG_FS,X86_REG_GS):
                return f"(void)({val});   /* flat model: segment write has no effect */"
            raise Unsupported(f"register write {r}")
        if op.type==X86_OP_MEM:
            return {1:'MR8',2:'MR16',4:'MR32',8:'MR64'}[op.size] + self.ea(op.mem) + f" = ({val});"
        raise Unsupported("target operand")

    # ---------- jump tables (JTSWITCH=1) ----------
    # MSVC's switch: "cmp r32, n; ja default" (or "jbe table_block") ends one
    # block, the next is "jmp dword [r32*4 + table]" with n+1 entries in .text.
    # Without JTSWITCH the jmp is a tail call through the dispatcher, so every
    # table target is entered as a function of its own: a copy of the rest of
    # the body (sub_00620ae0 has 20 such copies of ~660 KB of wasm each).  With
    # it the targets are blocks of this function and the jmp becomes a C
    # switch (see lift): each case checks the table entry at run time, and
    # anything else still takes the dispatcher, so the semantics do not change.
    # instructions that may sit between the cmp and the ja, and before the jmp:
    # they write no flags, and the check below makes sure not the index either
    _JT_PASS = frozenset(('mov', 'movzx', 'movsx', 'push', 'pop', 'lea', 'nop'))
    def _jt_writes(self, i, fam):
        """does i write any part of the register family fam ('eax', ...)?"""
        try: return any((R32.get(r) or R16.get(r) or R8L.get(r) or R8H.get(r)) == fam for r in i.regs_access()[1])
        except Exception: return True

    def _jt_bound(self, ins):
        """(reg, n, the block the bound holds in) for a block ending in 'cmp r32, n; [...]; ja/jbe'"""
        j = ins[-1]
        if j.mnemonic not in ('ja', 'jbe') or j.operands[0].type != X86_OP_IMM: return None
        between = []
        for k in reversed(ins[:-1]):
            if k.mnemonic == 'cmp':
                o = k.operands
                if len(o) != 2 or o[0].type != X86_OP_REG or o[0].reg not in R32 or o[1].type != X86_OP_IMM: return None
                if any(self._jt_writes(x, R32[o[0].reg]) for x in between): return None
                held = j.address + j.size if j.mnemonic == 'ja' else j.operands[0].imm
                return o[0].reg, o[1].imm & 0xffffffff, held
            if k.mnemonic not in self._JT_PASS: return None
            between.append(k)
        return None

    def _jt_table(self, ins, bound):
        """(index reg, table, [targets]) for a block 'jmp dword [r*4 + table]' under bound
        (r32, n), maybe after 'movzx r2, byte [r32 + bytes]' (MSVC's two-level switch:
        the index is r2, up to the largest byte) and instructions that leave the index
        alone; else None"""
        reg, n = bound
        if not ins or n > 1023: return None
        for k in ins[:-1]:
            if k.mnemonic == 'movzx' and len(k.operands) == 2 and k.operands[1].type == X86_OP_MEM and k.operands[1].size == 1 \
               and k.operands[0].type == X86_OP_REG and k.operands[0].reg in R32:
                m = k.operands[1].mem
                if not m.segment and ((m.base == reg and not m.index) or (not m.base and m.index == reg and m.scale == 1)):
                    bd = m.disp & 0xffffffff
                    if not (self.in_text(bd) and self.in_text(bd + n)): return None
                    n = max(self.TDATA[bd - self.TVA:bd - self.TVA + n + 1]); reg = k.operands[0].reg
                    continue
            if k.mnemonic not in self._JT_PASS or self._jt_writes(k, R32[reg]): return None
        i = ins[-1]
        if i.mnemonic != 'jmp' or not i.operands or i.operands[0].type != X86_OP_MEM or i.operands[0].size != 4: return None
        m = i.operands[0].mem
        if m.segment or m.base or m.index != reg or m.scale != 4: return None
        d = m.disp & 0xffffffff
        if not (self.in_text(d) and self.in_text(d + 4 * n + 3)): return None
        ents = list(struct.unpack_from(f'<{n + 1}I', self.TDATA, d - self.TVA))
        return (reg, d, ents) if all(self.in_text(t) for t in ents) else None

    # ---------- function discovery (CFG) ----------
    def discover(self, entry, maxins=4000):
        blocks={}; q=collections.deque([entry]); seen=set([entry]); calls=set()
        jt = _os_pc.environ.get('JTSWITCH', '') == '1'
        bounds = {}; self.jtab = {}
        while True:
            while q:
                va=q.popleft(); ins=[]
                while True:
                    if not self.in_text(va): break
                    off=va-self.TVA
                    try: i=next(self.md.disasm(self.TDATA[off:off+16],va))
                    except StopIteration: break
                    ins.append(i); m=i.mnemonic
                    if m in ('ret','retf'): break
                    if m=='jmp':
                        o=i.operands[0]
                        if o.type==X86_OP_IMM and self.in_text(o.imm):
                            t=o.imm
                            if t not in seen: seen.add(t); q.append(t)
                        break
                    if (m.startswith('j') and m[1:] in CC) or \
                       m in ('jecxz','jcxz','loop','loope','loopz','loopne','loopnz'):
                        o=i.operands[0]
                        if o.type==X86_OP_IMM:
                            for t in (o.imm, i.address+i.size):
                                if self.in_text(t) and t not in seen: seen.add(t); q.append(t)
                        break
                    if m=='call':
                        o=i.operands[0]
                        if o.type==X86_OP_IMM: calls.add(o.imm)
                    if m == 'int3':
                        # A single int3 = assertion/debug break -> treat it as a no-op, GO ON.
                        # A run of 0xCC = padding between functions -> stop.
                        off2 = va + i.size - self.TVA
                        pad = self.TDATA[off2:off2+3]
                        if pad == b'\xcc\xcc\xcc': break
                        va = i.address + i.size
                        continue
                    if m in ('hlt','ud2'): break
                    va=i.address+i.size
                    if len(ins)>maxins: break
                blocks[ins[0].address if ins else va]=ins
                if jt and ins:
                    b = self._jt_bound(ins)
                    if b: bounds[b[2]] = b[:2]
            # table jumps whose block's bound is known by now: their targets join
            new = False
            for ba, ins in list(blocks.items()):
                if not (jt and ins and ba in bounds and ins[-1].address not in self.jtab): continue
                tab = self._jt_table(ins, bounds[ba])
                if tab is None: continue
                self.jtab[ins[-1].address] = tab
                for t in tab[2]:
                    if t not in seen: seen.add(t); q.append(t); new = True
            if not new: break
        return blocks, calls

    # ---------- single instruction -> C ----------
    def _fm(self):
        """Mask of the flags LIVE after this instruction.  When it is not set
        (RawLifter / tests), conservatively all of them."""
        return "0x%02xu" % getattr(self, 'cur_fmask', F_ALL)

    def insn(self, i, targets):
        m=i.mnemonic; ops=i.operands; o=ops
        if _LOCKSTRIP and m[:5] == 'lock ' and m[5:] in _LOCKABLE and ops and ops[0].type == X86_OP_MEM:
            m = m[5:]
        E=lambda k: self.rd(o[k]); W=lambda k,v: self.wr(o[k],v)
        bits = (o[0].size*8) if o else 32
        L=[]
        def flags_arith(kind,a,b,res):
            L.append(f"{{ uint32_t _a={a}, _b={b}, _r=(uint32_t)({res}); FLAGS_{kind}(_a,_b,_r,{bits}); }}")
        if m=='nop' or m.startswith('nop'): return ["/* nop */"]
        if m=='mov':      return [W(0,E(1))]
        if m=='movzx':    return [W(0,f"(uint32_t)({E(1)})")]
        if m=='movsx':
            sb=o[1].size*8
            return [W(0,f"(uint32_t)(int32_t)(int{sb}_t)({E(1)})")]
        if m=='lea':      return [W(0,self.ea(o[1].mem))]
        if m=='push':     return [f"PUSH32({E(0)});"]
        if m=='pop':      return [f"{{ uint32_t _t; POP32(_t); {W(0,'_t')} }}"]
        if m=='enter':
            # enter imm16, imm8 -- only the common (n, 0) form
            sz2 = o[0].imm & 0xffff
            return ["PUSH32(c->ebp);", "c->ebp = c->esp;",
                    f"c->esp -= {sz2}u;"]
        if m=='leave':    return ["c->esp = c->ebp;", "{ uint32_t _t; POP32(_t); c->ebp=_t; }"]
        if m=='cdq':      return ["c->edx = (uint32_t)((int32_t)c->eax >> 31);"]
        if m=='cwde':     return ["c->eax = (uint32_t)(int32_t)(int16_t)(uint16_t)c->eax;"]
        if m in ('add','sub','adc','sbb'):
            a,b=E(0),E(1)
            if m=='add':   L.append(f"{{ uint32_t _a={a},_b={b},_r=_a+_b; FLAGS_ADD_M(_a,_b,_r,{bits},{self._fm()}); {W(0,'_r')} }}")
            elif m=='sub': L.append(f"{{ uint32_t _a={a},_b={b},_r=_a-_b; FLAGS_SUB_M(_a,_b,_r,{bits},{self._fm()}); {W(0,'_r')} }}")
            elif m=='adc': L.append(f"{{ uint32_t _ci=c->cf&1u,_a={a},_b={b},_r=_a+_b+_ci; FLAGS_ADC_M(_a,_b,_ci,_r,{bits},{self._fm()}); {W(0,'_r')} }}")
            else:          L.append(f"{{ uint32_t _ci=c->cf&1u,_a={a},_b={b},_r=_a-_b-_ci; FLAGS_SBB_M(_a,_b,_ci,_r,{bits},{self._fm()}); {W(0,'_r')} }}")
            return L
        if m=='cmp':  return [f"{{ uint32_t _a={E(0)},_b={E(1)},_r=_a-_b; FLAGS_SUB_M(_a,_b,_r,{bits},{self._fm()}); }}"]
        if m=='test': return [f"{{ uint32_t _r={E(0)} & {E(1)}; FLAGS_LOG_M(_r,{bits},{self._fm()}); }}"]
        if m in ('and','or','xor'):
            c_op={'and':'&','or':'|','xor':'^'}[m]
            return [f"{{ uint32_t _r=({E(0)}) {c_op} ({E(1)}); FLAGS_LOG_M(_r,{bits},{self._fm()}); {W(0,'_r')} }}"]
        if m=='not':  return [W(0,f"~({E(0)})")]
        if m=='neg':  return [f"{{ uint32_t _a={E(0)},_r=(uint32_t)0-_a; FLAGS_SUB_M((uint32_t)0,_a,_r,{bits},{self._fm()}); {W(0,'_r')} }}"]
        if m=='inc':  return [f"{{ uint32_t _a={E(0)},_r=_a+1; uint32_t _cf=c->cf; FLAGS_ADD_M(_a,1u,_r,{bits},{self._fm()}|F_CF); c->cf=_cf; {W(0,'_r')} }}"]
        if m=='dec':  return [f"{{ uint32_t _a={E(0)},_r=_a-1; uint32_t _cf=c->cf; FLAGS_SUB_M(_a,1u,_r,{bits},{self._fm()}|F_CF); c->cf=_cf; {W(0,'_r')} }}"]
        if m=='imul':
            if len(o)==1 and bits==8:
                return [f"{{ int32_t _p=(int32_t)(int8_t)c->eax*(int32_t)(int8_t)({E(0)});"
                        f" c->eax=(c->eax & 0xffff0000u)|((uint32_t)_p & 0xffffu);"
                        f" c->cf=c->of=((int32_t)(int8_t)_p != _p); }}"]
            if len(o)==1 and bits==16:
                return [f"{{ int32_t _p=(int32_t)(int16_t)c->eax*(int32_t)(int16_t)({E(0)});"
                        f" c->eax=(c->eax & 0xffff0000u)|((uint32_t)_p & 0xffffu);"
                        f" c->edx=(c->edx & 0xffff0000u)|(((uint32_t)_p>>16) & 0xffffu);"
                        f" c->cf=c->of=((int32_t)(int16_t)_p != _p); }}"]
            if len(o)==1:  return [f"{{ int64_t _p=(int64_t)(int32_t)c->eax*(int64_t)(int32_t)({E(0)}); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); c->cf=c->of=((int64_t)(int32_t)_p!=_p); }}"]
            if len(o)==2:  return [f"{{ int64_t _p=(int64_t)(int32_t)({E(0)})*(int64_t)(int32_t)({E(1)}); c->cf=c->of=((int64_t)(int32_t)_p!=_p); {W(0,'(uint32_t)_p')} }}"]
            return [f"{{ int64_t _p=(int64_t)(int32_t)({E(1)})*(int64_t)(int32_t)({E(2)}); c->cf=c->of=((int64_t)(int32_t)_p!=_p); {W(0,'(uint32_t)_p')} }}"]
        if m=='mul':
            # 8-bit: AX = AL*src | 16-bit: DX:AX = AX*src | 32-bit: EDX:EAX
            if bits==8:
                return [f"{{ uint32_t _p=(uint32_t)(uint8_t)c->eax*(uint32_t)(uint8_t)({E(0)});"
                        f" c->eax=(c->eax & 0xffff0000u)|(_p & 0xffffu);"
                        f" c->cf=c->of=((_p>>8)!=0); }}"]
            if bits==16:
                return [f"{{ uint32_t _p=(uint32_t)(uint16_t)c->eax*(uint32_t)(uint16_t)({E(0)});"
                        f" c->eax=(c->eax & 0xffff0000u)|(_p & 0xffffu);"
                        f" c->edx=(c->edx & 0xffff0000u)|((_p>>16) & 0xffffu);"
                        f" c->cf=c->of=((_p>>16)!=0); }}"]
            return [f"{{ uint64_t _p=(uint64_t)c->eax*(uint64_t)({E(0)}); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); c->cf=c->of=(c->edx!=0); }}"]
        if m=='div':
            if bits==8:
                return [f"{{ uint32_t _n=(uint16_t)c->eax, _d=(uint8_t)({E(0)});"
                        f" if(_d){{ c->eax=(c->eax & 0xffff0000u)|((_n/_d)&0xffu)|(((_n%_d)&0xffu)<<8); }} }}"]
            if bits==16:
                return [f"{{ uint32_t _n=(((uint32_t)(uint16_t)c->edx)<<16)|(uint16_t)c->eax,"
                        f" _d=(uint16_t)({E(0)});"
                        f" if(_d){{ c->eax=(c->eax & 0xffff0000u)|((_n/_d)&0xffffu);"
                        f" c->edx=(c->edx & 0xffff0000u)|((_n%_d)&0xffffu); }} }}"]
            return [f"{{ uint64_t _n=((uint64_t)c->edx<<32)|c->eax; uint32_t _d={E(0)}; if(_d){{ c->eax=(uint32_t)(_n/_d); c->edx=(uint32_t)(_n%_d); }} }}"]
        if m=='idiv':
            if bits==8:
                return [f"{{ int32_t _n=(int16_t)c->eax, _d=(int8_t)({E(0)});"
                        f" if(_d){{ c->eax=(c->eax & 0xffff0000u)|((uint32_t)(_n/_d)&0xffu)"
                        f"|(((uint32_t)(_n%_d)&0xffu)<<8); }} }}"]
            if bits==16:
                return [f"{{ int32_t _n=(int32_t)((((uint32_t)(uint16_t)c->edx)<<16)|(uint16_t)c->eax),"
                        f" _d=(int16_t)({E(0)});"
                        f" if(_d){{ c->eax=(c->eax & 0xffff0000u)|((uint32_t)(_n/_d)&0xffffu);"
                        f" c->edx=(c->edx & 0xffff0000u)|((uint32_t)(_n%_d)&0xffffu); }} }}"]
            return [f"{{ int64_t _n=(int64_t)(((uint64_t)c->edx<<32)|c->eax); int32_t _d=(int32_t)({E(0)}); if(_d){{ c->eax=(uint32_t)(_n/_d); c->edx=(uint32_t)(_n%_d); }} }}"]
        if m in ('shl','sal','shr','sar'):
            cnt = E(1) if len(o)>1 else "1u"
            # x86: CF = the last bit shifted out; SAR sign-extends (at the operand size)
            if m in ('shl','sal'):
                expr = "_v << _s"
                cf   = f"((_v >> ({bits}u - _s)) & 1u)"
            elif m=='shr':
                expr = "_v >> _s"
                cf   = "((_v >> (_s - 1u)) & 1u)"
            else:
                sb = {8:'int8_t',16:'int16_t',32:'int32_t'}[bits]
                expr = f"(uint32_t)(({sb})_v >> _s)"
                cf   = f"((uint32_t)(({sb})_v >> (_s - 1u)) & 1u)"
            mask = {8:'0xffu',16:'0xffffu',32:'0xffffffffu'}[bits]
            of_ = ("(((_rm >> (%du)) & 1u) ^ (_cf & 1u))" % (bits-1)) if m in ('shl','sal') else \
                  (("((_v >> (%du)) & 1u)" % (bits-1)) if m=='shr' else "0u")
            return [f"{{ uint32_t _v={E(0)} & {mask}, _s=({cnt})&31u; if(_s){{ "
                    f"uint32_t _cf={cf}; uint32_t _r={expr}; uint32_t _rm=_r & {mask}; "
                    f"SETZSP_M(_rm,{bits},{self._fm()}); c->cf=_cf & 1u; c->of={of_}; c->af=0; {W(0,'_rm')} }} }}"]
        if m.startswith('set') and m[3:] in CC:
            return [W(0,f"({CC[m[3:]]}) ? 1u : 0u")]
        if m.startswith('cmov') and m[4:] in CC:
            return [f"if({CC[m[4:]]}) {{ {W(0,E(1))} }}"]
        if m=='xchg':
            return [f"{{ uint32_t _t0={E(0)}, _t1={E(1)}; {W(0,'_t1')} {W(1,'_t0')} }}"]
        if m=='call':
            if o[0].type==X86_OP_IMM:
                t=o[0].imm
                if not self.in_text(t):
                    # call outside .text (misdecoded data or an import thunk)
                    self.oob_calls = getattr(self,'oob_calls',0)+1
                    return [f"PUSH32(0x{i.address+i.size:x}u); dispatch_indirect(c, 0x{t & 0xffffffff:x}u);"]
                targets.add(t)
                if t == _FTOL2_VA and _FTOL2INL:
                    # FTOL2INL=1: the CRT's _ftol2 in place (cpu.h FTOL2_INL), not a call
                    return [f"PUSH32(0x{i.address+i.size:x}u); FTOL2_INL(c);"]
                if t in self.retpast:
                    # a callee that may return past this function (tools/retpast.txt):
                    # when the address it returned to is not this call's, this
                    # function's own return address is gone too; return as well
                    return [f"PUSH32(0x{i.address+i.size:x}u); sub_{t:08x}(c);",
                            f"if (c->eip != 0x{i.address+i.size:x}u) return;"]
                return [f"PUSH32(0x{i.address+i.size:x}u); sub_{t:08x}(c);"]
            # x86: the call target is read BEFORE the return address is pushed (call [esp+N]!)
            return [f"{{ uint32_t _ct = {E(0)}; PUSH32(0x{i.address+i.size:x}u); DISPATCH_CALL(c, _ct); }}"]
        if m in ('ret','retf'):
            n = o[0].imm if o else 0
            # RETEIP=0: the return address is not written to c->eip (only popped off the stack).
            # A lifted caller always continues right after the call; the only place
            # that reads c->eip (genexec) now uses the address it pushed.  The
            # callees of tools/retpast.txt always write it: their callers compare it.
            import os as _os
            if _os.environ.get('RETEIP','1') == '0' and self.entry not in self.retpast:
                return [f"c->esp += {4 + n}u; return;"]
            return [f"POP32(c->eip);" + (f" c->esp += {n}u;" if n else "") + " return;"]

        # ---------------- x87 FPU ----------------
        # X87PC=1 (lift time): results of FADD/FSUB/FMUL/FDIV(+P/R/I forms)
        # and FSQRT go through X87R(), which rounds them to a 24-bit
        # significand when the guest thread's control word selects single
        # precision (PC=00), like the hardware.  PC=53/64 stay plain double.
        # X87R(x87_r2(a, b, r)) is emitted as X87R2(a, b, r): one test on the
        # fast path instead of two (cpu.h).
        def PCR(e):
            if not _X87PC: return e
            if e.startswith('x87_r2(') and e.endswith(')'): return 'X87R2(' + e[len('x87_r2('):]
            return f"X87R({e})"
        ST_REGS = {X86_REG_ST0:0,X86_REG_ST1:1,X86_REG_ST2:2,X86_REG_ST3:3,
                   X86_REG_ST4:4,X86_REG_ST5:5,X86_REG_ST6:6,X86_REG_ST7:7}
        def is_st(k): return o[k].type==X86_OP_REG and o[k].reg in ST_REGS
        def sti(k):   return ST_REGS[o[k].reg]
        def fmem(k):
            sz=o[k].size
            if sz==4: return f"(double)MRF32{self.ea(o[k].mem)}"
            if sz==8: return f"MRF64{self.ea(o[k].mem)}"
            if sz==10: return f"f80_load(g_mem + (uint32_t){self.ea(o[k].mem)})"
            raise Unsupported(f"{m} size {sz}")
        def imem(k):
            sz=o[k].size; ea=self.ea(o[k].mem)
            if sz==2: return f"(double)(int16_t)MR16{ea}"
            if sz==4: return f"(double)(int32_t)MR32{ea}"
            if sz==8: return f"(double)(int64_t)MR64{ea}"
            raise Unsupported(f"fild size {sz}")

        if m in ('fld','fldl'):
            return [f"FPUSH({'ST(%d)'%sti(0) if is_st(0) else fmem(0)});"]
        if m=='fild':  return [f"FPUSH({imem(0)});"]
        if m in ('fldz','fld1','fldpi','fldl2e','fldl2t','fldlg2','fldln2'):
            k={'fldz':'0.0','fld1':'1.0','fldpi':'3.14159265358979323846',
               'fldl2e':'1.44269504088896340736','fldl2t':'3.32192809488736234787',
               'fldlg2':'0.30102999566398119521','fldln2':'0.69314718055994530942'}[m]
            return [f"FPUSH({k});"]
        if m in ('fst','fstp'):
            pop = ["FPOP();"] if m=='fstp' else []
            if is_st(0): return [f"ST({sti(0)}) = ST(0);"]+pop
            sz=o[0].size
            if sz==4: return [f"MRF32{self.ea(o[0].mem)} = (float)ST(0);"]+pop
            if sz==8: return [f"MRF64{self.ea(o[0].mem)} = ST(0);"]+pop
            if sz==10: return [f"f80_store(g_mem + (uint32_t){self.ea(o[0].mem)}, ST(0));"]+pop
            raise Unsupported(f"{m} size {sz}")
        if m in ('fist','fistp','fisttp'):
            pop = [] if m=='fist' else ["FPOP();"]
            sz=o[0].size; ea=self.ea(o[0].mem)
            # fisttp (SSE3) always truncates; fist/fistp follow the RC field.
            # Out of range / NaN -> "integer indefinite" (the most negative value).
            conv = "trunc(ST(0))" if m=='fisttp' else "x87_round(c->fcw, ST(0))"
            cvt = {2:'x86_cvt_i16',4:'x86_cvt_i32',8:'x86_cvt_i64'}.get(sz)
            st  = {2:'MR16',4:'MR32',8:'MR64'}.get(sz)
            if not cvt: raise Unsupported(f"{m} size {sz}")
            return [f"{st}{ea} = {cvt}({conv});"]+pop
        FAR={'fadd':'+','fsub':'-','fmul':'*','fdiv':'/'}
        if m in ('fadd','fsub','fmul','fdiv','fsubr','fdivr'):
            rev = m.endswith('r') and m in ('fsubr','fdivr')
            base = m[:-1] if rev else m
            op_c = FAR[base]
            if len(o)==0: raise Unsupported(m)
            if len(o)==1:
                src = f"ST({sti(0)})" if is_st(0) else fmem(0)
                lhs, rhs = ("_s","_d") if rev else ("_d","_s")
                return [f"{{ double _d=ST(0), _s=({src});"
                        f" ST(0)={PCR(f'x87_r2(_d,_s,{lhs} {op_c} {rhs})')}; }}"]
            d = sti(0) if is_st(0) else None
            s2 = f"ST({sti(1)})" if is_st(1) else fmem(1)
            if d is None: raise Unsupported(m)
            lhs, rhs = ("_s","_d") if rev else ("_d","_s")
            return [f"{{ double _d=ST({d}), _s=({s2});"
                    f" ST({d})={PCR(f'x87_r2(_d,_s,{lhs} {op_c} {rhs})')}; }}"]
        if m in ('faddp','fsubp','fmulp','fdivp','fsubrp','fdivrp'):
            rev = m in ('fsubrp','fdivrp')
            base = {'faddp':'fadd','fsubp':'fsub','fmulp':'fmul','fdivp':'fdiv',
                    'fsubrp':'fsub','fdivrp':'fdiv'}[m]
            op_c = FAR[base]
            d = sti(0) if (len(o)>0 and is_st(0)) else 1
            lhs, rhs = ("_s","_d") if rev else ("_d","_s")
            return [f"{{ double _d=ST({d}), _s=ST(0);"
                    f" ST({d})={PCR(f'x87_r2(_d,_s,{lhs} {op_c} {rhs})')}; }}", "FPOP();"]
        if m in ('fcom','fcomp','fcompp','fucom','fucomp','fucompp'):
            npop = 2 if m.endswith('pp') else (1 if m.endswith('p') else 0)
            if len(o)==0: src="ST(1)"
            elif is_st(0): src=f"ST({sti(0)})"
            else: src=fmem(0)
            return [f"fcompare(c, ST(0), {src});"] + ["FPOP();"]*npop
        if m in ('fcomi','fcomip','fcompi','fucomi','fucomip','fucompi'):
            src=f"ST({sti(1)})" if len(o)>1 and is_st(1) else (f"ST({sti(0)})" if is_st(0) else fmem(0))
            pops = m.endswith('ip') or m.endswith('pi')
            return [f"FCOMI(ST(0), {src});"] + (["FPOP();"] if pops else [])
        if m=='ftst':   return ["fcompare(c, ST(0), 0.0);"]
        if m=='fchs':   return ["ST(0) = -ST(0);"]
        if m=='fabs':   return ["ST(0) = fabs(ST(0));"]
        if m=='fsqrt':  return ["{ double _a=ST(0); ST(0)=" + PCR("x87_r1(_a, sqrt(_a))") + "; }"]
        if m=='fsin':   return ["ST(0) = sin(ST(0));"]
        # v12na: D3DX math (0x63d907.. rotation helpers) uses fsincos.
        # Result: ST(0)=cos(x), ST(1)=sin(x).
        if m=='fsincos': return ["FPUSH(cos(ST(0)));", "ST(1) = sin(ST(1));"]
        if m=='fxam':
            return ["{ double _a=ST(0); c->fsw &= (uint16_t)~(FSW_C0|FSW_C1|FSW_C2|FSW_C3);"
                    " if (signbit(_a)) c->fsw |= FSW_C1;"
                    " if (isnan(_a)) c->fsw |= FSW_C0;"
                    " else if (isinf(_a)) c->fsw |= FSW_C2|FSW_C0;"
                    " else if (_a == 0.0) c->fsw |= FSW_C3;"
                    " else if (fpclassify(_a) == FP_SUBNORMAL) c->fsw |= FSW_C3|FSW_C2;"
                    " else c->fsw |= FSW_C2; }"]
        if m in _FCMOV_USE:
            cond = {'fcmovb':'c->cf','fcmovnb':'!c->cf','fcmove':'c->zf','fcmovne':'!c->zf',
                    'fcmovbe':'(c->cf||c->zf)','fcmovnbe':'!(c->cf||c->zf)',
                    'fcmovu':'c->pf','fcmovnu':'!c->pf'}[m]
            j = sti(len(o)-1) if len(o) >= 1 and is_st(len(o)-1) else 1
            return [f"if ({cond}) ST(0) = ST({j});"]
        if m=='fcos':   return ["ST(0) = cos(ST(0));"]
        if m=='fptan':  return ["ST(0) = tan(ST(0));","FPUSH(1.0);"]
        if m=='fpatan': return ["ST(1) = atan2(ST(1), ST(0));","FPOP();"]
        if m=='fyl2x':  return ["ST(1) = ST(1) * log2(ST(0));","FPOP();"]
        if m=='f2xm1':  return ["ST(0) = exp2(ST(0)) - 1.0;"]
        if m=='fscale': return ["ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"]
        if m=='frndint':return ["ST(0) = x87_round(c->fcw, ST(0));"]
        # FPREM/FPREM1: with a large exponent difference the hardware does a PARTIAL
        # reduction and leaves C2=1; we always give the full result and set C2=0 --
        # the game's "repeat until C2 is clear" loop exits on the first round, same result.
        # C0/C3/C1 carry the low 3 bits of the quotient (fsin/fcos argument reduction).
        if m in ('fprem','fprem1'):
            fn = 'fmod(_a,_b)' if m=='fprem' else 'remainder(_a,_b)'
            return [f"{{ double _a=ST(0), _b=ST(1), _r=x87_r2(_a,_b,{fn});"
                    " double _q = (_b!=0.0) ? trunc(_a/_b) : 0.0;"
                    " uint32_t _qi = (uint32_t)(int64_t)fmod(fabs(_q), 8.0);"
                    " c->fsw &= (uint16_t)~(FSW_C0|FSW_C1|FSW_C2|FSW_C3);"
                    " if (_qi & 1u) c->fsw |= FSW_C1;"
                    " if (_qi & 2u) c->fsw |= FSW_C3;"
                    " if (_qi & 4u) c->fsw |= FSW_C0;"
                    " ST(0)=_r; }"]
        if m=='fxch':
            # capstone gives (st(0), st(i)) for fxch: the target is the LAST operand
            j = 1
            if len(o) >= 2 and is_st(len(o)-1): j = sti(len(o)-1)
            elif len(o) == 1 and is_st(0):      j = sti(0)
            return [f"{{ double _t=ST(0); ST(0)=ST({j}); ST({j})=_t; }}"]
        if m in ('fnstsw','fstsw'):
            if len(o)>0 and o[0].type==X86_OP_REG:
                return ["c->eax = (c->eax & 0xffff0000u) | (uint32_t)fsw_get(c);"]
            return [f"MR16{self.ea(o[0].mem)} = fsw_get(c);"]
        if m in ('fnstcw','fstcw'): return [f"MR16{self.ea(o[0].mem)} = c->fcw;"]
        if m=='fldcw':              return [f"c->fcw = MR16{self.ea(o[0].mem)};"]
        if m in ('ffree','ffreep','fnclex','fclex','fnop','fwait','wait','fdecstp','fincstp'):
            return [f"/* {m}: no effect in our model */"] + (["FPOP();"] if m=='ffreep' else [])
        if m in ('finit','fninit'):
            return ["memset(c->st,0,sizeof c->st); c->top=0; c->fcw=0x037f; c->fsw=0;"]
        # X87ENV=1: the environment image is CW, SW (TOP included), TW, then
        # FIP, FCS/opcode, FDP, FDS: one dword each, the reserved upper halves
        # of the first three written as ones like the hardware.  The model
        # keeps no tag word and no instruction or operand pointers: TW is
        # written as all empty, the pointers as 0, and loads ignore all three.
        # fnsave appends ST(0)..ST(7) as 80-bit values and then initializes
        # the FPU (fninit); fnstenv masks all exceptions after the store.  The
        # 16-bit images (0x66 prefix) stay traps.
        if _X87ENV and m in ('fnstenv','fldenv','fnsave','frstor'):
            if not o or o[0].type!=X86_OP_MEM or i.prefix[2]==0x66: raise Unsupported(m)
            ea=self.ea(o[0].mem)
            if m in ('fnstenv','fnsave'):
                st=[f"uint32_t _e={ea};", "MR32(_e) = 0xffff0000u | c->fcw;", "MR32(_e+4u) = 0xffff0000u | fsw_get(c);",
                    "MR32(_e+8u) = 0xffffffffu;"] + [f"MR32(_e+{k}u) = 0u;" for k in (12, 16, 20, 24)]
                if m=='fnstenv': st.append("c->fcw |= 0x3fu;")
                else: st += [f"f80_store(g_mem + (uint32_t)(_e+{28+10*k}u), ST({k}));" for k in range(8)] \
                          + ["memset(c->st,0,sizeof c->st); c->top=0; c->fcw=0x037f; c->fsw=0;"]
            else:
                st=[f"uint32_t _e={ea}; uint16_t _sw=MR16(_e+4u);", "c->fcw = MR16(_e);",
                    "c->fsw = (uint16_t)(_sw & ~0x3800u); c->top = (int32_t)((_sw >> 11) & 7u);"]
                if m=='frstor': st += [f"ST({k}) = f80_load(g_mem + (uint32_t)(_e+{28+10*k}u));" for k in range(8)]
            return ["{ " + " ".join(st) + " }"]

        # ---------------- string instructions ----------------
        if m=='cld': return ["c->df = 0;"]
        if m=='std': return ["c->df = 1;"]
        parts=m.split()
        pfx = parts[0] if len(parts)>1 and parts[0] in ('rep','repe','repz','repne','repnz') else None
        base = parts[-1]
        SW={'b':(1,'MR8'),'w':(2,'MR16'),'d':(4,'MR32')}
        if base[:-1] in ('movs','stos','lods','scas','cmps') and base[-1] in SW and \
           not any(op2.type==X86_OP_REG and op2.reg not in R32 and op2.reg not in R16 and op2.reg not in R8L and op2.reg not in R8H for op2 in o):
            n,acc = SW[base[-1]]; kind = base[:-1]
            areg = {1:'(uint8_t)c->eax',2:'(uint16_t)c->eax',4:'c->eax'}[n]
            # the repe/repne body reads the ZF it wrote ITSELF with 'if(!c->zf) break;';
            # ZF must be computed even when it looks dead.
            _zf = "|F_ZF" if pfx in ('repe','repz','repne','repnz') else ""
            body={
             'movs': f"{acc}(c->edi) = {acc}(c->esi); c->esi += _s; c->edi += _s;",
             'stos': f"{acc}(c->edi) = {areg}; c->edi += _s;",
             'lods': f"c->eax = " + ("(c->eax & 0xffffff00u)|" if n==1 else "(c->eax & 0xffff0000u)|" if n==2 else "") + f"{acc}(c->esi); c->esi += _s;",
             'scas': f"{{ uint32_t _a={areg}, _b={acc}(c->edi), _r=_a-_b; FLAGS_SUB_M(_a,_b,_r,{n*8},{self._fm()}{_zf}); }} c->edi += _s;",
             'cmps': f"{{ uint32_t _a={acc}(c->esi), _b={acc}(c->edi), _r=_a-_b; FLAGS_SUB_M(_a,_b,_r,{n*8},{self._fm()}{_zf}); }} c->esi += _s; c->edi += _s;",
            }[kind]
            pre=f"int32_t _s = DFSTEP({n});"
            if pfx is None: return [f"{{ {pre} {body} }}"]
            if pfx=='rep': return [f"{{ {pre} while(c->ecx){{ {body} c->ecx--; }} }}"]
            cond = "!c->zf" if pfx in ('repe','repz') else "c->zf"
            return [f"{{ {pre} while(c->ecx){{ {body} c->ecx--; if({cond}) break; }} }}"]

        # ---------------- miscellaneous ----------------
        if m in ('rol','ror','rcl','rcr'):
            cnt = E(1) if len(o)>1 else "1u"
            mask = {8:'0xffu',16:'0xffffu',32:'0xffffffffu'}[bits]
            if m=='rol':
                fn = f"(((_v<<_s)|(_v>>({bits}u-_s))) & {mask})"
                cfx = "(_r & 1u)"
            elif m=='ror':
                fn = f"(((_v>>_s)|(_v<<({bits}u-_s))) & {mask})"
                cfx = f"((_r >> ({bits-1}u)) & 1u)"
            else:
                # rcl/rcr: rotate through CF (a bits+1-bit rotation)
                nb = bits + 1
                if m=='rcl':
                    step = "{ uint32_t _hi=(_v>>({b}u))&1u; _v=((_v<<1)&{mask})|(_cf&1u); _cf=_hi; }"
                else:
                    step = "{ uint32_t _lo=_v&1u; _v=(_v>>1)|((_cf&1u)<<({b}u)); _cf=_lo; }"
                step = step.replace("{b}", str(bits-1)).replace("{mask}", mask)
                return [f"{{ uint32_t _v={E(0)} & {mask}, _s=(({cnt})&31u)%{nb}u, _cf=c->cf;"
                        f" while(_s--) {step} c->cf=_cf&1u; {W(0,'_v')} }}"]
            of1 = ("(((_r >> (%du)) & 1u) ^ (_r & 1u))" % (bits-1)) if m=='rol' else \
                  ("(((_r >> (%du)) & 1u) ^ ((_r >> (%du)) & 1u))" % (bits-1, bits-2))
            fn0 = fn.replace("_v<<_s", "_v<<_s").replace("_v>>_s", "_v>>_s")
            return [f"{{ uint32_t _v={E(0)} & {mask}, _c5=({cnt})&31u, _s=_c5 % {bits}u;"
                    f" if(_c5){{ uint32_t _r = _s ? ({fn0}) : _v; c->cf={cfx};"
                    f" if(_c5==1u) c->of={of1}; {W(0,'_r')} }} }}"]
        if m == 'xadd':
            # temporary: source and destination are swapped, then added
            return [f"{{ uint32_t _d={E(0)}, _s={E(1)}, _r=_d+_s;"
                    f" FLAGS_ADD_M(_d,_s,_r,{bits},{self._fm()}); {W(1,'_d')} {W(0,'_r')} }}"]
        if m == 'cmpxchg':
            # compare with acc; if equal write the source to the destination, else the destination to acc
            acc = {8:"(uint8_t)c->eax",16:"(uint16_t)c->eax",32:"c->eax"}[bits]
            wacc = {8:"c->eax=(c->eax & 0xffffff00u)|((_d)&0xffu);",
                    16:"c->eax=(c->eax & 0xffff0000u)|((_d)&0xffffu);",
                    32:"c->eax=(_d);"}[bits]
            # On the real CPU the flags come from an implicit "DEST - ACC" compare
            # (measured with Rosetta); ZF is the same either way round.
            return [f"{{ uint32_t _a={acc}, _d={E(0)}, _r=_d-_a;"
                    f" FLAGS_SUB_M(_d,_a,_r,{bits},{self._fm()});"
                    f" if(c->zf){{ {W(0,E(1))} }} else {{ {wacc} }} }}"]
        if m in ('bsf','bsr'):
            # if the source is 0, ZF=1 and the destination does NOT CHANGE
            fn = "_i=0; while(!((_v>>_i)&1u)) _i++;" if m=='bsf' else \
                 f"_i={bits-1}; while(!((_v>>_i)&1u)) _i--;"
            return [f"{{ uint32_t _v=({E(1)}) & {{8:0xffu,16:0xffffu,32:0xffffffffu}}[{bits}];"
                    .replace("{8:0xffu,16:0xffffu,32:0xffffffffu}[%d]" % bits,
                             {8:'0xffu',16:'0xffffu',32:'0xffffffffu'}[bits]) +
                    f" if(_v==0){{ c->zf=1; }} else {{ uint32_t _i; {fn} c->zf=0; {W(0,'_i')} }} }}"]
        if m=='cbw':
            return ["c->eax = (c->eax & 0xffff0000u) | "
                    "(uint32_t)(uint16_t)(int16_t)(int8_t)(uint8_t)c->eax;"]
        if m in ('bt','bts','btr','btc'):
            if o[0].type==X86_OP_REG:
                base=E(0); bitn=f"(({E(1)}) & {bits-1}u)"
                setexpr={'bt':None,'bts':'_b | (1u<<_n)','btr':'_b & ~(1u<<_n)','btc':'_b ^ (1u<<_n)'}[m]
                lines=[f"{{ uint32_t _b={base}, _n={bitn}; c->cf=(_b>>_n)&1u;"]
                if setexpr: lines[0]+=f" {W(0,setexpr)}"
                lines[0]+=" }"
                return lines
            ea3=self.ea(o[0].mem)
            return [f"{{ uint32_t _n={E(1)}; uint32_t _a=({ea3})+(_n>>3); uint32_t _m=1u<<(_n&7);"
                    f" c->cf=(MR8(_a)&_m)?1u:0u;"
                    + ("" if m=='bt' else
                       (f" MR8(_a)|= _m;" if m=='bts' else
                        (f" MR8(_a)&=~_m;" if m=='btr' else f" MR8(_a)^= _m;")))
                    + " }"]
        if m=='bswap': return [W(0,f"__builtin_bswap32({E(0)})")]
        if m=='sahf':  return ["{ uint32_t _f=(c->eax>>8)&0xffu; c->cf=_f&1; c->pf=(_f>>2)&1; c->af=(_f>>4)&1; c->zf=(_f>>6)&1; c->sf=(_f>>7)&1; }"]
        if m=='lahf':  return ["c->eax = (c->eax & 0xffff00ffu) | ((c->sf<<7|c->zf<<6|c->af<<4|c->pf<<2|2u|c->cf)<<8);"]
        if m in ('xlatb','xlat'):  # v12na: AL = [EBX + AL]
            return ["c->eax = (c->eax & 0xffffff00u) | (uint32_t)MR8(c->ebx + (c->eax & 0xffu));"]
        if m=='cpuid': return ["cpuid_shim(c);"]
        if m=='rdtsc': return ["{ uint64_t _t=rdtsc_shim(); c->eax=(uint32_t)_t; c->edx=(uint32_t)(_t>>32); }"]
        if m in ('clc',): return ["c->cf=0;"]
        if m in ('stc',): return ["c->cf=1;"]
        if m in ('cmc',): return ["c->cf=!c->cf;"]


        # ---------------- flag stack / double shifts ----------------
        if m in ('pushal','pushad','pusha'):
            return ["{ uint32_t _sp=c->esp; PUSH32(c->eax); PUSH32(c->ecx); PUSH32(c->edx);"
                    " PUSH32(c->ebx); PUSH32(_sp); PUSH32(c->ebp); PUSH32(c->esi); PUSH32(c->edi); }"]
        if m in ('popal','popad','popa'):
            return ["{ uint32_t _t; POP32(c->edi); POP32(c->esi); POP32(c->ebp); POP32(_t);"
                    " POP32(c->ebx); POP32(c->edx); POP32(c->ecx); POP32(c->eax); (void)_t; }"]
        if m in ('pushfd','pushf'):  return ["PUSH32(eflags_get(c));"]
        if m in ('popfd','popf'):    return ["{ uint32_t _f; POP32(_f); eflags_set(c,_f); }"]
        if m in ('shrd','shld'):
            cnt = E(2) if len(o)>2 else "c->ecx"
            if m=='shrd':
                expr = "(_d >> _s) | (_v << (32u-_s))"
            else:
                expr = "(_d << _s) | (_v >> (32u-_s))"
            # CAUTION: FLAGS_LOG clears CF; CF must be written AFTER it.
            return [f"{{ uint32_t _d={E(0)}, _v={E(1)}, _s=({cnt})&31u; if(_s){{ uint32_t _r={expr}; "
                    f"FLAGS_LOG_M(_r,32,{self._fm()}); c->cf=(({'_d >> (_s-1u)' if m=='shrd' else '_d >> (32u-_s)'})&1u); {W(0,'_r')} }} }}"]

        # ---------------- x87 integer operations ----------------
        if m in ('fiadd','fimul','fisub','fisubr','fidiv','fidivr','ficom','ficomp'):
            sz=o[0].size; ea2=self.ea(o[0].mem)
            src = {2:f"(double)(int16_t)MR16{ea2}",4:f"(double)(int32_t)MR32{ea2}"}.get(sz)
            if src is None: raise Unsupported(f"{m} size {sz}")
            PCR = (lambda e: f"X87R({e})") if _X87PC else (lambda e: e)
            if m=='fiadd':  return [f"ST(0) = {PCR(f'ST(0) + ({src})')};"]
            if m=='fimul':  return [f"ST(0) = {PCR(f'ST(0) * ({src})')};"]
            if m=='fisub':  return [f"ST(0) = {PCR(f'ST(0) - ({src})')};"]
            if m=='fisubr': return [f"ST(0) = {PCR(f'({src}) - ST(0)')};"]
            if m=='fidiv':  return [f"ST(0) = {PCR(f'ST(0) / ({src})')};"]
            if m=='fidivr': return [f"ST(0) = {PCR(f'({src}) / ST(0)')};"]
            if m=='ficom':  return [f"fcompare(c, ST(0), {src});"]
            return [f"fcompare(c, ST(0), {src});", "FPOP();"]

        # ---------------- MMX / SSE ----------------
        MM = {X86_REG_MM0:0,X86_REG_MM1:1,X86_REG_MM2:2,X86_REG_MM3:3,
              X86_REG_MM4:4,X86_REG_MM5:5,X86_REG_MM6:6,X86_REG_MM7:7}
        XM = {X86_REG_XMM0:0,X86_REG_XMM1:1,X86_REG_XMM2:2,X86_REG_XMM3:3,
              X86_REG_XMM4:4,X86_REG_XMM5:5,X86_REG_XMM6:6,X86_REG_XMM7:7}
        def is_mm(k): return o[k].type==X86_OP_REG and o[k].reg in MM
        def is_xm(k): return o[k].type==X86_OP_REG and o[k].reg in XM
        def is_gp(k): return o[k].type==X86_OP_REG and o[k].reg in R32
        def is_mem(k): return o[k].type==X86_OP_MEM

        if m=='emms': return ["/* emms */"]
        # v12na: MXCSR is not modelled -> default 0x1F80 (all exceptions masked, round to nearest)
        if m=='stmxcsr': return [f"MR32{self.ea(o[0].mem)} = 0x1F80u;"]
        if m=='ldmxcsr': return [f"(void)MR32{self.ea(o[0].mem)};   /* MXCSR is not modelled */"]
        if m in ('movq','movd'):
            w = 8 if m=='movq' else 4
            T = 'uint64_t' if w==8 else 'uint32_t'
            def rd_(k):
                if is_mm(k):  return f"c->mm[{MM[o[k].reg]}]"
                if is_xm(k):  return f"c->xmm[{XM[o[k].reg]}].u64[0]" if w==8 else f"c->xmm[{XM[o[k].reg]}].u32[0]"
                if is_gp(k):  return f"c->{R32[o[k].reg]}"
                if is_mem(k): return ("MR64" if w==8 else "MR32") + self.ea(o[k].mem)
                raise Unsupported(m)
            def wr_(k,v):
                if is_mm(k):  return f"c->mm[{MM[o[k].reg]}] = ({T})({v});"
                if is_xm(k):
                    i2=XM[o[k].reg]
                    return (f"c->xmm[{i2}].u64[0] = (uint64_t)({v}); c->xmm[{i2}].u64[1] = 0;" if w==8
                            else f"c->xmm[{i2}].u32[0] = (uint32_t)({v}); c->xmm[{i2}].u32[1]=0; c->xmm[{i2}].u64[1]=0;")
                if is_gp(k):  return f"c->{R32[o[k].reg]} = (uint32_t)({v});"
                if is_mem(k): return ("MR64" if w==8 else "MR32") + self.ea(o[k].mem) + f" = ({T})({v});"
                raise Unsupported(m)
            return [wr_(0, rd_(1))]
        if m in ('movaps','movups','movdqa','movdqu','movapd','movupd'):
            def rd16(k):
                if is_xm(k): return f"c->xmm[{XM[o[k].reg]}]"
                return None
            if is_xm(0) and is_xm(1):
                return [f"c->xmm[{XM[o[0].reg]}] = c->xmm[{XM[o[1].reg]}];"]
            if is_xm(0) and is_mem(1):
                ea2=self.ea(o[1].mem)
                return [f"memcpy(&c->xmm[{XM[o[0].reg]}], g_mem + (uint32_t){ea2}, 16);"]
            if is_mem(0) and is_xm(1):
                ea2=self.ea(o[0].mem)
                return [f"memcpy(g_mem + (uint32_t){ea2}, &c->xmm[{XM[o[1].reg]}], 16);"]
    
        if m in ('movss','movsd') and (is_xm(0) or is_xm(1)):
            w = 4 if m=='movss' else 8
            if is_xm(0) and is_xm(1):
                i0,i1=XM[o[0].reg],XM[o[1].reg]
                return [f"c->xmm[{i0}].u32[0] = c->xmm[{i1}].u32[0];"] if w==4 else                        [f"c->xmm[{i0}].u64[0] = c->xmm[{i1}].u64[0];"]
            if is_xm(0) and is_mem(1):
                i0=XM[o[0].reg]; ea2=self.ea(o[1].mem)
                return [f"memset(&c->xmm[{i0}],0,16); memcpy(&c->xmm[{i0}], g_mem + (uint32_t){ea2}, {w});"]
            if is_mem(0) and is_xm(1):
                i1=XM[o[1].reg]; ea2=self.ea(o[0].mem)
                return [f"memcpy(g_mem + (uint32_t){ea2}, &c->xmm[{i1}], {w});"]
    
        if m in ('pxor','xorps','xorpd','por','orps','pand','andps','andpd','orpd','pandn','andnps','andnpd'):
            if not (is_xm(0) or is_mm(0)): raise Unsupported(m)
            if is_mm(0):
                a=f"c->mm[{MM[o[0].reg]}]"
                b=(f"c->mm[{MM[o[1].reg]}]" if is_mm(1) else "MR64"+self.ea(o[1].mem))
                if m in ('pandn','andnps'):     # dst = ~dst & src
                    return [f"{a} = (~{a}) & ({b});"]
                op_c={'pxor':'^','por':'|','pand':'&'}.get(m)
                if op_c is None: raise Unsupported(m)
                return [f"{a} = {a} {op_c} ({b});"]
            i0=XM[o[0].reg]
            src = (f"c->xmm[{XM[o[1].reg]}]" if is_xm(1) else None)
            if m in ('pandn','andnps','andnpd'):     # dst = ~dst & src
                if src:
                    return [f"c->xmm[{i0}].u64[0] = (~c->xmm[{i0}].u64[0]) & {src}.u64[0];"
                            f" c->xmm[{i0}].u64[1] = (~c->xmm[{i0}].u64[1]) & {src}.u64[1];"]
                ea3=self.ea(o[1].mem)
                return [f"{{ uint64_t _a=MR64({ea3}), _b=MR64(({ea3})+8u);"
                        f" c->xmm[{i0}].u64[0] = (~c->xmm[{i0}].u64[0]) & _a;"
                        f" c->xmm[{i0}].u64[1] = (~c->xmm[{i0}].u64[1]) & _b; }}"]
            op_c={'pxor':'^','xorps':'^','xorpd':'^','por':'|','orps':'|',
                  'pand':'&','andps':'&','andpd':'&','orpd':'|'}.get(m)
            if op_c is None: raise Unsupported(m)
            if src:
                return [f"c->xmm[{i0}].u64[0] {op_c}= {src}.u64[0]; c->xmm[{i0}].u64[1] {op_c}= {src}.u64[1];"]
            ea2=self.ea(o[1].mem)
            return [f"{{ uint64_t _a=MR64({ea2}), _b=MR64(({ea2})+8u); "
                    f"c->xmm[{i0}].u64[0] {op_c}= _a; c->xmm[{i0}].u64[1] {op_c}= _b; }}"]


        # ---------------- MMX integer operations ----------------
        MMOPS = {
          'paddb':(1,'+'),'paddw':(2,'+'),'paddd':(4,'+'),
          'psubb':(1,'-'),'psubw':(2,'-'),'psubd':(4,'-'),
          'pmullw':(2,'*'),
          'pand':(0,'&'),'por':(0,'|'),'pxor':(0,'^'),
        }
        SATOPS = {'paddsb':(1,1),'paddsw':(2,1),'psubsb':(1,-1),'psubsw':(2,-1),
                  'paddusb':(1,2),'paddusw':(2,2),'psubusb':(1,-2),'psubusw':(2,-2)}
        def mm_src(k, wide):
            if is_mm(k): return f"c->mm[{MM[o[k].reg]}]"
            if is_xm(k): return f"c->xmm[{XM[o[k].reg]}].u64[0]"
            if is_mem(k): return ("MR64" if not wide else "MR64") + self.ea(o[k].mem)
            raise Unsupported(m)
        def mm_dst_idx():
            if is_mm(0): return ('mm', MM[o[0].reg])
            if is_xm(0): return ('xmm', XM[o[0].reg])
            raise Unsupported(m)

        def qwords(k, half):
            """64-bit half number `half` of operand k (0/1 for XMM)."""
            if is_mm(k):  return f"c->mm[{MM[o[k].reg]}]"
            if is_xm(k):  return f"c->xmm[{XM[o[k].reg]}].u64[{half}]"
            if is_mem(k): return "MR64" + self.ea(o[k].mem, extra=8*half)
            raise Unsupported(m)

        if m in MMOPS and (is_mm(0) or is_xm(0)):
            esz, op_c = MMOPS[m]
            # with an XMM destination the operation is 128-bit: process both 64-bit halves.
            halves = [0,1] if is_xm(0) else [0]
            out = []
            for h in halves:
                dst = qwords(0, h); src = qwords(1, h)
                if esz == 0:
                    out.append(f"{dst} = {dst} {op_c} ({src});"); continue
                n = 8 // esz
                ty = {1:'uint8_t',2:'uint16_t',4:'uint32_t'}[esz]
                out.append(f"{{ uint64_t _a={dst}, _b=({src}), _r=0;"
                           f" for(int _k=0;_k<{n};_k++){{ {ty} _x=({ty})(_a>>(_k*{esz*8}));"
                           f" {ty} _y=({ty})(_b>>(_k*{esz*8})); {ty} _z=({ty})(_x {op_c} _y);"
                           f" _r |= (uint64_t)_z << (_k*{esz*8}); }} {dst}=_r; }}")
            return out

        # --- multiply/pack/shuffle ---
        PMUL = {'pmulhw':1,'pmulhuw':0}
        if m in PMUL and (is_mm(0) or is_xm(0)):
            signed = PMUL[m]
            halves = [0,1] if is_xm(0) else [0]
            out=[]
            for h in halves:
                dst = qwords(0,h); src = qwords(1,h)
                ty = 'int16_t' if signed else 'uint16_t'
                acc = 'int32_t' if signed else 'uint32_t'
                out.append(f"{{ uint64_t _a={dst}, _b=({src}), _r=0;"
                           f" for(int _k=0;_k<4;_k++){{ {ty} _x=({ty})(uint16_t)(_a>>(_k*16));"
                           f" {ty} _y=({ty})(uint16_t)(_b>>(_k*16));"
                           f" uint16_t _z=(uint16_t)((({acc})_x*({acc})_y)>>16);"
                           f" _r |= (uint64_t)_z << (_k*16); }} {dst}=_r; }}")
            return out
        if m == 'pmaddwd' and (is_mm(0) or is_xm(0)):
            halves = [0,1] if is_xm(0) else [0]
            out=[]
            for h in halves:
                dst = qwords(0,h); src = qwords(1,h)
                out.append(f"{{ uint64_t _a={dst}, _b=({src}), _r=0;"
                           f" for(int _k=0;_k<2;_k++){{"
                           f" int32_t _s=0;"
                           f" for(int _j=0;_j<2;_j++){{ int _i=_k*2+_j;"
                           f" int16_t _x=(int16_t)(uint16_t)(_a>>(_i*16));"
                           f" int16_t _y=(int16_t)(uint16_t)(_b>>(_i*16));"
                           f" _s += (int32_t)_x*(int32_t)_y; }}"
                           f" _r |= (uint64_t)(uint32_t)_s << (_k*32); }} {dst}=_r; }}")
            return out
        PACK = {'packsswb':(2,1,1),'packssdw':(4,2,1),'packuswb':(2,1,0)}
        if m in PACK and (is_mm(0) or is_xm(0)):
            esz, dsz, signed = PACK[m]
            n = 8 // esz
            sty = {2:'int16_t',4:'int32_t'}[esz]
            lo  = -(1 << (dsz*8-1)) if signed else 0
            hi  = (1 << (dsz*8-1)) - 1 if signed else (1 << (dsz*8)) - 1
            dty = {1:'uint8_t',2:'uint16_t'}[dsz]
            if is_xm(0):
                # 128-bit: the destination's 2n elements pack into the low
                # qword, the source's into the high one (not each half alone)
                return [f"{{ uint64_t _a[4]={{{qwords(0,0)},{qwords(0,1)},({qwords(1,0)}),({qwords(1,1)})}}, _r[2]={{0,0}};"
                        f" for(int _k=0;_k<{4*n};_k++){{"
                        f" {sty} _x=({sty})(_a[_k/{n}]>>((_k%{n})*{esz*8}));"
                        f" if(_x<{lo}) _x={lo}; if(_x>{hi}) _x={hi};"
                        f" _r[_k/{2*n}] |= (uint64_t)({dty})_x << ((_k%{2*n})*{dsz*8}); }}"
                        f" {qwords(0,0)}=_r[0]; {qwords(0,1)}=_r[1]; }}"]
            dst = qwords(0,0); src = qwords(1,0)
            return [f"{{ uint64_t _a={dst}, _b=({src}), _r=0;"
                    f" for(int _k=0;_k<{n};_k++){{"
                    f" {sty} _x=({sty})(_a>>(_k*{esz*8}));"
                    f" if(_x<{lo}) _x={lo}; if(_x>{hi}) _x={hi};"
                    f" _r |= (uint64_t)({dty})_x << (_k*{dsz*8}); }}"
                    f" for(int _k=0;_k<{n};_k++){{"
                    f" {sty} _y=({sty})(_b>>(_k*{esz*8}));"
                    f" if(_y<{lo}) _y={lo}; if(_y>{hi}) _y={hi};"
                    f" _r |= (uint64_t)({dty})_y << (({n}+_k)*{dsz*8}); }}"
                    f" {dst}=_r; }}"]
        if m == 'pextrw' and o[0].type==X86_OP_REG and (is_mm(1) or is_xm(1)):
            sel = o[2].imm & (7 if is_xm(1) else 3)
            if is_mm(1):
                return [W(0, f"(uint32_t)(uint16_t)(c->mm[{MM[o[1].reg]}] >> {sel*16})")]
            return [W(0, f"(uint32_t)(uint16_t)(c->xmm[{XM[o[1].reg]}].u64[{sel//4}] >> {(sel%4)*16})")]
        if m == 'pinsrw' and (is_mm(0) or is_xm(0)):
            sel = o[2].imm & (7 if is_xm(0) else 3)
            val = f"(uint64_t)(uint16_t)({E(1)})"
            if is_mm(0):
                d0=f"c->mm[{MM[o[0].reg]}]"
                return [f"{d0} = ({d0} & ~((uint64_t)0xFFFFu << {sel*16})) | ({val} << {sel*16});"]
            d0=f"c->xmm[{XM[o[0].reg]}].u64[{sel//4}]"
            sh=(sel%4)*16
            return [f"{d0} = ({d0} & ~((uint64_t)0xFFFFu << {sh})) | ({val} << {sh});"]
        if m == 'pmovmskb' and o[0].type==X86_OP_REG:
            if is_mm(1):
                return [f"{{ uint32_t _r=0; uint64_t _a=c->mm[{MM[o[1].reg]}];"
                        f" for(int _k=0;_k<8;_k++) if((_a>>(_k*8+7))&1u) _r|=1u<<_k;"
                        f" {W(0,'_r')} }}"]
            if is_xm(1):
                return [f"{{ uint32_t _r=0;"
                        f" for(int _k=0;_k<16;_k++)"
                        f" if(c->xmm[{XM[o[1].reg]}].b[_k] & 0x80u) _r|=1u<<_k;"
                        f" {W(0,'_r')} }}"]
        if m == 'pshufd' and is_xm(0):
            imm = o[2].imm & 0xff
            sel = [(imm>>0)&3,(imm>>2)&3,(imm>>4)&3,(imm>>6)&3]
            src = (f"c->xmm[{XM[o[1].reg]}]" if is_xm(1) else None)
            if src is None: raise Unsupported(m)
            di = XM[o[0].reg]
            return [f"{{ uint32_t _t[4]; " +
                    " ".join(f"_t[{k}]={src}.u32[{sel[k]}];" for k in range(4)) +
                    " " + " ".join(f"c->xmm[{di}].u32[{k}]=_t[{k}];" for k in range(4)) + " }"]
        if m in ('movq2dq',) and is_xm(0) and is_mm(1):
            return [f"c->xmm[{XM[o[0].reg]}].u64[0] = c->mm[{MM[o[1].reg]}];"
                    f" c->xmm[{XM[o[0].reg]}].u64[1] = 0;"]
        if m in ('movdq2q',) and is_mm(0) and is_xm(1):
            return [f"c->mm[{MM[o[0].reg]}] = c->xmm[{XM[o[1].reg]}].u64[0];"]
        if m == 'movntq' and is_mem(0) and is_mm(1):
            return [f"MR64{self.ea(o[0].mem)} = c->mm[{MM[o[1].reg]}];"]
        if m in ('movntps','movntdq') and is_mem(0) and is_xm(1):
            ea4=self.ea(o[0].mem)
            return [f"memcpy(g_mem + (uint32_t){ea4}, &c->xmm[{XM[o[1].reg]}], 16);"]

        # --- packed compare / min / max / average / psadbw ---
        PCMP = {'pcmpeqb':(1,'==',0),'pcmpeqw':(2,'==',0),'pcmpeqd':(4,'==',0),
                'pcmpgtb':(1,'>',1),'pcmpgtw':(2,'>',1),'pcmpgtd':(4,'>',1)}
        PMIX = {'pminsw':(2,1,'<'),'pmaxsw':(2,1,'>'),
                'pminub':(1,0,'<'),'pmaxub':(1,0,'>')}
        PAVG = {'pavgb':1,'pavgw':2}
        if m in PCMP and (is_mm(0) or is_xm(0)):
            esz, cop, signed = PCMP[m]
            uty = {1:'uint8_t',2:'uint16_t',4:'uint32_t'}[esz]
            sty = {1:'int8_t',2:'int16_t',4:'int32_t'}[esz]
            ty  = sty if signed else uty
            halves = [0,1] if is_xm(0) else [0]
            out=[]
            for h in halves:
                dst = qwords(0,h); src = qwords(1,h); n = 8//esz
                out.append(f"{{ uint64_t _a={dst}, _b=({src}), _r=0;"
                           f" for(int _k=0;_k<{n};_k++){{ {ty} _x=({ty})({uty})(_a>>(_k*{esz*8}));"
                           f" {ty} _y=({ty})({uty})(_b>>(_k*{esz*8}));"
                           f" {uty} _z = (_x {cop} _y) ? ({uty})~({uty})0 : 0;"
                           f" _r |= (uint64_t)_z << (_k*{esz*8}); }} {dst}=_r; }}")
            return out
        if m in PMIX and (is_mm(0) or is_xm(0)):
            esz, signed, cop = PMIX[m]
            uty = {1:'uint8_t',2:'uint16_t'}[esz]
            sty = {1:'int8_t',2:'int16_t'}[esz]
            ty  = sty if signed else uty
            halves = [0,1] if is_xm(0) else [0]
            out=[]
            for h in halves:
                dst = qwords(0,h); src = qwords(1,h); n = 8//esz
                out.append(f"{{ uint64_t _a={dst}, _b=({src}), _r=0;"
                           f" for(int _k=0;_k<{n};_k++){{ {ty} _x=({ty})({uty})(_a>>(_k*{esz*8}));"
                           f" {ty} _y=({ty})({uty})(_b>>(_k*{esz*8}));"
                           f" {uty} _z=({uty})((_x {cop} _y)?_x:_y);"
                           f" _r |= (uint64_t)_z << (_k*{esz*8}); }} {dst}=_r; }}")
            return out
        if m in PAVG and (is_mm(0) or is_xm(0)):
            esz = PAVG[m]; uty = {1:'uint8_t',2:'uint16_t'}[esz]
            halves = [0,1] if is_xm(0) else [0]
            out=[]
            for h in halves:
                dst = qwords(0,h); src = qwords(1,h); n = 8//esz
                out.append(f"{{ uint64_t _a={dst}, _b=({src}), _r=0;"
                           f" for(int _k=0;_k<{n};_k++){{ uint32_t _x=({uty})(_a>>(_k*{esz*8}));"
                           f" uint32_t _y=({uty})(_b>>(_k*{esz*8}));"
                           f" {uty} _z=({uty})((_x+_y+1u)>>1);"
                           f" _r |= (uint64_t)_z << (_k*{esz*8}); }} {dst}=_r; }}")
            return out
        if m == 'psadbw' and (is_mm(0) or is_xm(0)):
            halves = [0,1] if is_xm(0) else [0]
            out=[]
            for h in halves:
                dst = qwords(0,h); src = qwords(1,h)
                out.append(f"{{ uint64_t _a={dst}, _b=({src}); uint32_t _s=0;"
                           f" for(int _k=0;_k<8;_k++){{ int _x=(uint8_t)(_a>>(_k*8));"
                           f" int _y=(uint8_t)(_b>>(_k*8)); _s += (uint32_t)(_x>_y?_x-_y:_y-_x); }}"
                           f" {dst}=(uint64_t)_s; }}")
            return out

        if m in SATOPS and (is_mm(0) or is_xm(0)):
            esz, mode = SATOPS[m]
            kind, di = mm_dst_idx()
            dst = f"c->mm[{di}]" if kind=='mm' else f"c->xmm[{di}].u64[0]"
            src = mm_src(1, False)
            n = 8 // esz; bits2 = esz*8
            if abs(mode)==1:      # signed saturation
                st = {1:'int8_t',2:'int16_t'}[esz]
                lo = -(1<<(bits2-1)); hi = (1<<(bits2-1))-1
                opx = '+' if mode>0 else '-'
                body = (f"int32_t _v = (int32_t)({st})(_a>>(_k*{bits2})) {opx} (int32_t)({st})(_b>>(_k*{bits2}));"
                        f" if(_v<{lo}) _v={lo}; if(_v>{hi}) _v={hi};"
                        f" _r |= ((uint64_t)(uint{bits2}_t)(int{bits2}_t)_v) << (_k*{bits2});")
            else:                 # unsigned saturation
                ut = {1:'uint8_t',2:'uint16_t'}[esz]
                hi = (1<<bits2)-1
                opx = '+' if mode>0 else '-'
                body = (f"int32_t _v = (int32_t)({ut})(_a>>(_k*{bits2})) {opx} (int32_t)({ut})(_b>>(_k*{bits2}));"
                        f" if(_v<0) _v=0; if(_v>{hi}) _v={hi};"
                        f" _r |= ((uint64_t)(uint{bits2}_t)_v) << (_k*{bits2});")
            if kind == 'xmm':
                # 128-bit: both 64-bit halves (the elements stay in their half)
                return [f"{{ uint64_t _a={qwords(0,h)}, _b=({qwords(1,h)}), _r=0;"
                        f" for(int _k=0;_k<{n};_k++){{ {body} }} {qwords(0,h)}=_r; }}" for h in (0, 1)]
            return [f"{{ uint64_t _a={dst}, _b=({src}), _r=0;"
                    f" for(int _k=0;_k<{n};_k++){{ {body} }} {dst}=_r; }}"]

        # shifts
        PSH = {'psllw':(2,'<<'),'pslld':(4,'<<'),'psllq':(8,'<<'),
               'psrlw':(2,'>>'),'psrld':(4,'>>'),'psrlq':(8,'>>'),
               'psraw':(2,'a'),'psrad':(4,'a')}
        if m in PSH and (is_mm(0) or is_xm(0)):
            esz, kindop = PSH[m]
            kind, di = mm_dst_idx()
            dst = f"c->mm[{di}]" if kind=='mm' else f"c->xmm[{di}].u64[0]"
            cnt2 = (f"{o[1].imm & 0xff}u" if o[1].type==X86_OP_IMM else mm_src(1, False))
            # a register or memory count: all 64 bits of it (2^32+4 empties the
            # destination, not a shift by 4), read once (it may be the destination's
            # own low qword); an XMM destination: both 64-bit halves by that count
            xcnt = f"uint64_t _c=(uint64_t)({cnt2}); uint32_t _s=_c>63?64u:(uint32_t)_c;"
            mcnt = f"uint32_t _s=(uint32_t)({cnt2});" if o[1].type==X86_OP_IMM else xcnt
            halves = (f"c->xmm[{di}].u64[0]", f"c->xmm[{di}].u64[1]") if kind=='xmm' else ()
            if esz==8:
                if halves:
                    return [f"{{ {xcnt}" + "".join(f" {d} = _s>63?0:({d} {kindop} _s);" for d in halves) + " }"]
                return [f"{{ {mcnt} {dst} = _s>63?0:({dst} {kindop} _s); }}"]
            n = 8 // esz; bits2 = esz*8
            ut = {2:'uint16_t',4:'uint32_t'}[esz]
            st = {2:'int16_t',4:'int32_t'}[esz]
            if kindop=='a':
                body=(f"{st} _x=({st})(_a>>(_k*{bits2})); {st} _z = _s>={bits2} ? (_x<0?-1:0) : (_x >> _s);"
                      f" _r |= ((uint64_t)({ut})_z) << (_k*{bits2});")
            elif kindop=='<<':
                body=(f"{ut} _x=({ut})(_a>>(_k*{bits2})); {ut} _z = _s>={bits2} ? 0 : ({ut})(_x << _s);"
                      f" _r |= ((uint64_t)_z) << (_k*{bits2});")
            else:
                body=(f"{ut} _x=({ut})(_a>>(_k*{bits2})); {ut} _z = _s>={bits2} ? 0 : ({ut})(_x >> _s);"
                      f" _r |= ((uint64_t)_z) << (_k*{bits2});")
            if halves:
                return [f"{{ {xcnt}" + "".join(f" {{ uint64_t _a={d}, _r=0; for(int _k=0;_k<{n};_k++){{ {body} }} {d}=_r; }}"
                                               for d in halves) + " }"]
            return [f"{{ uint64_t _a={dst}, _r=0; {mcnt}"
                    f" for(int _k=0;_k<{n};_k++){{ {body} }} {dst}=_r; }}"]

        # punpck / packuswb
        UNP = {'punpcklbw':(1,0),'punpcklwd':(2,0),'punpckldq':(4,0),
               'punpckhbw':(1,1),'punpckhwd':(2,1),'punpckhdq':(4,1)}
        if m in UNP and is_xm(0):
            # 128-bit: the low (punpckl*) or high (punpckh*) qwords of both
            # operands interleave into the whole register
            esz, hi = UNP[m]
            di = XM[o[0].reg]
            n = 8 // esz; bits2 = esz*8; mask = (1 << bits2) - 1
            return [f"{{ uint64_t _a=c->xmm[{di}].u64[{hi}], _b=({qwords(1, hi)}), _r[2]={{0,0}};"
                    f" for(int _k=0;_k<{n};_k++){{ int _p=2*_k*{bits2};"
                    f" _r[_p>>6] |= ((_a>>(_k*{bits2})) & 0x{mask:x}ull) << (_p&63);"
                    f" _r[(_p+{bits2})>>6] |= ((_b>>(_k*{bits2})) & 0x{mask:x}ull) << ((_p+{bits2})&63); }}"
                    f" c->xmm[{di}].u64[0]=_r[0]; c->xmm[{di}].u64[1]=_r[1]; }}"]
        if m in UNP and is_mm(0):
            esz, hi = UNP[m]
            di = MM[o[0].reg]; src = mm_src(1, False)
            n = 4 // esz if esz<8 else 1
            bits2 = esz*8
            base = f"({'4' if hi else '0'})"
            return [f"{{ uint64_t _a=c->mm[{di}], _b=({src}), _r=0; int _o={'(8/2)/'+str(esz) if False else ('4/'+str(esz))} * {1 if hi else 0};"
                    f" for(int _k=0;_k<{n};_k++){{"
                    f" uint64_t _x=(_a>>((_o+_k)*{bits2})) & ((1ull<<{bits2})-1);"
                    f" uint64_t _y=(_b>>((_o+_k)*{bits2})) & ((1ull<<{bits2})-1);"
                    f" _r |= _x << ((2*_k)*{bits2}); _r |= _y << ((2*_k+1)*{bits2}); }}"
                    f" c->mm[{di}]=_r; (void){base}; }}"]
        if m=='packuswb' and is_mm(0):
            di = MM[o[0].reg]; src = mm_src(1, False)
            return [f"{{ uint64_t _a=c->mm[{di}], _b=({src}), _r=0;"
                    f" for(int _k=0;_k<4;_k++){{ int32_t _x=(int16_t)(_a>>(_k*16)); if(_x<0)_x=0; if(_x>255)_x=255;"
                    f" _r |= (uint64_t)(uint8_t)_x << (_k*8); }}"
                    f" for(int _k=0;_k<4;_k++){{ int32_t _y=(int16_t)(_b>>(_k*16)); if(_y<0)_y=0; if(_y>255)_y=255;"
                    f" _r |= (uint64_t)(uint8_t)_y << ((4+_k)*8); }} c->mm[{di}]=_r; }}"]
        if m=='pmaddwd' and is_mm(0):
            di = MM[o[0].reg]; src = mm_src(1, False)
            return [f"{{ uint64_t _a=c->mm[{di}], _b=({src}), _r=0;"
                    f" for(int _k=0;_k<2;_k++){{ int32_t _s=0;"
                    f" for(int _j=0;_j<2;_j++){{ int _i=_k*2+_j;"
                    f" _s += (int32_t)(int16_t)(_a>>(_i*16)) * (int32_t)(int16_t)(_b>>(_i*16)); }}"
                    f" _r |= (uint64_t)(uint32_t)_s << (_k*32); }} c->mm[{di}]=_r; }}"]

        # ---------------- SSE packed single precision ----------------
        # ---------------- SSE2 packed/scalar double ----------------
        PD = {'addpd':'+','subpd':'-','mulpd':'*','divpd':'/'}
        if m in PD and is_xm(0):
            di = XM[o[0].reg]; op_c = PD[m]
            src = (lambda k: f"c->xmm[{XM[o[1].reg]}].d[{k}]") if is_xm(1) else \
                  (lambda k: f"MRF64({self.ea(o[1].mem, extra=8*k)})")
            return [f"{{ double _a=c->xmm[{di}].d[{k}], _b={src(k)};"
                    f" c->xmm[{di}].d[{k}]=sse_r2d(_a,_b,_a {op_c} _b); }}" for k in (0,1)]
        if m in ('minpd','maxpd','sqrtpd') and is_xm(0):
            di = XM[o[0].reg]
            src = (lambda k: f"c->xmm[{XM[o[1].reg]}].d[{k}]") if is_xm(1) else \
                  (lambda k: f"MRF64({self.ea(o[1].mem, extra=8*k)})")
            if m == 'sqrtpd':
                return [f"{{ double _a={src(k)}; c->xmm[{di}].d[{k}]=sse_r1d(_a,sqrt(_a)); }}"
                        for k in (0,1)]
            cop = '>' if m=='maxpd' else '<'
            return [f"{{ double _a=c->xmm[{di}].d[{k}], _b={src(k)};"
                    f" c->xmm[{di}].d[{k}] = (_a {cop} _b) ? _a : _b; }}" for k in (0,1)]

        # ---------------- SSE scalar min/max/sqrt ----------------
        if m in ('minss','maxss','sqrtss','rcpss','rsqrtss') and is_xm(0):
            di = XM[o[0].reg]
            src = (f"c->xmm[{XM[o[1].reg]}].f[0]" if is_xm(1) else f"MRF32{self.ea(o[1].mem)}")
            if m == 'sqrtss':
                return [f"{{ float _a={src}; c->xmm[{di}].f[0]=sse_r1f(_a,sqrtf(_a)); }}"]
            if m == 'rcpss':   return [f"c->xmm[{di}].f[0] = 1.0f/({src});"]
            if m == 'rsqrtss': return [f"c->xmm[{di}].f[0] = 1.0f/sqrtf({src});"]
            cop = '>' if m=='maxss' else '<'
            return [f"{{ float _a=c->xmm[{di}].f[0], _b={src};"
                    f" c->xmm[{di}].f[0] = (_a {cop} _b) ? _a : _b; }}"]
        if m in ('minsd','maxsd','sqrtsd') and is_xm(0):
            di = XM[o[0].reg]
            src = (f"c->xmm[{XM[o[1].reg]}].d[0]" if is_xm(1) else f"MRF64{self.ea(o[1].mem)}")
            if m == 'sqrtsd':
                return [f"{{ double _a={src}; c->xmm[{di}].d[0]=sse_r1d(_a,sqrt(_a)); }}"]
            cop = '>' if m=='maxsd' else '<'
            return [f"{{ double _a=c->xmm[{di}].d[0], _b={src};"
                    f" c->xmm[{di}].d[0] = (_a {cop} _b) ? _a : _b; }}"]

        # ---------------- SSE compares (cmpps/cmpss/cmppd/cmpsd) -------------
        # capstone folds the predicate into the mnemonic: cmpeqps, cmpnltss, ...
        CMPPRED = {'eq':'(_a==_b)', 'lt':'(_a<_b)', 'le':'(_a<=_b)',
                   'unord':'(_a!=_a||_b!=_b)', 'neq':'!(_a==_b)',
                   'nlt':'!(_a<_b)', 'nle':'!(_a<=_b)',
                   'ord':'(_a==_a&&_b==_b)'}
        for _pred, _ex in CMPPRED.items():
            for _sfx, _lanes, _cty, _fld, _msk in (('ps',4,'float','f','0xFFFFFFFFu'),
                                                   ('ss',1,'float','f','0xFFFFFFFFu'),
                                                   ('pd',2,'double','d','0xFFFFFFFFFFFFFFFFull'),
                                                   ('sd',1,'double','d','0xFFFFFFFFFFFFFFFFull')):
                if m != 'cmp' + _pred + _sfx or not is_xm(0): continue
                di = XM[o[0].reg]
                esz = 4 if _cty == 'float' else 8
                uf  = 'u32' if _cty == 'float' else 'u64'
                if is_xm(1):
                    srcf = lambda k, si=XM[o[1].reg]: f"c->xmm[{si}].{_fld}[{k}]"
                else:
                    rdm = 'MRF32' if _cty == 'float' else 'MRF64'
                    srcf = lambda k: f"{rdm}({self.ea(o[1].mem, extra=esz*k)})"
                return [f"{{ {_cty} _a=c->xmm[{di}].{_fld}[{k}], _b={srcf(k)};"
                        f" c->xmm[{di}].{uf}[{k}] = {_ex} ? {_msk} : 0; }}"
                        for k in range(_lanes)]

        # ---------------- SSE conversions ----------------
        # MMX <-> SSE (v12na port): 0x73392f 'cvtpi2ps xmm6,[0x8191c0]' runs in
        # the race with performance level 4 and used to trap (no-op).
        if m == 'cvtpi2ps' and is_xm(0):          # 2 x int32 -> low 2 floats, high kept
            di = XM[o[0].reg]; q = mm_src(1, False)
            return [f"{{ uint64_t _q=({q}); c->xmm[{di}].f[0]=(float)(int32_t)(uint32_t)_q;"
                    f" c->xmm[{di}].f[1]=(float)(int32_t)(uint32_t)(_q>>32); }}"]
        if m == 'cvtpi2pd' and is_xm(0):          # 2 x int32 -> 2 doubles
            di = XM[o[0].reg]; q = mm_src(1, False)
            return [f"{{ uint64_t _q=({q}); c->xmm[{di}].d[0]=(double)(int32_t)(uint32_t)_q;"
                    f" c->xmm[{di}].d[1]=(double)(int32_t)(uint32_t)(_q>>32); }}"]
        if m in ('cvtps2pi','cvttps2pi') and is_mm(0):
            dm = MM[o[0].reg]
            src = (lambda k: f"c->xmm[{XM[o[1].reg]}].f[{k}]") if is_xm(1) else \
                  (lambda k: f"MRF32({self.ea(o[1].mem, extra=4*k)})")
            rnd = 'trunc' if m == 'cvttps2pi' else 'nearbyint'
            return [f"{{ double _a=(double)({src(0)}), _b=(double)({src(1)});"
                    f" c->mm[{dm}] = (uint64_t)x86_cvt_i32({rnd}(_a))"
                    f" | ((uint64_t)x86_cvt_i32({rnd}(_b)) << 32); }}"]
        if m in ('cvtpd2pi','cvttpd2pi') and is_mm(0):
            dm = MM[o[0].reg]
            src = (lambda k: f"c->xmm[{XM[o[1].reg]}].d[{k}]") if is_xm(1) else \
                  (lambda k: f"MRF64({self.ea(o[1].mem, extra=8*k)})")
            rnd = 'trunc' if m == 'cvttpd2pi' else 'nearbyint'
            return [f"{{ double _a=({src(0)}), _b=({src(1)});"
                    f" c->mm[{dm}] = (uint64_t)x86_cvt_i32({rnd}(_a))"
                    f" | ((uint64_t)x86_cvt_i32({rnd}(_b)) << 32); }}"]
        if m in ('cvtps2dq','cvttps2dq') and is_xm(0):
            di = XM[o[0].reg]
            src = (lambda k: f"c->xmm[{XM[o[1].reg]}].f[{k}]") if is_xm(1) else \
                  (lambda k: f"MRF32({self.ea(o[1].mem, extra=4*k)})")
            rnd = 'trunc' if m == 'cvttps2dq' else 'nearbyint'
            return [f"{{ double _t[4]; " +
                    " ".join(f"_t[{k}]=(double)({src(k)});" for k in range(4)) +
                    " " + " ".join(f"c->xmm[{di}].u32[{k}]=x86_cvt_i32({rnd}(_t[{k}]));"
                                   for k in range(4)) + " }"]
        if m == 'cvtdq2ps' and is_xm(0):
            di = XM[o[0].reg]
            src = (lambda k: f"(int32_t)c->xmm[{XM[o[1].reg]}].u32[{k}]") if is_xm(1) else \
                  (lambda k: f"(int32_t)MR32({self.ea(o[1].mem, extra=4*k)})")
            return [f"{{ int32_t _t[4]; " +
                    " ".join(f"_t[{k}]={src(k)};" for k in range(4)) +
                    " " + " ".join(f"c->xmm[{di}].f[{k}]=(float)_t[{k}];" for k in range(4)) + " }"]
        if m == 'cvtdq2pd' and is_xm(0):
            di = XM[o[0].reg]
            src = (lambda k: f"(int32_t)c->xmm[{XM[o[1].reg]}].u32[{k}]") if is_xm(1) else \
                  (lambda k: f"(int32_t)MR32({self.ea(o[1].mem, extra=4*k)})")
            return [f"{{ int32_t _a={src(0)}, _b={src(1)};"
                    f" c->xmm[{di}].d[0]=(double)_a; c->xmm[{di}].d[1]=(double)_b; }}"]
        if m == 'cvtps2pd' and is_xm(0):
            di = XM[o[0].reg]
            src = (lambda k: f"c->xmm[{XM[o[1].reg]}].f[{k}]") if is_xm(1) else \
                  (lambda k: f"MRF32({self.ea(o[1].mem, extra=4*k)})")
            return [f"{{ float _a={src(0)}, _b={src(1)};"
                    f" c->xmm[{di}].d[0]=(double)_a; c->xmm[{di}].d[1]=(double)_b; }}"]
        if m == 'cvtpd2ps' and is_xm(0):
            di = XM[o[0].reg]
            src = (lambda k: f"c->xmm[{XM[o[1].reg]}].d[{k}]") if is_xm(1) else \
                  (lambda k: f"MRF64({self.ea(o[1].mem, extra=8*k)})")
            return [f"{{ double _a={src(0)}, _b={src(1)};"
                    f" c->xmm[{di}].f[0]=(float)_a; c->xmm[{di}].f[1]=(float)_b;"
                    f" c->xmm[{di}].u64[1]=0; }}"]
        if m in ('cvtpd2dq','cvttpd2dq') and is_xm(0):
            di = XM[o[0].reg]
            src = (lambda k: f"c->xmm[{XM[o[1].reg]}].d[{k}]") if is_xm(1) else \
                  (lambda k: f"MRF64({self.ea(o[1].mem, extra=8*k)})")
            rnd = 'trunc' if m == 'cvttpd2dq' else 'nearbyint'
            return [f"{{ double _a={src(0)}, _b={src(1)};"
                    f" c->xmm[{di}].u32[0]=x86_cvt_i32({rnd}(_a));"
                    f" c->xmm[{di}].u32[1]=x86_cvt_i32({rnd}(_b));"
                    f" c->xmm[{di}].u64[1]=0; }}"]

        PS = {'addps':'+','subps':'-','mulps':'*','divps':'/'}
        SS = {'addss':'+','subss':'-','mulss':'*','divss':'/'}
        if m in PS and is_xm(0):
            di = XM[o[0].reg]; op_c = PS[m]
            src = (f"c->xmm[{XM[o[1].reg]}].f[_k]" if is_xm(1)
                   else f"MRF32(({self.ea(o[1].mem)})+4u*(uint32_t)_k)")
            return [f"for(int _k=0;_k<4;_k++){{ float _a=c->xmm[{di}].f[_k], _b={src};"
                    f" c->xmm[{di}].f[_k]=sse_r2f(_a,_b,_a {op_c} _b); }}"]
        if m in SS and is_xm(0):
            di = XM[o[0].reg]; op_c = SS[m]
            src = (f"c->xmm[{XM[o[1].reg]}].f[0]" if is_xm(1) else f"MRF32{self.ea(o[1].mem)}")
            return [f"{{ float _a=c->xmm[{di}].f[0], _b={src};"
                    f" c->xmm[{di}].f[0]=sse_r2f(_a,_b,_a {op_c} _b); }}"]
        if m=='shufps' and is_xm(0) and o[2].type==X86_OP_IMM:
            di=XM[o[0].reg]; imm=o[2].imm & 0xff
            sel=[(imm>>0)&3,(imm>>2)&3,(imm>>4)&3,(imm>>6)&3]
            if is_xm(1):
                src = f"c->xmm[{XM[o[1].reg]}].f"
                pre = ""
            else:                       # v12na: shufps xmm, m128, imm (0x68998d)
                src = "_m"; pre = f" float _m[4]; memcpy(_m, g_mem + (uint32_t){self.ea(o[1].mem)}, 16);"
            return [f"{{{pre} float _t[4]; _t[0]=c->xmm[{di}].f[{sel[0]}]; _t[1]=c->xmm[{di}].f[{sel[1]}];"
                    f" _t[2]={src}[{sel[2]}]; _t[3]={src}[{sel[3]}];"
                    f" for(int _k=0;_k<4;_k++) c->xmm[{di}].f[_k]=_t[_k]; }}"]
        if m in ('sqrtps','rcpps','rsqrtps','maxps','minps') and is_xm(0):
            di=XM[o[0].reg]
            if m in ('sqrtps','rcpps','rsqrtps'):
                fn={'sqrtps':'sqrtf(_x)','rcpps':'(1.0f/_x)','rsqrtps':'(1.0f/sqrtf(_x))'}[m]
                src = (f"c->xmm[{XM[o[1].reg]}].f[_k]" if is_xm(1) else f"MRF32({self.ea(o[1].mem)}+4u*(uint32_t)_k)")
                return [f"for(int _k=0;_k<4;_k++){{ float _x={src}; c->xmm[{di}].f[_k]={fn}; }}"]
            cmpf = 'a>b?a:b' if m=='maxps' else 'a<b?a:b'
            src = (f"c->xmm[{XM[o[1].reg]}].f[_k]" if is_xm(1) else f"MRF32({self.ea(o[1].mem)}+4u*(uint32_t)_k)")
            return [f"for(int _k=0;_k<4;_k++){{ float a=c->xmm[{di}].f[_k], b={src}; c->xmm[{di}].f[_k]=({cmpf}); }}"]
        if m in ('movlps','movhps') and is_xm(0) and is_mem(1):
            di=XM[o[0].reg]; ea2=self.ea(o[1].mem); off2 = 0 if m=='movlps' else 2
            return [f"c->xmm[{di}].u32[{off2}]=MR32({ea2}); c->xmm[{di}].u32[{off2+1}]=MR32(({ea2})+4u);"]
        if m in ('movlps','movhps') and is_mem(0) and is_xm(1):
            si=XM[o[1].reg]; ea2=self.ea(o[0].mem); off2 = 0 if m=='movlps' else 2
            return [f"MR32({ea2})=c->xmm[{si}].u32[{off2}]; MR32(({ea2})+4u)=c->xmm[{si}].u32[{off2+1}];"]
        if m in ('cvtsi2ss',) and is_xm(0):
            di=XM[o[0].reg]
            return [f"c->xmm[{di}].f[0] = (float)(int32_t)({E(1)});"]
        if m in ('cvttss2si','cvtss2si'):
            src = (f"c->xmm[{XM[o[1].reg]}].f[0]" if is_xm(1) else f"MRF32{self.ea(o[1].mem)}")
            # without 't' MXCSR rounding (default: nearest); with it, truncation
            expr = f"trunc((double)({src}))" if m=='cvttss2si' else f"nearbyint((double)({src}))"
            return [W(0, f"x86_cvt_i32({expr})")]
        if m in ('ucomiss','comiss'):
            a = (f"c->xmm[{XM[o[0].reg]}].f[0]" if is_xm(0) else f"MRF32{self.ea(o[0].mem)}")
            b = (f"c->xmm[{XM[o[1].reg]}].f[0]" if is_xm(1) else f"MRF32{self.ea(o[1].mem)}")
            return [f"{{ float _x={a}, _y={b};"
                    f" if(_x!=_x||_y!=_y){{ c->zf=1;c->pf=1;c->cf=1; }}"
                    f" else {{ c->pf=0; c->zf=(_x==_y); c->cf=(_x<_y); }} c->of=c->sf=c->af=0; }}"]
        # 3DNow! (AMD) - rarely real code, mostly misdecoded data

        # ---------------- 3DNow! (AMD) : 2x float in MMX registers ----------------
        PF = {'pfadd':'+','pfsub':'-','pfmul':'*','pfsubr':'r-','pfmax':'max','pfmin':'min',
              'pfacc':'acc','pfcmpeq':'==','pfcmpge':'>=','pfcmpgt':'>'}
        if m in PF and is_mm(0):
            di = MM[o[0].reg]
            src = (f"c->mm[{MM[o[1].reg]}]" if is_mm(1) else "MR64"+self.ea(o[1].mem))
            op_c = PF[m]
            pre = (f"{{ uint64_t _av=c->mm[{di}], _bv=({src}); float _a0,_a1,_b0,_b1,_r0,_r1;"
                   f" memcpy(&_a0,(char*)&_av,4); memcpy(&_a1,(char*)&_av+4,4);"
                   f" memcpy(&_b0,(char*)&_bv,4); memcpy(&_b1,(char*)&_bv+4,4);")
            if op_c in ('+','-','*'):
                body = f" _r0=_a0{op_c}_b0; _r1=_a1{op_c}_b1;"
            elif op_c=='r-':
                body = " _r0=_b0-_a0; _r1=_b1-_a1;"
            elif op_c=='max':
                body = " _r0=_a0>_b0?_a0:_b0; _r1=_a1>_b1?_a1:_b1;"
            elif op_c=='min':
                body = " _r0=_a0<_b0?_a0:_b0; _r1=_a1<_b1?_a1:_b1;"
            elif op_c=='acc':
                body = " _r0=_a0+_a1; _r1=_b0+_b1;"
            else:
                cmp2 = {'==':'==','>=':'>=','>':'>'}[op_c]
                return [pre + f" uint32_t _c0=(_a0{cmp2}_b0)?0xFFFFFFFFu:0u, _c1=(_a1{cmp2}_b1)?0xFFFFFFFFu:0u;"
                        f" uint64_t _rv=((uint64_t)_c1<<32)|_c0; c->mm[{di}]=_rv; }}"]
            return [pre + body + f" uint64_t _rv=0; memcpy((char*)&_rv,&_r0,4); memcpy((char*)&_rv+4,&_r1,4);"
                    f" c->mm[{di}]=_rv; }}"]
        if m in ('pfrcp','pfrsqrt') and is_mm(0):
            di = MM[o[0].reg]
            src = (f"c->mm[{MM[o[1].reg]}]" if is_mm(1) else "MR64"+self.ea(o[1].mem))
            fn = "(1.0f/_b0)" if m=='pfrcp' else "(1.0f/sqrtf(_b0))"
            return [f"{{ uint64_t _bv=({src}); float _b0; memcpy(&_b0,(char*)&_bv,4);"
                    f" float _r={fn}; uint64_t _rv=0; memcpy((char*)&_rv,&_r,4); memcpy((char*)&_rv+4,&_r,4);"
                    f" c->mm[{di}]=_rv; }}"]
        if m=='pi2fd' and is_mm(0):
            di = MM[o[0].reg]
            src = (f"c->mm[{MM[o[1].reg]}]" if is_mm(1) else "MR64"+self.ea(o[1].mem))
            return [f"{{ uint64_t _bv=({src}); float _r0=(float)(int32_t)(uint32_t)_bv,"
                    f" _r1=(float)(int32_t)(uint32_t)(_bv>>32); uint64_t _rv=0;"
                    f" memcpy((char*)&_rv,&_r0,4); memcpy((char*)&_rv+4,&_r1,4); c->mm[{di}]=_rv; }}"]
        if m=='pf2id' and is_mm(0):
            di = MM[o[0].reg]
            src = (f"c->mm[{MM[o[1].reg]}]" if is_mm(1) else "MR64"+self.ea(o[1].mem))
            return [f"{{ uint64_t _bv=({src}); float _b0,_b1; memcpy(&_b0,(char*)&_bv,4);"
                    f" memcpy(&_b1,(char*)&_bv+4,4);"
                    f" uint64_t _rv=((uint64_t)(uint32_t)(int32_t)_b1<<32)|(uint32_t)(int32_t)_b0;"
                    f" c->mm[{di}]=_rv; }}"]
        if m=='femms': return ["/* femms */"]
        if m in ('pmulhw','pmulhuw') and is_mm(0):
            di = MM[o[0].reg]
            src = (f"c->mm[{MM[o[1].reg]}]" if is_mm(1) else "MR64"+self.ea(o[1].mem))
            sgn = 'int16_t' if m=='pmulhw' else 'uint16_t'
            return [f"{{ uint64_t _a=c->mm[{di}], _b=({src}), _r=0;"
                    f" for(int _k=0;_k<4;_k++){{ int32_t _p=(int32_t)({sgn})(_a>>(_k*16))"
                    f" * (int32_t)({sgn})(_b>>(_k*16));"
                    f" _r |= ((uint64_t)(uint16_t)(_p>>16)) << (_k*16); }} c->mm[{di}]=_r; }}"]
        if m in ('pcmpeqb','pcmpeqw','pcmpeqd','pcmpgtb','pcmpgtw','pcmpgtd') and is_mm(0):
            esz = {'b':1,'w':2,'d':4}[m[-1]]
            gt  = 'gt' in m
            di = MM[o[0].reg]
            src = (f"c->mm[{MM[o[1].reg]}]" if is_mm(1) else "MR64"+self.ea(o[1].mem))
            n = 8//esz; bits2 = esz*8
            st = {1:'int8_t',2:'int16_t',4:'int32_t'}[esz]
            cmp2 = '>' if gt else '=='
            return [f"{{ uint64_t _a=c->mm[{di}], _b=({src}), _r=0;"
                    f" for(int _k=0;_k<{n};_k++){{"
                    f" {st} _x=({st})(_a>>(_k*{bits2})), _y=({st})(_b>>(_k*{bits2}));"
                    f" uint64_t _m2 = (_x {cmp2} _y) ? ((1ull<<{bits2})-1) : 0ull;"
                    f" _r |= _m2 << (_k*{bits2}); }} c->mm[{di}]=_r; }}"]

        # ---------------- last missing ones ----------------
        if m in ('in','out','insb','insw','insd','outsb','outsw','outsd',
                 'cli','sti','hlt','clts','invd','wbinvd','lgdt','lidt','lldt','ltr',
                 'arpl','bound','into','sysenter','sysexit','rdmsr','wrmsr','rdpmc'):
            # These never run in user mode; they are misdecoded data.
            return [f"/* {m}: privileged/data - no effect */"]
        SD = {'addsd':'+','subsd':'-','mulsd':'*','divsd':'/'}
        if m in SD and is_xm(0):
            di=XM[o[0].reg]; op_c=SD[m]
            src=(f"c->xmm[{XM[o[1].reg]}].d[0]" if is_xm(1) else f"MRF64{self.ea(o[1].mem)}")
            return [f"c->xmm[{di}].d[0] {op_c}= {src};"]
        if m in ('sqrtsd',) and is_xm(0):
            di=XM[o[0].reg]
            src=(f"c->xmm[{XM[o[1].reg]}].d[0]" if is_xm(1) else f"MRF64{self.ea(o[1].mem)}")
            return [f"c->xmm[{di}].d[0] = sqrt({src});"]
        if m in ('sqrtss',) and is_xm(0):
            di=XM[o[0].reg]
            src=(f"c->xmm[{XM[o[1].reg]}].f[0]" if is_xm(1) else f"MRF32{self.ea(o[1].mem)}")
            return [f"c->xmm[{di}].f[0] = sqrtf({src});"]
        if m in ('movhlps','movlhps') and is_xm(0) and is_xm(1):
            di=XM[o[0].reg]; si=XM[o[1].reg]
            if m=='movhlps':
                return [f"c->xmm[{di}].u32[0]=c->xmm[{si}].u32[2]; c->xmm[{di}].u32[1]=c->xmm[{si}].u32[3];"]
            return [f"c->xmm[{di}].u32[2]=c->xmm[{si}].u32[0]; c->xmm[{di}].u32[3]=c->xmm[{si}].u32[1];"]
        if m in ('cvtsi2sd',) and is_xm(0):
            di=XM[o[0].reg]
            return [f"c->xmm[{di}].d[0] = (double)(int32_t)({E(1)});"]
        if m in ('cvttsd2si','cvtsd2si'):
            src=(f"c->xmm[{XM[o[1].reg]}].d[0]" if is_xm(1) else f"MRF64{self.ea(o[1].mem)}")
            expr = f"trunc({src})" if m=='cvttsd2si' else f"nearbyint({src})"
            return [W(0, f"x86_cvt_i32({expr})")]
        if m in ('cvtss2sd',) and is_xm(0):
            di=XM[o[0].reg]
            src=(f"c->xmm[{XM[o[1].reg]}].f[0]" if is_xm(1) else f"MRF32{self.ea(o[1].mem)}")
            return [f"c->xmm[{di}].d[0] = (double)({src});"]
        if m in ('cvtsd2ss',) and is_xm(0):
            di=XM[o[0].reg]
            src=(f"c->xmm[{XM[o[1].reg]}].d[0]" if is_xm(1) else f"MRF64{self.ea(o[1].mem)}")
            return [f"c->xmm[{di}].f[0] = (float)({src});"]
        if m in ('ucomisd','comisd'):
            a=(f"c->xmm[{XM[o[0].reg]}].d[0]" if is_xm(0) else f"MRF64{self.ea(o[0].mem)}")
            b=(f"c->xmm[{XM[o[1].reg]}].d[0]" if is_xm(1) else f"MRF64{self.ea(o[1].mem)}")
            return [f"{{ double _x={a}, _y={b};"
                    f" if(_x!=_x||_y!=_y){{ c->zf=1;c->pf=1;c->cf=1; }}"
                    f" else {{ c->pf=0; c->zf=(_x==_y); c->cf=(_x<_y); }} c->of=c->sf=c->af=0; }}"]
        if m == 'int3':
            return ["/* int3: padding in release code - no effect */"]
        if m in ('pandn','andnps','andnpd'):
            if is_mm(0):
                di=MM[o[0].reg]
                src=(f"c->mm[{MM[o[1].reg]}]" if is_mm(1) else "MR64"+self.ea(o[1].mem))
                return [f"c->mm[{di}] = (~c->mm[{di}]) & ({src});"]
            if is_xm(0):
                di=XM[o[0].reg]
                if is_xm(1):
                    si=XM[o[1].reg]
                    return [f"c->xmm[{di}].u64[0] = (~c->xmm[{di}].u64[0]) & c->xmm[{si}].u64[0];"
                            f" c->xmm[{di}].u64[1] = (~c->xmm[{di}].u64[1]) & c->xmm[{si}].u64[1];"]
                ea2=self.ea(o[1].mem)
                return [f"{{ uint64_t _a=MR64({ea2}), _b=MR64(({ea2})+8u);"
                        f" c->xmm[{di}].u64[0] = (~c->xmm[{di}].u64[0]) & _a;"
                        f" c->xmm[{di}].u64[1] = (~c->xmm[{di}].u64[1]) & _b; }}"]
            raise Unsupported(m)
        if m in ('unpcklps','unpckhps') and is_xm(0):
            di=XM[o[0].reg]; hi = (m=='unpckhps')
            if is_xm(1):
                sv=f"c->xmm[{XM[o[1].reg]}].u32"
            else:
                ea2=self.ea(o[1].mem); sv=None
            o0 = 2 if hi else 0
            if sv:
                return [f"{{ uint32_t _t[4]; _t[0]=c->xmm[{di}].u32[{o0}]; _t[1]={sv}[{o0}];"
                        f" _t[2]=c->xmm[{di}].u32[{o0+1}]; _t[3]={sv}[{o0+1}];"
                        f" for(int _k=0;_k<4;_k++) c->xmm[{di}].u32[_k]=_t[_k]; }}"]
            return [f"{{ uint32_t _t[4]; _t[0]=c->xmm[{di}].u32[{o0}]; _t[1]=MR32(({ea2})+{4*o0}u);"
                    f" _t[2]=c->xmm[{di}].u32[{o0+1}]; _t[3]=MR32(({ea2})+{4*(o0+1)}u);"
                    f" for(int _k=0;_k<4;_k++) c->xmm[{di}].u32[_k]=_t[_k]; }}"]
        if m=='packuswb' and is_xm(0):
            di=XM[o[0].reg]
            src=(f"c->xmm[{XM[o[1].reg]}].u64[0]" if is_xm(1) else "MR64"+self.ea(o[1].mem))
            return [f"{{ uint64_t _a=c->xmm[{di}].u64[0], _b=({src}), _r=0;"
                    f" for(int _k=0;_k<4;_k++){{ int32_t _x=(int16_t)(_a>>(_k*16)); if(_x<0)_x=0; if(_x>255)_x=255;"
                    f" _r |= (uint64_t)(uint8_t)_x << (_k*8); }}"
                    f" for(int _k=0;_k<4;_k++){{ int32_t _y=(int16_t)(_b>>(_k*16)); if(_y<0)_y=0; if(_y>255)_y=255;"
                    f" _r |= (uint64_t)(uint8_t)_y << ((4+_k)*8); }} c->xmm[{di}].u64[0]=_r; }}"]
        if m in ('movlpd','movhpd'):
            base_m = 'movlps' if m=='movlpd' else 'movhps'
            off2 = 0 if m=='movlpd' else 2
            if is_xm(0) and is_mem(1):
                di=XM[o[0].reg]; ea2=self.ea(o[1].mem)
                return [f"c->xmm[{di}].u32[{off2}]=MR32({ea2}); c->xmm[{di}].u32[{off2+1}]=MR32(({ea2})+4u);"]
            if is_mem(0) and is_xm(1):
                si=XM[o[1].reg]; ea2=self.ea(o[0].mem)
                return [f"MR32({ea2})=c->xmm[{si}].u32[{off2}]; MR32(({ea2})+4u)=c->xmm[{si}].u32[{off2+1}];"]
            raise Unsupported(m)
        if m in ('pfrcpit1','pfrcpit2','pfrsqit1'):
            # 3DNow Newton-Raphson refinement steps: dst is accurate enough already
            return [f"/* {m}: refinement step skipped */"]
        if m in ('pfnacc','pfpnacc') and is_mm(0):
            return [f"/* {m} */"]
        if m in ('prefetch','prefetchw','prefetchnta','prefetcht0','prefetcht1','prefetcht2',
                 'sfence','lfence','mfence','pause','nop2'):
            return [f"/* {m}: no effect */"]

        raise Unsupported(m)

    # ---------- function -> C ----------
    def lift(self, entry):
        blocks, calls = self.discover(entry)
        self.last_blocks = blocks; self.entry = entry
        addrs = sorted(blocks)
        # Flag liveness: for every instruction the mask "which flags are read
        # after this one".  FLAGOPT=0 turns it off (for A/B comparisons).
        # A callee of tools/retpast.txt counts as returning flags: when it
        # returns past its caller, the caller's early return hands them on.
        import os as _os
        if _os.environ.get('FLAGOPT','1') == '0':
            self.fafter = {}
        else:
            abi = abi_table()
            try:
                if abi:
                    _, self.fafter, _ = abi_liveness(abi_cfg(blocks, self.in_text, self.jtab), entry, abi[0],
                                                     entry in abi[1] or entry in self.retpast)
                else:
                    self.fafter = flag_liveness(blocks, addrs, self.in_text)
            except Exception: self.fafter = {}
        # collect the branch targets
        lbl=set(addrs)
        out=[]; targets=set()
        body=[]
        x87opt = _os.environ.get('X87OPT','') == '1'
        xb = X87Region()
        for ba in addrs:
            body.append(f"L_{ba:08x}:; /* --- block --- */")
            ins=blocks[ba]
            xb.reset()
            for k,i in enumerate(ins):
                m=i.mnemonic
                if x87opt and xb.active and (m=='jmp' or (m.startswith('j') and m[1:] in CC)
                        or m in ('jecxz','jcxz','loop','loope','loopz','loopne','loopnz')):
                    body += ["    "+l for l in xb.close()]
                if self.watch:
                    body.append(f"    WCHK(0x{i.address:x}u);")
                if self.comments:
                    body.append(f"    /* {i.address:08x}  {i.mnemonic} {i.op_str} */")
                if i.address in self.hooks:
                    # a hook reads and may change ST(0) (widescreen FOV) or
                    # other x87 state: the open X87OPT region is written back
                    # first, and the next x87 instruction reloads from c->st
                    if x87opt and xb.active:
                        body += ["    "+l for l in xb.close()]
                    body.append(f"    guest_hook(c, 0x{i.address:x}u);")
                if m=='jmp':
                    o0=i.operands[0]
                    if o0.type==X86_OP_IMM and self.in_text(o0.imm):
                        body.append(f"    goto L_{o0.imm:08x};"); continue
                    if o0.type==X86_OP_IMM:
                        self.oob_branch = getattr(self,'oob_branch',0)+1
                        body.append(f"    DISPATCH_TAIL(c, 0x{o0.imm & 0xffffffff:x}u);")
                        continue
                    if o0.type!=X86_OP_IMM:
                        tab = self.jtab.get(i.address)
                        if tab:
                            reg, d, ents = tab
                            body.append(f"    switch (c->{R32[reg]}) {{   /* jump table 0x{d:x} */")
                            for k, t in enumerate(ents):
                                body.append(f"    case {k}u: if (MR32(0x{d + 4 * k:x}u) == 0x{t:x}u) goto L_{t:08x}; break;")
                            body.append("    }")
                        body.append(f"    DISPATCH_TAIL(c, {self.rd(o0)});   /* indirect tail call */")
                        continue
                # jecxz/jcxz and the loop family: the condition is not in the CC table.
                # CRITICAL: if these are not lifted, the early-exit branches of CRT
                # routines such as strncmp/strlen are lost.
                LOOPC = {'jecxz':'(c->ecx == 0u)', 'jcxz':'((c->ecx & 0xffffu) == 0u)'}
                if m in LOOPC and i.operands[0].type==X86_OP_IMM:
                    _t=i.operands[0].imm
                    if self.in_text(_t):
                        body.append(f"    if({LOOPC[m]}) goto L_{_t:08x};")
                    else:
                        body.append(f"    if({LOOPC[m]}) DISPATCH_TAIL(c, 0x{_t & 0xffffffff:x}u)")
                    nxt=i.address+i.size
                    if k==len(ins)-1 and nxt in lbl: body.append(f"    goto L_{nxt:08x};")
                    continue
                if m in ('loop','loope','loopz','loopne','loopnz') and i.operands[0].type==X86_OP_IMM:
                    extra = {'loop':'', 'loope':' && c->zf', 'loopz':' && c->zf',
                             'loopne':' && !c->zf', 'loopnz':' && !c->zf'}[m]
                    _t=i.operands[0].imm
                    body.append(f"    c->ecx--;")
                    if self.in_text(_t):
                        body.append(f"    if(c->ecx != 0u{extra}) goto L_{_t:08x};")
                    else:
                        body.append(f"    if(c->ecx != 0u{extra}) DISPATCH_TAIL(c, 0x{_t & 0xffffffff:x}u)")
                    nxt=i.address+i.size
                    if k==len(ins)-1 and nxt in lbl: body.append(f"    goto L_{nxt:08x};")
                    continue
                if m.startswith('j') and m[1:] in CC and i.operands[0].type==X86_OP_IMM:
                    _t=i.operands[0].imm
                    if self.in_text(_t):
                        body.append(f"    if({CC[m[1:]]}) goto L_{_t:08x};")
                    else:
                        self.oob_branch = getattr(self,'oob_branch',0)+1
                        body.append(f"    if({CC[m[1:]]}) {{ unsupported_insn(c, 0x{i.address:x}u, \"branch-out\"); POP32(c->eip); return; }}")
                    nxt=i.address+i.size
                    if k==len(ins)-1 and nxt in lbl: body.append(f"    goto L_{nxt:08x};")
                    continue
                self.cur_fmask = self.fafter.get(i.address, F_ALL)
                try:
                    lines = self.insn(i, targets)
                    if x87opt:
                        if m[:1]=='f' or X86_GRP_FPU in i.groups:
                            new_lines = xb.translate(lines)
                            if new_lines is None:
                                body += ["    "+l for l in xb.close()]
                            else:
                                lines = new_lines
                        elif xb.active and any(_BOUNDARY_RE.search(l) for l in lines):
                            body += ["    "+l for l in xb.close()]
                    for line in lines:
                        body.append("    "+line)
                except Unsupported as e:
                    if not self.trap: raise
                    self.traps+=1; self.trap_kinds[str(e)]+=1
                    esc=str(e).replace('"','')
                    body.append(f'    unsupported_insn(c, 0x{i.address:x}u, "{esc}");')
                except Exception as e:
                    if not self.trap: raise
                    self.traps+=1; self.trap_kinds[f"<{type(e).__name__}>"]+=1
                    body.append(f'    unsupported_insn(c, 0x{i.address:x}u, "internal");')
            if x87opt: body += ["    "+l for l in xb.close()]
            # if the block falls through, jump to the next block
            last=ins[-1] if ins else None
            if last is not None and last.mnemonic not in ('ret','retf','jmp') and not (last.mnemonic.startswith('j') and last.mnemonic[1:] in CC):
                nxt=last.address+last.size
                if nxt in lbl: body.append(f"    goto L_{nxt:08x};")
        # stub labels for the branch targets not generated because of truncation
        refs=set()
        for ba in addrs:
            for i in blocks[ba]:
                m=i.mnemonic
                if (m=='jmp' or (m.startswith('j') and m[1:] in CC)
                    or m in ('jecxz','jcxz','loop','loope','loopz','loopne','loopnz')) \
                   and i.operands and i.operands[0].type==X86_OP_IMM:
                    if self.in_text(i.operands[0].imm): refs.add(i.operands[0].imm)
        missing = sorted(refs - set(addrs))
        for t in missing:
            body.append(f"L_{t:08x}: unsupported_insn(c, 0x{t:x}u, \"truncated\"); POP32(c->eip); return;")
        self.truncated = len(missing)
        out.append(f"/* ===== sub_{entry:08x}  ({sum(len(b) for b in blocks.values())} insns, {len(blocks)} blocks) ===== */")
        # restrict: the guest registers live in the CPU struct, and MR32() writes
        # the SAME type (uint32_t) into g_mem -- so the compiler had to reload
        # fields such as c->eax after every guest memory write.  The CPU
        # struct is not INSIDE g_mem, so restrict is correct and safe
        # (measured: race frame -1.7%).
        out.append(f"void sub_{entry:08x}(CPU *restrict c) {{")
        if self.cov:
            # COVERAGE: to see which functions NEVER run.
            # A single increment; the run-time cost is negligible.
            out.append(f"    cov_hit(0x{entry:08x}u);")
        # CRITICAL: the blocks are printed in address order; the entry block need
        # not have the lowest address.  C execution starts at the top of the body,
        # so the entry block must be jumped to explicitly.
        if xb.used:
            out.append("    uint32_t _T = 0; double _x[16];")
        out.append(f"    goto L_{entry:08x};")
        out += body
        out.append("}")
        code = "\n".join(out)
        if _os.environ.get('LREGS', '') == '1':
            # registers and flags as locals, stored only where others look (tools/lregs.py)
            # (the early 'if (c->eip != ...) return;' after a call to a callee of
            # tools/retpast.txt is a return site like any other: the call has just
            # brought the locals in step, so it stores nothing)
            abi = abi_table()
            code = _lregs().transform(code, abi[0] if abi else None, (abi[1] | self.retpast) if abi else None)
        return code, targets|calls

if __name__=='__main__':
    exe=sys.argv[1]; L=Lifter(exe)
    hdr=['#include "cpu.h"','#include <math.h>','void dispatch_indirect(CPU*,uint32_t);','void dispatch_tail(CPU*);','void cpuid_shim(CPU*);','uint64_t rdtsc_shim(void);','']
    decls=[]; bodies=[]
    for a in sys.argv[2:]:
        ea=int(a,16)
        try:
            code,tg = L.lift(ea)
            decls.append(f"void sub_{ea:08x}(CPU *c);")
            for t in sorted(tg):
                if L.in_text(t): decls.append(f"void sub_{t:08x}(CPU *c);")
            bodies.append(code)
        except Unsupported as e:
            print(f"// 0x{ea:08x} COULD NOT BE LIFTED: unsupported '{e}'", file=sys.stderr)
    print("\n".join(hdr + sorted(set(decls)) + [''] + bodies))


# RawLifter shares Lifter's decoding/generation methods
for _m in ('ea','rd','wr','discover','insn','lift','_fm','_jt_bound','_jt_table','_jt_writes','_JT_PASS'):
    setattr(RawLifter, _m, getattr(Lifter, _m))
