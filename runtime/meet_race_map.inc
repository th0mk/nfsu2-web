/* Native map markers owned by the native map/HUD lists. No GPS items are
 * created: choosing a route remains part of the race. */
#include "meet_race_icon.inc"
static struct { uint32_t map, marker[6], object[6]; } g_mr_map;
static struct { uint32_t icons, block, icon, visual, object, hash; } g_mr_mm[6];
int roam_meet_race_marker_seat(uint32_t object){
    for(int i=0;i<6;i++) if(object && g_mr_mm[i].object==object) return i/3;
    return -1;
}
/* Reuse the native GPS ring texture, but animate our own image. The native
 * GPS group's Pulse script owns visibility/child bindings that cannot safely
 * be shared with an independent destination. No GPS state is touched. */
static uint32_t mr_pulse_texture(CPU *c){
    uint32_t args[3]={0xe2848973u,0,0}; /* MINIMAP_GPS_SELECTOR */
    return rh_call(c,FN_TEXINFO,0,0,3,args)?args[0]:0;
}
static void mr_pulse_setup(CPU *c,uint32_t o,uint32_t texture){
    uint32_t data=rd32(o+0x2c);
    MR32(o+0x54)=0;
    uint32_t args[2]={o,texture};rh_call(c,FN_FE_SETTEX,0,0,2,args);
    MRF32(data+0x44)=MRF32(data+0x48)=0;MRF32(data+0x4c)=MRF32(data+0x50)=1;
    MRF32(data+0x28)=MRF32(data+0x2c)=MRF32(data+0x30)=0;MRF32(data+0x34)=1;
}
static void mr_pulse_frame(CPU *c,uint32_t o,int ring){
    uint32_t data=rd32(o+0x2c);if(!ok(data,0x54))return;
    /* Two expanding, fading waves. Elapsed time, never frame count, owns the
     * phase so the effect has the same speed at every rendering frame rate. */
    float phase=fmodf((float)(roam_plat_now_us()%800000)/800000.f+ring*.5f,1.f);
    float size=10.f+22.f*phase;
    MRF32(data+0x38)=MRF32(data+0x3c)=size;
    fe_colour_set(c,o,255,255,255,(int)(220.f*(1.f-phase)));
    MR32(o+0x1c)|=0x400000u;
}
static int mr_map(CPU *c,uint32_t map,uint32_t pkg,uint32_t pk){
    if(g_mr_map.map!=map){memset(&g_mr_map,0,sizeof g_mr_map);g_mr_map.map=map;}
    int changed=0;
    for(int k=0;k<6;k++){
        int i=k/3,pulse=k%3<2;
        uint32_t m=g_mr_map.marker[k],o=g_mr_map.object[k];
        if(m && !fe_list_of(pk+0x4c,o,0)){m=0;g_mr_map.marker[k]=g_mr_map.object[k]=0;}
        int active=mr_active(&g_mr_view[i]);
        if(!m && active && wm_room(pk)>2){
            uint32_t texture=pulse?mr_pulse_texture(c):mr_finish_texture(c);if(!texture)continue;
            uint32_t base=wm_find(c,pkg,"SPRINTEVENTINDICATOR_0");
            if(!base) base=meet_icon(c,pkg);
            char name[24];snprintf(name,sizeof name,pulse?"MEET_RING_MAP_%d":"MEET_FINISH_%d",pulse?k:i);
            o=base?fe_clone(c,pkg,base,name):0;
            uint32_t size=WM_ICON_SZ;
            m=o?rh_call(c,FN_MALLOC,0,0,1,&size):0;
            if(!ok(m,WM_ICON_SZ))continue;
            memset(GPTR(m),0,WM_ICON_SZ);MR32(m+8)=WM_CAR;MR32(m+0xc)=o;
            uint32_t head=map+4,tail=rd32(head+4);
            MR32(tail)=m;MR32(head+4)=m;MR32(m)=head;MR32(m+4)=tail;
            g_mr_map.marker[k]=m;g_mr_map.object[k]=o;
            if(!pulse)rlog("[meet-race] map marker seat %d at 0x%08x destination %.1f %.1f\n",i+1,m,(double)g_mr_view[i].destination[0],(double)g_mr_view[i].destination[1]);
            if(pulse)mr_pulse_setup(c,o,texture);else mr_finish_icon(c,o,texture);
        }
        if(m){
            float x=active?g_mr_view[i].destination[0]:WMF_AWAY;
            float y=active?g_mr_view[i].destination[1]:WMF_AWAY;
            if(MRF32(m+0x10)!=x || MRF32(m+0x14)!=y){wmf_move(m,-1,x,y,0);changed=1;}
        }
    }
    return changed;
}
/* Native placement may set icon sizes again after the roam update. Apply
 * pulse size at the actual FE draw, after per-seat snapshots are restored. */
