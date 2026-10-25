#!/usr/bin/env python3
"""DIFFERENTIAL test for x86 semantics: real x86 (Rosetta) vs the lifter.

The same instruction bytes run in two places:
  1) oracle : compiled as x86-64, runs under Rosetta    -> the real CPU
  2) guest  : translated to C by RawLifter, runs on ARM64 -> our output
The outputs (6 GPRs + flags + 128 bytes of memory) are compared.

Encodings that mean the same in 64-bit mode are used:
  - eax..esi only (no REX, no r8+)
  - memory is always N(%rdi) / [edi+N]
  - edi and esp are NOT USED as destinations
"""
import os, sys, random, struct, subprocess, hashlib
sys.path.insert(0,'tools')
from lift import RawLifter, Unsupported

SCRATCH = 0x00E00000          # address of the memory buffer in the guest: test memory, outside both games' images (0x401000-0xA79000)
MEMSZ   = 128
BASE    = 0x00400000

R32 = ['%eax','%ecx','%edx','%ebx','%ebp','%esi']
R16 = ['%ax','%cx','%dx','%bx','%bp','%si']
R8  = ['%al','%cl','%dl','%bl','%ah','%ch']      # ah/ch: the high-byte forms
XMM = ['%xmm0','%xmm1','%xmm2','%xmm3']

# flag mask: CF PF AF ZF SF OF  (+DF is kept separately)
F_CF,F_PF,F_AF,F_ZF,F_SF,F_OF = 1,4,0x10,0x40,0x80,0x800
F_ALL = F_CF|F_PF|F_AF|F_ZF|F_SF|F_OF

def M(rng, lo=0, hi=96, align=4):
    off = rng.randrange(lo, hi, align)
    return f"{off}(%rdi)"

class Tmpl:
    def __init__(self, name, gen, undef=0, skipmem=False, ptr=False):
        self.name, self.gen, self.undef = name, gen, undef
        self.skipmem, self.ptr = skipmem, ptr

T = []
def tmpl(name, undef=0, skipmem=False, ptr=False):
    """skipmem: the result depends on the precision difference between 80-bit
    x87 and double (fprem, fsin...) -- the memory result is not compared."""
    def d(f): T.append(Tmpl(name, f, undef, skipmem, ptr)); return f
    return d

# ---------------- integer core ----------------
ARITH = ['add','sub','adc','sbb','and','or','xor','cmp']
for op in ARITH:
    @tmpl(f"{op} r32,r32")
    def _(rng, op=op): return f"{op}l {rng.choice(R32)}, {rng.choice(R32)}"
    @tmpl(f"{op} m32,r32")
    def _(rng, op=op): return f"{op}l {M(rng)}, {rng.choice(R32)}"
    @tmpl(f"{op} r32,m32")
    def _(rng, op=op): return f"{op}l {rng.choice(R32)}, {M(rng)}"
    @tmpl(f"{op} imm32,r32")
    def _(rng, op=op): return f"{op}l ${rng.getrandbits(32)}, {rng.choice(R32)}"
    @tmpl(f"{op} imm8,r32")
    def _(rng, op=op): return f"{op}l ${rng.randrange(-128,128)}, {rng.choice(R32)}"
    @tmpl(f"{op} r16,r16")
    def _(rng, op=op): return f"{op}w {rng.choice(R16)}, {rng.choice(R16)}"
    @tmpl(f"{op} r8,r8")
    def _(rng, op=op): return f"{op}b {rng.choice(R8)}, {rng.choice(R8)}"
    @tmpl(f"{op} m8,r8")
    def _(rng, op=op): return f"{op}b {M(rng,0,96,1)}, {rng.choice(R8)}"

@tmpl("test r32,r32")
def _(rng): return f"testl {rng.choice(R32)}, {rng.choice(R32)}"
@tmpl("test imm32,r32")
def _(rng): return f"testl ${rng.getrandbits(32)}, {rng.choice(R32)}"
@tmpl("inc/dec r32")
def _(rng): return f"{rng.choice(['incl','decl'])} {rng.choice(R32)}"
@tmpl("inc/dec m32")
def _(rng): return f"{rng.choice(['incl','decl'])} {M(rng)}"
@tmpl("neg/not r32")
def _(rng): return f"{rng.choice(['negl','notl'])} {rng.choice(R32)}"
@tmpl("neg/not r8")
def _(rng): return f"{rng.choice(['negb','notb'])} {rng.choice(R8)}"

