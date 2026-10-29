#!/usr/bin/env python3
"""Pick the functions tests/mworacle.c compares, and read each one's argument
spec from its code (tests/mworacle.sh runs this).

    python -I tests/mworacle_pick.py <speed.exe> <gen/mw/all> <flagabi.json> <out.txt> [n] [seed]

A function qualifies when it and every function it calls (directly, the
closure) are fully lifted code that needs no import, no HLE and no other
runtime help: in the lifted C no indirect call or jump (DISPATCH_CALL,
DISPATCH_TAIL, dispatch_indirect: that is every import call and every
virtual call; the one DISPATCH_TAIL after a JTSWITCH switch aside), no trap
(unsupported_insn), no CPUID or RDTSC shim, no int3 (lifted as a no-op on
purpose), no instruction hook (guest_hook) and no function the game's runtime
overrides (the lift names its body sub_X_orig; a call to sub_X runs the
runtime's code, which tests/mwshards.sh gives the test only as a stand-in
that stops the run); in the x86 no far transfer, no int/into, no in/out, hlt or ud2, no
segment register operand; MMX and x87 not both in one closure (the lift
keeps the MMX registers apart from the x87 stack, cpu.h); at most
CLOSURE_MAX functions in the closure ("near-leaf").  Memory use is bounded
by the test itself: everything outside the image, the scratch area, the
stack and the TEB is unmapped on both sides, so a stray access is a fault
compared by address.

From the qualifying functions a seeded random sample of n is drawn, plus
every qualifying function that returns flags (tools/flagabi.py's list: the
only ones whose flags a caller reads) or has a JTSWITCH switch (lifted as a
C switch, tools/lift.py).  Each line of out.txt:
  <va> <nfn> <args> <regs> <traits> <xsites>
  args   one letter per 4-byte stack argument the function reads, from its
         code (esp/ebp tracked through its control flow, stdcall pops, what
         it pushes for its callees and their specs):
         p pointer (used as an address, here or by a callee it is passed to),
         i index (used as an index register), f float, d/D the low/high half
         of a double (x87 or SSE loads of that size), v value
  regs   eax ecx edx ebx esi edi ebp, the same letters for the registers'
         entry values
  traits x87 w(fnstsw) m(mx) s(se) F(s: segment) R<mask>(eturns flags,
         FLAGABI; the EFLAGS bits every path defines) or '-'; xN: the x87
         code reads N values of its caller's x87 stack
  xsites the closure's x87 instructions the lift computes with libm or loads
         as a double constant: hex address, ':' and s(in) c(os) b(oth,
         fsincos) t(an) e (f2xm1) l (fyl2x) a(tan) m M (fprem fprem1);
         p q r g n (fldpi fldl2e fldl2t fldlg2 fldln2), or '-'
Addresses only: nothing made from the game's files goes into the repo.
"""
import sys, os, re, json, random, collections
os.environ.setdefault('JTSWITCH', '1')          # the web lift's switch tables (gen/mw/all/web-lift.txt)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from capstone.x86 import *
from lift import Lifter

EXE, GEN, FLAGABI, OUT = sys.argv[1:5]
N = int(sys.argv[5]) if len(sys.argv) > 5 else 1200
SEED = int(sys.argv[6]) if len(sys.argv) > 6 else 1
CLOSURE_MAX = 12

# ---- the lifted C: one body per function --------------------------------
HEAD = re.compile(r'^/\* ===== sub_([0-9a-f]{8}) ')
CALL = re.compile(r'\bsub_([0-9a-f]{8})\(c\)')
DENY = ('DISPATCH_CALL', 'DISPATCH_TAIL', 'dispatch_indirect', 'unsupported_insn', 'cpuid_shim', 'rdtsc_shim',
        'guest_hook', 'cov_hit', 'WCHK', '/* int3')
