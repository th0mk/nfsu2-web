#!/usr/bin/env python3
"""Validation suite for x86 instruction semantics.
   Hand-encoded machine code + the results the Intel spec expects (one case,
   named "(lift: doubles)", pins the lift's x87 model where it differs)."""
import sys, os, subprocess, struct
sys.path.insert(0,'tools')
# lift the tests the way gen.sh lifts the game (x87 precision control on;
# the tests run at CW 0x037F unless they load another word)
os.environ.setdefault('X87PC', '1')
# lock-prefixed read-modify-writes are lifted as MW's lift does them
# (games/mw/game.conf LOCKSTRIP=1); without it they are traps
os.environ.setdefault('LOCKSTRIP', '1')
# the x87 environment and state images are lifted as MW's lift does them
# (games/mw/game.conf X87ENV=1); without it they are traps
os.environ.setdefault('X87ENV', '1')
from lift import RawLifter, Unsupported

TESTS = []
# calls: offsets in code of further functions the test calls (each is lifted as a
# function of its own); retpast: offsets of callees that may return past their
# caller (the lifter's tools/retpast.txt list); env: lifter options set while
# the test is lifted (e.g. RETEIP=0)
def T(name, code, setup=None, mem=None, expect=None, fmem=None, memf=None, fmemf=None, calls=(), retpast=(), env=None):
    TESTS.append(dict(name=name, code=bytes(code), setup=setup or {},
                      mem=mem or {}, expect=expect or {}, fmem=fmem or {},
                      memf=memf or {}, fmemf=fmemf or {}, calls=calls, retpast=retpast, env=env or {}))

def imm32(v): return list(struct.pack('<I', v & 0xffffffff))
MOV_EAX = lambda v: [0xB8]+imm32(v)
MOV_ECX = lambda v: [0xB9]+imm32(v)
MOV_EDX = lambda v: [0xBA]+imm32(v)

# ---------------- shift / rotate ----------------
T("shr eax,1  (eax=3)",      [0xD1,0xE8], {'eax':3},          expect={'eax':1,'cf':1,'zf':0})
T("shr eax,1  (eax=2)",      [0xD1,0xE8], {'eax':2},          expect={'eax':1,'cf':0})
T("shr eax,4  (eax=0xFF)",   [0xC1,0xE8,4], {'eax':0xFF},     expect={'eax':0x0F,'cf':1})
T("shr eax,4  (eax=0xF0)",   [0xC1,0xE8,4], {'eax':0xF0},     expect={'eax':0x0F,'cf':0})
T("shl eax,1  (0x80000000)", [0xD1,0xE0], {'eax':0x80000000}, expect={'eax':0,'cf':1,'zf':1,'of':1})
T("shl eax,1  (0x40000000)", [0xD1,0xE0], {'eax':0x40000000}, expect={'eax':0x80000000,'cf':0,'sf':1,'of':1})
T("shl eax,4  (0x12345678)", [0xC1,0xE0,4], {'eax':0x12345678}, expect={'eax':0x23456780,'cf':1})
T("sar eax,1  (0xFFFFFFFE)", [0xD1,0xF8], {'eax':0xFFFFFFFE}, expect={'eax':0xFFFFFFFF,'cf':0,'sf':1})
T("sar eax,1  (0xFFFFFFFF)", [0xD1,0xF8], {'eax':0xFFFFFFFF}, expect={'eax':0xFFFFFFFF,'cf':1})
T("sar eax,4  (0x80000000)", [0xC1,0xF8,4], {'eax':0x80000000}, expect={'eax':0xF8000000,'cf':0})
T("sar al,1   (al=0x80)",    [0xD0,0xF8], {'eax':0x80},       expect={'eax':0xC0,'cf':0})
T("sar al,1   (al=0x81)",    [0xD0,0xF8], {'eax':0x81},       expect={'eax':0xC0,'cf':1})
T("shr al,4   (al=0xFF)",    [0xC0,0xE8,4], {'eax':0xFF},     expect={'eax':0x0F,'cf':1})
T("rol al,1   (al=0x80)",    [0xD0,0xC0], {'eax':0x80},       expect={'eax':0x01,'cf':1})
T("ror al,1   (al=0x01)",    [0xD0,0xC8], {'eax':0x01},       expect={'eax':0x80,'cf':1})
T("rol eax,4  (0x12345678)", [0xC1,0xC0,4], {'eax':0x12345678}, expect={'eax':0x23456781})
T("ror eax,4  (0x12345678)", [0xC1,0xC8,4], {'eax':0x12345678}, expect={'eax':0x81234567})
T("shrd eax,edx,4",  [0x0F,0xAC,0xD0,4], {'eax':0x12345678,'edx':0xABCDEF01}, expect={'eax':0x11234567})
T("shld eax,edx,4",  [0x0F,0xA4,0xD0,4], {'eax':0x12345678,'edx':0xABCDEF01}, expect={'eax':0x2345678A})

# ---------------- bit tests ----------------
T("bt  eax,5  (bit set)",  [0x0F,0xBA,0xE0,5], {'eax':0x20},   expect={'cf':1,'eax':0x20})
T("bt  eax,4  (bit clear)",[0x0F,0xBA,0xE0,4], {'eax':0x20},   expect={'cf':0})
T("bts eax,3",             [0x0F,0xBA,0xE8,3], {'eax':0},      expect={'eax':8,'cf':0})
T("btr eax,3",             [0x0F,0xBA,0xF0,3], {'eax':0x0F},   expect={'eax':7,'cf':1})
T("btc eax,0",             [0x0F,0xBA,0xF8,0], {'eax':1},      expect={'eax':0,'cf':1})

# ---------------- arithmetic + flags ----------------
T("add eax,1 (0xFFFFFFFF)", [0x05]+imm32(1), {'eax':0xFFFFFFFF}, expect={'eax':0,'cf':1,'zf':1})
T("add eax,1 (0x7FFFFFFF)", [0x05]+imm32(1), {'eax':0x7FFFFFFF}, expect={'eax':0x80000000,'of':1,'sf':1,'cf':0})
T("sub eax,1 (0)",          [0x2D]+imm32(1), {'eax':0},          expect={'eax':0xFFFFFFFF,'cf':1,'sf':1})
T("cmp eax,5 (eax=5)",      [0x3D]+imm32(5), {'eax':5},          expect={'zf':1,'cf':0})
T("cmp eax,5 (eax=3)",      [0x3D]+imm32(5), {'eax':3},          expect={'zf':0,'cf':1,'sf':1})
T("inc eax keeps CF",     [0xF9,0x40],     {'eax':1},          expect={'eax':2,'cf':1})   # stc; inc
T("dec eax (1->0)",         [0x48],          {'eax':1},          expect={'eax':0,'zf':1})
T("neg eax (5)",            [0xF7,0xD8],     {'eax':5},          expect={'eax':0xFFFFFFFB,'cf':1,'sf':1})
T("neg eax (0)",            [0xF7,0xD8],     {'eax':0},          expect={'eax':0,'cf':0,'zf':1})
T("adc eax,1 (CF=1)",       [0xF9,0x15]+imm32(1), {'eax':1},     expect={'eax':3})          # stc; adc
T("sbb eax,1 (CF=1)",       [0xF9,0x1D]+imm32(1), {'eax':5},     expect={'eax':3})          # stc; sbb
T("not eax",                [0xF7,0xD0],     {'eax':0x0F0F0F0F}, expect={'eax':0xF0F0F0F0})

# ---------------- multiply / divide ----------------
T("imul eax,edx",       [0x0F,0xAF,0xC2], {'eax':7,'edx':6},        expect={'eax':42})
T("imul eax,edx (neg)", [0x0F,0xAF,0xC2], {'eax':0xFFFFFFFF,'edx':5}, expect={'eax':0xFFFFFFFB})
T("imul eax,edx,10",    [0x6B,0xC2,10],   {'edx':7},                expect={'eax':70})
T("mul edx (2 x 3)",    [0xF7,0xE2],      {'eax':2,'edx':3},        expect={'eax':6,'edx':0})
T("mul edx (large)",    [0xF7,0xE2],      {'eax':0x10000,'edx':0x10000}, expect={'eax':0,'edx':1})
T("div ecx (100/7)",    [0xBA,0,0,0,0, 0xF7,0xF1], {'eax':100,'ecx':7}, expect={'eax':14,'edx':2})
T("idiv ecx (-100/7)",  [0x99,0xF7,0xF9], {'eax':0xFFFFFF9C,'ecx':7},  expect={'eax':0xFFFFFFF2,'edx':0xFFFFFFFE})
T("cdq (negative)",      [0x99],           {'eax':0xFFFFFFFF},       expect={'edx':0xFFFFFFFF})
T("cdq (positive)",      [0x99],           {'eax':0x7FFFFFFF},       expect={'edx':0})
T("cwde (0xFFFF)",      [0x98],           {'eax':0xFFFF},           expect={'eax':0xFFFFFFFF})

# ---------------- extension / conditional ----------------
T("movzx eax,dl",  [0x0F,0xB6,0xC2], {'edx':0xFF},   expect={'eax':0xFF})
T("movsx eax,dl",  [0x0F,0xBE,0xC2], {'edx':0xFF},   expect={'eax':0xFFFFFFFF})
T("movzx eax,dx",  [0x0F,0xB7,0xC2], {'edx':0xFFFF}, expect={'eax':0xFFFF})
T("movsx eax,dx",  [0x0F,0xBF,0xC2], {'edx':0x8000}, expect={'eax':0xFFFF8000})
T("sete al (equal)",     [0x3D]+imm32(5)+[0x0F,0x94,0xC0], {'eax':5}, expect={'eax':1})
T("setne al (equal)",    [0x3D]+imm32(5)+[0x0F,0x95,0xC0], {'eax':5}, expect={'eax':0})
T("setl al (-1 < 1)",   [0x3D]+imm32(1)+[0x0F,0x9C,0xC0], {'eax':0xFFFFFFFF}, expect={'eax':0xFFFFFF01})
T("setg al (2 > 1)",    [0x3D]+imm32(1)+[0x0F,0x9F,0xC0], {'eax':2}, expect={'eax':1})

