#include "usb_net.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t r16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static uint32_t r32(const uint8_t *p) { return r16(p)|(uint32_t)r16(p+2)<<16; }
static void w16(uint8_t *p,uint16_t n) { p[0]=n;p[1]=n>>8; }
static void w32(uint8_t *p,uint32_t n) { w16(p,n);w16(p+2,n>>16); }
static const uint8_t factory_mac[]={2,0x11,0x22,0x33,0x44,0x55};
static uint32_t message_type,message_id,oid,filter,ntb_size;
static bool wrong_id,bad_ncm;
static unsigned sleeps,empty_response,requests;
static uint8_t phy_register;
static uint16_t phy[32];
static uint8_t banks[2][65536];
static usb_net_driver_t model;
static unsigned fail_request;
static void sleep_io(void *ctx,unsigned ms) { (void)ctx; sleeps+=ms; }
static int control_io(void *ctx,uint8_t type,uint8_t request,uint16_t value,uint16_t index,void *buffer,uint16_t size)
{
    (void)ctx; uint8_t *data=buffer;
    if(++requests==fail_request) return -1;
    if(model==USB_NET_RNDIS) {
        if(type==1 && request==11) { assert(value==2 && index==7); return 0; }
        if(type==0x21 && request==0) {
            message_type=r32(data); message_id=r32(data+8); oid=size>=28?r32(data+12):0;
            if(message_type==2) assert(r32(data+20)>=4096);
            else if(message_type==5) { filter=r32(data+28); assert(r32(data+12)==0x1010e); }
            return 0;
        }
        assert(type==0xa1 && request==1); memset(data,0,size);
        if(empty_response) { --empty_response; return 0; }
        unsigned length=16;
        w32(data,message_type|0x80000000); w32(data+8,message_id+(wrong_id?1:0));
        if(message_type==2) { length=52; w32(data+16,1); w32(data+24,1); w32(data+36,4096); }
        else if(message_type==4) {
            length=oid==0x1010102?30:28; w32(data+16,length-24); w32(data+20,16);
            if(oid==0x1010102) memcpy(data+24,factory_mac,6);
            else if(oid==0x10114) w32(data+24,0);
            else { assert(oid==0x10107); w32(data+24,1000000); }
        }
        w32(data+4,length); return length;
    }
    if(model==USB_NET_NCM || model==USB_NET_ECM) {
        if(type==0x80 && request==6) {
            memset(data,0,size);
            if(value==0x300) { assert(size==4); data[0]=4;data[1]=3;w16(data+2,0x409); }
            else { assert(size==26 && index==0x409); data[0]=26;data[1]=3;const char *mac="021122334455";for(unsigned i=0;i<12;++i)data[2+i*2]=mac[i]; }
            return size;
        }
        if(type==0xa1 && request==0x80) {
            memset(data,0,size); assert(size==28); w16(data,28);w16(data+2,3);w32(data+4,32768);w32(data+16,8192);
            w16(data+20,bad_ncm?0:4);w16(data+24,4);return size;
        }
        if(request==0x43) { assert(value==15);filter=value; }
        if(request==0x86) { assert(size==4);ntb_size=r32(data); }
        assert(type==0x21 || type==1); return 0;
    }
    if(model==USB_NET_DM9601) {
        assert(type==0x40 || type==0xc0);
        if(type==0xc0) {
            memset(data,0,size);
            if(index==0x10) memcpy(data,factory_mac,6);
            else if(index==0x2c) data[0]=1;
            else if(index==0x91) data[0]=0xff;
            else if(index==0x0d) w16(data,phy[phy_register]);
            return size;
        }
        if(request==3) { if(index==0xc) phy_register=value&31; if(index==0x91) assert(value==0x7f); if(index==5) filter=value; }
        return 0;
    }
    if(model==USB_NET_AX88772) {
        if(type==0xc0) {
            memset(data,0,size);
            if(request==0x13) memcpy(data,factory_mac,6);
            else if(request==0x19) data[1]=16;
            else if(request==9) data[0]=0x21; /* AX88772B + software MII owner. */
            else if(request==7) w16(data,phy[index&31]);
            return size;
        }
        if(request==8) phy[index&31]=r16(data);
        else if(request==0x10) filter=value;
        return 0;
    }
    if(model==USB_NET_LAN95XX) {
        assert(index+size<=65536);
        if(type==0xc0) { memcpy(data,banks[0]+index,size);return size; }
        memcpy(banks[0]+index,data,size);
        if(index==0x14) banks[0][index]&=~8;
        if(index==0x20) banks[0][index]&=~0x10;
        if(index==0x114) { uint32_t mii=r32(data);unsigned reg=(mii>>6)&31;assert((mii>>11)==1);
            if(mii&2) phy[reg]=r32(banks[0]+0x118); else w32(banks[0]+0x118,phy[reg]);
            banks[0][index]&=~1;
        }
        return 0;
    }
    if(model==USB_NET_AX88179) {
        if(type==0xc0) {
            memset(data,0,size);
            if(request==1 && value==0x10) memcpy(data,factory_mac,6);
            else if(request==2 && index==0x11) w16(data,0x6400);
            return size;
        }
        if(request==1 && value==0x0b) filter=r16(data);
        return 0;
    }
    assert(model==USB_NET_RTL8152 && request==5 && value+size<=65536);
    unsigned bank=(index&0x100)?1:0;
    if(type==0xc0) { memcpy(data,banks[bank]+value,size);return size; }
    for(unsigned i=0;i<size;++i) {
        unsigned enable=(i<4?index:(i+4>=size?index>>4:0xf))&0xf;
        if(enable&(1u<<(i%4))) banks[bank][value+i]=data[i];
    }
    banks[1][0xe813]&=~0x10; /* MAC reset self-clears. */
    return 0;
}
static void reset_model(usb_net_driver_t driver)
{
    model=driver;requests=filter=sleeps=fail_request=0;wrong_id=bad_ncm=false;
    phy[0]=0x1000;phy[1]=4;phy[4]=phy[5]=0x1e1;
    memset(banks,0,sizeof(banks));
    memcpy(banks[0]+0x108,factory_mac,4);memcpy(banks[0]+0x104,factory_mac+4,2);
    memcpy(banks[1]+0xc000,factory_mac,6); w16(banks[1]+0xe612,0x4c10);
    banks[1][0xe84f]=0xff; banks[1][0xe908]=0xb;
}
int main(void)
{
    usb_net_descriptor_t dev={.driver=USB_NET_RNDIS,.endpoints={.control_interface=3,.data_interface=7,.data_alt=2}};
    usb_net_parameters_t params; uint8_t mac[6];
    reset_model(USB_NET_RNDIS); empty_response=1;
    usb_net_io_t io={.control=control_io,.sleep=sleep_io};
    assert(usb_net_class_init(&dev,&io,&params,mac)); assert(!memcmp(mac,factory_mac,6) && filter==0x2f);
    bool up,full;uint32_t speed;assert(usb_net_rndis_link(&dev,&io,&params,&up,&speed) && up && speed==100);
    reset_model(USB_NET_RNDIS);wrong_id=true;io.failed=false;
    assert(!usb_net_class_init(&dev,&io,&params,mac) && io.failed);
    reset_model(USB_NET_NCM);io.failed=false;io.full_speed=true;dev.driver=USB_NET_NCM;dev.ncm_capabilities=0x11;dev.endpoints.mac_string=4;
    assert(usb_net_class_init(&dev,&io,&params,mac));assert(ntb_size==4096 && filter==15);
    reset_model(USB_NET_NCM);bad_ncm=true;io.failed=false;assert(!usb_net_class_init(&dev,&io,&params,mac));
    for(usb_net_driver_t driver=USB_NET_AX88772;driver<=USB_NET_AX88179;++driver) {
        reset_model(driver);io.failed=false;uint16_t revision;
        assert(usb_net_native_init(driver,&io,mac,&revision) && !memcmp(mac,factory_mac,6));
        if(driver==USB_NET_AX88772 || driver==USB_NET_AX88179) assert(filter&1);
        if(driver==USB_NET_DM9601) assert(filter&2);
        if(driver==USB_NET_LAN95XX) assert(r32(banks[0]+0x100)&0x40000);
        if(driver==USB_NET_RTL8152) { assert((r32(banks[1]+0xc010)&0xf)==0xf);assert(r16(banks[0]+0xd406)&0x10);assert(!(r16(banks[1]+0xe854)&0x40)); }
        unsigned successful_requests=requests;
        phy[0]=0x1000;phy[4]=phy[5]=0x1e1;
        assert(usb_net_native_link(driver,&io,&up,&speed,&full) && up && speed==100 && full);
        for(unsigned fail=1;fail<=successful_requests && fail<40;++fail) {
            reset_model(driver);io.failed=false;fail_request=fail;
            assert(!usb_net_native_init(driver,&io,mac,&revision) && io.failed && requests==fail);
        }
    }
    reset_model(USB_NET_RTL8152);io.failed=false;w16(banks[1]+0xe612,0x5c30);uint16_t rev;
    assert(!usb_net_native_init(USB_NET_RTL8152,&io,mac,&rev) && rev==0x5c30); /* RTL8153 must use its class-mode fallback. */
    puts("RNDIS/NCM negotiation, native initialization/link, promiscuous RX and control failure propagation passed");
}
