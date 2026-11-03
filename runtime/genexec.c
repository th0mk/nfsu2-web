/* Run x86 code the guest GENERATES AT RUN TIME.
 *
 * The real blind spot of static recompilation: the game writes plain x86
 * to the heap and calls it there; we have no generated C counterpart for
 * those addresses.  NFSU2's material/render state pipeline uses this
 * heavily -- measured: more than 17,000 calls in one session, all through
 * `call [eax+8]` in sub_00725520.  A typical generated block:
 *
 *     push esi                       ; a heap address such as 0x105fdbe4
 *     mov  esi,[esp+8]               ; arg1 = material object
 *     push esi
 *     call sub_0071feef              ; a small, REGISTERED accessor in .text
 *     add  esi,0x14
 *     ...
 *     mov  [esi+0xa0],eax  ...  mov [esi+0x150],eax
 *
 * The call targets are real lifted functions, so we can hand them straight
 * to dispatch_indirect.  Swallowing these calls led to crashes at high
 * detail settings.
 *
 * DESIGN PRINCIPLE: when we see an unknown byte we do NOT SILENTLY GO ON.
 * We stop, restore esp and report -- better visible than running wrong.
 * Likewise every memory access goes through the commit check; instead of
 * crashing the host on a bad pointer we bail.                           */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef NFSU2_HOST_PLATFORM
extern uint8_t *g_mem;
#endif
void dispatch_indirect(CPU *c, uint32_t addr);
int  mem_is_committed(uint32_t a);

uint64_t g_gen_calls = 0, g_gen_insns = 0, g_gen_bail = 0;
static uint32_t g_gen_bop = 0, g_gen_bpc = 0;
static uint32_t g_gen_bail_ops[16]; static int g_gen_bail_n = 0;

#define BAIL(pc_,op_) do{ g_gen_bail++; g_gen_bop=(op_); g_gen_bpc=(pc_); \
        { int _d=0; for(int _q=0;_q<g_gen_bail_n;_q++) if(g_gen_bail_ops[_q]==(op_)) _d=1; \
          if(!_d && g_gen_bail_n<16) g_gen_bail_ops[g_gen_bail_n++]=(op_); } \
        goto bail; }while(0)

/* commit-checked access; bail on failure */
#define CHK(a_,n_) do{ if(!mem_is_committed(a_) || !mem_is_committed((a_)+(n_)-1)) \
                         BAIL(pc, 0xFFFFFFFEu); }while(0)

typedef struct { int is_reg; int reg; uint32_t ea; int len; } Opnd;

/* Decode ModRM (+SIB +disp).  len = bytes consumed from the modrm on.    */
static Opnd modrm(CPU *c, uint32_t p, int *rf){
    Opnd o = {0,0,0,0};
    uint8_t m = MR8(p);
    int mod = m >> 6, rm = m & 7;
    *rf = (m >> 3) & 7;
    int len = 1; uint32_t base = 0;
    if (mod == 3){ o.is_reg = 1; o.reg = rm; o.len = 1; return o; }
    if (rm == 4){                                   /* SIB */
        uint8_t sib = MR8(p + 1); len++;
        int sc = sib >> 6, ix = (sib >> 3) & 7, bs = sib & 7;
        if (bs == 5 && mod == 0){ base = MR32(p + len); len += 4; }
        else base = c->r[bs];
        if (ix != 4) base += c->r[ix] << sc;
    } else if (rm == 5 && mod == 0){ base = MR32(p + 1); len += 4; }
    else base = c->r[rm];
    if      (mod == 1){ base += (uint32_t)(int32_t)(int8_t)MR8(p + len); len += 1; }
    else if (mod == 2){ base += MR32(p + len); len += 4; }
    o.ea = base; o.len = len; return o;
}

