/* Cached, text-only per-car plates. The car draw scopes a texture override;
 * the model hook swaps only LICENSE_PLATE while queuing that model's parts.
 * The game retains the texture in its deferred draw bucket, so shared car
 * models can have different plates without changing their replacement tables. */
#ifndef ROAM_HOST_WIN
static char g_plate_self[9];
static int g_plate_init;
static const char *plate_self(void){
    if (!g_plate_init){
        g_plate_init=1;
        const char *s=getenv("NFSU2_LICENSE_PLATE");
        size_t n=s?strlen(s):0;
        if (plate_valid(s,n)) { if(n) memcpy(g_plate_self,s,n); g_plate_self[n]=0; }
    }
#ifdef __wasm__
    uint32_t roam_plat_plate(char out[9]);
    char s[9];
    if (roam_plat_plate(s) && plate_valid(s,strnlen(s,9))) memcpy(g_plate_self,s,9);
#endif
    return g_plate_self;
}
#define PLATE_CACHE 128
static struct { char text[9]; uint32_t ti; long used, retry_after; } g_plates[PLATE_CACHE];
static long g_plate_frame=-1;
static unsigned g_plate_uploads;
void metal_upload_texture(unsigned,unsigned,const void *,unsigned,unsigned,unsigned,unsigned);
static uint32_t plate_texture(CPU *c, const char *text){
    long frame=rh_frame();
    int slot=-1;
    for(int k=0;k<PLATE_CACHE;k++){
        if(g_plates[k].ti && !strcmp(g_plates[k].text,text)){g_plates[k].used=frame;return g_plates[k].ti;}
        if(frame >= g_plates[k].retry_after && (slot<0 || g_plates[k].used<g_plates[slot].used)) slot=k;
    }
    if(g_plate_frame!=frame){g_plate_frame=frame;g_plate_uploads=0;}
    if(slot<0 || g_plate_uploads>=2 || (g_plates[slot].ti && frame-g_plates[slot].used<3)) return 0;
    g_plate_uploads++;
    uint32_t ti=g_plates[slot].ti;
    if(!ti){
        uint32_t args[3]={0x029a22f7u,0,0},src=rh_call(c,FN_TEXINFO,0,0,3,args);
        if(!ok(src,TI_SZ) || !ok(rd32(src),PLAT_SZ)) return 0;
        uint32_t size=TI_SZ+PLAT_SZ+PLATE_BYTES+64;
        ti=rh_call(c,FN_MALLOC,0,0,1,&size);
        if(!ti) return 0;
        uint32_t pl=ti+TI_SZ,pix=(pl+PLAT_SZ+63)&~63u;
        memcpy(GPTR(ti),GPTR(src),TI_SZ);memcpy(GPTR(pl),GPTR(rd32(src)),PLAT_SZ);
        MR32(pl)=MR32(pl+4)=MR32(pl+0x18)=MR32(pl+0x1c)=0;
        /* +0x1c is the current deferred draw bucket, not a material property. */
        /* Preserve the original plate material properties, but upload our BGRA pixels. */
        MR32(pl+0x14)=21;
        MR32(ti)=pl;MR32(ti+0x70)=pix;MR32(ti+0x6c)=ti;
        MR32(ti+0x24)=0x504c0000u+(uint32_t)slot;
        MR32(ti+0x38)=PLATE_BYTES; MR32(ti+0x3c)=0; MR32(ti+0x74)=0; MR8(ti+0x4a)=0x20;
        MR32(ti+0x40)=PLATE_W*PLATE_H*4;MR16(ti+0x44)=PLATE_W;MR16(ti+0x46)=PLATE_H;
        MR8(ti+0x48)=8;MR8(ti+0x49)=6;MR8(ti+0x4e)=PLATE_MIPS;
        g_plates[slot].ti=ti; /* reuse this allocation if GPU creation needs a retry */
    }
    uint32_t pix=rd32(ti+0x70),tex=rd32(rd32(ti)+0x18);
    plate_pixels(GPTR(pix),text,strlen(text));
    if(!tex){
        gcall0(c,FN_TEX_CREATE,ti);
    } else {
        unsigned mid=MR32(tex+28),offset=0,w=PLATE_W,h=PLATE_H;
        for(unsigned m=0;m<PLATE_MIPS;m++){
            metal_upload_texture(mid,m,GPTR(pix+offset),w*4,PLATE_W,PLATE_H,21);
            offset+=w*h*4;w>>=1;h>>=1;
        }
    }
    if(!rd32(rd32(ti)+0x18)) {
        /* Back off this slot without blocking other plates or reallocating its pixels. */
        g_plates[slot].retry_after=frame+60;
        return 0;
    }
    g_plates[slot].retry_after=0;
    snprintf(g_plates[slot].text,9,"%s",text);g_plates[slot].used=frame;
    return ti;
}
static uint32_t g_plate_drawing;
uint32_t roam_game_plate_begin(CPU *c, uint32_t ri){
    uint32_t previous = g_plate_drawing; g_plate_drawing = 0;
    if (!ok(ri, 0x13e8)) return previous;
    const char *text = 0;
    uint32_t car = rd32(ri);
    if (MR32(G_FLOW) == 3) text = plate_self();
    else {
        uint32_t p = rd32(G_PLAYER1), p2 = coop_player();
        if ((p && car == rd32(p + 4)) || (p2 && car == rd32(p2 + 4))) text = plate_self();
        else for (int k = 0; k < NSLOT; k++) if (g_sl[k].car && g_sl[k].car == car){ text = g_sl[k].plate; break; }
    }
    g_plate_drawing = text && text[0] && plate_valid(text, strnlen(text, 9)) ? plate_texture(c, text) : 0;
    return previous;
}
void roam_game_plate_end(uint32_t previous){ g_plate_drawing = previous; }
void roam_game_plate_model(uint32_t item, uint32_t keep[2]){
    keep[0] = 0;
    if (!g_plate_drawing || !ok(item, 0x18)) return;
    uint32_t solid = rd32(item + 0xc);
    if (!ok(solid, 0x30)) return;
    unsigned n = MR8(solid + 0x19), textures = rd32(solid + 0x2c);
    if (n > 127 || !ok(textures, n * 8)) return;
    for (unsigned k = 0; k < n; k++) if (MR32(textures + k * 8) == 0x029a22f7u){
        keep[0] = textures + k * 8 + 4; keep[1] = MR32(keep[0]);
        MR32(keep[0]) = g_plate_drawing; return;
    }
}
#endif
