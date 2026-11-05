/* Original camera glyph for native map icons. Shared trap positions are compiled into the client and hub. */
#define TRAP(id, ax, ay, az, bx, by, bz) {{ax, ay, az}, {bx, by, bz}},
static const float trap_edges[][2][3] = {
#include "../web/server-rs/crates/roam/src/speedtraps.def"
};
#undef TRAP
#define TRAP_CAP 64
static int trap_count(void){int n=__atomic_load_n(&g_dev_trap_n,__ATOMIC_ACQUIRE);return n>=0?n:(int)(sizeof trap_edges/sizeof trap_edges[0]);}
#define TRAP_COUNT trap_count()
static float trap_coord(int i, int axis){ if(__atomic_load_n(&g_dev_trap_n,__ATOMIC_ACQUIRE)>=0 && axis<2)return (float)__atomic_load_n(&g_dev_trap_xy[i][axis],__ATOMIC_RELAXED)/100.f; return (trap_edges[i][0][axis] + trap_edges[i][1][axis]) * 0.5f; }
static uint32_t trap_texture(CPU *c){
    if (g_trap_ti) return g_trap_hash;
    uint32_t a[3] = {0xe2a5c626u, 0, 0}, src = rh_call(c, FN_TEXINFO, 0, 0, 3, a);
    if (!ok(src, TI_SZ) || !ok(rd32(src), PLAT_SZ) || MR8(src + 0x4au) != 0x20u) return 0;
    uint32_t bytes = 32u*32u*4u, sz = TI_SZ+PLAT_SZ+bytes+64u;
    uint32_t ti = rh_call(c, FN_MALLOC, 0, 0, 1, &sz), pl = ti+TI_SZ;
    if (!ok(ti,sz)) return 0;
    uint32_t pix = (pl+PLAT_SZ+63u)&~63u;
    for (int y=0;y<32;y++) for (int x=0;x<32;x++){
        int body=x>=6&&x<=25&&y>=11&&y<=24;
        int top=x>=9&&x<=17&&y>=7&&y<=11;
        int lens=(x-16)*(x-16)+(y-17)*(y-17);
        int ink=(body||top)&&(!body||lens>20||lens<6);
        uint32_t q=pix+4u*(uint32_t)(y*32+x);
        MR8(q)=ink?110:12; MR8(q+1)=ink?255:23; MR8(q+2)=ink?190:16; MR8(q+3)=ink?255:0;
    }
    memcpy(GPTR(ti),GPTR(src),TI_SZ); memcpy(GPTR(pl),GPTR(rd32(src)),PLAT_SZ);
    MR32(pl)=0; MR32(pl+4)=0; MR32(pl+0x18u)=0;
    g_trap_hash=bhash("SPEEDTRAP_CAMERA");
    MR32(ti)=pl; MR32(ti+0x24u)=g_trap_hash; MR32(ti+0x70u)=pix;
    MR32(ti+0x40u)=bytes; MR16(ti+0x44u)=32; MR16(ti+0x46u)=32;
    MR8(ti+0x48u)=5; MR8(ti+0x49u)=5; MR8(ti+0x4eu)=1;
    if (MR32(ti+0x6cu)==src) MR32(ti+0x6cu)=ti;
    gcall0(c,FN_TEX_CREATE,ti);
    if (!rd32(pl+0x18u)) return 0;
    g_trap_ti=ti;
    return g_trap_hash;
}
/* Camera glyphs have no direction. Clear the cloned arrow's quaternion. */
static void trap_upright(uint32_t o){
    uint32_t d=ok(o,0x34)?rd32(o+0x2cu):0;
    if(!ok(d,0x38)) return;
    MRF32(d+0x28u)=0.f; MRF32(d+0x2cu)=0.f; MRF32(d+0x30u)=0.f; MRF32(d+0x34u)=1.f;
    MR32(o+0x1cu)|=0x400000u;
}
static uint32_t trap_image(CPU *c,uint32_t pkg,const char *name){
    uint32_t tex=trap_texture(c), sa=g_gmem+TAG_SCR+0x48u;
    if (!tex) return 0;
    gstr(sa,"AIRACERINDICATOR_0",24);
    uint32_t a[2]={pkg,gcall1(c,FN_FE_HASH,0,sa)}, base=rh_call(c,FN_FE_FINDOBJ,0,0,2,a);
    if (!ok(base,0x5c)) return 0;
    uint32_t o=fe_clone(c,pkg,base,name), d=o?rd32(o+0x2cu):0;
    if (!ok(d,0x54)) return 0;
    uint32_t args[2]={o,tex}; rh_call(c,FN_FE_SETTEX,0,0,2,args);
    MRF32(d+0x44u)=0; MRF32(d+0x48u)=0; MRF32(d+0x4cu)=1; MRF32(d+0x50u)=1;
    fe_colour_set(c,o,255,255,255,255); MR32(o+0x1cu)|=0x400000u;
    trap_upright(o);
    gcall1(c,FN_FE_SHOW,0,o);
    return o;
}
static void trap_map_open(CPU *c,uint32_t map,uint32_t pkg,uint32_t pk);
