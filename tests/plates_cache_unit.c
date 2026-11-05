/* Exercise the real cache with a failing guest texture allocator. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plate_pixels.h"
typedef struct { int unused; } CPU;
static union { uint64_t align; uint8_t bytes[16 * 1024 * 1024]; } memory;
#define GPTR(a) (memory.bytes+(a))
#define MR32(a) (*(uint32_t *)GPTR(a))
#define MR16(a) (*(uint16_t *)GPTR(a))
#define MR8(a) (*GPTR(a))
#define TI_SZ 0x7c
#define PLAT_SZ 0x20
#define FN_TEXINFO 1
#define FN_MALLOC 2
#define FN_TEX_CREATE 3
#define G_FLOW 4
#define G_PLAYER1 8
#define NSLOT 1
static struct { uint32_t car; char plate[9]; } g_sl[NSLOT];
static uint32_t heap=4096;
static long frame=100;
static int fail_create, allocations, creates, uploads;
static long rh_frame(void){return frame;}
static int ok(uint32_t a,unsigned n){return a && a <= sizeof memory.bytes-n;}
static uint32_t rd32(uint32_t a){return MR32(a);}
static uint32_t coop_player(void){return 0;}
static uint32_t rh_call(CPU *c,uint32_t fn,uint32_t a,uint32_t b,int n,const uint32_t *args){
    (void)c;(void)a;(void)b;(void)n;
    if(fn==FN_TEXINFO) return 256;
    assert(fn==FN_MALLOC); allocations++;
    uint32_t p=heap; heap=(heap+*args+63)&~63u; assert(heap<sizeof memory.bytes); return p;
}
static void gcall0(CPU *c,uint32_t fn,uint32_t ti){
    (void)c;assert(fn==FN_TEX_CREATE);creates++;
    if(fail_create) return;
    uint32_t tex=heap;heap+=64;
    MR32(rd32(ti)+0x18)=tex;MR32(tex+28)=creates;
}
void metal_upload_texture(unsigned id,unsigned level,const void *data,unsigned pitch,unsigned w,unsigned h,unsigned fmt){
    (void)level;(void)data;(void)pitch;(void)w;(void)h;assert(id && fmt==21);uploads++;
}
#include "../runtime/plates.inc"
int main(void){
    CPU c={0}; MR32(256)=512;
    fail_create=1;
    assert(!plate_texture(&c,"FIRST"));
    uint32_t retained=g_plates[0].ti;
    assert(retained && allocations==1 && creates==1);
    fail_create=0;
    assert(plate_texture(&c,"SECOND")); /* failed slot must not starve the others */
    assert(allocations==2 && creates==2);
    assert(!plate_texture(&c,"THIRD")); /* global per-frame upload limit */
    frame+=60;
    assert(plate_texture(&c,"FIRST")==retained);
    assert(allocations==2 && creates==3); /* retry uses the original allocation */
    assert(plate_texture(&c,"FIRST")==retained);
    assert(creates==3 && uploads==0); /* cache hits do no GPU work */
    frame+=3;
    /* Make every slot busy except the recovered texture: force its normal reuse. */
    for(int k=1;k<PLATE_CACHE;k++)g_plates[k].retry_after=frame+60;
    assert(plate_texture(&c,"REUSED")==retained);
    assert(allocations==2 && creates==3 && uploads==PLATE_MIPS);
    puts("plates cache: failure isolation, bounded retries, allocation reuse, upload budget and cache hits passed");
}
