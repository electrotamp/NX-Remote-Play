#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define UN_MAGIC 0x3150554eu
static inline void UnWrite(uint8_t* b,uint8_t role,uint32_t session,const uint8_t key[16]) {
    memset(b,0,28);b[0]='N';b[1]='U';b[2]='P';b[3]='1';b[4]=role;
    for(unsigned i=0;i<4;i++)b[8+i]=(uint8_t)(session>>(i*8));
    memcpy(b+12,key,16);
}
static inline int UnMatch(const uint8_t* b,size_t n,uint32_t session,const uint8_t key[16]) {
    if(n!=28 && n!=36)return 0;
    uint8_t expected[28];UnWrite(expected,b[4],session,key);
    uint8_t diff=0;for(unsigned i=0;i<28;i++)diff|=expected[i]^b[i];
    return !diff;
}
