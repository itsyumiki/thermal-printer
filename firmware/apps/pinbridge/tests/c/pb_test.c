#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/pb_core.h"
static unsigned now_ms, writes; static uint16_t outs[4], pads[4];
static int mode(void*x,unsigned p,unsigned b,unsigned m,unsigned l){(void)x;(void)m;if(l)outs[p]|=1u<<b;else outs[p]&=(uint16_t)~(1u<<b);return 0;}
static void wr(void*x,unsigned p,uint16_t mask,uint16_t val){(void)x;outs[p]=(outs[p]&~mask)|(val&mask);writes++;}
static uint16_t rd(void*x,unsigned p){(void)x;return pads[p]|outs[p];}
static int adc(void*x,int ch,unsigned*r,unsigned*m){(void)x;if(!r||!m)return ch<0?0:0;*r=412;*m=1219;return 0;}
static uint32_t ms(void*x){(void)x;return now_ms;}
static int tx(void*x,const char*s,size_t n){(void)x;fwrite(s,1,n,stdout);return 1;}
int main(void){pb_core c;pb_hal h={0,mode,wr,rd,adc,0,ms,tx};char line[512];pb_init(&c,&h);while(fgets(line,sizeof line,stdin)){if(line[0]=='@'){unsigned a,b;char pin[16];if(sscanf(line,"@time %u",&a)==1){now_ms=a;continue;}if(sscanf(line,"@advance %u",&a)==1){for(b=0;b<a;b+=10){now_ms+=10;pb_poll(&c);}continue;}if(sscanf(line,"@pad %15[^=]=%u",pin,&a)==2){int i=pb_pin(pin);if(i>=0){unsigned p=pb_defs[i].port,bit=pb_defs[i].bit;if(a)pads[p]|=1u<<bit;else pads[p]&=(uint16_t)~(1u<<bit);}continue;}if(sscanf(line,"@hw %15s",pin)==1)continue;if(sscanf(line,"@writes %u",&a)==1){if(writes!=a){fprintf(stderr,"writes expected %u got %u\n",a,writes);return 2;}writes=0;continue;}if(!strncmp(line,"@reboot",7)){pb_init(&c,&h);continue;}continue;}pb_rx(&c,(const unsigned char*)line,strlen(line));}return 0;}
