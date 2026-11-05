/* Surface validation shared by the native crowd and geometry regression tests.
 * The query returns the highest collision surface below its probe ceiling.
 * XY is always the server's position: clients never relocate a spectator. */
#ifndef MEET_GROUND_H
#define MEET_GROUND_H
#include <math.h>
typedef int (*meet_ground_query)(void *, float, float, float, float *, float *);
static int meet_ground_at(meet_ground_query query, void *ctx, float x, float y, float reference, float *ground){
    float best=0, distance=INFINITY;
    for(int i=0;i<2;i++){
        float z,nz;
        if(!query(ctx,x,y,reference+(i?3.f:0.5f),&z,&nz) || !isfinite(z) || !isfinite(nz) || fabsf(nz)<0.8f) continue;
        float d=fabsf(z-reference);
        if(d<=3.f && d<distance){ best=z; distance=d; }
    }
    if(!isfinite(distance)) return 0;
    static const float footprint[4][2]={{-.45f,0},{.45f,0},{0,-.45f},{0,.45f}};
    for(int i=0;i<4;i++){
        float z,nz;
        if(!query(ctx,x+footprint[i][0],y+footprint[i][1],best+0.4f,&z,&nz) ||
           !isfinite(z) || !isfinite(nz) || fabsf(nz)<0.8f || fabsf(z-best)>0.3f) return 0;
    }
    /* A low platform/ceiling above the floor leaves no room for a person. */
    float above,nz;
    if(query(ctx,x,y,best+1.8f,&above,&nz) && isfinite(above) && above>best+0.3f) return 0;
    *ground=best; return 1;
}
#endif