# ---------------- shift / rotate ----------------
for op in ['shl','shr','sar','rol','ror','rcl','rcr']:
    @tmpl(f"{op} imm,r32", undef=F_OF|F_AF)
    def _(rng, op=op): return f"{op}l ${rng.randrange(0,33)}, {rng.choice(R32)}"
    @tmpl(f"{op} cl,r32", undef=F_OF|F_AF)
    def _(rng, op=op): return f"{op}l %cl, {rng.choice(R32)}"
    @tmpl(f"{op} imm,r16", undef=F_OF|F_AF)
    def _(rng, op=op): return f"{op}w ${rng.randrange(0,17)}, {rng.choice(R16)}"
    @tmpl(f"{op} imm,r8", undef=F_OF|F_AF)
    def _(rng, op=op): return f"{op}b ${rng.randrange(0,9)}, {rng.choice(R8)}"
@tmpl("shld/shrd", undef=F_OF|F_AF)
def _(rng):
    op = rng.choice(['shldl','shrdl'])
    return f"{op} ${rng.randrange(0,32)}, {rng.choice(R32)}, {rng.choice(R32)}"

# ---------------- multiply / divide ----------------
@tmpl("imul r32,r32", undef=F_SF|F_ZF|F_AF|F_PF)
def _(rng): return f"imull {rng.choice(R32)}, {rng.choice(R32)}"
@tmpl("imul imm,r32,r32", undef=F_SF|F_ZF|F_AF|F_PF)
def _(rng): return f"imull ${rng.randrange(-2000,2000)}, {rng.choice(R32)}, {rng.choice(R32)}"
@tmpl("imul m32,r32", undef=F_SF|F_ZF|F_AF|F_PF)
def _(rng): return f"imull {M(rng)}, {rng.choice(R32)}"
@tmpl("mul/imul edx:eax", undef=F_ALL)
def _(rng):
    op = rng.choice(['mull','imull'])
    return f"{op} {rng.choice(['%ecx','%ebx','%esi'])}"
@tmpl("cdq/cwde/cbw")
def _(rng): return rng.choice(['cltd','cwtl','cbtw'])

# ---------------- move / extend ----------------
@tmpl("movzx/movsx 8->32")
def _(rng):
    op = rng.choice(['movzbl','movsbl'])
    return f"{op} {M(rng,0,96,1)}, {rng.choice(R32)}"
@tmpl("movzx/movsx 16->32")
def _(rng):
    op = rng.choice(['movzwl','movswl'])
    return f"{op} {M(rng,0,96,2)}, {rng.choice(R32)}"
@tmpl("xchg r32,r32")
def _(rng):
    a,b = rng.sample(R32,2); return f"xchgl {a}, {b}"
@tmpl("bswap r32")
def _(rng): return f"bswapl {rng.choice(R32)}"
@tmpl("lea")
def _(rng):
    # %rdi is NOT USED as a base: in the oracle it holds a 64-bit host pointer,
    # in the guest a 32-bit guest address -- the results legitimately differ.
    b = rng.choice(['%rax','%rcx','%rdx','%rbx'])
    i = rng.choice(['%rax','%rcx','%rdx','%rbx'])
    sc = rng.choice([1,2,4,8]); d = rng.randrange(-64,64)
    return f"leal {d}({b},{i},{sc}), {rng.choice(R32)}"

# ---------------- conditions ----------------
CC = ['o','no','b','ae','e','ne','be','a','s','ns','p','np','l','ge','le','g']
@tmpl("setcc")
def _(rng): return f"set{rng.choice(CC)} {rng.choice(R8[:4])}"
@tmpl("cmovcc")
def _(rng): return f"cmov{rng.choice(CC)}l {rng.choice(R32)}, {rng.choice(R32)}"

# ---------------- bit ----------------
@tmpl("bt/bts/btr/btc r32", undef=F_OF|F_SF|F_ZF|F_AF|F_PF)
def _(rng):
    op = rng.choice(['btl','btsl','btrl','btcl'])
    return f"{op} ${rng.randrange(0,32)}, {rng.choice(R32)}"
@tmpl("bsf/bsr", undef=F_OF|F_SF|F_AF|F_PF|F_CF)
def _(rng):
    op = rng.choice(['bsfl','bsrl'])
    return f"{op} {rng.choice(R32)}, {rng.choice(R32)}"