# ---------------- stack (bugs found here!) ----------------
T("push [esp+4] pushes the right slot",
  [0x52, 0x51, 0xFF,0x74,0x24,0x04, 0x58, 0x59, 0x5A],  # push edx;push ecx;push [esp+4];pop eax;pop ecx;pop edx
  {'edx':0xAAAA1111,'ecx':0xBBBB2222}, expect={'eax':0xAAAA1111,'ecx':0xBBBB2222,'edx':0xAAAA1111})
T("push esp pushes the original esp",
  [0x54, 0x58],                                 # push esp; pop eax
  {}, expect={})   # once esp is back, eax = the old esp; checked separately below
T("pop order",
  [0x52, 0x51, 0x58, 0x5A],                     # push edx; push ecx; pop eax; pop edx
  {'edx':0x11112222,'ecx':0x33334444}, expect={'eax':0x33334444,'edx':0x11112222})
T("pushfd/popfd keep the flags",
  [0xF9, 0x9C, 0xFC, 0x9D],                     # stc; pushfd; cld; popfd
  {}, expect={'cf':1})
T("sahf",   [0xB8,0x00,0xFF,0x00,0x00, 0x9E],   # mov eax,0xFF00 ; sahf
  {}, expect={'cf':1,'zf':1,'sf':1,'pf':1})
T("lahf",   [0xF9, 0x9F], {'eax':0}, expect={'eax':0x0300})   # stc; lahf -> ah = 0x03 (CF|bit1)
T("xchg eax,edx", [0x92], {'eax':1,'edx':2}, expect={'eax':2,'edx':1})

# ---------------- string instructions ----------------
T("rep stosd", [0xFC, 0xF3,0xAB],                 # cld; rep stosd
  {'eax':0xDEADBEEF,'ecx':4,'edi':0x00C00000},
  expect={'ecx':0,'edi':0x00C00010},
  fmem={0x00C00000:0xDEADBEEF, 0x00C0000C:0xDEADBEEF})
T("rep movsd", [0xFC, 0xF3,0xA5],
  {'ecx':3,'esi':0x00C01000,'edi':0x00C02000},
  mem={0x00C01000:0x11111111,0x00C01004:0x22222222,0x00C01008:0x33333333},
  expect={'ecx':0,'esi':0x00C0100C,'edi':0x00C0200C},
  fmem={0x00C02000:0x11111111,0x00C02008:0x33333333})
T("stosd backwards with std", [0xFD, 0xAB, 0xFC],       # std; stosd; cld
  {'eax':0x5A5A5A5A,'edi':0x00C03000},
  expect={'edi':0x00C02FFC}, fmem={0x00C03000:0x5A5A5A5A})
T("repe cmpsb (equal)", [0xFC, 0xB9,4,0,0,0, 0xF3,0xA6],
  {'esi':0x00C04000,'edi':0x00C04010},
  mem={0x00C04000:0x44434241, 0x00C04010:0x44434241},
  expect={'ecx':0,'zf':1})
T("repe cmpsb (different)", [0xFC, 0xB9,4,0,0,0, 0xF3,0xA6],
  {'esi':0x00C05000,'edi':0x00C05010},
  mem={0x00C05000:0x44434241, 0x00C05010:0x44435A41},
  expect={'zf':0,'ecx':2})
T("repne scasb (find)", [0xFC, 0xB8,0x43,0,0,0, 0xB9,8,0,0,0, 0xF2,0xAE],
  {'edi':0x00C06000}, mem={0x00C06000:0x44434241},
  expect={'zf':1,'ecx':5})
T("lodsb", [0xFC, 0xAC], {'esi':0x00C07000,'eax':0xFFFFFF00},
  mem={0x00C07000:0x000000AB}, expect={'eax':0xFFFFFFAB,'esi':0x00C07001})

# ---------------- x87 ----------------
FA = 0x00C08000
def a32(v): return list(struct.pack('<I', v))
T("fld+fadd+fstp (1.5+2.25=3.75)",
  [0xD9,0x05]+a32(FA)+[0xD8,0x05]+a32(FA+4)+[0xD9,0x1D]+a32(FA+8),
  {}, memf={FA:1.5, FA+4:2.25}, expect={}, fmemf={FA+8:3.75})
T("fld+fmul+fstp (3 x 4 = 12)",
  [0xD9,0x05]+a32(FA+0x10)+[0xD8,0x0D]+a32(FA+0x14)+[0xD9,0x1D]+a32(FA+0x18),
  {}, memf={FA+0x10:3.0, FA+0x14:4.0}, expect={}, fmemf={FA+0x18:12.0})
T("fld+fsub+fstp (10-3.5=6.5)",
  [0xD9,0x05]+a32(FA+0x20)+[0xD8,0x25]+a32(FA+0x24)+[0xD9,0x1D]+a32(FA+0x28),
  {}, memf={FA+0x20:10.0, FA+0x24:3.5}, expect={}, fmemf={FA+0x28:6.5})
T("fld+fdiv+fstp (7/2=3.5)",
  [0xD9,0x05]+a32(FA+0x30)+[0xD8,0x35]+a32(FA+0x34)+[0xD9,0x1D]+a32(FA+0x38),
  {}, memf={FA+0x30:7.0, FA+0x34:2.0}, expect={}, fmemf={FA+0x38:3.5})
T("fild int->float (-1234)",
  [0xDB,0x05]+a32(FA+0x40)+[0xD9,0x1D]+a32(FA+0x44),
  {}, mem={FA+0x40:0xFFFFFB2E}, expect={}, fmemf={FA+0x44:-1234.0})
T("fchs (5 -> -5)",
  [0xD9,0x05]+a32(FA+0x50)+[0xD9,0xE0]+[0xD9,0x1D]+a32(FA+0x54),
  {}, memf={FA+0x50:5.0}, expect={}, fmemf={FA+0x54:-5.0})
T("fabs (-5 -> 5)",
  [0xD9,0x05]+a32(FA+0x60)+[0xD9,0xE1]+[0xD9,0x1D]+a32(FA+0x64),
  {}, memf={FA+0x60:-5.0}, expect={}, fmemf={FA+0x64:5.0})
T("fsqrt (16 -> 4)",
  [0xD9,0x05]+a32(FA+0x70)+[0xD9,0xFA]+[0xD9,0x1D]+a32(FA+0x74),
  {}, memf={FA+0x70:16.0}, expect={}, fmemf={FA+0x74:4.0})
T("fcomp: 1.0 < 2.0 -> C0=1 (ah bit0)",
  [0xD9,0x05]+a32(FA+0x80)+[0xD8,0x1D]+a32(FA+0x84)+[0xDF,0xE0],
  {}, memf={FA+0x80:1.0, FA+0x84:2.0}, expect={'ah_and':(0x41,0x01)})
T("fcomp: 2.0 > 1.0 -> C0=0,C3=0",
  [0xD9,0x05]+a32(FA+0x90)+[0xD8,0x1D]+a32(FA+0x94)+[0xDF,0xE0],
  {}, memf={FA+0x90:2.0, FA+0x94:1.0}, expect={'ah_and':(0x41,0x00)})
T("fcomp: 3.0 == 3.0 -> C3=1 (ah bit6)",
  [0xD9,0x05]+a32(FA+0xA0)+[0xD8,0x1D]+a32(FA+0xA4)+[0xDF,0xE0],
  {}, memf={FA+0xA0:3.0, FA+0xA4:3.0}, expect={'ah_and':(0x41,0x40)})
T("fxch",
  [0xD9,0x05]+a32(FA+0xB0)+[0xD9,0x05]+a32(FA+0xB4)+[0xD9,0xC9]+[0xD9,0x1D]+a32(FA+0xB8),
  {}, memf={FA+0xB0:1.0, FA+0xB4:2.0}, expect={}, fmemf={FA+0xB8:1.0})


# ---------------- memory addressing modes ----------------
MB = 0x00C10000
T("mov eax,[imm32]",        [0xA1]+a32(MB),                {}, mem={MB:0x11223344}, expect={'eax':0x11223344})
T("mov eax,[ecx]",          [0x8B,0x01],                   {'ecx':MB}, mem={MB:0xAABBCCDD}, expect={'eax':0xAABBCCDD})
T("mov eax,[ecx+8]",        [0x8B,0x41,0x08],              {'ecx':MB}, mem={MB+8:0x12345678}, expect={'eax':0x12345678})
T("mov eax,[ecx+edx*4]",    [0x8B,0x04,0x91],              {'ecx':MB,'edx':2}, mem={MB+8:0xCAFEBABE}, expect={'eax':0xCAFEBABE})
T("mov eax,[ecx+edx*8+4]",  [0x8B,0x44,0xD1,0x04],         {'ecx':MB,'edx':1}, mem={MB+12:0x0BADF00D}, expect={'eax':0x0BADF00D})
T("movzx eax,word [ecx+eax*2]", [0x0F,0xB7,0x04,0x41],     {'ecx':MB+0x20,'eax':3}, mem={MB+0x24:0xBEEF1234}, expect={'eax':0xBEEF})
T("lea eax,[ecx+edx*4+16]", [0x8D,0x44,0x91,0x10],         {'ecx':0x1000,'edx':3}, expect={'eax':0x101C})
T("lea eax,[ecx+eax-1]",    [0x8D,0x44,0x01,0xFF],         {'ecx':10,'eax':5}, expect={'eax':14})
T("mov [ecx],eax; read back",[0x89,0x01, 0x8B,0x11],        {'ecx':MB+0x40,'eax':0x5A5A5A5A}, expect={'edx':0x5A5A5A5A})
T("mov byte [ecx],dl",      [0x88,0x11],                   {'ecx':MB+0x50,'edx':0xAB}, fmem={MB+0x50:0xAB})
T("mov word [ecx],dx",      [0x66,0x89,0x11],              {'ecx':MB+0x60,'edx':0x1234}, fmem={MB+0x60:0x1234})
T("add dword [ecx],5",      [0x83,0x01,0x05],              {'ecx':MB+0x70}, mem={MB+0x70:10}, fmem={MB+0x70:15})
T("inc dword [ecx]",        [0xFF,0x01],                   {'ecx':MB+0x80}, mem={MB+0x80:41}, fmem={MB+0x80:42})
T("cmp byte [ecx],0",       [0x80,0x39,0x00, 0x0F,0x94,0xC0], {'ecx':MB+0x90,'eax':0}, mem={MB+0x90:0}, expect={'eax':1})

