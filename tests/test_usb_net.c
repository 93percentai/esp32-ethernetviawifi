#include "usb_net.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned emitted;
static uint8_t expected[60];
static uint16_t r16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static void w16(uint8_t *p,uint16_t n) { p[0]=n;p[1]=n>>8; }
static void w32(uint8_t *p,uint32_t n) { w16(p,n);w16(p+2,n>>16); }
static bool frame(const uint8_t *data,size_t n,void *ctx) { (void)ctx; assert(n==sizeof(expected) && !memcmp(data,expected,n)); ++emitted; return true; }
static bool count_frame(const uint8_t *data,size_t n,void *ctx) { (void)data;(void)ctx;assert(n==60);++emitted;return true; }
static void descriptors(void)
{
    uint8_t d[]={9,2,0,0,2,1,0,0x80,50,9,4,3,0,1,2,6,0,0,5,0x24,6,3,7,
        13,0x24,0xf,4,0,0,0,0,0xea,5,0,0,0,7,5,0x83,3,16,0,8,
        9,4,7,0,0,10,0,0,0,9,4,7,2,2,10,0,0,0,7,5,0x85,2,0,2,0,7,5,6,2,0,2,0};
    d[2]=sizeof(d); usb_net_descriptor_t net;
    assert(usb_net_parse_config(d,sizeof(d),0,0,&net) && net.driver==USB_NET_ECM);
    assert(net.endpoints.control_interface==3 && net.endpoints.data_interface==7 && net.endpoints.data_alt==2);
    d[15]=0x0d; assert(usb_net_parse_config(d,sizeof(d),0,0,&net) && net.driver==USB_NET_NCM);
    d[14]=0xe0; d[15]=1; d[16]=3; assert(usb_net_parse_config(d,sizeof(d),0,0,&net) && net.driver==USB_NET_RNDIS);
    d[14]=0xff; assert(usb_net_parse_config(d,sizeof(d),0x0b95,0x7720,&net)==false); /* Control interface has no bulk pair. */
    assert(usb_net_native_match(0x05ac,0x1402)==USB_NET_AX88772);
    assert(usb_net_native_match(0x0424,0xec00)==USB_NET_LAN95XX);
    assert(usb_net_native_match(0x0a46,0x9621)==USB_NET_DM9601);
    assert(usb_net_native_match(0x0bda,0x8152)==USB_NET_RTL8152);
    assert(usb_net_native_match(0x0bda,0x8153)==USB_NET_NONE); /* Standard-class fallback; never wrong native revision. */
    assert(usb_net_native_match(0xffff,0xffff)==USB_NET_NONE);
    for(size_t n=0;n<sizeof(d);++n) assert(!usb_net_parse_config(d,n,0,0,&net));
}
static void ncm(void)
{
    uint8_t params[28]={0},block[4096]; usb_net_parameters_t p={0};
    w16(params,28); w16(params+2,1); w32(params+4,16384); w32(params+16,4096);
    w16(params+20,4); w16(params+22,0); w16(params+24,4);
    assert(usb_net_ncm_parameters(params,sizeof(params),&p));
    size_t n=usb_net_encode(USB_NET_NCM,&p,512,expected,sizeof(expected),block,sizeof(block));
    assert(n>60 && r16(block+8)==n && (r16(block+20)+14)%4==0);
    emitted=0; assert(usb_net_decode(USB_NET_NCM,block,n,frame,NULL)==1 && emitted==1);
    w16(block+18,12); emitted=0; assert(usb_net_decode(USB_NET_NCM,block,n,frame,NULL)<0 && !emitted); /* NDP self-cycle. */
    w16(block+18,0); w16(block+20,16); assert(usb_net_decode(USB_NET_NCM,block,n,frame,NULL)<0); /* Payload into metadata. */
    n=usb_net_encode(USB_NET_NCM,&p,512,expected,sizeof(expected),block,sizeof(block));
    for(size_t size=0;size<n;++size) assert(usb_net_decode(USB_NET_NCM,block,size,NULL,NULL)<0);
    w16(params+20,0); assert(!usb_net_ncm_parameters(params,sizeof(params),&p));
    w16(params+20,4); w16(params+22,4); assert(!usb_net_ncm_parameters(params,sizeof(params),&p));
    w16(params+22,0); w16(params+24,3); assert(!usb_net_ncm_parameters(params,sizeof(params),&p));
    w16(params+24,64); assert(usb_net_ncm_parameters(params,sizeof(params),&p));
    n=usb_net_encode(USB_NET_NCM,&p,64,expected,sizeof(expected),block,sizeof(block));
    assert(r16(block+10)==64 && (r16(block+72)+14)%4==0);
    assert(usb_net_decode(USB_NET_NCM,block,n,frame,NULL)==1);
    /* Two datagrams in one independent NCM fixture, then overlapping entries. */
    memset(block,0,160); w32(block,0x484d434e); w16(block+4,12); w16(block+8,160); w16(block+10,12);
    w32(block+12,0x304d434e); w16(block+16,20); w16(block+20,32); w16(block+22,60); w16(block+24,96); w16(block+26,60);
    memcpy(block+32,expected,60); memcpy(block+96,expected,60);
    emitted=0; assert(usb_net_decode(USB_NET_NCM,block,160,frame,NULL)==2 && emitted==2);
    block[44]=0x81;block[45]=0;emitted=0;
    assert(usb_net_decode(USB_NET_NCM,block,160,count_frame,NULL)==2 && emitted==2); /* Ethernet policy is per frame, not per USB aggregate. */
    w16(block+24,32); emitted=0; assert(usb_net_decode(USB_NET_NCM,block,160,frame,NULL)<0 && !emitted);
}
static void framing(void)
{
    uint8_t block[4096]; usb_net_parameters_t p={0};
    for(int d=USB_NET_ECM;d<=USB_NET_AX88179;++d) {
        if(d==USB_NET_NCM) continue;
        size_t n=usb_net_encode(d,&p,64,expected,sizeof(expected),block,sizeof(block)); assert(n>=60);
        if(d==USB_NET_AX88772) { assert(n==68 && block[0]==60 && block[2]==(uint8_t)~60); }
        else if(d==USB_NET_RTL8152) { assert(n==68 && block[3]==0xc0); memset(block,0,88); w32(block,64); memcpy(block+24,expected,60); n=88; }
        else if(d==USB_NET_DM9601) { assert(n==62 && r16(block)==60); memset(block,0,67); w16(block+1,64); memcpy(block+3,expected,60); n=67; }
        else if(d==USB_NET_AX88179) { assert(n==68 && r16(block)==60); memset(block,0,76);memcpy(block+2,expected,60);w32(block+64,62u<<16);w32(block+68,0x80000000);w32(block+72,(64u<<16)|2);n=76; }
        else if(d==USB_NET_LAN95XX) { assert(n==68 && (block[1]&0x30)==0x30); memset(block,0,68); w32(block,64u<<16); memcpy(block+4,expected,60); n=68; }
        emitted=0; assert(usb_net_decode(d,block,n,frame,NULL)==1 && emitted==1);
        if(d==USB_NET_DM9601) {
            memset(block+n,0,4); emitted=0;
            assert(usb_net_decode(d,block,n+4,frame,NULL)==1 && emitted==1);
            assert(usb_net_decode(d,block,n+5,NULL,NULL)<0);
        }
        if(d!=USB_NET_ECM && d!=USB_NET_AX88772) { emitted=0; assert(usb_net_decode(d,block,n-1,frame,NULL)<0 && !emitted); }
    }
    /* A malformed second message cannot partially publish the first. */
    size_t n=usb_net_encode(USB_NET_RNDIS,&p,512,expected,60,block,sizeof(block));
    memcpy(block+n,block,n); block[n+4]=0xff; emitted=0;
    assert(usb_net_decode(USB_NET_RNDIS,block,n*2,frame,NULL)<0 && !emitted);
    uint8_t odd_frame[62]={0}; memcpy(odd_frame,expected,60);
    n=usb_net_encode(USB_NET_DM9601,&p,64,odd_frame,62,block,sizeof(block)); assert(n==66 && r16(block)==64); /* maxpacket TX padding erratum */
}
static void randomized(void)
{
    uint8_t data[512]; usb_net_descriptor_t net;
    srand(91);
    for(unsigned i=0;i<12000;++i) {
        size_t n=rand()%sizeof(data); for(size_t j=0;j<n;++j) data[j]=rand();
        if(n>=9 && (i&1)) { data[0]=9; data[1]=2; w16(data+2,n); }
        usb_net_parse_config(data,n,0x0bda,0x8152,&net);
        for(int d=USB_NET_ECM;d<=USB_NET_AX88179;++d) usb_net_decode(d,data,n,NULL,NULL);
    }
}
int main(void)
{
    memset(expected,0,sizeof(expected)); expected[6]=2; expected[12]=8; expected[14]=0x45;
    descriptors(); ncm(); framing(); randomized();
    puts("ECM/NCM/RNDIS/native descriptors, framing, bounds, cycles and aggregate validation passed");
}
