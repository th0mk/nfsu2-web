/* Decorative native characters at server-reported car meets. The NIS bundle
 * supplies assets only: no scene is registered, no camera or race is started.
 * All guest calls run on the game thread. File I/O runs on a loader thread. */
#include <pthread.h>
#include <stdatomic.h>
#include "meet_ground.h"
int fileio_resolve_game(const char *, char *, size_t);
#define SPEC_N 32
#define SPEC_ASSETS 16
#define SPEC_DEF_SIZE 0x90u
static struct {
    _Atomic int read;                    /* 0 untouched, 1 reading, 2 ready, -1 unavailable */
    uint8_t *raw;
    uint32_t bytes, allocation, base, def, query;
    uint32_t chunks[SPEC_ASSETS];
    int nchunks, loaded, ndefs, ready, failed;
    uint8_t defs[6][SPEC_DEF_SIZE];
    uint32_t actor[SPEC_N], room;
    rc_meet layout;
    int active, next;
    unsigned skipped;
    int64_t check_at, retry_at;
} g_spec;
static uint32_t spec_u32(const void *p){ uint32_t v; memcpy(&v,p,4); return v; }
static int spec_contains(const uint8_t *p, uint32_t n, const char *s){
    size_t len = strlen(s);
    for (uint32_t i=0; i+len<=n; i++) if (!memcmp(p+i,s,len)) return 1;
    return 0;
}
static void *spec_read(void *unused){
    (void)unused;
    char path[1024]; FILE *f = 0; uint8_t *raw = 0; long n = 0;
    if (fileio_resolve_game("NIS\\Scene_EndingNis01_BundleB.bun",path,sizeof path) && (f=fopen(path,"rb"))){
        if (!fseek(f,0,SEEK_END) && (n=ftell(f))>0 && n<2*1024*1024 && !fseek(f,0,SEEK_SET)){
            raw=malloc((size_t)n);
            if (raw && fread(raw,1,(size_t)n,f)!=(size_t)n){ free(raw); raw=0; }
        }
        fclose(f);
    }
    g_spec.raw=raw; g_spec.bytes=raw?(uint32_t)n:0;
    if(!raw) rlog("[spectators] character bundle unavailable; crowds disabled\n");
    atomic_store(&g_spec.read,raw?2:-1);
    return 0;
}
static int spec_selected(uint32_t type,const uint8_t *p,uint32_t n){
    if (type==0xb3300000u) return spec_contains(p,n,"\\Spectators\\") || spec_contains(p,n,"\\SharedTextures\\");
    if (type==0x80134000u) return spec_contains(p,n,"\\crowd_") || spec_contains(p,n,"\\nick.BIN") || spec_contains(p,n,"\\lisha.BIN");
    if (type==0xe34009u) return spec_contains(p,n,"Bip02.") || spec_contains(p,n,"Bip03.");
    return type==0xe34010u && spec_contains(p,n,"crowd_female_");
}
static int spec_parse(void){
    const uint8_t *b=g_spec.raw; uint32_t size=g_spec.bytes;
    g_spec.nchunks=g_spec.ndefs=0;
    for(uint32_t at=0; at<size;){
        if(size-at<8) return 0;
        uint32_t type=spec_u32(b+at),n=spec_u32(b+at+4);
        if(n>size-at-8) return 0;
        if(spec_selected(type,b+at+8,n)){
            if(g_spec.nchunks==SPEC_ASSETS) return 0;
            g_spec.chunks[g_spec.nchunks++]=at;
        }
        if(type==0x80037020u){
            for(uint32_t s=at+8; s<at+8+n;){
                if(at+8+n-s<8) return 0;
                uint32_t t=spec_u32(b+s),len=spec_u32(b+s+4);
                if(len>at+n-s) return 0;
                /* The first eight payload bytes are alignment padding. Keep
                 * one idle/cheering definition per model, excluding the flag girl. */
                if(t==0x37040u && len>=8+SPEC_DEF_SIZE){
                    const uint8_t *d=b+s+16; uint32_t model=spec_u32(d+0x50);
                    int wanted=spec_u32(d)==1 && (spec_u32(d+0xc)==0x20 || spec_u32(d+0xc)==0x60) &&
                        (model==0x03d34948 || model==0x03d34947 || model==0x21acbf1c || model==0x0019ec84 || model==0x03338ff0 || model==0x21acbf1d);
                    for(int i=0;i<g_spec.ndefs;i++) if(spec_u32(g_spec.defs[i]+0x50)==model){
                        /* Prefer a full idle loop; some models only have a
                         * loop over a subrange (0x60) in the finish scene. */
                        if(wanted && spec_u32(d+0xc)==0x20) memcpy(g_spec.defs[i],d,SPEC_DEF_SIZE);
                        wanted=0;
                    }
                    if(wanted && g_spec.ndefs<6) memcpy(g_spec.defs[g_spec.ndefs++],d,SPEC_DEF_SIZE);
                }
                s+=8+len;
            }
        }
        at+=8+n;
    }
    return g_spec.nchunks>0 && g_spec.ndefs==6;
}
/* The engine routes EAGL skeleton/animation chunks before its general
 * bChunk loader registry, so they have dedicated load/unload entry points. */