# A JTSWITCH switch (tools/lift.py) is followed by one DISPATCH_TAIL line, the
# jump through the table for an index no case took or an entry that is no
# longer the lifted one: that line alone is allowed.  The tables are in
# read-only .text on both sides, so the test never takes it (and would see
# it as "left", a difference, if it did)
JT_SWITCH, JT_TAIL = '/* jump table 0x', '/* indirect tail call */'
body_bad, callees, has_jt, overridden = {}, {}, set(), set()
ORIG = re.compile(r'^void sub_([0-9a-f]{8})_orig\(')   # the body of a function the game's runtime overrides
for f in sorted(os.listdir(GEN)):
    if not re.match(r'shard_\d+\.c$', f): continue
    cur = None; sw = 0           # 1 in a switch, 2 on the line after its closing brace
    for ln in open(os.path.join(GEN, f)):
        m = HEAD.match(ln)
        if m:
            cur = int(m.group(1), 16); body_bad[cur] = False; callees[cur] = set(); sw = 0; continue
        if cur is None: continue
        mo = ORIG.match(ln)
        if mo: overridden.add(int(mo.group(1), 16))
        tail = sw == 2 and 'DISPATCH_TAIL(c, ' in ln and ln.count('DISPATCH_TAIL') == 1 and ln.rstrip().endswith(JT_TAIL)
        sw = 1 if JT_SWITCH in ln else 2 if sw == 1 and ln.strip() == '}' else 0 if sw == 2 else sw
        if JT_SWITCH in ln: has_jt.add(cur)
        if not body_bad[cur] and any(k in ln for k in DENY if not (tail and k == 'DISPATCH_TAIL')): body_bad[cur] = True
        for c in CALL.findall(ln): callees[cur].add(int(c, 16))
for a in callees: callees[a].discard(a)
fns = sorted(body_bad)
print(f"lifted functions: {len(fns)}", file=sys.stderr)

flags_out = {int(x, 16) for x in json.load(open(FLAGABI))['flags_out']}

# ---- the x86 of each function -------------------------------------------
L = Lifter(EXE)
X87_PREFIX = 'f'
NOT_X87 = {'fxsave', 'fxrstor'}
BAD_INSN = {'int', 'into', 'int1', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'hlt', 'ud2', 'lcall', 'ljmp',
            'iret', 'iretd', 'retf', 'sysenter', 'syscall', 'cpuid', 'rdtsc', 'int3', 'cli', 'sti', 'lds', 'les',
            'lfs', 'lgs', 'lss'}
MMX_REGS = set(range(X86_REG_MM0, X86_REG_MM7 + 1))
XMM_REGS = set(range(X86_REG_XMM0, X86_REG_XMM7 + 1))
# a segment register as an operand (mov r, ds; push es; mov es, r...): the
# lift has no selectors, Unicorn the test's GDT ones
SEG_REGS = {X86_REG_CS, X86_REG_DS, X86_REG_ES, X86_REG_FS, X86_REG_GS, X86_REG_SS}

def traits(blocks):
    t = set()
    for ins in blocks.values():
        for i in ins:
            m = i.mnemonic
            if m in BAD_INSN or m.startswith('lock ') and m[5:] not in ('xadd', 'add', 'sub', 'inc', 'dec', 'or', 'and', 'xor', 'cmpxchg', 'xchg', 'adc', 'sbb', 'not', 'neg', 'btc', 'bts', 'btr'):
                t.add('bad')
            if m.startswith(X87_PREFIX) and m not in NOT_X87: t.add('x87')
            if m in ('fnstsw', 'fstsw'): t.add('fsw')
            if m == 'emms': t.add('mmx')
            for o in i.operands:
                if o.type == X86_OP_REG:
                    if o.reg in MMX_REGS: t.add('mmx')
                    if o.reg in XMM_REGS: t.add('sse')
                    if o.reg in SEG_REGS: t.add('bad')
                elif o.type == X86_OP_MEM and o.mem.segment not in (0, X86_REG_DS, X86_REG_SS, X86_REG_ES):
                    if o.mem.segment == X86_REG_FS: t.add('fs')
                    else: t.add('bad')
    return t