# ---------------- x87 ----------------
@tmpl("x87 arithmetic -> memory")
def _(rng):
    op = rng.choice(['fadds','fsubs','fmuls','fdivs','fsubrs','fdivrs'])
    a,b,d = 0, 8, 64
    return f"flds {a}(%rdi)\n\t{op} {b}(%rdi)\n\tfstps {d}(%rdi)"
@tmpl("x87 excess precision (80-bit difference)", skipmem=True)
def _(rng):
    op = rng.choice(['fsin','fcos','f2xm1','fptan'])
    return f"flds 0(%rdi)\n\t{op}\n\tfstps 64(%rdi)"
@tmpl("x87 unary -> memory")
def _(rng):
    # f2xm1 is only defined for |x|<=1; it cannot be compared on random input
    op = rng.choice(['fabs','fchs','fsqrt','frndint'])
    return f"flds 0(%rdi)\n\t{op}\n\tfstps 64(%rdi)"
@tmpl("x87 fistp RC")
def _(rng):
    cw = rng.choice([0x037f,0x077f,0x0b7f,0x0f7f])
    return (f"movw ${cw}, 100(%rdi)\n\tfldcw 100(%rdi)\n\t"
            f"flds 0(%rdi)\n\tfistpl 64(%rdi)\n\tmovw $0x037f, 100(%rdi)\n\tfldcw 100(%rdi)")
@tmpl("x87 fild -> fstp")
def _(rng):
    return "fildl 0(%rdi)\n\tfstps 64(%rdi)"
@tmpl("x87 fxch/fcom")
def _(rng):
    return ("flds 0(%rdi)\n\tflds 8(%rdi)\n\tfxch %st(1)\n\t"
            "fcomps 16(%rdi)\n\tfnstsw %ax\n\tandl $0x7f00, %eax\n\tfstps 64(%rdi)")
@tmpl("x87 fprem/fscale", skipmem=True)
def _(rng):
    op = rng.choice(['fprem','fprem1','fscale'])
    return f"flds 8(%rdi)\n\tflds 0(%rdi)\n\t{op}\n\tfstps 64(%rdi)\n\tfstp %st(0)"
@tmpl("x87 fld st(i) - stack copy")
def _(rng):
    # "fld %st(i)" must read the source BEFORE top CHANGES.  Without this
    # template the FPUSH evaluation-order bug had gone unnoticed.
    i = rng.randrange(0, 3)
    return ("flds 0(%rdi)\n\tflds 8(%rdi)\n\tflds 16(%rdi)\n\t"
            f"fld %st({i})\n\t"
            "fstps 64(%rdi)\n\tfstps 68(%rdi)\n\t"
            "fstps 72(%rdi)\n\tfstps 76(%rdi)")
@tmpl("x87 fst/fxch st(i) mixed")
def _(rng):
    i = rng.randrange(1, 3)
    op = rng.choice([f"fst %st({i})", f"fxch %st({i})", f"fld %st({i})"])
    return ("flds 0(%rdi)\n\tflds 8(%rdi)\n\tflds 16(%rdi)\n\t"
            f"{op}\n\t"
            "fstps 64(%rdi)\n\tfstps 68(%rdi)\n\tfstps 72(%rdi)")
@tmpl("x87 __ftol-like (fld st(0) + fistp qword)")
def _(rng):
    return ("flds 0(%rdi)\n\tfld %st(0)\n\t"
            "fsts 64(%rdi)\n\tfistpq 72(%rdi)\n\t"
            "fildq 72(%rdi)\n\tfsubp %st(1)\n\tfstps 68(%rdi)")
@tmpl("x87 double")
def _(rng):
    op = rng.choice(['faddl','fsubl','fmull','fdivl'])
    return f"fldl 0(%rdi)\n\t{op} 8(%rdi)\n\tfstpl 64(%rdi)"

# ---------------- SSE / SSE2 ----------------
@tmpl("sse packed float")
def _(rng):
    op = rng.choice(['addps','subps','mulps','divps','minps','maxps','andps','orps','xorps'])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse scalar float")
def _(rng):
    op = rng.choice(['addss','subss','mulss','divss','minss','maxss','sqrtss'])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse2 packed double")
def _(rng):
    op = rng.choice(['addpd','subpd','mulpd','divpd','minpd','maxpd'])
    return f"movupd 0(%rdi), %xmm0\n\tmovupd 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovupd %xmm0, 64(%rdi)"