static uint32_t spec_loader(uint32_t type,int unload){
    switch(type){
    case 0xb3300000u: return unload?0x00495040u:0x00494ea0u;
    case 0x80134000u: return unload?0x004940e0u:0x00494060u;
    case 0x00e34009u: return unload?0x004365b0u:0x00434e10u;
    case 0x00e34010u: return unload?0x004349a0u:0x004348f0u;
    default: return 0;
    }
}
static void spec_clear(CPU *c){
    for(int i=0;i<SPEC_N;i++) if(g_spec.actor[i]){
        gcall1(c,0x0043b1b0u,g_spec.actor[i],1); /* native character deleting destructor */
        g_spec.actor[i]=0;
    }
    g_spec.active=0; g_spec.next=0;
}
static void spec_release(CPU *c){
    spec_clear(c);
    /* Reverse registration order, before releasing the backing bytes. */
    while(g_spec.loaded){
        uint32_t chunk=g_spec.base+g_spec.chunks[--g_spec.loaded];
        uint32_t fn=spec_loader(MR32(chunk),1);
        if(fn) gcall1(c,fn,0,chunk);
    }
    if(g_spec.def) gcall1(c,0x00575630u,0,g_spec.def);
    if(g_spec.query) gcall1(c,0x00575630u,0,g_spec.query);
    if(g_spec.allocation) gcall1(c,0x00575630u,0,g_spec.allocation);
    g_spec.query=g_spec.def=g_spec.allocation=g_spec.base=0; g_spec.ready=0;
}
/* One asset registration per frame; the immutable file is cached across visits. */
static int spec_assets(CPU *c){
    if(g_spec.failed) return 0;
    int state=atomic_load(&g_spec.read);
    if(!state){
        atomic_store(&g_spec.read,1);
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);
        if(pthread_create(&t,&a,spec_read,0)) atomic_store(&g_spec.read,-1);
        pthread_attr_destroy(&a); return 0;
    }
    if(state!=2) return 0;
    if(!g_spec.allocation){
        if(!spec_parse()){ g_spec.failed=1; rlog("[spectators] unsupported character bundle; crowds disabled\n"); return 0; }
        g_spec.allocation=gcall1(c,0x00575620u,0,g_spec.bytes+127);
        g_spec.def=gcall1(c,0x00575620u,0,SPEC_DEF_SIZE);
        g_spec.query=gcall1(c,0x00575620u,0,0x360);
        if(!g_spec.allocation || !g_spec.def || !g_spec.query){ spec_release(c); g_spec.failed=1; return 0; }
        g_spec.base=(g_spec.allocation+127)&~127u;
        memcpy(GPTR(g_spec.base),g_spec.raw,g_spec.bytes);
    }
    if(g_spec.loaded<g_spec.nchunks){
        uint32_t chunk=g_spec.base+g_spec.chunks[g_spec.loaded];
        uint32_t fn=spec_loader(MR32(chunk),0);
        if(!fn || !gcall1(c,fn,0,chunk)){
            rlog("[spectators] asset registration failed for %08x\n",MR32(chunk));
            spec_release(c); g_spec.failed=1; return 0;
        }
        g_spec.loaded++; return 0;
    }
    if(!g_spec.ready) rlog("[spectators] %d native assets ready\n",g_spec.loaded);
    g_spec.ready=1; return 1;
}
/* The native world's collision query, including its streamed road surfaces.
 * Keep its temporary object registered only during this batch of probes. */