# ---------------- 8/16-bit arithmetic ----------------
T("add al,1 (0xFF)",   [0x04,0x01], {'eax':0x000000FF}, expect={'eax':0x00000000,'cf':1,'zf':1})
T("add ax,1 (0xFFFF)", [0x66,0x05,0x01,0x00], {'eax':0x1234FFFF}, expect={'eax':0x12340000,'cf':1,'zf':1})
T("sub al,1 (0)",      [0x2C,0x01], {'eax':0xFFFFFF00}, expect={'eax':0xFFFFFFFF,'cf':1})
T("cmp al,5 (al=5)",   [0x3C,0x05, 0x0F,0x94,0xC2], {'eax':5,'edx':0}, expect={'edx':1})
T("and al,0x0F",       [0x24,0x0F], {'eax':0x123456FF}, expect={'eax':0x1234560F})
T("or  ah,0xF0",       [0x80,0xCC,0xF0], {'eax':0x00000000}, expect={'eax':0x0000F000})
T("xor dl,dl",         [0x30,0xD2], {'edx':0x1234AB}, expect={'edx':0x123400,'zf':1})
T("test al,1 (odd)",   [0xA8,0x01, 0x0F,0x95,0xC2], {'eax':3,'edx':0}, expect={'edx':1})
T("test al,1 (even)",  [0xA8,0x01, 0x0F,0x95,0xC2], {'eax':4,'edx':0}, expect={'edx':0})
T("inc ax (0xFFFF)",   [0x66,0x40], {'eax':0x0000FFFF}, expect={'eax':0x00000000,'zf':1})
T("neg al (1)",        [0xF6,0xD8], {'eax':0x00000001}, expect={'eax':0x000000FF,'cf':1})
T("imul dx (word)",    [0x66,0xF7,0xEA], {'eax':0x0003,'edx':0x0004}, expect={'eax':0x000C,'edx':0})

# ---------------- every conditional setcc ----------------
def cc(name, opc, a, b, want):
    T(f"set{name} ({a} ? {b})", [0xB8]+imm32(a)+[0x3D]+imm32(b)+[0x0F,opc,0xC2],
      {'edx':0}, expect={'edx':want})
cc('o' ,0x90, 0x7FFFFFFF, 0xFFFFFFFF, 1)
cc('b' ,0x92, 1, 2, 1);         cc('ae',0x93, 2, 1, 1)
cc('e' ,0x94, 5, 5, 1);         cc('ne',0x95, 5, 6, 1)
cc('be',0x96, 2, 2, 1);         cc('a' ,0x97, 3, 2, 1)
cc('s' ,0x98, 1, 2, 1);         cc('ns',0x99, 2, 1, 1)
cc('l' ,0x9C, 0xFFFFFFFF, 1, 1);cc('ge',0x9D, 1, 0xFFFFFFFF, 1)
cc('le',0x9E, 0xFFFFFFFF, 1, 1);cc('g' ,0x9F, 1, 0xFFFFFFFF, 1)
cc('l_signed',0x9C, 1, 0xFFFFFFFF, 0)
cc('b_unsigned',0x92, 1, 0xFFFFFFFF, 1)

# ---------------- more string ----------------
MS = 0x00C20000
T("rep movsb", [0xFC, 0xF3,0xA4], {'ecx':5,'esi':MS,'edi':MS+0x100},
  mem={MS:0x44434241, MS+4:0x00000045}, expect={'ecx':0}, fmem={MS+0x100:0x44434241})
T("rep stosb", [0xFC, 0xF3,0xAA], {'eax':0x41,'ecx':4,'edi':MS+0x200},
  expect={'ecx':0,'edi':MS+0x204}, fmem={MS+0x200:0x41414141})
T("stosw", [0xFC, 0x66,0xAB], {'eax':0xBEEF,'edi':MS+0x300}, expect={'edi':MS+0x302})
T("scasd (equal)", [0xFC, 0xAF], {'eax':0x12345678,'edi':MS+0x400},
  mem={MS+0x400:0x12345678}, expect={'zf':1,'edi':MS+0x404})
T("cmpsd (different)", [0xFC, 0xA7], {'esi':MS+0x500,'edi':MS+0x510},
  mem={MS+0x500:1, MS+0x510:2}, expect={'zf':0,'cf':1})
T("repe cmpsd (equal block)", [0xFC, 0xB9,3,0,0,0, 0xF3,0xA7],
  {'esi':MS+0x600,'edi':MS+0x620},
  mem={MS+0x600:1,MS+0x604:2,MS+0x608:3, MS+0x620:1,MS+0x624:2,MS+0x628:3},
  expect={'ecx':0,'zf':1})

# ---------------- more x87 ----------------
FB = 0x00C30000
T("fld1 + fldz + faddp", [0xD9,0xE8, 0xD9,0xEE, 0xDE,0xC1, 0xD9,0x1D]+a32(FB),
  {}, expect={}, fmemf={FB:1.0})
T("fild qword", [0xDF,0x2D]+a32(FB+0x10)+[0xD9,0x1D]+a32(FB+0x18),
  {}, mem={FB+0x10:1000000, FB+0x14:0}, expect={}, fmemf={FB+0x18:1000000.0})
T("fistp dword (3.7 -> 4)", [0xD9,0x05]+a32(FB+0x20)+[0xDB,0x1D]+a32(FB+0x24),
  {}, memf={FB+0x20:3.7}, expect={}, fmem={FB+0x24:4})
T("fistp dword (-3.7 -> -4)", [0xD9,0x05]+a32(FB+0x30)+[0xDB,0x1D]+a32(FB+0x34),
  {}, memf={FB+0x30:-3.7}, expect={}, fmem={FB+0x34:0xFFFFFFFC})
T("fld qword (double)", [0xDD,0x05]+a32(FB+0x40)+[0xD9,0x1D]+a32(FB+0x48),
  {}, mem={FB+0x40:0x00000000, FB+0x44:0x40090000}, expect={}, fmemf={FB+0x48:3.125})
T("fstp qword", [0xD9,0x05]+a32(FB+0x50)+[0xDD,0x1D]+a32(FB+0x58),
  {}, memf={FB+0x50:2.5}, expect={}, fmem={FB+0x5C:0x40040000})
T("fnstsw then sahf/ja", [0xD9,0x05]+a32(FB+0x60)+[0xD8,0x1D]+a32(FB+0x64)+[0xDF,0xE0,0x9E,0x0F,0x97,0xC2],
  {'edx':0}, memf={FB+0x60:5.0, FB+0x64:2.0}, expect={'edx':1})
T("frndint (2.5 -> 2)", [0xD9,0x05]+a32(FB+0x70)+[0xD9,0xFC]+[0xD9,0x1D]+a32(FB+0x74),
  {}, memf={FB+0x70:2.5}, expect={}, fmemf={FB+0x74:2.0})
T("fiadd dword", [0xD9,0x05]+a32(FB+0x80)+[0xDA,0x05]+a32(FB+0x84)+[0xD9,0x1D]+a32(FB+0x88),
  {}, memf={FB+0x80:1.5}, mem={FB+0x84:10}, expect={}, fmemf={FB+0x88:11.5})
T("fimul dword", [0xD9,0x05]+a32(FB+0x90)+[0xDA,0x0D]+a32(FB+0x94)+[0xD9,0x1D]+a32(FB+0x98),
  {}, memf={FB+0x90:2.5}, mem={FB+0x94:4}, expect={}, fmemf={FB+0x98:10.0})

# ---------------- x87 rounding control (RC field) ----------------
# fistp must follow the RC field of the control word.  The CRT's __ftol routine
# sets RC=11 (truncate); using nearbyint gives one too many for fractions >= 0.5.
FD = 0x00C40000
def _rc(name, cw, val, want):
    # fldcw [FD]; fld dword [FD+8]; fistp dword [FD+12]
    T(name, [0xD9,0x2D]+a32(FD) + [0xD9,0x05]+a32(FD+8) + [0xDB,0x1D]+a32(FD+12),
      {}, mem={FD:cw}, memf={FD+8:val}, expect={}, fmem={FD+12:want & 0xFFFFFFFF})

_rc("fistp RC=nearest 3.7 -> 4",   0x037F,  3.7,  4)
_rc("fistp RC=nearest 2.5 -> 2",   0x037F,  2.5,  2)   # round to even
_rc("fistp RC=nearest 3.5 -> 4",   0x037F,  3.5,  4)
_rc("fistp RC=trunc  3.7 -> 3",    0x0F7F,  3.7,  3)
_rc("fistp RC=trunc  -3.7 -> -3",  0x0F7F, -3.7, -3)
_rc("fistp RC=trunc  6.999 -> 6",  0x0F7F,  6.999, 6)
_rc("fistp RC=down   3.7 -> 3",    0x077F,  3.7,  3)
_rc("fistp RC=down   -3.2 -> -4",  0x077F, -3.2, -4)
_rc("fistp RC=up     3.2 -> 4",    0x0B7F,  3.2,  4)
_rc("fistp RC=up     -3.7 -> -3",  0x0B7F, -3.7, -3)

