#include <assert.h>
#include <stdio.h>
#include "meet_ground.h"
static int mode,loaded=1;
static int query(void *ctx,float x,float y,float ceiling,float *z,float *nz){
    (void)ctx;(void)y;
    if(!loaded || (mode==2 && x>0.3f)) return 0;
    *z=mode==1?20+x*.2f:20; *nz=1;
    if(mode==3) *z=x>0.3f?18:20;
    if(mode==4) *nz=.5f;
    if(mode==5) *z=ceiling>=26?26:20;
    if(mode==8) *z=ceiling>=21.5f?21.5f:20;
    if(mode==6) *z=40;
    if(mode==7){ *z=NAN; return 1; }
    if(mode==9) *nz=NAN;
    return *z<=ceiling;
}
int main(void){
    float z;
    assert(meet_ground_at(query,0,0,0,20.4f,&z) && z==20);
    mode=1; assert(meet_ground_at(query,0,0,0,20.4f,&z));
    mode=2; assert(!meet_ground_at(query,0,0,0,20.4f,&z));
    mode=3; assert(!meet_ground_at(query,0,0,0,20.4f,&z));
    mode=4; assert(!meet_ground_at(query,0,0,0,20.4f,&z));
    mode=5;
    assert(meet_ground_at(query,0,0,0,20.4f,&z) && z==20);
    assert(meet_ground_at(query,0,0,0,26.4f,&z) && z==26);
    mode=6; assert(!meet_ground_at(query,0,0,0,20.4f,&z));
    mode=8; assert(!meet_ground_at(query,0,0,0,20.4f,&z));
    mode=9; assert(!meet_ground_at(query,0,0,0,20.4f,&z));
    mode=7; assert(!meet_ground_at(query,0,0,0,20.4f,&z));
    mode=0; loaded=0; assert(!meet_ground_at(query,0,0,0,20.4f,&z));
    loaded=1; assert(meet_ground_at(query,0,0,0,20.4f,&z));
    puts("PASS: flat/sloped surfaces, ledges, steep faces, road levels, nonfinite data, streaming retry");
}