int run_generated(CPU *c, uint32_t entry){
    uint32_t pc = entry, esp0 = c->esp;
    g_gen_calls++;
    for (int budget = 0; budget < 200000; budget++){
        if (!mem_is_committed(pc)) BAIL(pc, 0xFFFFFFFFu);
        uint8_t op = MR8(pc);
        int osz16 = 0;
        if (op == 0x66){ osz16 = 1; pc++; op = MR8(pc); }   /* operand size 16 */
        g_gen_insns++;
        int rf; Opnd o;
        uint32_t a, b, r;

        switch (op){
        /* ---- stack ---- */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            PUSH32(c->r[op-0x50]); pc++; continue;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            POP32(c->r[op-0x58]); pc++; continue;
        case 0x68: PUSH32(MR32(pc+1)); pc += 5; continue;                    /* push imm32 */
        case 0x6A: PUSH32((uint32_t)(int32_t)(int8_t)MR8(pc+1)); pc += 2; continue;
        case 0x9C: PUSH32(0x202u | (c->cf) | (c->pf<<2) | (c->zf<<6)
                          | (c->sf<<7) | (c->of<<11)); pc++; continue;       /* pushfd */
        case 0x9D: { uint32_t f; POP32(f);
                     c->cf=f&1; c->pf=(f>>2)&1; c->zf=(f>>6)&1;
                     c->sf=(f>>7)&1; c->of=(f>>11)&1; pc++; continue; }

        /* ---- mov ---- */
        case 0x8B: o = modrm(c,pc+1,&rf);
            if (o.is_reg) c->r[rf] = c->r[o.reg];
            else { CHK(o.ea,4); if (osz16) c->r[rf]=(c->r[rf]&0xFFFF0000u)|MR16(o.ea);
                   else c->r[rf]=MR32(o.ea); }
            pc += 1+o.len; continue;
        case 0x89: o = modrm(c,pc+1,&rf);
            if (o.is_reg){ if (osz16) c->r[o.reg]=(c->r[o.reg]&0xFFFF0000u)|(c->r[rf]&0xFFFFu);
                           else c->r[o.reg]=c->r[rf]; }
            else { CHK(o.ea,4); if (osz16) MR16(o.ea)=(uint16_t)c->r[rf]; else MR32(o.ea)=c->r[rf]; }
            pc += 1+o.len; continue;
        case 0x8A: o = modrm(c,pc+1,&rf);            /* mov r8, r/m8 */
            a = o.is_reg ? ((o.reg<4)?(c->r[o.reg]&0xFFu):((c->r[o.reg-4]>>8)&0xFFu))
                         : ({ CHK(o.ea,1); (uint32_t)MR8(o.ea); });
            if (rf<4) c->r[rf]=(c->r[rf]&~0xFFu)|a; else c->r[rf-4]=(c->r[rf-4]&~0xFF00u)|(a<<8);
            pc += 1+o.len; continue;
        case 0x88: o = modrm(c,pc+1,&rf);            /* mov r/m8, r8 */
            a = (rf<4)?(c->r[rf]&0xFFu):((c->r[rf-4]>>8)&0xFFu);
            if (o.is_reg){ if (o.reg<4) c->r[o.reg]=(c->r[o.reg]&~0xFFu)|a;
                           else c->r[o.reg-4]=(c->r[o.reg-4]&~0xFF00u)|(a<<8); }
            else { CHK(o.ea,1); MR8(o.ea)=(uint8_t)a; }
            pc += 1+o.len; continue;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {                        /* mov r8, imm8 */
            int k = op-0xB0; a = MR8(pc+1);
            if (k<4) c->r[k]=(c->r[k]&~0xFFu)|a; else c->r[k-4]=(c->r[k-4]&~0xFF00u)|(a<<8);
            pc += 2; continue; }
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:                          /* mov r32, imm32 */
            if (osz16){ c->r[op-0xB8]=(c->r[op-0xB8]&0xFFFF0000u)|MR16(pc+1); pc += 3; }
            else { c->r[op-0xB8] = MR32(pc+1); pc += 5; }
            continue;
        case 0xC6: o = modrm(c,pc+1,&rf); if (rf) BAIL(pc,op);
            a = MR8(pc+1+o.len);
            if (o.is_reg){ if (o.reg<4) c->r[o.reg]=(c->r[o.reg]&~0xFFu)|a;
                           else c->r[o.reg-4]=(c->r[o.reg-4]&~0xFF00u)|(a<<8); }
            else { CHK(o.ea,1); MR8(o.ea)=(uint8_t)a; }
            pc += 1+o.len+1; continue;
        case 0xC7: o = modrm(c,pc+1,&rf); if (rf) BAIL(pc,op);
            if (osz16){ a = MR16(pc+1+o.len);
                if (o.is_reg) c->r[o.reg]=(c->r[o.reg]&0xFFFF0000u)|a;
                else { CHK(o.ea,2); MR16(o.ea)=(uint16_t)a; }
                pc += 1+o.len+2; }
            else { a = MR32(pc+1+o.len);
                if (o.is_reg) c->r[o.reg]=a; else { CHK(o.ea,4); MR32(o.ea)=a; }
                pc += 1+o.len+4; }
            continue;
        case 0xA1: a = MR32(pc+1); CHK(a,4); c->eax = MR32(a); pc += 5; continue;
        case 0xA3: a = MR32(pc+1); CHK(a,4); MR32(a) = c->eax; pc += 5; continue;
        case 0x8D: o = modrm(c,pc+1,&rf); if (o.is_reg) BAIL(pc,op);
            c->r[rf] = o.ea; pc += 1+o.len; continue;

        /* ---- ALU: r/m32,r32 (op) and r32,r/m32 (op+2) ---- */
        case 0x01: case 0x09: case 0x11: case 0x19:
        case 0x21: case 0x29: case 0x31: case 0x39:
        case 0x03: case 0x0B: case 0x13: case 0x1B:
        case 0x23: case 0x2B: case 0x33: case 0x3B: {
            int dir = (op & 2) != 0;                 /* 1: dest register */
            int fn  = (op >> 3) & 7;
            o = modrm(c,pc+1,&rf);
            uint32_t rm = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
            a = dir ? c->r[rf] : rm;
            b = dir ? rm       : c->r[rf];
            switch (fn){
            case 0: r=a+b; FLAGS_ADD(a,b,r,32); break;
            case 1: r=a|b; FLAGS_LOG(r,32); break;
            case 2: { uint32_t ci=c->cf; r=a+b+ci; FLAGS_ADC(a,b,r,ci,32); } break;
            case 3: { uint32_t ci=c->cf; r=a-b-ci; FLAGS_SBB(a,b,r,ci,32); } break;
            case 4: r=a&b; FLAGS_LOG(r,32); break;
            case 5: r=a-b; FLAGS_SUB(a,b,r,32); break;
            case 6: r=a^b; FLAGS_LOG(r,32); break;
            default: r=a-b; FLAGS_SUB(a,b,r,32); pc += 1+o.len; continue;  /* cmp: no write */
            }
            if (dir) c->r[rf]=r; else if (o.is_reg) c->r[o.reg]=r; else MR32(o.ea)=r;
            pc += 1+o.len; continue; }

        /* ---- group1: 0x83 sign-extended imm8, 0x81 imm32 ---- */
        case 0x81: case 0x83: {
            o = modrm(c,pc+1,&rf);
            int imm8 = (op == 0x83);
            b = imm8 ? (uint32_t)(int32_t)(int8_t)MR8(pc+1+o.len) : MR32(pc+1+o.len);
            a = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
            switch (rf){
            case 0: r=a+b; FLAGS_ADD(a,b,r,32); break;
            case 1: r=a|b; FLAGS_LOG(r,32); break;
            case 2: { uint32_t ci=c->cf; r=a+b+ci; FLAGS_ADC(a,b,r,ci,32); } break;
            case 3: { uint32_t ci=c->cf; r=a-b-ci; FLAGS_SBB(a,b,r,ci,32); } break;
            case 4: r=a&b; FLAGS_LOG(r,32); break;
            case 5: r=a-b; FLAGS_SUB(a,b,r,32); break;
            case 6: r=a^b; FLAGS_LOG(r,32); break;
            default: r=a-b; FLAGS_SUB(a,b,r,32);                        /* cmp */
                     pc += 1+o.len+(imm8?1:4); continue;
            }
            if (o.is_reg) c->r[o.reg]=r; else MR32(o.ea)=r;
            pc += 1+o.len+(imm8?1:4); continue; }

        /* ---- test / inc / dec ---- */
        case 0x85: o = modrm(c,pc+1,&rf);
            a = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
            r = a & c->r[rf]; FLAGS_LOG(r,32); pc += 1+o.len; continue;
        case 0xA9: r = c->eax & MR32(pc+1); FLAGS_LOG(r,32); pc += 5; continue;
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            { uint32_t cf0=c->cf; a=c->r[op-0x40]; r=a+1; FLAGS_ADD(a,1u,r,32);
              c->cf=cf0; c->r[op-0x40]=r; } pc++; continue;
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            { uint32_t cf0=c->cf; a=c->r[op-0x48]; r=a-1; FLAGS_SUB(a,1u,r,32);
              c->cf=cf0; c->r[op-0x48]=r; } pc++; continue;

        /* ---- shifts ---- */
        case 0xC1: case 0xD1: case 0xD3: {
            o = modrm(c,pc+1,&rf);
            int extra = (op==0xC1) ? 1 : 0;
            uint32_t sh = (op==0xC1) ? (MR8(pc+1+o.len)&31u)
                        : (op==0xD1) ? 1u : (c->ecx & 31u);
            a = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
            switch (rf){
            case 4: case 6: r = sh ? (a << sh) : a; if(sh) c->cf=(a>>(32-sh))&1; break; /* shl */
            case 5: r = sh ? (a >> sh) : a;         if(sh) c->cf=(a>>(sh-1))&1; break;  /* shr */
            case 7: r = sh ? (uint32_t)((int32_t)a >> sh) : a;
                    if(sh) c->cf=((int32_t)a>>(sh-1))&1; break;                          /* sar */
            default: BAIL(pc,op);
            }
            if (sh){ c->zf=(r==0); c->sf=(r>>31)&1; c->pf=parity8(r); }
            if (o.is_reg) c->r[o.reg]=r; else MR32(o.ea)=r;
            pc += 1+o.len+extra; continue; }

        /* ---- group3 (0xF7) ---- */
        case 0xF7: {
            o = modrm(c,pc+1,&rf);
            a = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
            switch (rf){
            case 0: r = a & MR32(pc+1+o.len); FLAGS_LOG(r,32);
                    pc += 1+o.len+4; continue;                                   /* test */
            case 2: r = ~a; break;                                               /* not  */
            case 3: r = 0u-a; FLAGS_SUB(0u,a,r,32); break;                       /* neg  */
            case 4: { uint64_t p=(uint64_t)c->eax*(uint64_t)a;                   /* mul  */
                      c->eax=(uint32_t)p; c->edx=(uint32_t)(p>>32);
                      c->cf=c->of=(c->edx!=0); pc += 1+o.len; continue; }
            case 5: { int64_t p=(int64_t)(int32_t)c->eax*(int64_t)(int32_t)a;    /* imul */
                      c->eax=(uint32_t)p; c->edx=(uint32_t)((uint64_t)p>>32);
                      c->cf=c->of=((int64_t)(int32_t)c->eax!=p); pc += 1+o.len; continue; }
            case 6: { if(!a) BAIL(pc,op);                                        /* div  */
                      uint64_t n=((uint64_t)c->edx<<32)|c->eax;
                      c->eax=(uint32_t)(n/a); c->edx=(uint32_t)(n%a);
                      pc += 1+o.len; continue; }
            case 7: { if(!a) BAIL(pc,op);                                        /* idiv */
                      int64_t n=(int64_t)(((uint64_t)c->edx<<32)|c->eax);
                      c->eax=(uint32_t)(n/(int32_t)a); c->edx=(uint32_t)(n%(int32_t)a);
                      pc += 1+o.len; continue; }
            default: BAIL(pc,op);
            }
            if (o.is_reg) c->r[o.reg]=r; else MR32(o.ea)=r;
            pc += 1+o.len; continue; }

        /* ---- imul imm ---- */
        case 0x69: case 0x6B: {
            o = modrm(c,pc+1,&rf);
            a = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
            b = (op==0x6B) ? (uint32_t)(int32_t)(int8_t)MR8(pc+1+o.len) : MR32(pc+1+o.len);
            int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
            c->r[rf] = (uint32_t)p; c->cf = c->of = ((int64_t)(int32_t)(uint32_t)p != p);
            pc += 1+o.len+((op==0x6B)?1:4); continue; }

        /* ---- conditional jumps ---- */
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int t; switch (op & 0xF){
            case 0x0:t=CC_O;break;  case 0x1:t=CC_NO;break; case 0x2:t=CC_B;break;  case 0x3:t=CC_AE;break;
            case 0x4:t=CC_E;break;  case 0x5:t=CC_NE;break; case 0x6:t=CC_BE;break; case 0x7:t=CC_A;break;
            case 0x8:t=CC_S;break;  case 0x9:t=CC_NS;break; case 0xA:t=CC_P;break;  case 0xB:t=CC_NP;break;
            case 0xC:t=CC_L;break;  case 0xD:t=CC_GE;break; case 0xE:t=CC_LE;break; default:t=CC_G;break; }
            pc += 2; if (t) pc += (uint32_t)(int32_t)(int8_t)MR8(pc-1);
            continue; }
        case 0xE9: pc += 5 + MR32(pc+1); continue;
        case 0xEB: pc += 2 + (uint32_t)(int32_t)(int8_t)MR8(pc+1); continue;

        /* ---- 0x0F two-byte ---- */
        case 0x0F: {
            uint8_t o2 = MR8(pc+1);
            if (o2 >= 0x80 && o2 <= 0x8F){                       /* jcc rel32 */
                int t; switch (o2 & 0xF){
                case 0x0:t=CC_O;break;  case 0x1:t=CC_NO;break; case 0x2:t=CC_B;break;  case 0x3:t=CC_AE;break;
                case 0x4:t=CC_E;break;  case 0x5:t=CC_NE;break; case 0x6:t=CC_BE;break; case 0x7:t=CC_A;break;
                case 0x8:t=CC_S;break;  case 0x9:t=CC_NS;break; case 0xA:t=CC_P;break;  case 0xB:t=CC_NP;break;
                case 0xC:t=CC_L;break;  case 0xD:t=CC_GE;break; case 0xE:t=CC_LE;break; default:t=CC_G;break; }
                uint32_t rel = MR32(pc+2); pc += 6; if (t) pc += rel; continue; }
            if (o2 >= 0x90 && o2 <= 0x9F){                       /* setcc r/m8 */
                int t; switch (o2 & 0xF){
                case 0x0:t=CC_O;break;  case 0x1:t=CC_NO;break; case 0x2:t=CC_B;break;  case 0x3:t=CC_AE;break;
                case 0x4:t=CC_E;break;  case 0x5:t=CC_NE;break; case 0x6:t=CC_BE;break; case 0x7:t=CC_A;break;
                case 0x8:t=CC_S;break;  case 0x9:t=CC_NS;break; case 0xA:t=CC_P;break;  case 0xB:t=CC_NP;break;
                case 0xC:t=CC_L;break;  case 0xD:t=CC_GE;break; case 0xE:t=CC_LE;break; default:t=CC_G;break; }
                o = modrm(c,pc+2,&rf);
                if (o.is_reg){ if (o.reg<4) c->r[o.reg]=(c->r[o.reg]&~0xFFu)|(uint32_t)t;
                               else c->r[o.reg-4]=(c->r[o.reg-4]&~0xFF00u)|((uint32_t)t<<8); }
                else { CHK(o.ea,1); MR8(o.ea)=(uint8_t)t; }
                pc += 2+o.len; continue; }
            if (o2 == 0xB6 || o2 == 0xB7 || o2 == 0xBE || o2 == 0xBF){   /* movzx/movsx */
                int wide = (o2 & 1), sx = (o2 >= 0xBE);
                o = modrm(c,pc+2,&rf);
                if (o.is_reg) a = wide ? (c->r[o.reg]&0xFFFFu)
                                       : ((o.reg<4)?(c->r[o.reg]&0xFFu):((c->r[o.reg-4]>>8)&0xFFu));
                else { CHK(o.ea, wide?2:1); a = wide ? MR16(o.ea) : MR8(o.ea); }
                c->r[rf] = sx ? (wide ? (uint32_t)(int32_t)(int16_t)a
                                      : (uint32_t)(int32_t)(int8_t)a) : a;
                pc += 2+o.len; continue; }
            if (o2 == 0xAF){                                     /* imul r32, r/m32 */
                o = modrm(c,pc+2,&rf);
                a = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
                int64_t p = (int64_t)(int32_t)c->r[rf] * (int64_t)(int32_t)a;
                c->r[rf] = (uint32_t)p; c->cf = c->of = ((int64_t)(int32_t)(uint32_t)p != p);
                pc += 2+o.len; continue; }
            if (o2 == 0x1F){ o = modrm(c,pc+2,&rf); pc += 2+o.len; continue; }   /* nop */
            BAIL(pc, 0x0F00u | o2); }

        /* ---- group5 (0xFF) ---- */
        case 0xFF: {
            o = modrm(c,pc+1,&rf);
            switch (rf){
            case 0: case 1: {                                    /* inc/dec r/m32 */
                a = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
                uint32_t cf0=c->cf;
                if (rf==0){ r=a+1; FLAGS_ADD(a,1u,r,32); } else { r=a-1; FLAGS_SUB(a,1u,r,32); }
                c->cf=cf0;
                if (o.is_reg) c->r[o.reg]=r; else MR32(o.ea)=r;
                pc += 1+o.len; continue; }
            case 2: {                                            /* call r/m32 */
                uint32_t t = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
                uint32_t next = pc+1+o.len;
                PUSH32(next); dispatch_indirect(c, t); pc = next; continue; }   /* see below */
            case 4: {                                            /* jmp r/m32 */
                uint32_t t = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
                pc = t; continue; }
            case 6: {                                            /* push r/m32 */
                a = o.is_reg ? c->r[o.reg] : ({ CHK(o.ea,4); MR32(o.ea); });
                PUSH32(a); pc += 1+o.len; continue; }
            default: BAIL(pc,op);
            } }
        case 0x8F: o = modrm(c,pc+1,&rf); if (rf) BAIL(pc,op);   /* pop r/m32 */
            { uint32_t v; POP32(v);
              if (o.is_reg) c->r[o.reg]=v; else { CHK(o.ea,4); MR32(o.ea)=v; } }
            pc += 1+o.len; continue;

        /* ---- call / return ---- */
        case 0xE8: {
            uint32_t next = pc + 5, t = next + MR32(pc+1);
            PUSH32(next);
            dispatch_indirect(c, t);
            /* return point = the pushed address (NOT c->eip): with RETEIP=0 a
             * lifted ret does not write c->eip; the model already ignores modified
             * return addresses, and a lifted caller always continues at 'next'. */
            pc = next; continue; }
        case 0xC3: POP32(c->eip); return 1;
        case 0xC2: { uint16_t n = MR16(pc+1); POP32(c->eip); c->esp += n; return 1; }
        case 0x90: pc++; continue;

        default: BAIL(pc,op);
        }
    }
    BAIL(pc, 0xFFFFFFFDu);                 /* budget exceeded */
bail:
    c->esp = esp0;                         /* the caller's return address is on top again */
    return 0;
}

void genexec_report(void){
    if (!g_gen_calls) return;
    fprintf(stderr, "  generated guest code: %llu calls, %llu instructions",
            (unsigned long long)g_gen_calls, (unsigned long long)g_gen_insns);
    if (g_gen_bail){
        fprintf(stderr, ", %llu FAILED (last: 0x%02x @0x%08x; seen:",
                (unsigned long long)g_gen_bail, g_gen_bop, g_gen_bpc);
        for (int i = 0; i < g_gen_bail_n; i++) fprintf(stderr, " %02x", g_gen_bail_ops[i]);
        fprintf(stderr, ")");
    }
    fprintf(stderr, "\n");
}
