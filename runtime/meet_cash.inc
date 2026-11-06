/* Receipt journal beside the native profile. A payload's fingerprint is logged
 * before the game may write it. Recovery can therefore recognize a saved prize
 * even when the process died before the profile-written callback. Cash and
 * career progression remain separate: no race-win/story routines are called. */
#include <unistd.h>
#define MC_MAX 4096
#define MC_PAYLOAD 54930u
#define MC_FILE_SIZE 54966u
#define MC_BANK (0x0085ad40u+0x7134u)
typedef struct { uint8_t id[24]; uint32_t amount,saved; uint64_t payload[8]; } McReceipt;
static struct {char name[33];McReceipt r[MC_MAX];uint8_t applied[MC_MAX];uint32_t n;int ready,blocked,corrupt,write_error;uint64_t loaded,seq;} mc;
int fileio_resolve_save(const char *,char *,size_t);
int fileio_resolve_save_w(const char *,char *,size_t);
static uint64_t mc_hash(const uint8_t *b,size_t n){uint64_t h=0xcbf29ce484222325ull;for(size_t i=0;i<n;i++)h=(h^b[i])*0x100000001b3ull;return h;}
static int mc_path(char *out,size_t n,const char *file){
    if(!mc.name[0]||strchr(mc.name,'/')||strchr(mc.name,'\\')||strchr(mc.name,'.'))return 0;
    char win[160];snprintf(win,sizeof win,"NFS Underground 2\\%s\\%s",mc.name,file);out[0]=0;fileio_resolve_save_w(win,out,n);return out[0]!=0;
}
static int mc_write(void){
    char path[1100];uint64_t seq=mc.seq+1;
    if(!mc_path(path,sizeof path,seq&1?"MEETRACE_A.DAT":"MEETRACE_B.DAT"))return 0;
    FILE *f=fopen(path,"wb");if(!f)return 0;
    uint64_t hd[5]={0x3152434d3253464eull,seq,mc.n,sizeof(McReceipt),0};
    hd[4]=mc_hash((const uint8_t *)mc.r,mc.n*sizeof(McReceipt))^mc_hash((const uint8_t *)hd,32);
    int good=fwrite(hd,sizeof hd,1,f)==1 && fwrite(mc.r,sizeof(McReceipt),mc.n,f)==mc.n && fflush(f)==0 && fsync(fileno(f))==0;
    good=fclose(f)==0 && good;if(good)mc.seq=seq;return good;
}
static int mc_has(const McReceipt *r,uint64_t h){for(int j=0;j<8;j++)if(h&&r->payload[j]==h)return 1;return 0;}
static void mc_reconcile(const uint8_t *payload){
    uint64_t h=mc_hash(payload,MC_PAYLOAD);int changed=0;uint8_t was[MC_MAX];for(uint32_t i=0;i<mc.n;i++)was[i]=(uint8_t)mc.r[i].saved;
    for(uint32_t i=0;i<mc.n;i++)if(!mc.r[i].saved && mc_has(&mc.r[i],h)){mc.r[i].saved=1;changed=1;}
    if(changed&&!mc_write()){mc.blocked=1;for(uint32_t i=0;i<mc.n;i++)mc.r[i].saved=was[i];}
}
static void mc_load(const char *name){
    if(mc.ready && !strcmp(mc.name,name))return;
    memset(&mc,0,sizeof mc);snprintf(mc.name,sizeof mc.name,"%s",name);mc.ready=1;
    char path[1100];if(!mc_path(path,sizeof path,"MEETRACE.DAT")){mc.blocked=1;return;}
    FILE *f;int files=0,good_files=0;static McReceipt candidate[MC_MAX];
    for(int slot=0;slot<2;slot++){
        if(!mc_path(path,sizeof path,slot?"MEETRACE_B.DAT":"MEETRACE_A.DAT")||!(f=fopen(path,"rb")))continue;
        files++;uint64_t hd[5]={0};int good=fread(hd,sizeof hd,1,f)==1 && hd[0]==0x3152434d3253464eull && hd[1]>0 && hd[2]<=MC_MAX && hd[3]==sizeof(McReceipt);
        if(good)good=fread(candidate,sizeof(McReceipt),(size_t)hd[2],f)==hd[2] && fgetc(f)==EOF &&
            (mc_hash((const uint8_t *)candidate,(size_t)hd[2]*sizeof(McReceipt))^mc_hash((const uint8_t *)hd,32))==hd[4];
        fclose(f);if(!good)continue;good_files++;
        if(hd[1]>mc.seq){mc.seq=hd[1];mc.n=(uint32_t)hd[2];memcpy(mc.r,candidate,mc.n*sizeof(McReceipt));}
    }
    if(files&&!good_files){mc.blocked=mc.corrupt=1;fprintf(stderr,"[meet-race] invalid reward journal; payments deferred\n");return;}
    if(mc_path(path,sizeof path,mc.name)&&(f=fopen(path,"rb"))){uint8_t data[MC_FILE_SIZE];size_t n=fread(data,1,sizeof data,f);fclose(f);if(n==sizeof data&&!memcmp(data,"20CM",4))mc_reconcile(data+36);}
    for(uint32_t i=0;i<mc.n;i++)mc.applied[i]=(uint8_t)mc.r[i].saved;
}
/* Called on a complete primary-profile load, including restoration of the
 * kept payload. A matching unsaved payload already contains the prize. */
