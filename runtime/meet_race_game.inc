/* Display-only meet races: no GPS, native race transition, control lock or
 * automatic career progression. The two existing free-roam seats are peers. */
static rc_meet_race g_mr_view[2];
static uint32_t g_mr_jump,g_mr_reset[2],g_mr_gen[2];
static int64_t g_mr_sent[2],g_mr_pay_at[2];
int meet_cash_receive(const char *,const uint8_t *,uint32_t);
void meet_cash_frame(const char *);
int meet_cash_status(const char *,uint64_t,uint64_t,uint64_t);
static int mr_active(const rc_meet_race *r){return (r->phase==3||r->phase==4)&&r->mine==2;}
#include "meet_race_map.inc"
static void mr_frame(CPU *c,int in_roam,int64_t host){
    (void)c;char profile[32]={0};for(int i=0;i<31;i++){uint8_t ch=MR8(0x0083a9e0u+(uint32_t)i);if(ch<32||ch==127)break;profile[i]=(char)ch;}
    int career=in_roam && rd32(0x00850078u)==1 && mprace_car_ok() && profile[0];
    if(career)meet_cash_frame(profile);
    for(int seat=0;seat<2;seat++){
        uint32_t car=seat?g_c2.car:player_car();int exists=!seat||g_c2.on;
        uint32_t reset=car&&ok(car,0x978)?MR32(car+0x974):0;
        /* Nearby markers/lobbies are passive offers. Only entering another
         * activity ends participation in a meet race. */
        int eligible=career&&exists&&car&&!g_racing&&g_du.st==DU_IDLE&&g_mp.st==MP_IDLE;
        int action=roam_plat_meet_race_pick(seat);uint32_t gen;
        roam_plat_lock();rc_session *s=seat?&g_c2.s:&g_s;
        if(exists){
            if(strcmp(s->race_profile,profile)){memcpy(s->race_profile,profile,32);memset(&s->race,0,sizeof s->race);s->race_action=0;s->race_waiting=0;s->race_req++;}
            s->race_eligible=(uint8_t)eligible;
            if(g_mr_reset[seat]&&g_mr_reset[seat]!=reset)s->race_jump++;
            s->race_jump+=g_mr_jump;
            if(action==1||action==2){rlog("[meet-race] seat %d command %d event %llu eligible %d\n",seat+1,action,(unsigned long long)s->race.id,eligible);rc_meet_race_command(s,action);}
            g_mr_view[seat]=s->race;gen=s->race_gen;
        }else{memset(&g_mr_view[seat],0,sizeof g_mr_view[seat]);gen=0;}
        roam_plat_unlock();g_mr_reset[seat]=reset;
        rc_meet_race *r=&g_mr_view[seat];
        if(career&&r->amount&&host>=g_mr_pay_at[seat]){
            g_mr_pay_at[seat]=host+1000000;
            if(meet_cash_receive(profile,r->receipt,r->amount)){roam_plat_lock();rc_meet_race_ack(s,r->receipt);roam_plat_unlock();}
        }
        if(gen==g_mr_gen[seat] && host-g_mr_sent[seat]<500000)continue;
        g_mr_gen[seat]=gen;g_mr_sent[seat]=host;
        char buf[1472],id[40];MpJs j={buf,0,sizeof buf};js_raw(&j,"{");js_int(&j,"meetRace",seat+1);
        js_int(&j,"split",g_c2.on&&g_c2.car?1:0);js_int(&j,"device",seat?coop_device():0);
        js_int(&j,"phase",in_roam&&(eligible||r->mine)?r->phase:0);js_int(&j,"map",g_sync_paused&&g_wm.done);js_int(&j,"mine",r->mine);js_int(&j,"error",r->error);
        snprintf(id,sizeof id,"%016llx:%016llx",(unsigned long long)r->epoch,(unsigned long long)r->id);js_str(&j,"id",id);
        int64_t now=(int64_t)r->server_ms+(host-r->received_us)/1000;
        js_int(&j,"go",(int)((int64_t)r->go_ms-now));js_int(&j,"left",(int)((int64_t)r->close_ms-now));js_int(&j,"elapsed",(int)(now-(int64_t)r->go_ms));
        js_int(&j,"saved",meet_cash_status(profile,r->epoch,r->id,r->seat));js_int(&j,"shared",g_c2.on?1:0);
        if(car&&mr_active(r)){float dx=MRF32(car+0x60)-r->destination[0],dy=MRF32(car+0x64)-r->destination[1];js_int(&j,"distance",(int)sqrtf(dx*dx+dy*dy));}
        js_key(&j,"rows");js_raw(&j,"[");
        for(int i=0;i<r->n;i++){rc_meet_racer *p=&r->racer[i];char values[100];if(i)js_raw(&j,",");js_raw(&j,"[");js_val(&j,p->name);snprintf(values,sizeof values,",%u,%u,%u,%u,%d]",p->outcome,p->place,p->elapsed_ms,p->award,p->seat==r->seat);js_raw(&j,values);}
        js_raw(&j,"]}");roam_plat_lobby(buf);
    }
    mr_minimap(c,in_roam);
    g_mr_jump=0;
}
