/* Camera pins owned by the native HUD/map packages, using the existing map transforms and clipping. */
static struct {
    uint32_t hud,icons,blk,original;
    uint32_t image[TRAP_CAP],icon[TRAP_CAP],vis[TRAP_CAP],hash[TRAP_CAP];
    int ncopy,n;
} g_trap_mm;
static int trap_icons_live(uint32_t icons){
    int found=0;
    for (uint32_t n=rd32(icons),j=0;n&&n!=icons&&j<MM_POOL&&ok(n,0x20);n=rd32(n),j++)
        for(int k=0;k<g_trap_mm.n;k++) if(n==g_trap_mm.icon[k] && rd32(n+0x1cu)==g_trap_mm.vis[k]){
            uint32_t v=g_trap_mm.vis[k];
            if(ok(v,8)&&rd32(v)==g_trap_mm.image[k]&&rd32(v+4)==MM_PLAYER) found++;
            break;
        }
    return found==g_trap_mm.n;
}
static void trap_minimap(CPU *c,int driving){
    uint32_t pl=rd32(G_PLAYER1),hud=ok(pl,PL_HUD+4)?rd32(pl+PL_HUD):0,icons=rd32(G_MM_ICONS);
    if(hud!=g_trap_mm.hud){
        uint32_t blk=g_trap_mm.blk; memset(&g_trap_mm,0,sizeof g_trap_mm); g_trap_mm.blk=blk; g_trap_mm.hud=hud;
    }
    if(!hud||!ok(icons,0x1c)||!g_gmem) return;
    uint32_t pkg=rd32(icons+0x18u),pk=gcall1(c,FN_FE_PACKAGE,0,pkg);
    if(!ok(pk,0x54)) return;
    if(g_trap_mm.ncopy && (!fe_list_of(pk+0x4cu,g_trap_mm.image[0],0) || rd32(g_trap_mm.image[0]+0x10u)!=g_trap_mm.hash[0])){
        g_trap_mm.ncopy=g_trap_mm.n=0;g_trap_mm.original=0;
    }
    if(g_trap_mm.icons!=icons||!trap_icons_live(icons)){g_trap_mm.n=0;g_trap_mm.icons=icons;}
    if(!g_trap_mm.blk){
        uint32_t sz=NEAR_BLK*TRAP_CAP; g_trap_mm.blk=rh_call(c,FN_MALLOC,0,0,1,&sz);
        if(!ok(g_trap_mm.blk,sz)){g_trap_mm.blk=0;return;}
        memset(GPTR(g_trap_mm.blk),0,sz);
    }
    for(int k=g_trap_mm.n;driving&&k<TRAP_COUNT;k++){
        if(wm_room(pk)<TAG_ALL+1) break;
        if(k>=g_trap_mm.ncopy){
            char name[24];snprintf(name,sizeof name,"TRAP_CAMERA_%d",k);
            uint32_t o=trap_image(c,pkg,name);if(!o) break;
            g_trap_mm.image[k]=o;g_trap_mm.hash[k]=rd32(o+0x10u);g_trap_mm.ncopy++;
        }
        uint32_t vp=rd32(G_MM_VISPOOL),ip=rd32(G_MM_ICONPOOL);
        uint32_t v=vp?rh_call(c,FN_POOL_ALLOC,0,0,1,&vp):0,ic=ok(v,8)&&ip?rh_call(c,FN_POOL_ALLOC,0,0,1,&ip):0;
        if(!ok(ic,0x20)){if(ok(v,8)){uint32_t a[2]={vp,v};rh_call(c,FN_POOL_FREE,0,0,2,a);}break;}
        uint32_t d=g_trap_mm.blk+(uint32_t)k*NEAR_BLK;
        MR32(d)=d;MR32(d+4)=d;MRF32(d+0x50u)=trap_coord(k,0);MRF32(d+0x54u)=trap_coord(k,1);
        MR32(v)=g_trap_mm.image[k];MR32(v+4)=MM_PLAYER;
        memset(GPTR(ic),0,0x20);MR32(ic+8u)=(d-G_PLAYER1)>>2;MR32(ic+0x18u)=MM_EDGE;MR32(ic+0x1cu)=v;
        uint32_t tail=rd32(icons+4u);MR32(tail)=ic;MR32(icons+4u)=ic;MR32(ic)=icons;MR32(ic+4u)=tail;
        g_trap_mm.icon[k]=ic;g_trap_mm.vis[k]=v;g_trap_mm.n++;
        rlog("[speedtrap] minimap camera %d at %.1f %.1f\n",k,(double)trap_coord(k,0),(double)trap_coord(k,1));
    }
    if(!g_trap_mm.original) g_trap_mm.original=wm_find(c,pkg,"AIRACERINDICATOR_0");
    int visible=driving && ok(g_trap_mm.original,0x20) && !(rd32(g_trap_mm.original+0x1cu)&1u);
    for(int k=0;k<g_trap_mm.ncopy;k++){
        uint32_t o=g_trap_mm.image[k];
        int show=visible && k<TRAP_COUNT;
        if(((rd32(o+0x1cu)&1u)==0)!=show) gcall1(c,show?FN_FE_SHOW:FN_FE_HIDE,0,o);
    }
    for(int k=0;k<g_trap_mm.n;k++){
        if(k<TRAP_COUNT){MRF32(g_trap_mm.blk+(uint32_t)k*NEAR_BLK+0x50u)=trap_coord(k,0);MRF32(g_trap_mm.blk+(uint32_t)k*NEAR_BLK+0x54u)=trap_coord(k,1);}
        MR32(g_trap_mm.icon[k]+0x18u)=MM_EDGE|((driving&&k<TRAP_COUNT)?0:MM_HIDE);
    }
}
static void trap_map_open(CPU *c,uint32_t map,uint32_t pkg,uint32_t pk){
    for(int k=0;k<TRAP_COUNT&&wm_room(pk)>2;k++){
        char name[24];snprintf(name,sizeof name,"TRAP_MAP_%d",k);
        uint32_t o=trap_image(c,pkg,name);if(!o)break;
        uint32_t sz=WM_MARK_SZ,m=rh_call(c,FN_MALLOC,0,0,1,&sz);if(!ok(m,sz))break;
        memset(GPTR(m),0,sz);MR32(m+8u)=WM_CAR;MR32(m+0xcu)=o;
        /* The map's car-marker basis adds a quarter turn; cancel it for the camera glyph. */
        MRF32(m+0x18u)=16384.f*MRF32(G_WM_HEADING_K);
        MRF32(m+0x10u)=trap_coord(k,0);MRF32(m+0x14u)=trap_coord(k,1);
        uint32_t head=map+4u,tail=rd32(head+4u);MR32(tail)=m;MR32(head+4u)=m;MR32(m+4u)=tail;MR32(m)=head;
        wmf_track(map,m,WMF_TRAPS);
        gcall0(c,FN_WM_PLACE,map);
        uint32_t d=rd32(o+0x2cu);
        rlog("[speedtrap] pin placed %.1f %.1f draw %.1f %.1f pivot %.1f %.1f\n",(double)MRF32(m+0x1cu),(double)MRF32(m+0x20u),(double)MRF32(d+0x1cu),(double)MRF32(d+0x20u),(double)MRF32(d+0x10u),(double)MRF32(d+0x14u));
        rlog("[speedtrap] world map camera %d at %.1f %.1f\n",k,(double)trap_coord(k,0),(double)trap_coord(k,1));
    }
}