void meet_cash_loaded(uint32_t in,uint32_t size){
    if(size<MC_PAYLOAD)return;char name[33]={0};memcpy(name,g_mem+in+53761,32);mc.ready=0;mc_load(name);mc.loaded=mc_hash(g_mem+in,MC_PAYLOAD);
    for(uint32_t i=0;i<mc.n;i++)mc.applied[i]=(uint8_t)(mc.r[i].saved||mc_has(&mc.r[i],mc.loaded));
}
/* Return 1 only after the native save AND receipt marker are durable. The
 * caller may then acknowledge the server. Unsaved grants stay retryable. */
int meet_cash_receive(const char *name,const uint8_t id[24],uint32_t amount){
    if(amount!=100 && amount!=200 && amount!=300)return 0;mc_load(name);if(mc.blocked)return 0;
    uint32_t i=0;for(;i<mc.n;i++)if(!memcmp(mc.r[i].id,id,24))break;
    if(i==mc.n){if(mc.n>=MC_MAX){mc.write_error=1;return 0;}memset(&mc.r[i],0,sizeof mc.r[i]);memcpy(mc.r[i].id,id,24);mc.r[i].amount=amount;mc.n++;
        if(!mc_write()){mc.n--;mc.write_error=1;return 0;}}
    if(mc.r[i].amount!=amount)return 0;mc.write_error=0;
    if(!mc.applied[i]){uint32_t bank=MR32(MC_BANK);if(bank>0x7fffffffu-amount){mc.write_error=1;return 0;}MR32(MC_BANK)=bank+amount;mc.applied[i]=1;fprintf(stderr,"[meet-race] prize: %u credits, bank %u\n",amount,bank+amount);}
    return mc.r[i].saved!=0;
}
/* Pending receipts are reapplied after an unsaved exit even before reconnect. */
void meet_cash_frame(const char *name){
    mc_load(name);if(mc.blocked)return;
    for(uint32_t i=0;i<mc.n;i++)if(!mc.applied[i]&&!mc.r[i].saved){uint32_t bank=MR32(MC_BANK);if(bank<=0x7fffffffu-mc.r[i].amount){MR32(MC_BANK)=bank+mc.r[i].amount;mc.applied[i]=1;}}
}
void meet_cash_payload(uint32_t out,uint32_t size){
    if(size<MC_PAYLOAD||!mc.ready||!mc.name[0]||strncmp((const char *)(g_mem+out+53761),mc.name,32))return;
    uint64_t h=mc_hash(g_mem+out,MC_PAYLOAD);int changed=0;
    for(uint32_t i=0;i<mc.n;i++)if(mc.applied[i]&&!mc.r[i].saved&&!mc_has(&mc.r[i],h)){memmove(mc.r[i].payload+1,mc.r[i].payload,7*sizeof(uint64_t));mc.r[i].payload[0]=h;changed=1;}
    if((changed||mc.blocked)&&!mc.corrupt){mc.blocked=!mc_write();if(mc.blocked)fprintf(stderr,"[meet-race] reward journal write failed; save deferred\n");}
}
int meet_cash_save_ready(void){return !mc.blocked;}
int meet_cash_save_path_ready(const char *path){
    const char *base=strrchr(path,'/');base=base?base+1:path;
    return !mc.blocked || strcmp(base,mc.name)!=0;
}
void meet_cash_written(const char *path){
    if(!mc.ready||!mc.n||mc.blocked)return;const char *base=strrchr(path,'/');base=base?base+1:path;if(strcmp(base,mc.name))return;
    FILE *f=fopen(path,"rb");if(!f)return;uint8_t data[MC_FILE_SIZE];size_t n=fread(data,1,sizeof data,f);int good=fsync(fileno(f))==0;fclose(f);
    if(good&&n==sizeof data&&!memcmp(data,"20CM",4))mc_reconcile(data+36);
}

/* Presentation distinguishes an announced prize from cash actually applied. */
int meet_cash_status(const char *name,uint64_t epoch,uint64_t race,uint64_t seat){
    if(!mc.ready || strcmp(mc.name,name))return 0;
    uint8_t id[24];uint64_t words[3]={epoch,race,seat};
    for(int i=0;i<3;i++)for(int j=0;j<8;j++)id[i*8+j]=(uint8_t)(words[i]>>(56-j*8));
    for(uint32_t i=0;i<mc.n;i++)if(!memcmp(mc.r[i].id,id,24))return mc.r[i].saved?2:mc.applied[i]?1:(mc.blocked||mc.write_error)?-1:0;
    return (mc.blocked||mc.write_error)?-1:0;
}

/* Pin the exact bytes handed to the native file writer as well as speculative
 * payload builds. A long sequence of previews cannot evict the save in flight. */
int meet_cash_before_write(const char *path,uint32_t buffer,uint32_t length){
    if(mc.ready && mc.n){
        if(length==MC_FILE_SIZE && !memcmp(g_mem+buffer,"20CM",4))meet_cash_payload(buffer+36,MC_PAYLOAD);
        else if(length==MC_PAYLOAD)meet_cash_payload(buffer,length);
    }
    return meet_cash_save_path_ready(path);
}