info = {}
def get(a):
    if a not in info:
        try: blocks, _ = L.discover(a)
        except Exception: blocks = {}
        jt = dict(L.jtab)
        pops = {i.operands[0].imm for ins in blocks.values() for i in ins
                if i.mnemonic == 'ret' and i.operands and i.operands[0].type == X86_OP_IMM}
        rets = any(ins and ins[-1].mnemonic == 'ret' for ins in blocks.values())
        info[a] = dict(blocks=blocks, jtab=jt, traits=traits(blocks) if blocks else {'bad'},
                       pop=(pops.pop() if len(pops) == 1 else (None if pops else 0)), rets=rets)
    return info[a]

# ---- qualifying functions ------------------------------------------------
def closure(a):
    seen, q = {a}, [a]
    while q:
        x = q.pop()
        for c in callees.get(x, ()):
            if c not in seen:
                seen.add(c); q.append(c)
                if len(seen) > CLOSURE_MAX: return None
    return seen

qual = []
why = collections.Counter()
for a in fns:
    cl = closure(a)
    if cl is None: why['closure > %d' % CLOSURE_MAX] += 1; continue
    if cl & overridden: why['the runtime overrides'] += 1; continue
    if any(x not in body_bad or body_bad[x] for x in cl): why['indirect, trap, shim, int3 or hook'] += 1; continue
    t = set()
    for x in cl: t |= get(x)['traits']
    if 'bad' in t: why['instruction'] += 1; continue
    if 'mmx' in t and 'x87' in t: why['MMX and x87'] += 1; continue
    if not get(a)['rets']: why['no ret'] += 1; continue
    qual.append((a, len(cl), t, cl))
print(f"qualifying: {len(qual)}; left out: " + ', '.join(f'{k} {v}' for k, v in why.most_common()), file=sys.stderr)

# ---- argument spec from the code ----------------------------------------
GP = {X86_REG_EAX: 'eax', X86_REG_ECX: 'ecx', X86_REG_EDX: 'edx', X86_REG_EBX: 'ebx',
      X86_REG_ESI: 'esi', X86_REG_EDI: 'edi', X86_REG_EBP: 'ebp', X86_REG_ESP: 'esp'}
FAM = {}
for r, n in GP.items(): FAM[r] = n
for r, n in ((X86_REG_AX, 'eax'), (X86_REG_AL, 'eax'), (X86_REG_AH, 'eax'), (X86_REG_CX, 'ecx'), (X86_REG_CL, 'ecx'),
             (X86_REG_CH, 'ecx'), (X86_REG_DX, 'edx'), (X86_REG_DL, 'edx'), (X86_REG_DH, 'edx'), (X86_REG_BX, 'ebx'),
             (X86_REG_BL, 'ebx'), (X86_REG_BH, 'ebx'), (X86_REG_SI, 'esi'), (X86_REG_DI, 'edi'), (X86_REG_BP, 'ebp'),
             (X86_REG_SP, 'esp')):
    FAM[r] = n
REGS7 = ('eax', 'ecx', 'edx', 'ebx', 'esi', 'edi', 'ebp')

