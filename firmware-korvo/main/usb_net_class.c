#include "usb_net.h"
#include <string.h>

static uint16_t r16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static uint32_t r32(const uint8_t *p) { return r16(p)|(uint32_t)r16(p+2)<<16; }
static void w16(uint8_t *p,uint16_t n) { p[0]=n;p[1]=n>>8; }
static void w32(uint8_t *p,uint32_t n) { w16(p,n);w16(p+2,n>>16); }

static int rndis_exchange(const usb_net_descriptor_t *dev,usb_net_io_t *io,
                          uint8_t *message,size_t length,uint8_t *reply,size_t capacity)
{
    uint32_t type=r32(message),id=r32(message+8);
    if(usb_net_control(io,0x21,0,0,dev->endpoints.control_interface,message,length)<0) return -1;
    /* RESPONSE_AVAILABLE may arrive interleaved with status indications. All
     * requests have a bounded response deadline and validate type, ID and status. */
    for(unsigned i=0;i<20 && !io->failed;++i) {
        usb_net_sleep(io,10);
        int n=io->control(io->context,0xa1,1,0,dev->endpoints.control_interface,reply,capacity);
        if(n<0) { io->failed=true; return -1; }
        if(n<8 || !r32(reply)) continue;
        uint32_t size=r32(reply+4),response=r32(reply);
        if(size<8 || size>(unsigned)n) break;
        if(response==7) continue; /* Unsolicited media indication. Poll media OID separately. */
        if(response!=(type|0x80000000) || size<16 || r32(reply+8)!=id || r32(reply+12)) break;
        return size;
    }
    io->failed=true; io->error="RNDIS reply timeout or mismatched type, request ID or status"; return -1;
}

static int rndis_query(const usb_net_descriptor_t *dev,usb_net_io_t *io,usb_net_parameters_t *params,
                       uint32_t oid,void *out,size_t capacity)
{
    uint8_t message[28]={0},reply[512]={0};
    w32(message,4); w32(message+4,sizeof(message)); w32(message+8,++params->request_id); w32(message+12,oid);
    int n=rndis_exchange(dev,io,message,sizeof(message),reply,sizeof(reply));
    if(n<24) return -1;
    uint32_t length=r32(reply+16),offset=r32(reply+20);
    if(offset>UINT32_MAX-8 || (offset+=8)<24 || offset>(unsigned)n || length>(unsigned)n-offset || length>capacity) {
        io->failed=true; return -1;
    }
    memcpy(out,reply+offset,length); return length;
}

bool usb_net_class_init(const usb_net_descriptor_t *dev,usb_net_io_t *io,usb_net_parameters_t *params,uint8_t mac[6])
{
    memset(params,0,sizeof(*params)); params->in_max=params->out_max=io->full_speed?4096:16384;
    const ecm_descriptor_t *ep=&dev->endpoints;
    if(dev->driver==USB_NET_RNDIS) {
        uint8_t message[32]={0},reply[512]={0};
        w32(message,2); w32(message+4,24); w32(message+8,++params->request_id);
        w32(message+12,1); w32(message+20,params->in_max);
        int n=rndis_exchange(dev,io,message,24,reply,sizeof(reply));
        if(n<52 || r32(reply+16)!=1 || r32(reply+28)!=0 || r32(reply+36)<BRIDGE_FRAME_MAX+44) { io->error="Invalid RNDIS INIT completion, medium or transfer limit"; return false; }
        params->out_max=r32(reply+36)>16384?16384:r32(reply+36);
        if(r32(reply+40)>7) { io->error="RNDIS packet alignment factor is unsupported"; return false; }
        params->out_alignment=1u<<r32(reply+40);
        if(rndis_query(dev,io,params,0x01010102,mac,6)!=6) { io->error="RNDIS MAC query failed or has invalid length"; return false; }
        memset(message,0,sizeof(message)); w32(message,5); w32(message+4,32);
        w32(message+8,++params->request_id); w32(message+12,0x0001010e);
        w32(message+16,4); w32(message+20,20); w32(message+28,0x2f);
        if(rndis_exchange(dev,io,message,sizeof(message),reply,sizeof(reply))<16) return false;
        if(ep->data_alt) usb_net_control(io,1,11,ep->data_alt,ep->data_interface,NULL,0);
        static const uint8_t zero[6];
        return !io->failed && !(mac[0]&1) && memcmp(mac,zero,6);
    }
    uint8_t language[4]={0},string[26]={0};
    usb_net_control(io,0x80,6,0x0300,0,language,sizeof(language));
    if(io->failed || language[1]!=3 || language[0]<4) return false;
    usb_net_control(io,0x80,6,0x0300|ep->mac_string,r16(language+2),string,sizeof(string));
    if(io->failed || !ecm_parse_mac(string,sizeof(string),mac)) return false;
    if(dev->driver==USB_NET_NCM) {
        uint8_t data[28]={0};
        usb_net_control(io,0xa1,0x80,0,ep->control_interface,data,sizeof(data));
        if(io->failed || !usb_net_ncm_parameters(data,sizeof(data),params)) { if(!io->error) io->error="NCM16 unsupported or invalid NTB size/alignment parameters"; return false; }
        if(io->full_speed && params->in_max>4096) params->in_max=4096;
        if(r16(data+2)&2) usb_net_control(io,0x21,0x84,0,ep->control_interface,NULL,0);
        if(dev->ncm_capabilities&0x10) usb_net_control(io,0x21,0x8a,0,ep->control_interface,NULL,0);
        w32(data,params->in_max);
        usb_net_control(io,0x21,0x86,0,ep->control_interface,data,4);
    }
    usb_net_control(io,1,11,ep->data_alt,ep->data_interface,NULL,0);
    if(dev->driver==USB_NET_ECM || (dev->ncm_capabilities&1))
        usb_net_control(io,0x21,0x43,0xf,ep->control_interface,NULL,0);
    return !io->failed;
}

bool usb_net_rndis_link(const usb_net_descriptor_t *dev,usb_net_io_t *io,usb_net_parameters_t *params,bool *up,uint32_t *speed)
{
    uint8_t media[4],rate[4];
    if(rndis_query(dev,io,params,0x00010114,media,sizeof(media))!=4) return false;
    *up=r32(media)==0; *speed=0;
    if(*up) {
        if(rndis_query(dev,io,params,0x00010107,rate,sizeof(rate))!=4) return false;
        *speed=r32(rate)/10000;
    }
    return !io->failed;
}