# x87 NaN propagation (Intel SDM vol. 1 table 4-7): with two NaN operands the
# result is the one with the larger significand, whichever is the
# destination.  The drift oracle found the integrator 0x5AA480 summing NaN
# garbage of unused state slots.
FN_ = 0x00C70000
T("fadd QNaN+QNaN -> larger significand (dst smaller)",
  [0xD9,0x05]+a32(FN_)+[0xD8,0x05]+a32(FN_+4)+[0xD9,0x1D]+a32(FN_+8),
  {}, mem={FN_:0x7FC00001, FN_+4:0x7FC00100}, expect={}, fmem={FN_+8:0x7FC00100})
T("fadd QNaN+QNaN -> larger significand (dst larger)",
  [0xD9,0x05]+a32(FN_+0x10)+[0xD8,0x05]+a32(FN_+0x14)+[0xD9,0x1D]+a32(FN_+0x18),
  {}, mem={FN_+0x10:0x7FC00100, FN_+0x14:0xFFC00001}, expect={}, fmem={FN_+0x18:0x7FC00100})
# x87 precision control (X87PC=1): fldcw [FP]; fld1; fdiv dword [FP+4] (3.0);
# fstp qword [FP+8].  PC=24 rounds the quotient to a float, PC=53 does not.
FP_ = 0x00C71000
def _pc(name, cw, lo, hi, base):
    T(name, [0xD9,0x2D]+a32(base) + [0xD9,0xE8] + [0xD8,0x35]+a32(base+4) + [0xDD,0x1D]+a32(base+8),
      {}, mem={base:cw}, memf={base+4:3.0}, expect={}, fmem={base+8:lo, base+12:hi})
_pc("fdiv 1/3 at PC=24 (CW 0x007F) -> float-rounded", 0x007F, 0x60000000, 0x3FD55555, FP_)
_pc("fdiv 1/3 at PC=53 (CW 0x027F) -> double",        0x027F, 0x55555555, 0x3FD55555, FP_+0x20)
_pc("fdiv 1/3 at PC=64 (CW 0x037F) -> double",        0x037F, 0x55555555, 0x3FD55555, FP_+0x40)
T("fmul QNaN*1.0 -> the NaN",
  [0xD9,0x05]+a32(FN_+0x20)+[0xD8,0x0D]+a32(FN_+0x24)+[0xD9,0x1D]+a32(FN_+0x28),
  {}, mem={FN_+0x20:0x3F800000, FN_+0x24:0x7FC0ABC0}, expect={}, fmem={FN_+0x28:0x7FC0ABC0})
# CRT __ftol pattern: fnstcw / or ah,0xc / fldcw / fistp
T("__ftol pattern (2.6+0.001 -> 2)",
  [0xD9,0x3D]+a32(FD+0x20) +                    # fnstcw [FD+0x20]
  [0x66,0xA1]+a32(FD+0x20) +                    # mov ax, [FD+0x20]
  [0x80,0xCC,0x0C] +                            # or ah, 0x0C
  [0x66,0xA3]+a32(FD+0x24) +                    # mov [FD+0x24], ax
  [0xD9,0x2D]+a32(FD+0x24) +                    # fldcw [FD+0x24]
  [0xD9,0x05]+a32(FD+0x28) +                    # fld dword [FD+0x28]
  [0xDB,0x1D]+a32(FD+0x2C),                     # fistp dword [FD+0x2C]
  {}, memf={FD+0x28:2.601}, expect={}, fmem={FD+0x2C:2})

# ---------------- MMX/SSE moves ----------------
MX = 0x00C40000
T("movq mm0,[m]; movq [m2],mm0", [0x0F,0x6F,0x05]+a32(MX)+[0x0F,0x7F,0x05]+a32(MX+8),
  {}, mem={MX:0x11223344, MX+4:0x55667788}, fmem={MX+8:0x11223344, MX+12:0x55667788})
T("movd mm0,eax; movd edx,mm0", [0x0F,0x6E,0xC0, 0x0F,0x7E,0xC2],
  {'eax':0xDEADBEEF}, expect={'edx':0xDEADBEEF})
T("movaps xmm0,[m]; movaps [m2],xmm0", [0x0F,0x28,0x05]+a32(MX+0x20)+[0x0F,0x29,0x05]+a32(MX+0x30),
  {}, mem={MX+0x20:1,MX+0x24:2,MX+0x28:3,MX+0x2C:4},
  fmem={MX+0x30:1,MX+0x34:2,MX+0x38:3,MX+0x3C:4})
T("pxor xmm0,xmm0 then store", [0x0F,0x28,0x05]+a32(MX+0x40)+[0x66,0x0F,0xEF,0xC0]+[0x0F,0x29,0x05]+a32(MX+0x50),
  {}, mem={MX+0x40:0xFFFFFFFF,MX+0x44:0xFFFFFFFF,MX+0x48:0xFFFFFFFF,MX+0x4C:0xFFFFFFFF},
  fmem={MX+0x50:0,MX+0x54:0,MX+0x58:0,MX+0x5C:0})


# ---------------- newly added instructions ----------------
T("rcl eax,1 (CF=1, 0x80000000)", [0xF9, 0xD1,0xD0], {'eax':0x80000000}, expect={'eax':0x00000001,'cf':1})
T("rcr eax,1 (CF=1, 1)",          [0xF9, 0xD1,0xD8], {'eax':0x00000001}, expect={'eax':0x80000000,'cf':1})
T("rcl al,1 (CF=0, 0x80)",        [0xF8, 0xD0,0xD0], {'eax':0x80},       expect={'eax':0x00,'cf':1})
MM2 = 0x00C50000
T("paddw mm0,[m]", [0x0F,0x6F,0x05]+a32(MM2)+[0x0F,0xFD,0x05]+a32(MM2+8)+[0x0F,0x7F,0x05]+a32(MM2+16),
  {}, mem={MM2:0x00020001, MM2+4:0x00040003, MM2+8:0x00100010, MM2+12:0x00100010},
  fmem={MM2+16:0x00120011, MM2+20:0x00140013})
T("psubw mm0,[m]", [0x0F,0x6F,0x05]+a32(MM2+0x20)+[0x0F,0xF9,0x05]+a32(MM2+0x28)+[0x0F,0x7F,0x05]+a32(MM2+0x30),
  {}, mem={MM2+0x20:0x00200020, MM2+0x24:0x00200020, MM2+0x28:0x00010002, MM2+0x2C:0x00030004},
  fmem={MM2+0x30:0x001F001E, MM2+0x34:0x001D001C})
T("pmullw mm0,[m]", [0x0F,0x6F,0x05]+a32(MM2+0x40)+[0x0F,0xD5,0x05]+a32(MM2+0x48)+[0x0F,0x7F,0x05]+a32(MM2+0x50),
  {}, mem={MM2+0x40:0x00030002, MM2+0x44:0x00050004, MM2+0x48:0x00020002, MM2+0x4C:0x00020002},
  fmem={MM2+0x50:0x00060004, MM2+0x54:0x000A0008})
T("paddusb saturation", [0x0F,0x6F,0x05]+a32(MM2+0x60)+[0x0F,0xDC,0x05]+a32(MM2+0x68)+[0x0F,0x7F,0x05]+a32(MM2+0x70),
  {}, mem={MM2+0x60:0xFFFFFFFF, MM2+0x64:0x01010101, MM2+0x68:0x02020202, MM2+0x6C:0x02020202},
  fmem={MM2+0x70:0xFFFFFFFF, MM2+0x74:0x03030303})
T("psubusb saturation", [0x0F,0x6F,0x05]+a32(MM2+0x80)+[0x0F,0xD8,0x05]+a32(MM2+0x88)+[0x0F,0x7F,0x05]+a32(MM2+0x90),
  {}, mem={MM2+0x80:0x01010101, MM2+0x84:0x10101010, MM2+0x88:0x02020202, MM2+0x8C:0x02020202},
  fmem={MM2+0x90:0x00000000, MM2+0x94:0x0E0E0E0E})
T("pcmpeqw", [0x0F,0x6F,0x05]+a32(MM2+0xA0)+[0x0F,0x75,0x05]+a32(MM2+0xA8)+[0x0F,0x7F,0x05]+a32(MM2+0xB0),
  {}, mem={MM2+0xA0:0x00010001, MM2+0xA4:0x00020002, MM2+0xA8:0x00010002, MM2+0xAC:0x00020002},
  fmem={MM2+0xB0:0xFFFF0000, MM2+0xB4:0xFFFFFFFF})
T("psrlw mm0,4", [0x0F,0x6F,0x05]+a32(MM2+0xC0)+[0x0F,0x71,0xD0,0x04]+[0x0F,0x7F,0x05]+a32(MM2+0xC8),
  {}, mem={MM2+0xC0:0xF0F0F0F0, MM2+0xC4:0x12341234},
  fmem={MM2+0xC8:0x0F0F0F0F, MM2+0xCC:0x01230123})
T("psraw mm0,4 (negative)", [0x0F,0x6F,0x05]+a32(MM2+0xE0)+[0x0F,0x71,0xE0,0x04]+[0x0F,0x7F,0x05]+a32(MM2+0xE8),
  {}, mem={MM2+0xE0:0x8000F000, MM2+0xE4:0x00000000},
  fmem={MM2+0xE8:0xF800FF00, MM2+0xEC:0x00000000})
T("packuswb", [0x0F,0x6F,0x05]+a32(MM2+0x100)+[0x0F,0x67,0x05]+a32(MM2+0x108)+[0x0F,0x7F,0x05]+a32(MM2+0x110),
  {}, mem={MM2+0x100:0x00FF0001, MM2+0x104:0xFFFF0100, MM2+0x108:0x00020003, MM2+0x10C:0x00040005},
  fmem={MM2+0x110:0x00FFFF01, MM2+0x114:0x04050203})
