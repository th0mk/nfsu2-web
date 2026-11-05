/* Protocol adversaries and command retries against the production C core. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef memcpy
#undef memset
#undef memmove
#include "roam_core.c"
static uint8_t sent[84];static int sends;
static void send_packet(void *u,uint32_t ip,uint16_t port,const uint8_t *b,int n){(void)u;(void)ip;(void)port;if(n==84&&!memcmp(b,"MRQ1",4)){memcpy(sent,b,84);sends++;}}
static void response(rc_session *s,uint8_t *b){memset(b,0,164);memcpy(b,"MRP1",4);mr_put32(b+4,s->conf.room);mr_put32(b+8,s->race_req);mr_put64(b+12,1);mr_put64(b+20,2);mr_put64(b+28,1000);mr_put64(b+36,300000);mr_put64(b+52,3);b[60]=1;b[62]=1;b[63]=1;mr_put64(b+120,3);memcpy(b+128,"Driver",7);b[152]=1;}
int main(void){
 static rc_session s;rc_config cf={0};cf.room=123;cf.hub_ip=0x0a4d0001;cf.hub_port=47210;cf.self_id=7;cf.port=47210;rc_io io={0,send_packet,0,0};rc_session_init(&s,&cf,&io);strcpy(s.race_profile,"RACER");s.race_eligible=1;s.hub_heard_us=1;
 race_tick(&s,1,1000000);assert(sends==1&&sent[21]==1);uint8_t b[164];response(&s,b);
 rc_receive(&s,0x0a4d0002,47210,b,164,1100000);assert(!s.race.id);
 rc_receive(&s,0x0a4d0001,47210,b,164,1100000);assert(s.race.id==2&&s.race.mine==1);
 rc_meet_race_command(&s,2);rc_receive(&s,0x0a4d0001,47210,b,164,1200000);assert(s.race_action==2); /* old poll cannot erase leave */
 race_tick(&s,1,1300000);assert(sent[20]==2);uint32_t first=mr_u32(sent+8);
 race_tick(&s,1,1600000);assert(sent[20]==2&&mr_u32(sent+8)==first); /* lost response retries the same command */
 response(&s,b);b[62]=17;race_answer(&s,b,164,1700000);assert(s.race_action==2);
 response(&s,b);b[152]=7;race_answer(&s,b,164,1700000);assert(s.race_action==2);
 response(&s,b);mr_put32(b+4,999);race_answer(&s,b,164,1700000);assert(s.race_action==2);
 response(&s,b);race_answer(&s,b,163,1700000);assert(s.race_action==2);
 race_answer(&s,b,164,1700000);assert(!s.race_action);

 // RTT exceeds polling interval: the response remains valid after a retry.
 race_tick(&s,1,2000000);response(&s,b);uint32_t poll=s.race_req;
 race_tick(&s,1,2250000);assert(s.race_req==poll);
 unsigned gen=s.race_gen;race_answer(&s,b,164,2300000);assert(s.race_gen==gen+1);
 race_answer(&s,b,164,2350000);assert(s.race_gen==gen+1); /* duplicate reply ignored */
 race_tick(&s,1,2500000);assert(s.race_req!=poll);response(&s,b);
 rc_meet_race_command(&s,1);race_answer(&s,b,164,2800000);assert(s.race_action==1);
 race_tick(&s,1,2800000);response(&s,b);race_tick(&s,1,3050000);
 race_answer(&s,b,164,3100000);assert(s.race_action==0); /* delayed action reply accepted */
 race_tick(&s,0,8000000);assert(!s.race.phase); /* stale or disconnected state hides UI/markers */
 puts("meet race core: source/room/nonce, malformed replies, lost response, command ordering and expiry passed");
}