RANK = {'v': 0, 'i': 1, 'f': 2, 'd': 2, 'D': 2, 'p': 3}
_spec, _busy = {}, set()
def spec(a):
    """(arg letters, reg letters) from a's code, and from the specs of the
    functions it calls for what it passes them (pushed arguments, ecx...)"""
    if a in _spec: return _spec[a]
    if a in _busy: return ('', 'vvvvvvv')          # recursion: what is known so far
    _busy.add(a)
    blocks = get(a)['blocks']
    arg = {}            # slot -> letter
    regk = {}           # register -> letter, for the registers' entry values
    def mark_src(src, ch):
        if not src: return
        if src[0] == 'A':
            k = src[1]
            if k < 0 or k > 31: return
            old = arg.get(k, 'v')
            if ch in ('d', 'D') or RANK[ch] > RANK[old]: arg[k] = ch
        elif src[0] == 'R' and RANK[ch] > RANK[regk.get(src[1], 'v')]: regk[src[1]] = ch
    def slot_of(o, esp, ebp):
        """the argument slot (from the entry esp) a memory operand names, or None"""
        b, d = o.mem.base, o.mem.disp
        off = esp + d if b == X86_REG_ESP and esp is not None else ebp + d if b == X86_REG_EBP and ebp is not None else None
        return (off - 4) // 4 if off is not None and off >= 4 and o.mem.index == 0 else None
    # state: (esp delta, ebp delta, origin per register, origin per stack slot below the entry esp)
    entry = (0, None, {r: ('R', r) for r in REGS7}, {})
    state_in = {a: entry}
    work = [a]; visits = collections.Counter()
    while work:
        ba = work.pop()
        visits[ba] += 1
        if visits[ba] > 8 or ba not in blocks: continue
        esp, ebp, org, stk = state_in[ba][0], state_in[ba][1], dict(state_in[ba][2]), dict(state_in[ba][3])
        ins = blocks[ba]
        for i in ins:
            m = i.mnemonic; ops = i.operands
            # memory operands: argument slots read, registers used as addresses or indexes
            for o in ops:
                if o.type != X86_OP_MEM: continue
                k = slot_of(o, esp, ebp)
                if k is not None and m != 'lea':
                    if m.startswith('f') and m not in ('fild', 'fist', 'fistp', 'fisttp', 'fiadd', 'fisub', 'fisubr', 'fimul', 'fidiv', 'fidivr', 'ficom', 'ficomp', 'fnstcw', 'fldcw', 'fnstsw', 'fnstenv', 'fldenv', 'fnsave', 'frstor'):
                        if o.size == 8: mark_src(('A', k), 'd'); mark_src(('A', k + 1), 'D')
                        elif o.size == 4: mark_src(('A', k), 'f')
                    elif any(x.type == X86_OP_REG and x.reg in XMM_REGS for x in ops) and o.size in (4, 8):
                        if o.size == 8: mark_src(('A', k), 'd'); mark_src(('A', k + 1), 'D')
                        else: mark_src(('A', k), 'f')
                    elif k not in arg: arg[k] = 'v'
                    continue
                b, x = o.mem.base, o.mem.index
                if b in FAM and b != X86_REG_ESP and not (b == X86_REG_EBP and ebp is not None): mark_src(org.get(FAM[b]), 'p')
                if x in FAM and x != X86_REG_ESP:
                    if b == 0 and o.mem.disp & 0xffffffff >= 0x400000: mark_src(org.get(FAM[x]), 'i')   # a table index
                    elif b != 0: mark_src(org.get(FAM[x]), 'i')
            # the calls: what the callee's spec says about what is passed to it
            if m == 'call' and ops and ops[0].type == X86_OP_IMM and ops[0].imm in body_bad:
                cargs, cregs = spec(ops[0].imm)
                for j, ch in enumerate(cargs):
                    if ch != 'v' and esp is not None: mark_src(stk.get(esp + 4 * j), ch)
                for r, ch in zip(REGS7, cregs):
                    if ch != 'v': mark_src(org.get(r), ch)
            # effects
            try: written = {FAM[r] for r in i.regs_access()[1] if r in FAM}
            except Exception: written = set(GP.values())
            new = {}
            if m == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[0].reg in GP:
                dst = GP[ops[0].reg]
                if ops[1].type == X86_OP_REG and ops[1].reg in GP:
                    sname = GP[ops[1].reg]
                    if dst == 'ebp' and sname == 'esp': ebp = esp
                    elif dst == 'esp' and sname == 'ebp': esp = ebp
                    new[dst] = org.get(sname) if sname != 'esp' else None
                elif ops[1].type == X86_OP_MEM:
                    k = slot_of(ops[1], esp, ebp)
                    if k is not None and ops[1].size == 4: new[dst] = ('A', k)
                    else:
                        mm = ops[1].mem
                        off = esp + mm.disp if mm.base == X86_REG_ESP and esp is not None and not mm.index else None
                        new[dst] = stk.get(off) if off is not None else None
            elif m == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[0].mem.base == X86_REG_ESP and \
                    not ops[0].mem.index and esp is not None:
                off = esp + ops[0].mem.disp
                stk[off] = org.get(GP[ops[1].reg]) if ops[1].type == X86_OP_REG and ops[1].reg in GP and ops[1].reg != X86_REG_ESP else None
            if m == 'push':
                if esp is not None:
                    esp -= 4
                    o = ops[0]
                    if o.type == X86_OP_REG and o.reg in GP and o.reg != X86_REG_ESP: stk[esp] = org.get(GP[o.reg])
                    elif o.type == X86_OP_MEM and slot_of(o, esp + 4, ebp) is not None: stk[esp] = ('A', slot_of(o, esp + 4, ebp))
                    else: stk[esp] = None
            elif m == 'pop':
                if esp is not None:
                    o = ops[0]
                    if o.type == X86_OP_REG and o.reg in GP: new[GP[o.reg]] = stk.get(esp)
                    esp += 4
            elif m == 'pushal': esp = None if esp is None else esp - 32
            elif m == 'popal': esp = None if esp is None else esp + 32
            elif m in ('pushfd', 'pushf'): esp = None if esp is None else esp - 4
            elif m in ('popfd', 'popf'): esp = None if esp is None else esp + 4
            elif m in ('sub', 'add') and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_ESP:
                esp = None if esp is None or ops[1].type != X86_OP_IMM else esp + (ops[1].imm if m == 'add' else -ops[1].imm)
            elif m == 'lea' and ops[0].type == X86_OP_REG and ops[0].reg in (X86_REG_ESP, X86_REG_EBP):
                mm = ops[1].mem; base = esp if mm.base == X86_REG_ESP else ebp if mm.base == X86_REG_EBP else None
                v = None if base is None or mm.index else base + mm.disp
                if ops[0].reg == X86_REG_ESP: esp = v
                else: ebp = v
            elif m == 'leave':
                esp = None if ebp is None else ebp + 4; ebp = None
            elif m == 'call':
                o = ops[0]
                pop = get(o.imm)['pop'] if o.type == X86_OP_IMM else None
                esp = None if esp is None or pop is None else esp + pop
                for r in ('eax', 'ecx', 'edx'): org[r] = None
            elif m == 'mov' and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_EBP and not new:
                ebp = None
            elif 'esp' in written and m != 'mov':
                esp = None
            if 'ebp' in written and m not in ('mov', 'lea', 'leave', 'push', 'pop') and ebp is not None:
                ebp = None
            if m == 'pop' and ops and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_EBP: ebp = None
            for r in written:
                if r == 'esp': continue
                org[r] = new.get(r)
            if esp is not None: stk = {k: v for k, v in stk.items() if k >= esp}
        last = ins[-1] if ins else None
        succ = []
        if last is not None:
            m = last.mnemonic
            nxt = last.address + last.size
            if m == 'jmp':
                if last.operands[0].type == X86_OP_IMM: succ = [last.operands[0].imm]
                elif last.address in get(a)['jtab']: succ = list(get(a)['jtab'][last.address][2])
            elif m.startswith('j') or m.startswith('loop'):
                succ = [last.operands[0].imm, nxt] if last.operands[0].type == X86_OP_IMM else []
            elif m not in ('ret', 'retf', 'hlt', 'ud2'): succ = [nxt]
        for sa in succ:
            st = (esp, ebp, org, stk)
            if sa not in state_in: state_in[sa] = st; work.append(sa)
            else:
                o = state_in[sa]
                merged = (o[0] if o[0] == esp else None, o[1] if o[1] == ebp else None,
                          {r: (o[2].get(r) if o[2].get(r) == org.get(r) else None) for r in REGS7},
                          {k: v for k, v in o[3].items() if stk.get(k) == v} if o[0] == esp else {})
                if merged != o: state_in[sa] = merged; work.append(sa)
    n = max(arg) + 1 if arg else 0
    pop = get(a)['pop'] or 0
    n = min(max(n, pop // 4), 16)       # the test fills 16 argument slots (0x7A588C: data that reads "ret 0x7A57")
    res = (''.join(arg.get(k, 'v') for k in range(n)), ''.join(regk.get(r, 'v') for r in REGS7))
    _busy.discard(a); _spec[a] = res
    return res

# ---- x87 stack arguments ------------------------------------------------
# A function is called with an empty x87 stack, unless it takes arguments
# there (the C runtime's _CIpow and friends, _ftol2): how many it reads below
# what it pushed itself, from its code and its callees' (their need and net
# effect).  The test gives it exactly that many: more would overflow the
# eight registers of a function that uses them all (0x6C88D0), which the
# hardware turns into the indefinite NaN and neither Unicorn nor the lift
# models.
ST = {getattr(sys.modules['capstone.x86'], 'X86_REG_ST%d' % k): k for k in range(8)}
X_PUSH = {'fld', 'fild', 'fbld', 'fldz', 'fld1', 'fldpi', 'fldl2e', 'fldl2t', 'fldlg2', 'fldln2'}
X_NOREAD = {'fild', 'fbld', 'fldz', 'fld1', 'fldpi', 'fldl2e', 'fldl2t', 'fldlg2', 'fldln2'}
X_POP1 = {'fstp', 'fistp', 'fisttp', 'fbstp', 'faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp', 'fcomp', 'fucomp',
          'ficomp', 'fcomip', 'fucomip', 'fpatan', 'fyl2x', 'fyl2xp1', 'ffreep'}
X_POP2 = {'fcompp', 'fucompp'}
X_PUSHAFTER = {'fsincos', 'fptan', 'fxtract'}
X_CTRL = {'fnstsw', 'fstsw', 'fnstcw', 'fstcw', 'fldcw', 'fninit', 'finit', 'fwait', 'wait', 'fnclex', 'fclex', 'fnstenv',
          'fstenv', 'fldenv', 'fnsave', 'fsave', 'frstor', 'fxsave', 'fxrstor', 'ffree', 'fdecstp', 'fincstp', 'fnop', 'femms'}
X_ST1 = {'fpatan', 'fyl2x', 'fyl2xp1', 'fscale', 'fprem', 'fprem1', 'fcompp', 'fucompp'}
_x87, _xbusy = {}, set()
def x87_need(a):
    """(values a reads from its caller's x87 stack, its net push count at ret)"""
    if a in _x87: return _x87[a]
    if a in _xbusy: return (0, 0)
    _xbusy.add(a)
    blocks = get(a)['blocks']
    need, net = 0, None
    depth_in = {a: 0}; work = [a]
    while work:
        ba = work.pop()
        if ba not in blocks: continue
        d = depth_in[ba]
        for i in blocks[ba]:
            m = i.mnemonic
            if m == 'call' and i.operands and i.operands[0].type == X86_OP_IMM and i.operands[0].imm in body_bad:
                cn, cnet = x87_need(i.operands[0].imm)
                need = max(need, cn - d); d += cnet
                continue
            if not m.startswith('f') or m in X_CTRL or m in NOT_X87: continue
            r = -1
            if not (m in X_NOREAD or (m == 'fld' and i.operands and i.operands[0].type == X86_OP_MEM)): r = 0
            for o in i.operands:
                if o.type == X86_OP_REG and o.reg in ST: r = max(r, ST[o.reg])
            if m in X_ST1 or (m in ('fcom', 'fcomp', 'fucom', 'fucomp', 'fxch') and not i.operands): r = max(r, 1)
            need = max(need, r + 1 - d)
            d += 1 if m in X_PUSH or m in X_PUSHAFTER else -1 if m in X_POP1 else -2 if m in X_POP2 else 0
        last = blocks[ba][-1] if blocks[ba] else None
        if last is None: continue
        if last.mnemonic == 'ret': net = d if net is None else max(net, d); continue
        nxt = last.address + last.size; succ = []
        if last.mnemonic == 'jmp':
            if last.operands[0].type == X86_OP_IMM: succ = [last.operands[0].imm]
            elif last.address in get(a)['jtab']: succ = list(get(a)['jtab'][last.address][2])
        elif last.mnemonic.startswith('j') or last.mnemonic.startswith('loop'):
            succ = [last.operands[0].imm, nxt] if last.operands[0].type == X86_OP_IMM else []
        elif last.mnemonic not in ('hlt', 'ud2'): succ = [nxt]
        for sa in succ:
            if sa not in depth_in: depth_in[sa] = d; work.append(sa)
    res = (max(need, 0), net or 0)
    _xbusy.discard(a); _x87[a] = res
    return res

# ---- the flags a flag-returning function defines -------------------------
# A caller may read them (tools/flagabi.py), but an instruction that leaves a
# flag undefined (imul: SF ZF AF PF) leaves it to the implementation: the lift
# keeps the old value (lift.py _DEF_CLAMP), Unicorn computes one (0x56F100:
# cmp, then imul before the ret).  Compared are the flags every path to a ret
# defines: EFLAGS bits, CF 0x1 PF 0x4 AF 0x10 ZF 0x40 SF 0x80 OF 0x800.
FL = ((0x001, X86_EFLAGS_MODIFY_CF | X86_EFLAGS_RESET_CF | X86_EFLAGS_SET_CF, X86_EFLAGS_UNDEFINED_CF),
      (0x004, X86_EFLAGS_MODIFY_PF | X86_EFLAGS_RESET_PF | X86_EFLAGS_SET_PF, X86_EFLAGS_UNDEFINED_PF),
      (0x010, X86_EFLAGS_MODIFY_AF | X86_EFLAGS_RESET_AF | X86_EFLAGS_SET_AF, X86_EFLAGS_UNDEFINED_AF),
      (0x040, X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_RESET_ZF | X86_EFLAGS_SET_ZF, X86_EFLAGS_UNDEFINED_ZF),
      (0x080, X86_EFLAGS_MODIFY_SF | X86_EFLAGS_RESET_SF | X86_EFLAGS_SET_SF, X86_EFLAGS_UNDEFINED_SF),
      (0x800, X86_EFLAGS_MODIFY_OF | X86_EFLAGS_RESET_OF | X86_EFLAGS_SET_OF, X86_EFLAGS_UNDEFINED_OF))
_fdef, _fbusy = {}, set()
def flags_defined(a):
    """EFLAGS mask of the arithmetic flags every path of a to a ret defines"""
    if a in _fdef: return _fdef[a]
    if a in _fbusy: return 0
    _fbusy.add(a)
    blocks = get(a)['blocks']
    st_in = {a: 0x8D5}; work = [a]; at_ret = 0x8D5
    while work:
        ba = work.pop()
        if ba not in blocks: continue
        d = st_in[ba]
        for i in blocks[ba]:
            if i.mnemonic == 'call':
                o = i.operands[0]
                d = flags_defined(o.imm) if o.type == X86_OP_IMM and o.imm in flags_out else 0
                continue
            if i.mnemonic in ('mul', 'imul'):  # SDM: CF OF; SF ZF AF PF undefined (capstone marks SF written)
                d = (d & ~0x0D4) | 0x801
                continue
            ef = i.eflags
            for bit, mod, und in FL:
                if ef & und: d &= ~bit
                elif ef & mod: d |= bit
        last = blocks[ba][-1] if blocks[ba] else None
        if last is None: continue
        if last.mnemonic == 'ret': at_ret &= d; continue
        nxt = last.address + last.size; succ = []
        if last.mnemonic == 'jmp':
            if last.operands[0].type == X86_OP_IMM: succ = [last.operands[0].imm]
            elif last.address in get(a)['jtab']: succ = list(get(a)['jtab'][last.address][2])
        elif last.mnemonic.startswith('j') or last.mnemonic.startswith('loop'):
            succ = [last.operands[0].imm, nxt] if last.operands[0].type == X86_OP_IMM else []
        elif last.mnemonic not in ('hlt', 'ud2'): succ = [nxt]
        for sa in succ:
            if sa not in st_in: st_in[sa] = d; work.append(sa)
            elif st_in[sa] & ~d: st_in[sa] &= d; work.append(sa)
    _fbusy.discard(a); _fdef[a] = at_ret
    return at_ret

# the x87 instructions the lift computes with libm (lift.py): sine, cosine,
# sincos, tangent, 2^x-1, y*log2(x), arctangent, partial remainder; and the constants it loads
# as doubles (x87 has 64-bit significands for them)
XSITE = {'fsin': 's', 'fcos': 'c', 'fsincos': 'b', 'fptan': 't', 'f2xm1': 'e', 'fyl2x': 'l', 'fpatan': 'a',
         'fprem': 'm', 'fprem1': 'M', 'fldpi': 'p', 'fldl2e': 'q', 'fldl2t': 'r', 'fldlg2': 'g', 'fldln2': 'n'}
def x87_sites(cl):
    """the closure's XSITE instructions: '<hex>:<tag>' each"""
    return sorted({'%x:%s' % (i.address, XSITE[i.mnemonic]) for x in cl for ins in get(x)["blocks"].values()
                   for i in ins if i.mnemonic in XSITE})

rng = random.Random(SEED)
pick = list(qual)
rng.shuffle(pick)
# and every qualifying function that returns flags (FLAGABI's list) or has a
# JTSWITCH switch (the lift's jump tables, few among the random ones)
nflag = sum(1 for q in pick[N:] if q[0] in flags_out)
njt = sum(1 for q in pick[N:] if q[0] in has_jt and q[0] not in flags_out)
pick = sorted(pick[:N] + [q for q in pick[N:] if q[0] in flags_out or q[0] in has_jt])
tc = collections.Counter()
with open(OUT, 'w') as f:
    f.write(f"# tests/mworacle_pick.py: {len(pick)} of {len(qual)} qualifying functions (of {len(fns)}), seed {SEED}, "
            f"closure <= {CLOSURE_MAX}; {nflag} of them added as functions that return flags, {njt} as functions "
            f"with a jump table\n")
    for a, n, t, cl in pick:
        args, regs = spec(a)
        tr = ''.join(ch for ch, k in (('x', 'x87'), ('w', 'fsw'), ('m', 'mmx'), ('s', 'sse'), ('F', 'fs')) if k in t)
        if 'x87' in t and x87_need(a)[0]: tr = tr.replace('x', 'x%d' % min(x87_need(a)[0], 7)); tc['xin'] += 1
        if a in flags_out: tr += 'R%x' % flags_defined(a); tc['R'] += 1
        tc['jt'] += a in has_jt
        for k in t: tc[k] += 1
        tc['args'] += len(args); tc['p'] += args.count('p') + regs.count('p'); tc['i'] += args.count('i') + regs.count('i')
        ts = x87_sites(cl)
        tc['trig'] += bool(ts)
        f.write(f"0x{a:08x} {n} {args or '-'} {regs} {tr or '-'} {','.join(ts) or '-'}\n")
print(f"sample: {len(pick)} (seed {SEED}): x87 {tc['x87']} ({tc['xin']} with x87 stack arguments, {tc['trig']} with libm-lifted x87 instructions or constants), MMX {tc['mmx']}, "
      f"SSE {tc['sse']}, fs {tc['fs']}, returning flags {tc['R']}, with a jump table {tc['jt']}, "
      f"{tc['args']} stack arguments, {tc['p']} pointer and {tc['i']} index inputs", file=sys.stderr)
