/* Optional authoritative hub extension; never accepted from another player. */
static uint64_t mr_u64(const uint8_t *p){uint64_t v=0;for(int i=7;i>=0;i--)v=(v<<8)|p[i];return v;}
static uint32_t mr_u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void mr_put64(uint8_t *p,uint64_t v){for(int i=0;i<8;i++){p[i]=(uint8_t)v;v>>=8;}}
static void mr_put32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++){p[i]=(uint8_t)v;v>>=8;}}
void rc_meet_race_command(rc_session *s,int action){
    if(action<1||action>2||!s->race.id)return;
    s->race_action=(uint8_t)action;s->race_target=s->race.id;s->race_asked_us=0;s->race_waiting=1;if(!++s->race_req)++s->race_req;
}
void rc_meet_race_ack(rc_session *s,const uint8_t receipt[24]){
    if(s->race_action)return;
    memcpy(s->race_ack,receipt,24);s->race_action=3;s->race_asked_us=0;s->race_waiting=1;if(!++s->race_req)++s->race_req;
}
static void race_answer(rc_session *s,const uint8_t *b,int len,int64_t now){
    if(s->closed||len<120||len>824||mr_u32(b+4)!=s->conf.room||!s->race_waiting||!s->race_req||mr_u32(b+8)!=s->race_req)return;
    int n=b[62];if(n>16||len!=120+n*44||b[60]>6||b[63]>6||b[61]>1)return;
    rc_meet_race r={0};r.epoch=mr_u64(b+12);r.id=mr_u64(b+20);r.server_ms=mr_u64(b+28);r.go_ms=mr_u64(b+36);r.close_ms=mr_u64(b+44);r.seat=mr_u64(b+52);
    r.phase=b[60];r.error=b[61];r.n=(uint8_t)n;r.mine=b[63];
    if(r.server_ms>INT64_MAX/1000||r.go_ms>INT64_MAX/1000||r.close_ms>INT64_MAX/1000)return;
    for(int k=0;k<3;k++){
        int32_t a=(int32_t)mr_u32(b+64+k*4),d=(int32_t)mr_u32(b+76+k*4);
        if(a < -3276700||a>3276700||d < -3276700||d>3276700)return;
        r.start[k]=a*0.01f;r.destination[k]=d*0.01f;
    }
    memcpy(r.receipt,b+92,24);r.amount=mr_u32(b+116);
    if(r.amount && r.amount!=100 && r.amount!=200 && r.amount!=300)return;
    for(int i=0;i<n;i++){
        const uint8_t *v=b+120+i*44;rc_meet_racer *p=&r.racer[i];p->seat=mr_u64(v);memcpy(p->name,v+8,24);p->name[23]=0;
        if(!p->seat||v[32]<1||v[32]>6||v[33]>16)return;
        for(int j=0;j<i;j++)if(r.racer[j].seat==p->seat)return;
        p->outcome=v[32];p->place=v[33];p->elapsed_ms=mr_u32(v+36);p->award=mr_u32(v+40);
        if(p->award && p->award!=100 && p->award!=200 && p->award!=300)return;
    }
    r.received_us=now;s->race=r;s->race_gen++;s->race_action=0;s->race_waiting=0;
}
static void race_tick(rc_session *s,int hub,int64_t now){
    int connected=hub&&s->hub_heard_us&&!s->closed;
    if(!connected||now-s->race.received_us>5000000){if(s->race.phase){memset(&s->race,0,sizeof s->race);s->race_gen++;}}
    if(!connected||!s->race_profile[0]||!s->io.send)return;
    int64_t gap=s->race.id?250000:2000000;
    if(s->race_asked_us && now-s->race_asked_us<gap)return;
    uint8_t b[84]={0};memcpy(b,"MRQ1",4);mr_put32(b+4,s->conf.room);/* Retry the same request until answered: RTT may exceed the poll interval.
     * New user commands supersede this ID, so old replies cannot erase them. */
    if(!s->race_waiting){if(!++s->race_req)++s->race_req;s->race_waiting=1;}mr_put32(b+8,s->race_req);
    mr_put64(b+12,s->race_action?s->race_target:s->race.id);b[20]=s->race_action;b[21]=s->race_eligible;mr_put32(b+24,s->race_jump);
    memcpy(b+28,s->race_profile,32);if(s->race_action==3)memcpy(b+60,s->race_ack,24);
    s->race_asked_us=now;s->io.send(s->io.user,s->conf.hub_ip,s->conf.hub_port,b,sizeof b);s->tx++;s->tx_bytes+=sizeof b;
}