@tmpl("sse shuffle/unpack")
def _(rng):
    op = rng.choice([f"shufps ${rng.getrandbits(8)}, %xmm1, %xmm0",
                     "unpcklps %xmm1, %xmm0", "unpckhps %xmm1, %xmm0",
                     "movlhps %xmm1, %xmm0", "movhlps %xmm1, %xmm0"])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op}\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse2 packed int")
def _(rng):
    op = rng.choice(['paddd','psubd','paddw','psubw','paddb','psubb','pand','por','pxor',
                     'pmullw','pminsw','pmaxsw','pavgb','pavgw','psadbw','pcmpeqd','pcmpgtd'])
    return f"movdqu 0(%rdi), %xmm0\n\tmovdqu 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovdqu %xmm0, 64(%rdi)"
@tmpl("sse cvt")
def _(rng):
    op = rng.choice(['cvtps2dq','cvttps2dq','cvtdq2ps','cvtpd2ps','cvtps2pd'])
    return f"movups 16(%rdi), %xmm1\n\tmovups 0(%rdi), %xmm0\n\t{op} %xmm0, %xmm1\n\tmovups %xmm1, 64(%rdi)"
@tmpl("sse cvt scalar")
def _(rng):
    op = rng.choice(['cvtsi2ssl','cvtsi2sdl'])
    return f"movups 16(%rdi), %xmm0\n\t{op} 0(%rdi), %xmm0\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse cvt -> int")
def _(rng):
    op = rng.choice(['cvttss2si','cvtss2si','cvttsd2si','cvtsd2si'])
    return f"movups 0(%rdi), %xmm0\n\t{op} %xmm0, %eax\n\tmovl %eax, 64(%rdi)"
@tmpl("sse cmp/compare")
def _(rng):
    op = rng.choice([f"cmpps ${rng.randrange(0,8)}, %xmm1, %xmm0",
                     f"cmpss ${rng.randrange(0,8)}, %xmm1, %xmm0"])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op}\n\tmovups %xmm0, 64(%rdi)"