# a register or memory count is all 64 bits of it: 2^32 is more than 63 (the
# qword is emptied), 2^32+1 more than 15 (each word becomes its sign)
T("psrlq mm1,mm0 (count 2^32)", [0x0F,0x6F,0x0D]+a32(MM2+0x120)+[0x0F,0x6F,0x05]+a32(MM2+0x128)+[0x0F,0xD3,0xC8]+[0x0F,0x7F,0x0D]+a32(MM2+0x130),
  {}, mem={MM2+0x120:0x9ABCDEF0, MM2+0x124:0x12345678, MM2+0x128:0x00000000, MM2+0x12C:0x00000001,
           MM2+0x130:0xFFFFFFFF, MM2+0x134:0xFFFFFFFF},
  fmem={MM2+0x130:0x00000000, MM2+0x134:0x00000000})
T("psraw mm0,[m] (count 2^32+1)", [0x0F,0x6F,0x05]+a32(MM2+0x140)+[0x0F,0xE1,0x05]+a32(MM2+0x148)+[0x0F,0x7F,0x05]+a32(MM2+0x150),
  {}, mem={MM2+0x140:0x7FFF8000, MM2+0x144:0x0001FFFE, MM2+0x148:0x00000001, MM2+0x14C:0x00000001},
  fmem={MM2+0x150:0x0000FFFF, MM2+0x154:0x0000FFFF})
# SSE2: the xmm forms of the MMX instructions work on all 128 bits.  A pack fills
# the low qword from the destination's elements and the high qword from the
# source's; an unpack interleaves the low (high) qwords of both into the whole
# register; the saturating adds and subtracts and the shifts do both halves, by
# one count.  They were lifted per 64-bit half (the VP6 decoder's SSE2 code).
# xmm0 = [x], xmm1 = [x+0x10] (or the operand itself), result [x+0x20].
X2 = [0x00C80000]
def _x2(name, op, a, b, want, m128=False):
    x = X2[0]; X2[0] += 0x40
    T(name + " (128-bit)", [0xF3,0x0F,0x6F,0x05]+a32(x)+[0xF3,0x0F,0x6F,0x0D]+a32(x+0x10)
      + op + (a32(x+0x10) if m128 else []) + [0xF3,0x0F,0x7F,0x05]+a32(x+0x20),
      {}, mem={x+4*k: v for k, v in enumerate(a + b)}, fmem={x+0x20+4*k: v for k, v in enumerate(want)})
_W1 = [0x7F010080, 0xFF7FFF80, 0x80001234, 0x00010000]; _W2 = [0x00FE0101, 0xFFFF0003, 0x7FFF0040, 0x80010100]
_B1 = [0x13121110, 0x17161514, 0x1B1A1918, 0x1F1E1D1C]; _B2 = [0xA3A2A1A0, 0xA7A6A5A4, 0xABAAA9A8, 0xAFAEADAC]
_S1 = [0x7FF08010, 0x00FF7FFF, 0xFF00C000, 0x40018000]; _S2 = [0x00207FF0, 0x0001FFFF, 0x0100C000, 0x3FFF0001]
_x2("packsswb xmm0,xmm1", [0x66,0x0F,0x63,0xC1], _W1, _W2, [0x80807F7F, 0x0100807F, 0xFF037F7F, 0x807F7F40])
_x2("packssdw xmm0,xmm1", [0x66,0x0F,0x6B,0xC1], [0x00007FFF, 0xFFFF8000, 0x00012345, 0xFFFE0000],
    [0x00000005, 0x80000000, 0xFFFFFFF0, 0x7FFFFFFF], [0x80007FFF, 0x80007FFF, 0x80000005, 0x7FFFFFF0])
_x2("packuswb xmm0,xmm1", [0x66,0x0F,0x67,0xC1], _W1, _W2, [0x0000FF80, 0x010000FF, 0x0003FEFF, 0x00FFFF40])
_x2("packuswb xmm0,[m128]", [0x66,0x0F,0x67,0x05], _W2, _W1, [0x0003FEFF, 0x00FFFF40, 0x0000FF80, 0x010000FF], m128=True)
_x2("punpcklbw xmm0,xmm1", [0x66,0x0F,0x60,0xC1], _B1, _B2, [0xA111A010, 0xA313A212, 0xA515A414, 0xA717A616])
_x2("punpckhbw xmm0,xmm1", [0x66,0x0F,0x68,0xC1], _B1, _B2, [0xA919A818, 0xAB1BAA1A, 0xAD1DAC1C, 0xAF1FAE1E])
_x2("punpcklwd xmm0,xmm1", [0x66,0x0F,0x61,0xC1], _B1, _B2, [0xA1A01110, 0xA3A21312, 0xA5A41514, 0xA7A61716])
_x2("punpckhwd xmm0,xmm1", [0x66,0x0F,0x69,0xC1], _B1, _B2, [0xA9A81918, 0xABAA1B1A, 0xADAC1D1C, 0xAFAE1F1E])
_x2("punpckldq xmm0,xmm1", [0x66,0x0F,0x62,0xC1], _B1, _B2, [0x13121110, 0xA3A2A1A0, 0x17161514, 0xA7A6A5A4])
_x2("punpckhdq xmm0,xmm1", [0x66,0x0F,0x6A,0xC1], _B1, _B2, [0x1B1A1918, 0xABAAA9A8, 0x1F1E1D1C, 0xAFAEADAC])
_x2("punpckhbw xmm0,[m128]", [0x66,0x0F,0x68,0x05], _B1, _B2, [0xA919A818, 0xAB1BAA1A, 0xAD1DAC1C, 0xAF1FAE1E], m128=True)
_x2("paddsw xmm0,xmm1", [0x66,0x0F,0xED,0xC1], _S1, _S2, [0x7FFF0000, 0x01007FFE, 0x00008000, 0x7FFF8001])
_x2("paddusb xmm0,xmm1", [0x66,0x0F,0xDC,0xC1], _S1, _S2, [0x7FFFFFFF, 0x00FFFFFF, 0xFF00FF00, 0x7FFF8001])
_x2("psubsb xmm0,xmm1", [0x66,0x0F,0xE8,0xC1], _S1, _S2, [0x7FD08020, 0x00FE7F00, 0xFE000000, 0x010280FF])
_x2("psubusw xmm0,[m128]", [0x66,0x0F,0xD9,0x05], _S1, _S2, [0x7FD00020, 0x00FE0000, 0xFE000000, 0x00027FFF], m128=True)
_x2("psllw xmm0,3", [0x66,0x0F,0x71,0xF0,0x03], _S1, _S2, [0xFF800080, 0x07F8FFF8, 0xF8000000, 0x00080000])
_x2("psrad xmm0,9", [0x66,0x0F,0x72,0xE0,0x09], _S1, _S2, [0x003FF840, 0x00007FBF, 0xFFFF8060, 0x002000C0])
_x2("psrlq xmm0,4", [0x66,0x0F,0x73,0xD0,0x04], _S1, _S2, [0xF7FF0801, 0x000FF7FF, 0x0FF00C00, 0x04001800])
_x2("psllq xmm0,xmm1 (count 12)", [0x66,0x0F,0xF3,0xC1], _S1, [12, 0, 0x55, 0x66], [0x08010000, 0xF7FFF7FF, 0x0C000000, 0x18000FF0])
# the count is the destination's own low qword: read before either half is shifted
_x2("psrlw xmm0,xmm0 (count 4)", [0x66,0x0F,0xD1,0xC0], [4, 0, 0xF00FABCD, 0x8000FFF0], [0, 0, 0, 0],
    [0x00000000, 0x00000000, 0x0F000ABC, 0x08000FFF])
# all 64 bits of the count: 2^32+4 is more than 15 (each word becomes its sign), not 4
_x2("psraw xmm0,xmm1 (count 2^32+4)", [0x66,0x0F,0xE1,0xC1], _S1, [4, 1, 0x55, 0x66], [0x0000FFFF, 0x00000000, 0xFFFFFFFF, 0x0000FFFF])
# A callee that returns past its caller (tools/retpast.txt), in the shape of the
# CRT's x87 2^x core (0x7653de): a range check (fcompp, fnstsw into the frame at
# ebp, test 0x41, jne) and, out of range, 'pop eax' -- its own return address --
# and a ret that goes to its caller's caller.  The test calls F (+0x12), F calls
# H (+0x21) and adds 1 and sets ebx only when H comes back to it.  ZF, CF and
# SF start at 1; out of range H's test leaves them 0, which the early return
# hands on (CF and SF check the FLAGABI lift: H's own jne reads only ZF).  With
# RETEIP=0 (a ret only pops) H still writes c->eip: F compares it.
RP = 0x00C90000
def _rp(name, x, rp, res, expect, env=None):
    T(name, [0xD9,0x05]+a32(rp) + [0xE8,0x07,0,0,0] + [0xD9,0x1D]+a32(rp+8) + [0xC3]   # fld [rp]; call F; fstp [rp+8]
      + [0xE8,0x0A,0,0,0, 0xD9,0xE8, 0xDE,0xC1, 0xBB,1,0,0,0, 0xC3]                     # F: call H; fld1; faddp; mov ebx,1
      + [0xD9,0xC0, 0xD9,0xE1, 0xD9,0x05]+a32(rp+4) + [0xDE,0xD9]                       # H: fld st0; fabs; fld [limit]; fcompp
      + [0x9B, 0xDD,0xBD,0x60,0xFF,0xFF,0xFF, 0xF6,0x85,0x61,0xFF,0xFF,0xFF,0x41]       # fnstsw [ebp-0xa0]; test [ebp-0x9f],0x41
      + [0x75,0x01, 0xC3, 0x58, 0xDD,0xD8, 0xD9,0x05]+a32(rp+12) + [0xC3],              # jne; ret | pop eax; fstp st0; fld [rp+12]
      {'ebp':rp+0x200, 'ebx':0, 'zf':1, 'cf':1, 'sf':1}, memf={rp:x, rp+4:100.0, rp+12:7.0}, expect=expect, fmemf={rp+8:res},
      calls=(0x12, 0x21), retpast=(0x21,), env=env)
