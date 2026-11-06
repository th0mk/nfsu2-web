/* Crash/retry tests for the native-profile reward journal, without the game. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static uint8_t memory[0x900000];
static uint8_t *g_mem=memory;
#define MR32(a) (*(uint32_t *)(g_mem+(uint32_t)(a)))
static char root[1024];
static int fail_journal;
int fileio_resolve_save_w(const char *win,char *out,size_t n){if(fail_journal&&strstr(win,"MEETRACE")){out[0]=0;return 0;}const char *p=strrchr(win,'\\');snprintf(out,n,"%s/%s",root,p?p+1:win);return 1;}
int fileio_resolve_save(const char *w,char *o,size_t n){return fileio_resolve_save_w(w,o,n);}
#include "meet_cash.inc"
static void payload(uint32_t bank){memset(g_mem+0x1000,0,MC_PAYLOAD);strcpy((char *)g_mem+0x1000+53761,"RACER");memcpy(g_mem+0x1000+0xA146,&bank,4);}
static void save(void){char p[1100];snprintf(p,sizeof p,"%s/RACER",root);FILE *f=fopen(p,"wb");assert(f);uint8_t hd[36]={0};memcpy(hd,"20CM",4);assert(fwrite(hd,36,1,f)==1);assert(fwrite(g_mem+0x1000,MC_PAYLOAD,1,f)==1);assert(!fclose(f));}
static void restart(uint32_t bank){memset(&mc,0,sizeof mc);MR32(MC_BANK)=bank;meet_cash_loaded(0x1000,MC_PAYLOAD);}
int main(void){
    char tmp[]="/tmp/nfsu2-meet-cash-XXXXXX";assert(mkdtemp(tmp));snprintf(root,sizeof root,"%s",tmp);
    uint8_t one[24]={1},two[24]={2};payload(1000);save();restart(1000);
    assert(!meet_cash_receive("RACER",one,300));assert(MR32(MC_BANK)==1300);
    assert(!meet_cash_receive("RACER",one,300));assert(MR32(MC_BANK)==1300);
    // Crash before a native save: recover the pending grant once.
    restart(1000);meet_cash_frame("RACER");assert(MR32(MC_BANK)==1300);meet_cash_frame("RACER");assert(MR32(MC_BANK)==1300);
    assert(!meet_cash_receive("RACER",two,200));assert(MR32(MC_BANK)==1500);
    // Spend some money. Recovery must use the payload identity, not a bank delta.
    MR32(MC_BANK)=1200;payload(1200);meet_cash_payload(0x1000,MC_PAYLOAD);assert(meet_cash_save_ready());save();
    // Crash after saving cash, before recording/acknowledging that save.
    restart(1200);assert(meet_cash_receive("RACER",one,300));assert(meet_cash_receive("RACER",two,200));assert(MR32(MC_BANK)==1200);
    assert(!meet_cash_receive("RACER",one,200));assert(MR32(MC_BANK)==1200);
    uint8_t three[24]={3};assert(!meet_cash_receive("RACER",three,100));assert(MR32(MC_BANK)==1300);
    // Interrupted next journal write preserves the previous durable copy.
    char path[1100];assert(mc_path(path,sizeof path,(mc.seq+1)&1?"MEETRACE_A.DAT":"MEETRACE_B.DAT"));FILE *f=fopen(path,"wb");assert(f);fputs("torn",f);fclose(f);
    restart(1200);meet_cash_frame("RACER");assert(MR32(MC_BANK)==1300);
    // Storage failure never grants unjournaled cash. A later retry grants once.
    uint8_t retry[24]={9};fail_journal=1;
    assert(!meet_cash_receive("RACER",retry,100));assert(mc.write_error);assert(MR32(MC_BANK)==1300);
    fail_journal=0;assert(!meet_cash_receive("RACER",retry,100));assert(MR32(MC_BANK)==1400);assert(!mc.write_error);
    payload(1400);fail_journal=1;meet_cash_payload(0x1000,MC_PAYLOAD);assert(!meet_cash_save_ready());assert(!meet_cash_save_path_ready("/profiles/RACER/RACER"));assert(meet_cash_save_path_ready("/profiles/OTHER/OTHER"));
    fail_journal=0;meet_cash_payload(0x1000,MC_PAYLOAD);assert(meet_cash_save_ready());save();
    restart(1400);assert(meet_cash_receive("RACER",retry,100));assert(MR32(MC_BANK)==1400);
    // A profile switch must not transfer pending cash to another profile.
    uint8_t pending[24]={10};assert(!meet_cash_receive("RACER",pending,200));assert(MR32(MC_BANK)==1600);
    char first_root[1024];strcpy(first_root,root);char other[]="/tmp/nfsu2-meet-other-XXXXXX";assert(mkdtemp(other));strcpy(root,other);
    payload(700);strcpy((char *)g_mem+0x1000+53761,"OTHER");restart(700);meet_cash_frame("OTHER");assert(MR32(MC_BANK)==700);
    strcpy(root,first_root);payload(1400);restart(1400);meet_cash_frame("RACER");assert(MR32(MC_BANK)==1600);meet_cash_frame("RACER");assert(MR32(MC_BANK)==1600);rmdir(other);
    // No new payout wraps the signed career-bank limit.
    MR32(MC_BANK)=0x7fffffffu;uint8_t four[24]={4};assert(!meet_cash_receive("RACER",four,300));assert(MR32(MC_BANK)==0x7fffffffu);
    for(int i=0;i<3;i++){snprintf(path,sizeof path,"%s/%s",root,i==0?"RACER":i==1?"MEETRACE_A.DAT":"MEETRACE_B.DAT");unlink(path);}rmdir(root);
    puts("meet cash: retry, unsaved exit, post-save crash, shared bank, torn journal and overflow passed");return 0;
}