@tmpl("ucomiss/comisd")
def _(rng):
    op = rng.choice(['ucomiss','comiss','ucomisd','comisd'])
    return (f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0")


# ---------------- string instructions (esi = source pointer) ----------------
@tmpl("movs/stos/lods", ptr=True)
def _(rng):
    op = rng.choice(['movsb','movsw','movsl','stosb','stosw','stosl','lodsb','lodsw','lodsl'])
    n  = rng.randrange(1, 5)
    d  = rng.choice(['cld','std'])
    # with std the pointers move backwards; start from the middle of the buffer
    pre = f"{d}\n\tmovl ${n}, %ecx\n\t"
    rep = 'rep ' if op.startswith(('movs','stos')) and rng.random() < 0.7 else ''
    return pre + rep + op + "\n\tcld"
@tmpl("scas/cmps", ptr=True, undef=0)
def _(rng):
    op = rng.choice(['scasb','scasl','cmpsb','cmpsl'])
    n  = rng.randrange(1, 5)
    rep = rng.choice(['', 'repe ', 'repne '])
    return f"cld\n\tmovl ${n}, %ecx\n\t" + rep + op

# ---------------- divide ----------------
@tmpl("div/idiv 32", undef=F_ALL)
def _(rng):
    op = rng.choice(['divl','idivl'])
    # the divisor must not be zero and the quotient must not overflow: small dividend, large divisor
    d = rng.randrange(1, 1000)
    return ("movl $0, %edx" + chr(10) + chr(9) + "andl $0xffff, %eax" + chr(10) + chr(9)
            + "movl $" + str(d) + ", %ecx" + chr(10) + chr(9) + op + " %ecx")
@tmpl("mul/imul 8-16", undef=F_ALL)
def _(rng):
    op = rng.choice(['mulb','imulb','mulw','imulw'])
    return f"{op} {rng.choice(['%cl','%bl']) if op.endswith('b') else rng.choice(['%cx','%bx'])}"

# ---------------- flag transfer ----------------
@tmpl("pushf/popf/sahf/lahf")
def _(rng):
    return rng.choice(["lahf", "sahf", "pushfq\n\tpopq %rax\n\tandl $0x8d5, %eax",
                       "clc", "stc", "cmc"])
@tmpl("xadd/cmpxchg")
def _(rng):
    if rng.random() < 0.5:
        return f"xaddl {rng.choice(R32[1:])}, {rng.choice(R32[1:])}"
    return f"cmpxchgl {rng.choice(['%ecx','%edx','%ebx'])}, {rng.choice(['%ecx','%edx','%ebx'])}"
@tmpl("movzx/movsx register")
def _(rng):
    op = rng.choice(['movzbl','movsbl','movzwl','movswl'])
    src = rng.choice(R8) if op.endswith('bl') else rng.choice(R16)
    return f"{op} {src}, {rng.choice(R32)}"
@tmpl("bit m32", undef=F_OF|F_SF|F_ZF|F_AF|F_PF)
def _(rng):
    op = rng.choice(['btl','btsl','btrl','btcl'])
    return f"{op} ${rng.randrange(0,32)}, {M(rng)}"
@tmpl("xchg m32")
def _(rng): return f"xchgl {rng.choice(R32)}, {M(rng)}"
@tmpl("mov variants")
def _(rng):
    return rng.choice([f"movl {M(rng)}, {rng.choice(R32)}",
                       f"movl {rng.choice(R32)}, {M(rng)}",
                       f"movw {M(rng,0,96,2)}, {rng.choice(R16)}",
                       f"movb {M(rng,0,96,1)}, {rng.choice(R8)}",
                       f"movl ${rng.getrandbits(32)}, {M(rng)}"])

def assemble(asm_list, workdir):
    """Assemble all test instructions in one file, return their byte strings.

    NO separator byte is used: an instruction's own bytes can merge with the
    separator (example: `andb %cl,%ah` -> 20 CC).  Instead every test gets a
    global symbol and the boundaries are read from the symbol table."""
    src = ['.text']
    for i, a in enumerate(asm_list):
        src.append('.globl _c%d' % i)
        src.append('_c%d:' % i)
        src.append('\t' + a)
    src.append('.globl _cend')
    src.append('_cend:')
    src.append('\tnop')
    sfile = os.path.join(workdir, 'fuzz.s')
    ofile = os.path.join(workdir, 'fuzz.o')
    open(sfile, 'w').write('\n'.join(src) + '\n')
    subprocess.run(['clang','-arch','x86_64','-c','-o',ofile,sfile], check=True)

    nm = subprocess.run(['nm','-n',ofile], capture_output=True, text=True, check=True).stdout
    addr = {}
    for line in nm.splitlines():
        f = line.split()
        if len(f) >= 3 and f[2].startswith('_c'):
            try: addr[f[2]] = int(f[0], 16)
            except ValueError: pass

    raw = subprocess.run(['otool','-t',ofile], capture_output=True, text=True, check=True).stdout
    byts = bytearray()
    for line in raw.splitlines():
        parts = line.split()
        if len(parts) < 2: continue
        if not all(len(x) == 2 for x in parts[1:]): continue
        try: int(parts[0], 16)
        except ValueError: continue
        for x in parts[1:]: byts.append(int(x, 16))

    out = []
    for i in range(len(asm_list)):
        a0 = addr.get('_c%d' % i)
        a1 = addr.get('_c%d' % (i+1), addr.get('_cend'))
        if a0 is None or a1 is None or a1 <= a0:
            raise SystemExit('assemble: no boundary found for %d' % i)
        out.append(bytes(byts[a0:a1]))
    return out

ORACLE_C = """
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>
static sigjmp_buf g_jb;
static volatile int g_infault;
static void onfault(int sig){ (void)sig; if (g_infault) siglongjmp(g_jb, 1); _exit(9); }
typedef struct { uint32_t r[6]; uint32_t fl; uint32_t pad; uint8_t mem[@MEMSZ@]; } St;
extern const unsigned char CODE[];
extern const unsigned CODE_OFF[];
extern const unsigned CODE_LEN[];
extern const unsigned NCASE;
extern const unsigned char INIT[];
static void *page;
static void run_one(St *s, unsigned long long rsiv){
  __asm__ volatile(
    "pushq %%rbx\\n\\tpushq %%rbp\\n\\tpushq %%r12\\n\\tpushq %%r13\\n\\t"
    "movq %0, %%r12\\n\\tmovq %1, %%r13\\n\\t"
    "leaq 32(%%r12), %%rdi\\n\\t"
    "movl 0(%%r12), %%eax\\n\\tmovl 4(%%r12), %%ecx\\n\\t"
    "movl 8(%%r12), %%edx\\n\\tmovl 12(%%r12), %%ebx\\n\\t"
    "movl 16(%%r12), %%ebp\\n\\tmovl 20(%%r12), %%esi\\n\\t"
    "fninit\\n\\t"                    /* every case starts with a clean FPU */
    "xorps %%xmm0, %%xmm0\\n\\txorps %%xmm1, %%xmm1\\n\\t"
    "xorps %%xmm2, %%xmm2\\n\\txorps %%xmm3, %%xmm3\\n\\t"
    "movl 24(%%r12), %%r11d\\n\\tpushq %%r11\\n\\tpopfq\\n\\t"
    "callq *%%r13\\n\\t"
    "pushfq\\n\\tpopq %%r11\\n\\t"
    "movl %%eax, 0(%%r12)\\n\\tmovl %%ecx, 4(%%r12)\\n\\t"
    "movl %%edx, 8(%%r12)\\n\\tmovl %%ebx, 12(%%r12)\\n\\t"
    "movl %%ebp, 16(%%r12)\\n\\tmovl %%esi, 20(%%r12)\\n\\t"
    "movl %%r11d, 24(%%r12)\\n\\t"
    "popq %%r13\\n\\tpopq %%r12\\n\\tpopq %%rbp\\n\\tpopq %%rbx\\n\\t"
    :: "r"(s), "r"(page), "r"(rsiv) : "rax","rcx","rdx","rsi","rdi","r11","memory","cc",
       "xmm0","xmm1","xmm2","xmm3","st","st(1)","st(2)","st(3)");
}
int main(void){
  setvbuf(stdout, 0, _IOLBF, 0);
  /* Some random cases can fault on the real CPU (divide, access);
   * skip that case and go on, do not lose the whole run. */
  signal(SIGSEGV, onfault); signal(SIGBUS, onfault);
  signal(SIGFPE, onfault);  signal(SIGILL, onfault); signal(SIGTRAP, onfault);
  page = mmap(0, 1<<20, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
  for (unsigned i = 0; i < NCASE; i++){
    unsigned char *p = (unsigned char*)page;
    memcpy(p, CODE + CODE_OFF[i], CODE_LEN[i]);
    p[CODE_LEN[i]] = 0xC3;
    St s; memcpy(&s, INIT + i*sizeof(St), sizeof(St));
    /* if pad != 0, esi is used as a POINTER (string instructions) */
    unsigned long long rsiv = s.pad ? (unsigned long long)(unsigned long)(s.mem + 64)
                                    : (unsigned long long)s.r[5];
    g_infault = 1;
    if (sigsetjmp(g_jb, 1) != 0){ g_infault = 0; continue; }   /* skip the case */
    run_one(&s, rsiv);
    g_infault = 0;
    printf("%u", i);
    for (int k = 0; k < 6; k++) printf(" %08x", s.r[k]);
    printf(" %08x ", s.fl & 0x8D5u);
    for (int k = 0; k < @MEMSZ@; k++) printf("%02x", s.mem[k]);
    printf("\\n");
  }
  return 0;
}
"""

GUEST_HDR = """
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
void mem_commit(uint32_t,uint32_t);
guest_fn_t g_dtab[DISPATCH_TEXT_SIZE];
void dispatch_indirect(CPU*c,uint32_t a){(void)c;(void)a;}
void unsupported_insn(CPU*c,uint32_t a,const char*m){(void)c;(void)a;(void)m;}
void guest_hook(CPU*c,uint32_t a){(void)c;(void)a;}
void cpuid_shim(CPU*c){(void)c;} uint64_t rdtsc_shim(void){return 0;}
"""

def gen_oracle(cases, chunks, workdir):
    blob = bytearray(); offs = []; lens = []
    for c in chunks:
        offs.append(len(blob)); lens.append(len(c)); blob += c
    init = bytearray()
    for cs in cases:
        init += struct.pack('<6III', *cs['regs'], cs['flags'],
                            1 if cs['ptr'] else 0) + cs['mem']
    o = [ORACLE_C.replace('@MEMSZ@', str(MEMSZ))]
    o.append('const unsigned NCASE = %d;' % len(cases))
    o.append('const unsigned char CODE[] = {' + ','.join(str(b) for b in blob) + '};')
    o.append('const unsigned CODE_OFF[] = {' + ','.join(map(str, offs)) + '};')
    o.append('const unsigned CODE_LEN[] = {' + ','.join(map(str, lens)) + '};')
    o.append('const unsigned char INIT[] = {' + ','.join(str(b) for b in init) + '};')
    src = os.path.join(workdir, 'oracle.c')
    open(src, 'w').write('\n'.join(o))
    exe = os.path.join(workdir, 'oracle')
    subprocess.run(['clang','-arch','x86_64','-O0','-o',exe,src], check=True)
    return exe

def gen_guest(cases, chunks, workdir):
    bodies = []; names = []; skipped = []
    for i, c in enumerate(chunks):
        L = RawLifter(bytes(c) + b'\xc3', base=BASE, comments=False)
        try:
            code, _ = L.lift(BASE)
        except Unsupported as e:
            names.append(None); skipped.append((i, str(e))); continue
        except Exception as e:
            names.append(None); skipped.append((i, 'ERROR: ' + str(e))); continue
        bodies.append(code.replace('sub_%08x' % BASE, 't%d' % i))
        names.append('t%d' % i)
    o = [GUEST_HDR, '\n'.join(bodies), 'static CPU cpu; static CPU *const c = &cpu;']
    o.append('int main(void){ mem_init(); mem_commit(0x0u,0x01000000u); mem_fault_handler_install();')
    o.append('  unsigned char m[%d];' % MEMSZ)
    for i, cs in enumerate(cases):
        if i >= len(names) or names[i] is None: continue
        regs = cs['regs']; f = cs['flags']
        o.append('  {')
        o.append('   memset(&cpu,0,sizeof cpu); cpu.esp=0x00B00000u; cpu.top=0; cpu.fcw=0x037f;')
        for k, rn in enumerate(['eax','ecx','edx','ebx','ebp','esi']):
            o.append('   cpu.%s=0x%08xu;' % (rn, regs[k]))
        if cs['ptr']:
            o.append('   cpu.esi=0x%08xu;' % (SCRATCH + 64))
        o.append('   cpu.edi=0x%08xu;' % SCRATCH)
        o.append('   cpu.cf=%d; cpu.pf=%d; cpu.af=%d;' % ((f>>0)&1, (f>>2)&1, (f>>4)&1))
        o.append('   cpu.zf=%d; cpu.sf=%d; cpu.of=%d; cpu.df=%d;' % ((f>>6)&1,(f>>7)&1,(f>>11)&1,(f>>10)&1))
        o.append('   static const unsigned char mi[%d]={%s};' % (MEMSZ, ','.join(str(b) for b in cs['mem'])))
        o.append('   memcpy(g_mem+0x%08xu, mi, %d);' % (SCRATCH, MEMSZ))
        o.append('   PUSH32(0xDEADC0DEu);')
        o.append('   %s(&cpu);' % names[i])
        o.append('   printf("%d");' % i)
        o.append('   printf(" %08x %08x %08x %08x %08x %08x",'
                 ' cpu.eax,cpu.ecx,cpu.edx,cpu.ebx,cpu.ebp,cpu.esi);')
        o.append('   { uint32_t fl = (cpu.cf?1u:0)|(cpu.pf?4u:0)|(cpu.af?0x10u:0)'
                 '|(cpu.zf?0x40u:0)|(cpu.sf?0x80u:0)|(cpu.of?0x800u:0);')
        o.append('     printf(" %08x ", fl); }')
        o.append('   memcpy(m, g_mem+0x%08xu, %d);' % (SCRATCH, MEMSZ))
        o.append('   for(int k=0;k<%d;k++) printf("%%02x", m[k]);' % MEMSZ)
        o.append('   printf("\\n");')
        o.append('  }')
    o.append('  return 0; }')
    src = os.path.join(workdir, 'guest.c')
    open(src, 'w').write('\n'.join(o))
    exe = os.path.join(workdir, 'guest')
    subprocess.run(['clang','-O1','-Wno-unused-label','-Iruntime','-o',exe,src,
                    'runtime/mem.c','-lm'], check=True)
    return exe, skipped

def main():
    n    = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 12345
    rng = random.Random(seed)
    workdir = os.environ.get('FUZZDIR', '/tmp/fuzzisa')
    os.makedirs(workdir, exist_ok=True)

    cases = []; asm = []
    for i in range(n):
        t = T[i % len(T)] if i < len(T) else rng.choice(T)
        a = t.gen(rng)
        asm.append(a)
        regs = [rng.getrandbits(32) for _ in range(6)]
        for k in range(6):
            if rng.random() < 0.35:
                regs[k] = rng.choice([0,1,2,0xFFFFFFFF,0x80000000,0x7FFFFFFF,
                                      rng.randrange(0,64), rng.randrange(0,4)])
        mem = bytearray(rng.getrandbits(8) for _ in range(MEMSZ))
        for off in range(0, 64, 4):
            if rng.random() < 0.7:
                v = rng.choice([0.0,1.0,-1.0,0.5,2.0,3.7,-3.7,1e-3,1e6,
                                rng.uniform(-100,100)])
                struct.pack_into('<f', mem, off, v)
        flags = 0x202 | (rng.getrandbits(12) & 0x8D5)
        cases.append(dict(regs=regs, mem=bytes(mem), flags=flags, tmpl=t.name,
                          undef=t.undef, skipmem=t.skipmem, ptr=t.ptr, asm=a))

    chunks = assemble(asm, workdir)
    with open(os.path.join(workdir,'cases.txt'),'w') as fh:
        for i,(a,ch) in enumerate(zip(asm,chunks)):
            fh.write('%d\t%s\t%s\n' % (i, ch.hex(), a.replace(chr(10),' ; ').replace(chr(9),'')))
    oexe = gen_oracle(cases, chunks, workdir)
    gexe, skipped = gen_guest(cases, chunks, workdir)

    ro = subprocess.run([oexe], capture_output=True, text=True)
    rg = subprocess.run([gexe], capture_output=True, text=True)
    if ro.returncode: print('  WARNING: oracle exit code %d' % ro.returncode)
    if rg.returncode: print('  WARNING: guest exit code %d  %s' % (rg.returncode, rg.stderr[:200]))
    def parse(lines):
        d = {}
        for l in lines:
            f = l.split()
            if len(f) == 9: d[f[0]] = f[1:]
        return d
    om = parse(ro.stdout.splitlines())
    gm = parse(rg.stdout.splitlines())

    bad = {}; checked = 0
    for i in range(n):
        k = str(i)
        if k not in gm or k not in om: continue
        checked += 1
        o, g = om[k], gm[k]
        undef = cases[i]['undef']
        diffs = []
        for j, nm in enumerate(['eax','ecx','edx','ebx','ebp','esi']):
            if nm == 'esi' and cases[i]['ptr']: continue   # the base address differs
            if o[j] != g[j]: diffs.append('%s: real %s != ours %s' % (nm, o[j], g[j]))
        of_ = int(o[6],16) & ~undef; gf_ = int(g[6],16) & ~undef
        if of_ != gf_: diffs.append('flags: real %03x != ours %03x' % (of_, gf_))
        if o[7] != g[7] and not cases[i]['skipmem']:
            for b in range(MEMSZ):
                a1, b1 = o[7][b*2:b*2+2], g[7][b*2:b*2+2]
                if a1 != b1:
                    diffs.append('mem[%d]: real %s != ours %s' % (b, a1, b1)); break
        if diffs:
            inp = 'input: ' + ' '.join('%s=%08x' % (n, v) for n, v in
                     zip(['eax','ecx','edx','ebx','ebp','esi'], cases[i]['regs'])) \
                  + ' flags=%03x' % (cases[i]['flags'] & 0x8d5)
            bad.setdefault(cases[i]['tmpl'], []).append((i, cases[i]['asm'], [inp] + diffs))

    print('\n  checked: %d/%d   not liftable: %d' % (checked, n, len(skipped)))
    if skipped:
        seen = set()
        for i, e in skipped:
            if e in seen: continue
            seen.add(e); print('    no lift: %-28s example: %r' % (e, cases[i]['asm']))
    # Do NOT SAY "matched" if no case was checked: when the lifter crashed at
    # the start (0/n checked) this line came out green and hid the breakage.
    if not checked:
        print('  \033[31mNO CASE WAS CHECKED -- the result is void\033[0m'); return 1
    if not bad:
        print('  \033[32mALL RESULTS MATCH\033[0m'); return 0
    print('\n  \033[31m%d mismatches, in %d templates\033[0m'
          % (sum(len(v) for v in bad.values()), len(bad)))
    for tn, lst in sorted(bad.items(), key=lambda x: -len(x[1])):
        print('\n  --- %s  (%d cases) ---' % (tn, len(lst)))
        for i, a, d in lst[:2]:
            print('    #%d  %r' % (i, a))
            for x in d[:4]: print('        ' + x)
    return 1

if __name__ == '__main__':
    sys.exit(main())