_rp("call returning past its caller: in range", 3.0, RP, 4.0, {'ebx':1})
_rp("call returning past its caller: out of range", 1000.0, RP+0x400, 7.0, {'ebx':0, 'zf':0, 'cf':0, 'sf':0, 'eax':0x00401017})
_rp("call returning past its caller: in range, RETEIP=0", 3.0, RP+0x800, 4.0, {'ebx':1}, env={'RETEIP':'0'})
_rp("call returning past its caller: out of range, RETEIP=0", 1000.0, RP+0xC00, 7.0, {'ebx':0, 'zf':0, 'cf':0, 'sf':0, 'eax':0x00401017},
    env={'RETEIP':'0'})
T("pushal/popal", [0x60, 0xB8,0x99,0,0,0, 0x61],   # pushad; mov eax,0x99; popad
  {'eax':0x11223344,'ecx':0x55667788}, expect={'eax':0x11223344,'ecx':0x55667788})
FC = 0x00C60000
T("fstp tbyte / fld tbyte (3.75)",
  [0xD9,0x05]+a32(FC)+[0xDB,0x3D]+a32(FC+0x10)+[0xDB,0x2D]+a32(FC+0x10)+[0xD9,0x1D]+a32(FC+0x20),
  {}, memf={FC:3.75}, expect={}, fmemf={FC+0x20:3.75})
T("fstp tbyte / fld tbyte (-1e20)",
  [0xDD,0x05]+a32(FC+0x30)+[0xDB,0x3D]+a32(FC+0x40)+[0xDB,0x2D]+a32(FC+0x40)+[0xDD,0x1D]+a32(FC+0x50),
  {}, mem={FC+0x30:0x00000000, FC+0x34:0xC415AF1D}, expect={},
  fmem={FC+0x50:0x00000000, FC+0x54:0xC415AF1D})
T("addsd xmm0,[m]", [0xF2,0x0F,0x10,0x05]+a32(FC+0x60)+[0xF2,0x0F,0x58,0x05]+a32(FC+0x68)+[0xF2,0x0F,0x11,0x05]+a32(FC+0x70),
  {}, mem={FC+0x60:0x00000000, FC+0x64:0x3FF00000, FC+0x68:0x00000000, FC+0x6C:0x40000000},
  fmem={FC+0x70:0x00000000, FC+0x74:0x40080000})
T("movhlps", [0x0F,0x28,0x05]+a32(FC+0x80)+[0x0F,0x12,0xC0]+[0x0F,0x29,0x05]+a32(FC+0x90),
  {}, mem={FC+0x80:1,FC+0x84:2,FC+0x88:3,FC+0x8C:4},
  fmem={FC+0x90:3, FC+0x94:4})
# v12na: MMX<->SSE conversions (0x73392f cvtpi2ps trapped in the race)
T("cvtpi2ps xmm0,[m64] (upper half kept)",
  [0x0F,0x28,0x05]+a32(FC+0xA0)+[0x0F,0x2A,0x05]+a32(FC+0xB0)+[0x0F,0x29,0x05]+a32(FC+0xC0),
  {}, memf={FC+0xA0:9.0,FC+0xA4:9.0,FC+0xA8:9.0,FC+0xAC:9.0},
  mem={FC+0xB0:0xFFFFFFFD, FC+0xB4:7},
  fmemf={FC+0xC0:-3.0, FC+0xC4:7.0, FC+0xC8:9.0, FC+0xCC:9.0})
T("cvttps2pi mm0,[m64] (truncation)",
  [0x0F,0x2C,0x05]+a32(FC+0xD0)+[0x0F,0x7F,0x05]+a32(FC+0xD8),
  {}, memf={FC+0xD0:2.7, FC+0xD4:-2.7}, fmem={FC+0xD8:2, FC+0xDC:0xFFFFFFFE})
# v12na: fsincos / fcmov / xlatb / shufps memory / fxam
T("fsincos (0.5): ST0=cos, ST1=sin",
  [0xD9,0x05]+a32(FC+0x100)+[0xD9,0xFB]+[0xD9,0x1D]+a32(FC+0x104)+[0xD9,0x1D]+a32(FC+0x108),
  {}, memf={FC+0x100:0.5}, fmemf={FC+0x104:0.8775825618903728, FC+0x108:0.479425538604203})
T("fcmovb st0,st1 (CF=1 copies)",
  [0xD9,0x05]+a32(FC+0x110)+[0xD9,0x05]+a32(FC+0x114)+[0xF9]+[0xDA,0xC1]+[0xD9,0x1D]+a32(FC+0x118)+[0xDD,0xD8],
  {}, memf={FC+0x110:7.0, FC+0x114:3.0}, fmemf={FC+0x118:7.0})
T("fcmovb st0,st1 (CF=0 does not copy)",
  [0xD9,0x05]+a32(FC+0x120)+[0xD9,0x05]+a32(FC+0x124)+[0xF8]+[0xDA,0xC1]+[0xD9,0x1D]+a32(FC+0x128)+[0xDD,0xD8],
  {}, memf={FC+0x120:7.0, FC+0x124:3.0}, fmemf={FC+0x128:3.0})
T("xlatb", [0xD7], {'ebx':FC+0x130, 'eax':0x11223302}, mem={FC+0x130:0x44332211},
  expect={'eax':0x11223333})
T("shufps xmm0,[m128],0x4e",
  [0x0F,0x28,0x05]+a32(FC+0x140)+[0x0F,0xC6,0x05]+a32(FC+0x150)+[0x4E]+[0x0F,0x29,0x05]+a32(FC+0x160),
  {}, mem={FC+0x140:1,FC+0x144:2,FC+0x148:3,FC+0x14C:4, FC+0x150:5,FC+0x154:6,FC+0x158:7,FC+0x15C:8},
  fmem={FC+0x160:3, FC+0x164:4, FC+0x168:5, FC+0x16C:6})
T("cvtps2pi mm0,[m64] (nearest even)",
  [0x0F,0x2D,0x05]+a32(FC+0xE0)+[0x0F,0x7F,0x05]+a32(FC+0xE8),
  {}, memf={FC+0xE0:2.5, FC+0xE4:3.5}, fmem={FC+0xE8:2, FC+0xEC:4})

# ---------------- xadd and lock-prefixed read-modify-writes ----------------
# MW's PATH audio library: lock xadd dword [ecx],eax (0x834E2D) and [eax],ecx (0x835645)
LK = 0x00C80000
T("xadd [ecx],eax",
  [0x0F,0xC1,0x01], {'ecx':LK, 'eax':3}, mem={LK:5}, expect={'eax':5,'cf':0,'zf':0}, fmem={LK:8})
T("lock xadd [ecx],eax",
  [0xF0,0x0F,0xC1,0x01], {'ecx':LK+0x10, 'eax':3}, mem={LK+0x10:5}, expect={'eax':5,'cf':0,'zf':0,'sf':0}, fmem={LK+0x10:8})
T("lock xadd [ecx],eax (wraps to 0)",
  [0xF0,0x0F,0xC1,0x01], {'ecx':LK+0x20, 'eax':1}, mem={LK+0x20:0xFFFFFFFF}, expect={'eax':0xFFFFFFFF,'cf':1,'zf':1}, fmem={LK+0x20:0})
T("lock xadd [eax],ecx (ecx=-1: decrement)",
  [0xF0,0x0F,0xC1,0x08], {'eax':LK+0x30, 'ecx':0xFFFFFFFF}, mem={LK+0x30:1}, expect={'ecx':1,'eax':LK+0x30,'zf':1,'cf':1}, fmem={LK+0x30:0})
T("lock xadd [ecx+8],eax; inc eax (InterlockedIncrement)",
  [0xF0,0x0F,0xC1,0x41,0x08, 0x40], {'ecx':LK+0x40, 'eax':1}, mem={LK+0x48:41}, expect={'eax':42}, fmem={LK+0x48:42})
T("lock xadd byte [ecx],al",
  [0xF0,0x0F,0xC0,0x01], {'ecx':LK+0x50, 'eax':0xAABBCC02}, mem={LK+0x50:0x112233FF}, expect={'eax':0xAABBCCFF,'cf':1}, fmem={LK+0x50:0x11223301})
T("lock add [ecx],eax (signed overflow)",
  [0xF0,0x01,0x01], {'ecx':LK+0x60, 'eax':1}, mem={LK+0x60:0x7FFFFFFF}, expect={'eax':1,'of':1,'sf':1,'cf':0,'zf':0}, fmem={LK+0x60:0x80000000})
T("lock add dword [m32],1",
  [0xF0,0x83,0x05]+a32(LK+0x70)+[0x01], {}, mem={LK+0x70:41}, expect={'zf':0}, fmem={LK+0x70:42})
T("lock sub [ecx],eax (to 0)",
  [0xF0,0x29,0x01], {'ecx':LK+0x80, 'eax':7}, mem={LK+0x80:7}, expect={'zf':1,'cf':0}, fmem={LK+0x80:0})
# 0x834EE0 and 0x834EF9: the same function adds edi, then ebx, to the counter at [esi]
T("lock xadd [esi],edi; lock xadd [esi],ebx",
  [0xF0,0x0F,0xC1,0x3E, 0xF0,0x0F,0xC1,0x1E], {'esi':LK+0x90, 'edi':2, 'ebx':0xFFFFFFFF}, mem={LK+0x90:10},
  expect={'edi':10, 'ebx':12, 'esi':LK+0x90, 'cf':1, 'zf':0}, fmem={LK+0x90:11})