static int spec_query(void *ctx,float x,float y,float z,float *height,float *normal_z){
    CPU *c=ctx,save=*c;
    uint32_t q=g_spec.query,p=q+0x320,normal=q+0x330,hit=q+0x340;
    /* Native GetHeight adds one metre to the input Z search ceiling. */
    MRF32(p)=x; MRF32(p+4)=y; MRF32(p+8)=z-1.f;
    memset(GPTR(normal),0,12); MR8(hit)=0;
    c->esp=(c->esp-1024u)&~15u;
    PUSH32(hit); PUSH32(normal); PUSH32(0); PUSH32(p);
    c->ecx=q; PUSH32(ROAM_HOOK_RET);
    dispatch_indirect(c,0x005e7e70u);
    *height=(float)c->st[c->top&7]; *normal_z=MRF32(normal+8);
    int good=MR8(hit)!=0;
    *c=save; return good;
}
static int spec_ground(CPU *c,float x,float y,float z,float *height){
    gcall0(c,0x005e0b40u,g_spec.query);
    int good=meet_ground_at(spec_query,c,x,y,z,height);
    gcall0(c,0x005e0bf0u,g_spec.query);
    return good;
}
static void spec_spawn(CPU *c,int i){
    const rc_spectator *npc=&g_spec.layout.spectators[i];
    float x=npc->p[0]*0.01f,y=npc->p[1]*0.01f,z=npc->p[2]*0.01f;
    float ground;
    if(!spec_ground(c,x,y,z,&ground)){
        if(!(g_spec.skipped&(1u<<i))) rlog("[spectators] waiting for safe surface at %.1f %.1f %.1f\n",(double)x,(double)y,(double)z);
        g_spec.skipped|=1u<<i; return;
    }
    uint32_t d=g_spec.def;
    memcpy(GPTR(d),g_spec.defs[npc->variant],SPEC_DEF_SIZE);
    MR32(d+4)=0x4d454500u+(uint32_t)i;
    float angle=(float)npc->heading*(6.283185307179586f/65536.f);
    memset(GPTR(d+0x10),0,64);
    MRF32(d+0x10)=cosf(angle); MRF32(d+0x14)=sinf(angle);
    MRF32(d+0x20)=-sinf(angle); MRF32(d+0x24)=cosf(angle);
    MRF32(d+0x38)=MRF32(d+0x4c)=1.f;
    MRF32(d+0x40)=x; MRF32(d+0x44)=y; MRF32(d+0x48)=ground+0.25f;
    uint32_t a=gcall1(c,0x00433020u,0,1),args[2]={d,0};
    if(!a) return;
    int good=rh_call(c,0x00437be0u,a,0,2,args)&0xffu;
    /* Native construction probes the road once. Missing/unstreamed geometry
     * is not a place to put a character. Never run that query every frame. */
    good=good && MR8(a+0x19) && isfinite(MRF32(a+0x20)) && fabsf(MRF32(a+0x20)-ground)<0.3f;
    MR8(a+0x18)=0;
    if(!good){
        if(!(g_spec.skipped&(1u<<i))) rlog("[spectators] waiting for road at %.1f %.1f %.1f\n",(double)x,(double)y,(double)z);
        g_spec.skipped|=1u<<i; gcall1(c,0x0043b1b0u,a,1); return;
    }
    g_spec.actor[i]=a;
    uint32_t anim=MR32(a+0x14);
    float start=0.f,end=MRF32(anim+0x64);
    if(MR32(anim+0x34)&0x40u){ start=MRF32(anim+0x40); end=MRF32(anim+0x44); }
    MRF32(anim+0x68)=start+(end-start)*(float)i/SPEC_N; /* stagger within the native loop range */
    rlog("[spectators] spawned %d at %.1f %.1f %.1f\n",i,(double)x,(double)y,(double)MRF32(a+0x20));
}
static int spec_cinematic(void){
    return MR32(0x00827b8cu)!=0x00827b8cu; /* native manager's active-scene list */
}
static void spec_leave(CPU *c){
    /* A cinematic can borrow already-loaded character resources by hash.
     * Keep their backing bytes until it has destroyed its own actors. */
    spec_clear(c);
    if(!spec_cinematic() && g_spec.allocation) spec_release(c);
    g_spec.check_at=0;
}
static void spectators_frame(CPU *c,int in_roam){
    if(!in_roam || !MR32(0x00870cd0u) || spec_cinematic()){
        if(g_spec.allocation || g_spec.active) spec_leave(c);
        return;
    }
    int64_t now=roam_plat_now_us();
    if(now>=g_spec.check_at){
        g_spec.check_at=now+500000;
        float nearest=120.f*120.f; rc_meet chosen={0}; int found=0;
        uint32_t me=player_car(),room;
        float px=MRF32(me+0x60),py=MRF32(me+0x64),pz=MRF32(me+0x68);
        roam_plat_lock(); room=g_s.conf.room;
        for(int i=0;i<g_s.meet_n;i++){
            const rc_meet *m=&g_s.meet[i];
            float dx=m->x-px,dy=m->y-py,dz=m->z-pz,dist=dx*dx+dy*dy;
            if(m->n_spectators && dist<nearest && dz*dz<400.f){ nearest=dist; chosen=*m; found=1; }
        }
        /* Keep the current meet near a boundary between two meets instead of
         * alternating crowds as the player moves a few centimetres. */
        for(int i=0;g_spec.active && i<g_s.meet_n;i++){
            const rc_meet *m=&g_s.meet[i];
            float dx=m->x-px,dy=m->y-py,dz=m->z-pz;
            if(m->id==g_spec.layout.id && m->n_spectators && dx*dx+dy*dy<140.f*140.f && dz*dz<400.f){ chosen=*m; found=1; break; }
        }
        roam_plat_unlock();
        if(g_spec.active && (!found || room!=g_spec.room || chosen.id!=g_spec.layout.id ||
            chosen.n_spectators!=g_spec.layout.n_spectators || memcmp(chosen.spectators,g_spec.layout.spectators,sizeof chosen.spectators))) spec_clear(c);
        if(found && !g_spec.active){
            g_spec.layout=chosen; g_spec.room=room; g_spec.active=1; g_spec.next=0; g_spec.retry_at=0; g_spec.skipped=0;
        }
        if(!found && g_spec.allocation) spec_release(c);
    }
    if(!g_spec.active || !spec_assets(c)) return;
    if(g_spec.next>=g_spec.layout.n_spectators && now>=g_spec.retry_at){ g_spec.next=0; g_spec.retry_at=now+5000000; }
    if(g_spec.next<g_spec.layout.n_spectators){
        int i=g_spec.next++; if(!g_spec.actor[i]) spec_spawn(c,i);
    }
    float dt=MRF32(G_WORLDDT);
    if(!isfinite(dt) || dt<=0 || dt>1) return;
    uint32_t arg; memcpy(&arg,&dt,4);
    for(int i=0;i<SPEC_N;i++) if(g_spec.actor[i]) gcall1(c,0x00436050u,g_spec.actor[i],arg);
}
