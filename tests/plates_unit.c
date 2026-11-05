/* cc -O2 -Iruntime tests/plates_unit.c runtime/roam_core.c -lm -o out/plates_unit */
#include "roam_core.h"
#include "plate_pixels.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
static void crc(uint8_t *b,int n){uint32_t c=rc_crc32(b,n-4);for(int k=0;k<4;k++) b[n-4+k]=(uint8_t)(c>>(8*k));}
int main(void){
    uint8_t px[PLATE_BYTES+16],b[RC_MAX_DGRAM];
    memset(px,0xa5,sizeof px);
    assert(plate_pixels(px,"ABC12345",8));
    for(int k=PLATE_BYTES;k<(int)sizeof px;k++) assert(px[k]==0xa5);
    assert(!plate_pixels(px,"a",1));assert(!plate_pixels(px,"123456789",9));
    for(int k=0;k<36;k++){char c=k<10?'0'+k:'A'+k-10;assert(plate_pixels(px,&c,1));}
    rc_msg m={0},d;m.h.type=RC_MSG_HELLO;m.h.sender=42;m.h.flags=RC_HF_CREW|RC_HF_PLATE;
    m.u.hello.core_ver=RC_CORE_VER;m.u.hello.status=RC_ST_ROAM;strcpy(m.u.hello.name,"Test");
    strcpy(m.u.hello.plate,"ABC12345");
    int n=rc_encode(&m,b,sizeof b);assert(n>0);assert(rc_decode(b,n,&d)==RC_OK);assert(!strcmp(d.u.hello.plate,"ABC12345"));
    b[15] &= (uint8_t)~(RC_HF_CREW >> 8);crc(b,n);assert(rc_decode(b,n,&d)!=RC_OK);
    n=rc_encode(&m,b,sizeof b);b[n-14]=7;crc(b,n);assert(rc_decode(b,n,&d)!=RC_OK);
    n=rc_encode(&m,b,sizeof b);
    b[n-12]='!';crc(b,n);assert(rc_decode(b,n,&d)!=RC_OK);
    n=rc_encode(&m,b,sizeof b);b[n-13]=9;crc(b,n);assert(rc_decode(b,n,&d)!=RC_OK);
    m.u.hello.plate[0]=0;n=rc_encode(&m,b,sizeof b);assert(rc_decode(b,n,&d)==RC_OK && !d.u.hello.plate[0]);
    m.h.flags=RC_HF_CREW;m.u.hello.core_ver=16;n=rc_encode(&m,b,sizeof b);assert(rc_decode(b,n,&d)==RC_OK && !d.u.hello.plate[0]);
    clock_t start=clock();for(int i=0;i<10000;i++) assert(plate_pixels(px,"ABCDEFGH",8));
    printf("plates: alphabet, buffer bounds, wire round trip, invalid text, clearing, legacy passed; %.3f ms/plate (CPU composition)\n",1000.0*(clock()-start)/CLOCKS_PER_SEC/10000);
}
