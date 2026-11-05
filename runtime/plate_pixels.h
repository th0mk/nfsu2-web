/* Shared ASCII plate alphabet and deterministic bitmap compositor. No fonts, files or GPU readback. */
#ifndef PLATE_PIXELS_H
#define PLATE_PIXELS_H
#include <stdint.h>
#include <stddef.h>
#define PLATE_MAX 8
#define PLATE_W 256
#define PLATE_H 64
#define PLATE_MIPS 7
#define PLATE_BYTES 87376
static inline int plate_valid(const char *s, size_t n) {
    if (n > PLATE_MAX) return 0;
    for (size_t i=0;i<n;i++) if (!((s[i]>='A' && s[i]<='Z') || (s[i]>='0' && s[i]<='9'))) return 0;
    return 1;
}
/* 36 glyphs, seven rows of five bits: the source bitmap atlas. */
static const uint8_t plate_glyphs[36][7] = {
    {14,17,19,21,25,17,14},
    {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2},
    {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},
    {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14},
    {14,17,17,31,17,17,17},
    {30,17,17,30,17,17,30},
    {15,16,16,16,16,16,15},
    {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31},
    {31,16,16,30,16,16,16},
    {15,16,16,23,17,17,15},
    {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17},
    {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},
    {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13},
    {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},
    {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10},
    {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},
    {31,1,2,4,8,16,31},
};
static inline int plate_pixels(uint8_t *dst, const char *text, size_t n) {
    if (!plate_valid(text,n)) return 0;
    for (unsigned y=0;y<PLATE_H;y++) for (unsigned x=0;x<PLATE_W;x++) {
        unsigned q=4*(y*PLATE_W+x); int edge=x<3 || y<3 || x>=PLATE_W-3 || y>=PLATE_H-3;
        dst[q]=edge?65:224; dst[q+1]=edge?65:235; dst[q+2]=edge?65:239; dst[q+3]=255;
    }
    unsigned start=(PLATE_W-(n ? n*28-8:0))/2;
    for (unsigned k=0;k<n;k++) {
        unsigned g=text[k]<='9' ? text[k]-'0' : text[k]-'A'+10;
        for(unsigned y=0;y<28;y++) for(unsigned x=0;x<20;x++) if(plate_glyphs[g][y/4] & (16>>(x/4))) {
            unsigned q=4*((y+(PLATE_H-28)/2)*PLATE_W+start+k*28+x);
            dst[q]=45; dst[q+1]=30; dst[q+2]=24;
        }
    }
    unsigned at=0,next=PLATE_W*PLATE_H*4,w=PLATE_W,h=PLATE_H;
    for(unsigned m=1;m<PLATE_MIPS;m++) {
        unsigned nw=w/2,nh=h/2;
        for(unsigned y=0;y<nh;y++) for(unsigned x=0;x<nw;x++) for(unsigned c=0;c<4;c++) {
            unsigned q=at+4*(2*y*w+2*x)+c;
            dst[next+4*(y*nw+x)+c]=(uint8_t)((dst[q]+dst[q+4]+dst[q+4*w]+dst[q+4*w+4]+2)/4);
        }
        at=next; next+=nw*nh*4; w=nw; h=nh;
    }
    return 1;
}
#endif