void roam_meet_race_pulse_draw(CPU *c,uint32_t object){
    for(int k=0;k<6;k++)if(k%3<2 && object &&
        (object==g_mr_map.object[k] || object==g_mr_mm[k].object)){
        mr_pulse_frame(c,object,k%3);return;
    }
}
static void mr_minimap(CPU *c,int in_roam){
    uint32_t icons=rd32(G_MM_ICONS);
    if(!ok(icons,0x1c))return;
    uint32_t pkg=rd32(icons+0x18),pk=gcall1(c,FN_FE_PACKAGE,0,pkg);
    if(!ok(pk,0x54))return;
    for(int k=0;k<6;k++){
        int i=k/3,pulse=k%3<2;
        uint32_t icon=g_mr_mm[k].icon,o=g_mr_mm[k].object;
        int live=0,copy_live=o && fe_list_of(pk+0x4c,o,0) && rd32(o+0x10)==g_mr_mm[k].hash;
        if(g_mr_mm[k].icons==icons && copy_live){
            for(uint32_t n=rd32(icons),guard=0;n && n!=icons && guard++<MM_POOL && ok(n,0x20);n=rd32(n))
                if(n==icon && rd32(n+0x1c)==g_mr_mm[k].visual){live=1;break;}
        }
        if(!live){g_mr_mm[k].icon=g_mr_mm[k].visual=0;icon=0;if(copy_live)gcall1(c,FN_FE_HIDE,0,o);else g_mr_mm[k].object=o=0;}
        int active=in_roam && hud_on_top(pkg) && mr_active(&g_mr_view[i]);
        if(!icon && active){
            if(!o && wm_room(pk)<=TAG_ALL+2)continue;
            if(!g_mr_mm[k].block)g_mr_mm[k].block=gcall1(c,FN_MALLOC,0,NEAR_BLK);
            uint32_t d=g_mr_mm[k].block;if(!ok(d,NEAR_BLK))continue;
            uint32_t texture=pulse?mr_pulse_texture(c):mr_finish_texture(c);if(!texture)continue;
            uint32_t base=wm_find(c,pkg,"SPRINTEVENTINDICATOR_0");
            if(!base)base=wm_find(c,pkg,"AIRACERINDICATOR_0");
            char name[24];snprintf(name,sizeof name,pulse?"MEET_RING_HUD_%d":"MEET_FINISH_HUD_%d",pulse?k:i);
            if(!o)o=base?fe_clone(c,pkg,base,name):0;if(!o)continue;
            g_mr_mm[k].object=o;g_mr_mm[k].hash=rd32(o+0x10);
            uint32_t vp=rd32(G_MM_VISPOOL),ip=rd32(G_MM_ICONPOOL);
            uint32_t v=vp?gcall1(c,FN_POOL_ALLOC,0,vp):0;
            icon=v&&ip?gcall1(c,FN_POOL_ALLOC,0,ip):0;
            if(!ok(icon,0x20)){
                if(v){uint32_t args[2]={vp,v};rh_call(c,FN_POOL_FREE,0,0,2,args);}
                gcall1(c,FN_FE_HIDE,0,o);continue;
            }
            memset(GPTR(d),0,NEAR_BLK);MR32(d)=MR32(d+4)=d;
            MR32(v)=o;MR32(v+4)=MM_PLAYER;memset(GPTR(icon),0,0x20);
            MR32(icon+8)=(d-G_PLAYER1)>>2;MR32(icon+0x1c)=v;
            uint32_t tail=rd32(icons+4);MR32(tail)=icon;MR32(icons+4)=icon;MR32(icon)=icons;MR32(icon+4)=tail;
            g_mr_mm[k].icons=icons;g_mr_mm[k].icon=icon;g_mr_mm[k].visual=v;g_mr_mm[k].object=o;g_mr_mm[k].hash=rd32(o+0x10);
            if(pulse)mr_pulse_setup(c,o,texture);else mr_finish_icon(c,o,texture);
        }
        if(icon){
            if(pulse&&active)mr_pulse_frame(c,o,k%3);
            uint32_t d=g_mr_mm[k].block;
            float x=g_mr_view[i].destination[0],y=g_mr_view[i].destination[1];
            uint32_t car=i?g_c2.car:player_car(),view=rd32(icons+MM_VIEW);
            if(active && ok(car,0x58) && ok(view,0xc4)){
                float zoom=MRF32(G_MM_ZOOM),scale=MRF32(view+0xb4);
                if(!(zoom>1.f))zoom=MRF32(view+0xc0);
                float radius=zoom>0.f?MM_EDGE_R*0.3f*scale/zoom*0.85f:0.f;
                float cx=MRF32(car+0x50),cy=MRF32(car+0x54),dx=x-cx,dy=y-cy,distance=hypotf(dx,dy);
                /* Feed the native HUD a point just inside its rim. Native car
                 * blips otherwise cull distant targets in some map modes.
                 * Only this visual proxy moves: world map and scoring retain
                 * the real destination. Each local seat uses its own origin. */
                if(isfinite(radius)&&radius>0.f&&isfinite(distance)&&distance>radius){
                    x=cx+dx*(radius/distance);y=cy+dy*(radius/distance);
                }
            }
            MRF32(d+0x50)=active?x:WMF_AWAY;
            MRF32(d+0x54)=active?y:WMF_AWAY;
            MR32(icon+0x18)=active?0:MM_HIDE;
            gcall1(c,active?FN_FE_SHOW:FN_FE_HIDE,0,o);
        }
    }
}