# ---------------- MW's x87 status word and environment ----------------
MW_ = 0x00C90000
# FUCOMPP writes only the x87 status word.  MW's sound gain routine 0x81CDC6
# keeps a CMP's ZF across one at 0x81CE0E (cmp ebx,-1; fld st(0); fld dword
# [ebp+0x10]; mov byte [edi+0x12],0; fucompp; jne): codepdbh's lifter took the
# integer flags from the compare and corrupted voice+0x118.  Here the compare
# says the opposite of the CMP each time; edx = 1 when the jne falls through.
def _fucompp(name, ebx, a, b, edx, cc, base):
    T(name, [0xD9,0x05]+a32(base) + [0x83,0xFB,0xFF] + [0xD9,0xC0] + [0xD9,0x05]+a32(base+4)
            + [0xC6,0x05]+a32(base+8)+[0x00] + [0xDA,0xE9] + [0x75,0x07] + [0xBA]+imm32(1) + [0xEB,0x05]
            + [0xBA]+imm32(2) + [0xDF,0xE0] + [0xD9,0x1D]+a32(base+12),
      {'ebx':ebx}, mem={base+8:0x11223344}, memf={base:a, base+4:b},
      expect={'edx':edx, 'ah_and':(0x45, cc)}, fmem={base+8:0x11223300}, fmemf={base+12:a})
_fucompp("cmp ebx,-1 (equal); fucompp (2.0 > 1.0); jne", 0xFFFFFFFF, 1.0, 2.0, 1, 0x00, MW_)
_fucompp("cmp ebx,-1 (not equal); fucompp (1.0 == 1.0); jne", 5, 1.0, 1.0, 2, 0x40, MW_+0x10)
# test r16 sets SF from bit 15 and ZF from the low 16 bits only: test bx,bx; jle
# (edx = 2 when taken).  The upper half of ebx says the opposite each time.
def _testw(name, ebx, edx):
    T(name, [0x66,0x85,0xDB, 0x7E,0x07, 0xBA]+imm32(1)+[0xEB,0x05, 0xBA]+imm32(2),
      {'ebx':ebx}, expect={'edx':edx, 'ebx':ebx})
_testw("test bx,bx; jle (bx=0x8000, ebx > 0: taken)", 0x00018000, 2)
_testw("test bx,bx; jle (bx=0, ebx != 0: taken)", 0x12340000, 2)
_testw("test bx,bx; jle (bx=1, ebx < 0: not taken)", 0xFFFF0001, 1)
_testw("test bx,bx; jle (bx=0x7FFF, ebx < 0: not taken)", 0x80007FFF, 1)
# X87ENV=1.  The CRT's _adj_fprem (0x7D96AB) sets C3/C1/C0 through the
# environment: fld1; mov eax,0x4100; sub esp,0x1c; fnstenv [esp]; and dword
# [esp+4],0xbcff; or [esp+4],eax; fldenv [esp]; add esp,0x1c; fnstsw ax;
# fstp dword [m].  ah = C3 | TOP 7 | C0; the image has CW with the reserved
# upper half set, and FIP 0.  TW is not checked: the model writes it as all
# empty (tools/lift.py), the hardware with the fld1's register valid.
_ENV = 0x00B00000 - 4 - 0x1C   # the harness's esp after the return address and the sub
T("fnstenv/fldenv [esp]: C3|C0 into the status word (_adj_fprem)",
  [0xD9,0xE8, 0xB8]+imm32(0x4100) + [0x83,0xEC,0x1C, 0xD9,0x34,0x24, 0x81,0x64,0x24,0x04]+imm32(0xBCFF)
  + [0x09,0x44,0x24,0x04, 0xD9,0x24,0x24, 0x83,0xC4,0x1C, 0xDF,0xE0, 0xD9,0x1D]+a32(MW_+0x20),
  {}, expect={'ah_and':(0xFF, 0x79)},
  fmem={_ENV:0xFFFF037F, _ENV+4:0x00007900, _ENV+12:0}, fmemf={MW_+0x20:1.0})
# fnstenv stores the control word, then masks all exceptions: fldcw [m] (0x0240);
# fnstenv [m+0x10]; fnstcw [m+0x30]
T("fnstenv [m32]: the image, then all exceptions masked",
  [0xD9,0x2D]+a32(MW_+0x40) + [0xD9,0x35]+a32(MW_+0x50) + [0xD9,0x3D]+a32(MW_+0x70),
  {}, mem={MW_+0x40:0x0240, MW_+0x5C:0x5A5A5A5A, MW_+0x68:0x5A5A5A5A, MW_+0x70:0xAAAA0000},
  fmem={MW_+0x50:0xFFFF0240, MW_+0x54:0xFFFF0000, MW_+0x58:0xFFFFFFFF, MW_+0x5C:0, MW_+0x68:0, MW_+0x70:0xAAAA027F})
# MW's x87 pow (0x7C474C): fnsave around the special-value helper, then frstor.
# fld dword [a] (2.0); fld dword [b] (3.0); fldcw [cw] (0x007F); fnsave [s];
# fnstcw [s+0x70]; fld1; fstp st(0); frstor [s]; fnstcw [s+0x74]; fstp dword [d];
# fstp dword [d+4].  fnsave leaves CW 0x037F and an empty stack; frstor brings
# back CW, TOP 6 and both values.  The image holds ST(0) = 3.0 and ST(1) = 2.0
# as 80-bit values after the 28-byte environment.  TW is not checked (the
# model's all empty, the hardware's two valid registers), as above.
_S = MW_ + 0x100
T("fnsave/frstor (x87 pow's special-value path)",
  [0xD9,0x05]+a32(MW_+0x80) + [0xD9,0x05]+a32(MW_+0x84) + [0xD9,0x2D]+a32(MW_+0x88) + [0xDD,0x35]+a32(_S)
  + [0xD9,0x3D]+a32(_S+0x70) + [0xD9,0xE8, 0xDD,0xD8] + [0xDD,0x25]+a32(_S) + [0xD9,0x3D]+a32(_S+0x74)
  + [0xD9,0x1D]+a32(MW_+0x90) + [0xD9,0x1D]+a32(MW_+0x94),
  {}, mem={MW_+0x88:0x007F, _S+0x70:0, _S+0x74:0}, memf={MW_+0x80:2.0, MW_+0x84:3.0},
  fmem={_S:0xFFFF007F, _S+4:0xFFFF3000, _S+0x1C:0, _S+0x20:0xC0000000, _S+0x24:0x00004000,
        _S+0x28:0, _S+0x2C:0x40008000, _S+0x70:0x037F, _S+0x74:0x007F},
  fmemf={MW_+0x90:3.0, MW_+0x94:2.0})
# frstor gives back what fnsave stored (cpu.h f80_load_x): a NaN with its sign
# and payload, a double's denormal (f80_load made them the host's NaN and 0).
# fld qword [a] (0x000000000000ABCD); fld qword [b] (0xFFF8123456789ABC);
# fnsave [s]; frstor [s]; fstp qword [d]; fstp qword [d+8]
_S2 = MW_ + 0x300
T("fnsave/frstor: a NaN's payload and a denormal come back",
  [0xDD,0x05]+a32(MW_+0x200) + [0xDD,0x05]+a32(MW_+0x208) + [0xDD,0x35]+a32(_S2) + [0xDD,0x25]+a32(_S2)
  + [0xDD,0x1D]+a32(MW_+0x210) + [0xDD,0x1D]+a32(MW_+0x218),
  {}, mem={MW_+0x200:0x0000ABCD, MW_+0x204:0, MW_+0x208:0x56789ABC, MW_+0x20C:0xFFF81234},
  fmem={MW_+0x210:0x56789ABC, MW_+0x214:0xFFF81234, MW_+0x218:0x0000ABCD, MW_+0x21C:0})
# the same for the smallest denormal (f80_load_x's lowest exponent) and a
# negative one (its sign): fld qword [a] (0x0000000000000001); fld qword [b]
# (0x800000000000ABCD); fnsave [s]; frstor [s]; fstp qword [d]; fstp qword [d+8]
_S3 = MW_ + 0x380
T("fnsave/frstor: the smallest denormal and a negative one come back",
  [0xDD,0x05]+a32(MW_+0x220) + [0xDD,0x05]+a32(MW_+0x228) + [0xDD,0x35]+a32(_S3) + [0xDD,0x25]+a32(_S3)
  + [0xDD,0x1D]+a32(MW_+0x230) + [0xDD,0x1D]+a32(MW_+0x238),
  {}, mem={MW_+0x220:1, MW_+0x224:0, MW_+0x228:0x0000ABCD, MW_+0x22C:0x80000000},
  fmem={MW_+0x230:0x0000ABCD, MW_+0x234:0x80000000, MW_+0x238:1, MW_+0x23C:0})
# The lift's model, not Intel's result: the x87 registers are doubles
# (cpu.h), so an 80-bit value no double holds does not survive fld m80
# (f80_load flushes a denormal to 0).  The C runtime's FDIV workaround
# 0x7D8CC0 (UG2 0x769C90) scales an 80-bit denormal divisor by 2^63, stores
# it and reads its significand's high dword back (0x7D8DB6 fmul, 0x7D8DBC
# fstp, 0x7D8CC8 mov eax):
# fld tbyte [a] (0000:00000001FFFFFFFF); fmul dword [b] (2^63); fstp tbyte
# [c]; mov eax,[c+4].  Intel gives eax 0xFFFFFFFF and the word 0x0021 at
# [c+8] (a value near 2^-16349); the lift 0 and ten zero bytes.  The oracle
# takes exactly this as its read-back class (tests/mworacle.c range_subst);
# MW never reaches 0x7D8CC0 with such an operand (_adjust_fdiv is 0).  A
# change to the model fails this case on purpose: then the oracle's rule and
# this note change with it.
_FX = 0x00CA0000
T("fld/fmul/fstp m80 of a denormal, mov eax (lift: doubles)",
  [0xDB,0x2D]+a32(_FX) + [0xD8,0x0D]+a32(_FX+0x10) + [0xDB,0x3D]+a32(_FX+0x20) + [0xA1]+a32(_FX+0x24),
  {'eax':0x55555555}, mem={_FX:0xFFFFFFFF, _FX+4:0x00000001, _FX+8:0, _FX+0x10:0x5F000000,
                           _FX+0x20:0x5A5A5A5A, _FX+0x24:0x5A5A5A5A, _FX+0x28:0x5A5A5A5A},
  expect={'eax':0}, fmem={_FX+0x20:0, _FX+0x24:0, _FX+0x28:0x5A5A0000})

