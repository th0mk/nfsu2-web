/* Small original checkered finish flag, shared by the native map and HUD.
 * Procedural pixels avoid shipping a game texture or depending on its atlas. */
static uint32_t mr_finish_texture(CPU *c){
    if(g_mr_finish_ti)return g_mr_finish_hash;
    uint32_t args[3]={0xe2a5c626u,0,0},src=rh_call(c,FN_TEXINFO,0,0,3,args);
    if(!ok(src,TI_SZ)||!ok(rd32(src),PLAT_SZ)||MR8(src+0x4a)!=0x20)return 0;
    const uint32_t bytes=4*(64*64+32*32+16*16+8*8);
    uint32_t size=TI_SZ+PLAT_SZ+bytes+64,ti=gcall1(c,FN_MALLOC,0,size);
    if(!ti)return 0;
    uint32_t pl=ti+TI_SZ,pix=(pl+PLAT_SZ+63)&~63u;
    uint8_t *pixels=GPTR(pix);memset(pixels,0,bytes);
    for(int y=0;y<64;y++)for(int x=0;x<64;x++){
        int total=0,alpha=0;
        for(int sy=0;sy<4;sy++)for(int sx=0;sx<4;sx++){
            float px=x+(sx+.5f)/4,py=y+(sy+.5f)/4;
            float top=12+3*sinf((px-14)*.13f),v=py-top;
            int colour=-1;
            if(px>=8&&px<=15&&py>=9&&py<=57)colour=255;
            if(px>=10&&px<=13&&py>=11&&py<=55)colour=25;
            if(px>=13&&px<=57&&v>=-2&&v<=30)colour=255;
            if(px>=14&&px<=56&&v>=-1&&v<=29)colour=25;
            if(px>=15&&px<=55&&v>=0&&v<=28)colour=(((int)((px-15)/8)+(int)(v/7))&1)?25:255;
            if(colour>=0){total+=colour;alpha++;}
        }
        uint8_t *p=pixels+4*(y*64+x);
        p[0]=p[1]=p[2]=alpha?(uint8_t)(total/alpha):0;p[3]=(uint8_t)((alpha*255+8)/16);
    }
    uint8_t *prev=pixels;
    for(int n=32;n>=8;n/=2){
        uint8_t *next=prev+4*(n*2)*(n*2);
        for(int y=0;y<n;y++)for(int x=0;x<n;x++)for(int k=0;k<4;k++){
            uint8_t *p=prev+4*(y*2*n*2+x*2)+k;
            next[4*(y*n+x)+k]=(uint8_t)((p[0]+p[4]+p[8*n]+p[8*n+4]+2)/4);
        }
        prev=next;
    }
    memcpy(GPTR(ti),GPTR(src),TI_SZ);memcpy(GPTR(pl),GPTR(rd32(src)),PLAT_SZ);
    MR32(pl)=MR32(pl+4)=MR32(pl+0x18)=0;
    const char *name="MEET_FINISH_FLAG";g_mr_finish_hash=bhash(name);
    memset(GPTR(ti+8),0,0x18);memcpy(GPTR(ti+8),name,strlen(name));
    MR32(ti)=pl;MR32(ti+0x24)=g_mr_finish_hash;MR32(ti+0x70)=pix;
    MR32(ti+0x40)=4*64*64;MR16(ti+0x44)=MR16(ti+0x46)=64;
    MR8(ti+0x48)=MR8(ti+0x49)=6;MR8(ti+0x4e)=4;
    if(MR32(ti+0x6c)==src)MR32(ti+0x6c)=ti;
    gcall0(c,FN_TEX_CREATE,ti);
    if(!rd32(pl+0x18)){gcall1(c,0x00575630u,0,ti);return 0;}
    g_mr_finish_ti=ti;return g_mr_finish_hash;
}
static void mr_finish_icon(CPU *c,uint32_t o,uint32_t texture){
    uint32_t data=rd32(o+0x2c);
    if(!ok(data,0x54))return;
    MRF32(data+0x28)=MRF32(data+0x2c)=MRF32(data+0x30)=0;MRF32(data+0x34)=1;
    MRF32(data+0x44)=MRF32(data+0x48)=0;MRF32(data+0x4c)=MRF32(data+0x50)=1;
    uint32_t args[2]={o,texture};rh_call(c,FN_FE_SETTEX,0,0,2,args);
    fe_colour_set(c,o,255,255,255,255);
}