# ---------------- switch jump tables (JTSWITCH=1 only) ----------------
# Lifted as a C switch whose cases check the table entry in guest memory (the
# tables are written there with mem=); without JTSWITCH the jmp is a tail call
# through the dispatcher, which this harness does not have.
if os.environ.get('JTSWITCH', '') == '1':
    _B = 0x00401000
    # cmp eax,2; ja default; jmp [eax*4+tab]; case0/1/2: mov ebx,10/20/30; default: mov ebx,99; ret; tab
    _JT1 = ([0x3D]+imm32(2)+[0x77,0x1C, 0xFF,0x24,0x85]+imm32(_B+0x29)
            + [0xBB]+imm32(10)+[0xEB,0x13] + [0xBB]+imm32(20)+[0xEB,0x0C] + [0xBB]+imm32(30)+[0xEB,0x05]
            + [0xBB]+imm32(99) + [0xC3] + imm32(_B+0x0E)+imm32(_B+0x15)+imm32(_B+0x1C))
    _JT1M = {_B+0x29:_B+0x0E, _B+0x2D:_B+0x15, _B+0x31:_B+0x1C}
    for _v, _w in ((0, 10), (1, 20), (2, 30), (3, 99), (0xFFFFFFFF, 99)):
        T(f"switch [eax*4] (eax={_v:#x})", _JT1, {'eax':_v, 'ebx':0}, mem=_JT1M, expect={'ebx':_w})
    # MSVC's two-level switch: cmp eax,3; ja default; movzx ecx, byte [eax+idx]; jmp [ecx*4+tab];
    # case0/1: mov ebx,10/20; default: mov ebx,99; ret; idx = 1,0,1,0; tab
    _JT2 = ([0x3D]+imm32(3)+[0x77,0x1C, 0x0F,0xB6,0x88]+imm32(_B+0x29) + [0xFF,0x24,0x8D]+imm32(_B+0x2D)
            + [0xBB]+imm32(10)+[0xEB,0x0C] + [0xBB]+imm32(20)+[0xEB,0x05] + [0xBB]+imm32(99) + [0xC3]
            + [1,0,1,0] + imm32(_B+0x15)+imm32(_B+0x1C))
    _JT2M = {_B+0x29:0x00010001, _B+0x2D:_B+0x15, _B+0x31:_B+0x1C}
    for _v, _w, _x in ((0, 20, 1), (1, 10, 0), (2, 20, 1), (3, 10, 0), (4, 99, None)):
        _e = {'ebx':_w}
        if _x is not None: _e['ecx'] = _x
        T(f"two-level switch (eax={_v})", _JT2, {'eax':_v, 'ebx':0, 'ecx':0x55}, mem=_JT2M, expect=_e)
    # instructions that leave the flags and the index alone between them (sub_00468160's shape):
    # cmp eax,1; push esi; ja default; mov ecx,esi; jmp [eax*4+tab]; case0/1: mov ebx,10/20;
    # default: mov ebx,99; pop esi; ret; tab
    _JT3 = ([0x3D]+imm32(1)+[0x56, 0x77,0x17, 0x8B,0xCE, 0xFF,0x24,0x85]+imm32(_B+0x26)
            + [0xBB]+imm32(10)+[0xEB,0x0C] + [0xBB]+imm32(20)+[0xEB,0x05] + [0xBB]+imm32(99) + [0x5E, 0xC3]
            + imm32(_B+0x11)+imm32(_B+0x18))
    _JT3M = {_B+0x26:_B+0x11, _B+0x2A:_B+0x18}
    for _v, _w, _c in ((0, 10, 0x1234), (1, 20, 0x1234), (2, 99, 0x77)):   # default: ja before the mov
        T(f"switch after push/mov (eax={_v})", _JT3, {'eax':_v, 'ebx':0, 'ecx':0x77, 'esi':0x1234}, mem=_JT3M, expect={'ebx':_w, 'ecx':_c, 'esi':0x1234})

# =====================  GENERATOR / RUNNER  =====================
BASE = 0x00401000
def build():
    fns=[]; body=[]
    for i,t in enumerate(TESTS):
        L = RawLifter(t['code'] + b'\xc3', base=BASE, comments=False)
        L.retpast = {BASE + x for x in t['retpast']}
        keep = {k: os.environ.get(k) for k in t['env']}; os.environ.update(t['env'])
        try:
            code,_ = L.lift(BASE)
            more = [L.lift(BASE + x)[0] for x in t['calls']]
        except Unsupported as e:
            print(f"  !! {t['name']}: could not be lifted ({e})"); fns.append(None); continue
        finally:
            for k, v in keep.items():
                if v is None: os.environ.pop(k, None)
                else: os.environ[k] = v
        names = {f"sub_{BASE:08x}": f"test_{i}", **{f"sub_{BASE + x:08x}": f"test_{i}_{x:x}" for x in t['calls']}}
        for a, b in names.items():
            code = code.replace(a, b); more = [s.replace(a, b) for s in more]
        body += [f"void test_{i}_{x:x}(CPU *c);" for x in t['calls']] + more + [code]
        fns.append(f"test_{i}")
    return fns, body

REGS = ['eax','ecx','edx','ebx','esp','ebp','esi','edi']
FLAGS = ['cf','pf','af','zf','sf','of','df']

def gen_c(fns, body):
    o=[]
    o.append('#include "cpu.h"\n#include <stdio.h>\n#include <string.h>\n#include <math.h>')
    o.append('void mem_commit(uint32_t,uint32_t);')
    o.append('guest_fn_t g_dtab[DISPATCH_TEXT_SIZE];   /* empty: indirect calls go to the stub below */')
    o.append('void dispatch_indirect(CPU*c,uint32_t a){(void)c;(void)a;}')
    o.append('void unsupported_insn(CPU*c,uint32_t a,const char*m){(void)c;(void)a;(void)m;fprintf(stderr,"TRAP 0x%x %s\\n",a,m);}')
    o.append('void guest_hook(CPU*c,uint32_t a){(void)c;(void)a;}')
    o.append('void cpuid_shim(CPU*c){(void)c;} uint64_t rdtsc_shim(void){return 0;}')
    # LREGS=1 (tools/lregs.py): the lifted bodies name the registers in locals; the harness below does not
    lr = os.environ.get('LREGS', '') == '1'
    if lr: o.append('#undef RG_\n#define RG_(x) _r_##x')
    o.append('\n'.join(body))
    if lr: o.append('#undef RG_\n#define RG_(x) c->x')
    o.append('static CPU cpu; static CPU *const c=&cpu;')
    o.append('#define STK 0x00B00000u')
    o.append('static int pass=0, fail=0;')
    o.append('static void chk(const char*t,const char*f,uint32_t got,uint32_t want){')
    o.append('  if(got==want){pass++;} else {fail++;')
    o.append('    printf("  \\033[31mFAIL\\033[0m %-38s %s = 0x%08x (expected 0x%08x)\\n",t,f,got,want);}}')
    o.append('static void chkf(const char*t,const char*f,float got,float want){')
    o.append('  if(memcmp(&got,&want,4)==0){pass++;} else {fail++;')
    o.append('    printf("  \\033[31mFAIL\\033[0m %-38s %s = %.9g (expected %.9g)\\n",t,f,(double)got,(double)want);}}')
    o.append('int main(void){ mem_init(); mem_commit(0x00000000u,0x01000000u); mem_fault_handler_install();')
    for i,t in enumerate(TESTS):
        nm = t['name'].replace('"','\\"')
        if fns[i] is None:   # the lifter refused it (build() said why): a failure, not a skip
            o.append(f'  fail++; printf("  \\033[31mFAIL\\033[0m %-38s could not be lifted\\n", "{nm}");')
            continue
        o.append(f'  /* --- {i}: {nm} --- */')
        o.append('  { memset(&cpu,0,sizeof cpu); cpu.esp=STK; cpu.top=0; cpu.fcw=0x037f;')
        for r,v in t['setup'].items():
            o.append(f'    cpu.{r} = 0x{v & 0xffffffff:x}u;')
        for a,v in t['mem'].items():
            o.append(f'    MR32(0x{a:x}u) = 0x{v & 0xffffffff:x}u;')
        for a,v in t.get('memf',{}).items():
            o.append(f'    MRF32(0x{a:x}u) = {v!r}f;')
        o.append('    uint32_t esp0 = cpu.esp;')
        o.append('    PUSH32(0xDEADC0DEu);')
        o.append(f'    {fns[i]}(&cpu);')
        o.append(f'    chk("{nm}","esp balance",cpu.esp,esp0);')
        for k,v in t['expect'].items():
            if k=='ah_and':
                mask,want = v
                o.append(f'    chk("{nm}","ah&0x{mask:02x}",((cpu.eax>>8)&0x{mask:02x}u),0x{want:02x}u);')
            elif k in REGS:
                o.append(f'    chk("{nm}","{k}",cpu.{k},0x{v & 0xffffffff:x}u);')
            elif k in FLAGS:
                o.append(f'    chk("{nm}","{k}",cpu.{k},{v}u);')
        for a,v in t['fmem'].items():
            o.append(f'    chk("{nm}","mem[0x{a:x}]",MR32(0x{a:x}u),0x{v & 0xffffffff:x}u);')
        for a,v in t.get('fmemf',{}).items():
            o.append(f'    chkf("{nm}","fmem[0x{a:x}]",MRF32(0x{a:x}u),{v!r}f);')
        o.append('  }')
    o.append('  printf("\\n  ISA tests: \\033[32m%d passed\\033[0m, %s%d failed\\033[0m\\n",')
    o.append('         pass, fail?"\\033[31m":"", fail);')
    o.append('  return fail?1:0; }')
    return '\n'.join(o)

if __name__=='__main__':
    fns, body = build()
    src = gen_c(fns, body)
    os.makedirs('gen', exist_ok=True)
    open('gen/isatest.c','w').write(src)
    print(f"gen/isatest.c written ({len(TESTS)} tests)")
