/* AX88772 reset sequencing references CherryUSB usbh_asix.c,
 * Copyright (c) 2024 sakumisu, Apache-2.0 (see vendor/LICENSE-CHERRYUSB).
 * Modified: checked transport, bounded polling, media negotiation, and the
 * additional project-native adapter register implementations below.
 */
#include "usb_net.h"
#include <string.h>

static uint8_t s_ax_phy;
static uint32_t s_last_media;
static uint16_t r16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static uint32_t r32(const uint8_t *p) { return r16(p)|(uint32_t)r16(p+2)<<16; }
static void w16(uint8_t *p,uint16_t n) { p[0]=n;p[1]=n>>8; }
static void w32(uint8_t *p,uint32_t n) { w16(p,n);w16(p+2,n>>16); }

static bool mac_valid(const uint8_t *mac)
{
    static const uint8_t zero[6];
    return !(mac[0]&1) && memcmp(mac,zero,6);
}
static uint16_t ax_mii(usb_net_io_t *io, uint8_t reg, bool write, uint16_t value)
{
    uint8_t status=0, data[2]={0};
    for(unsigned tries=0;tries<10 && !io->failed;++tries) {
        usb_net_control(io,0x40,6,0,0,NULL,0); usb_net_sleep(io,1);
        usb_net_control(io,0xc0,9,0,0,&status,1);
        if(status&1) break;
    }
    if(!(status&1)) { io->failed=true; return 0; }
    w16(data,value);
    usb_net_control(io,write?0x40:0xc0,write?8:7,s_ax_phy,reg,data,2);
    usb_net_control(io,0x40,0x0a,0,0,NULL,0);
    return r16(data);
}
static bool ax_init(usb_net_io_t *io,uint8_t mac[6],uint16_t *revision)
{
    uint8_t phy[2]={0},code=0;
    usb_net_control(io,0xc0,0x13,0,0,mac,6);
    usb_net_control(io,0xc0,0x19,0,0,phy,2); s_ax_phy=phy[1]&31;
    usb_net_control(io,0xc0,9,0,0,&code,1); code&=0x70; *revision=code;
    if(io->failed || !mac_valid(mac) || (code!=0 && code!=0x10 && code!=0x20)) return false;
    bool internal=s_ax_phy==16;
    usb_net_control(io,0x40,0x1f,code?0x80:0xb0,0,NULL,0); usb_net_sleep(io,5);
    usb_net_control(io,0x40,0x22,internal|(code?0x10:0),0,NULL,0);
    usb_net_control(io,0x40,0x20,code?0x60:0x40,0,NULL,0); usb_net_sleep(io,10);
    usb_net_control(io,0x40,0x20,code?0x20:0,0,NULL,0); usb_net_sleep(io,code?160:60);
    if(code) {
        usb_net_control(io,0x40,0x20,0,0,NULL,0);
        usb_net_control(io,0x40,0x20,0x20,0,NULL,0); usb_net_sleep(io,200);
        if(code==0x20) usb_net_control(io,0x40,0x2a,0x8000,0x8001,NULL,0);
        else {
            ax_mii(io,0x14,true,0x442c); ax_mii(io,0x15,true,0x03c8); ax_mii(io,0x16,true,0x4044);
        }
    } else {
        usb_net_control(io,0x40,0x20,internal?0x28:0x48,0,NULL,0); usb_net_sleep(io,150);
    }
    usb_net_control(io,0x40,0x12,0x0c15,0x12,NULL,0);
    usb_net_control(io,0x40,0x14,0,0,mac,6);
    ax_mii(io,0,true,0x8000); usb_net_sleep(io,100);
    ax_mii(io,4,true,0x01e1); ax_mii(io,0,true,0x1200);
    /* Start operation, broadcast, all multicast, promiscuous. No checksum offload. */
    usb_net_control(io,0x40,0x10,0x008b,0,NULL,0);
    s_last_media=UINT32_MAX;
    return !io->failed;
}

static uint32_t lan_read(usb_net_io_t *io,uint16_t reg)
{
    uint8_t data[4]={0}; usb_net_control(io,0xc0,0xa1,0,reg,data,4); return r32(data);
}
static void lan_write(usb_net_io_t *io,uint16_t reg,uint32_t value)
{
    uint8_t data[4]; w32(data,value); usb_net_control(io,0x40,0xa0,0,reg,data,4);
}
static bool lan_wait(usb_net_io_t *io,uint16_t reg,uint32_t mask)
{
    for(unsigned i=0;i<100 && !io->failed;++i) {
        if(!(lan_read(io,reg)&mask)) return true;
        usb_net_sleep(io,5);
    }
    io->failed=true; return false;
}
static uint16_t lan_mii(usb_net_io_t *io,uint8_t reg,bool write,uint16_t value)
{
    if(!lan_wait(io,0x114,1)) return 0;
    if(write) lan_write(io,0x118,value);
    lan_write(io,0x114,(1u<<11)|((uint32_t)reg<<6)|(write?3:1));
    if(!lan_wait(io,0x114,1)) return 0;
    return write?value:lan_read(io,0x118);
}
static bool lan_init(usb_net_io_t *io,uint8_t mac[6],uint16_t *revision)
{
    uint32_t id=lan_read(io,0); *revision=id&0xffff;
    lan_write(io,0x14,8); if(!lan_wait(io,0x14,8)) return false;
    lan_write(io,0x20,0x10); if(!lan_wait(io,0x20,0x10)) return false;
    uint32_t low=lan_read(io,0x108),high=lan_read(io,0x104);
    w32(mac,low); w16(mac+4,high);
    if(!mac_valid(mac)) return false;
    /* Single-frame USB RX, zero offset and no RX checksum trailer. */
    lan_write(io,0x14,0x1000); lan_write(io,0x38,0); lan_write(io,0x6c,0);
    lan_write(io,0x130,0); lan_write(io,0x120,0x8100);
    lan_write(io,0x2c,0x00f830a1); lan_write(io,0x11c,0); lan_write(io,8,UINT32_MAX);
    lan_mii(io,0,true,0x8000); usb_net_sleep(io,100);
    lan_mii(io,4,true,0x01e1); lan_mii(io,0,true,0x1200);
    lan_write(io,0x100,0x000c000c); /* Promiscuous, all multicast, RX and TX. */
    lan_write(io,0x10,4); s_last_media=UINT32_MAX;
    return !io->failed;
}

static uint8_t dm_read(usb_net_io_t *io,uint8_t reg)
{
    uint8_t n=0; usb_net_control(io,0xc0,0,0,reg,&n,1); return n;
}
static void dm_write(usb_net_io_t *io,uint8_t reg,uint8_t n)
{
    usb_net_control(io,0x40,3,n,reg,NULL,0);
}
static uint16_t dm_mii(usb_net_io_t *io,uint8_t reg,bool write,uint16_t n)
{
    uint8_t data[2]; w16(data,n);
    if(write) usb_net_control(io,0x40,1,0,0x0d,data,2);
    dm_write(io,0x0c,reg|0x40); dm_write(io,0x0b,write?0x1a:0x0c);
    bool ready=false;
    for(unsigned i=0;i<100 && !io->failed;++i) {
        if(!(dm_read(io,0x0b)&1)) { ready=true; break; }
        usb_net_sleep(io,1);
    }
    if(!ready) io->failed=true;
    dm_write(io,0x0b,0);
    if(!write) usb_net_control(io,0xc0,0,0,0x0d,data,2);
    return r16(data);
}
static bool dm_init(usb_net_io_t *io,uint8_t mac[6],uint16_t *revision)
{
    dm_write(io,0,1); usb_net_sleep(io,10);
    usb_net_control(io,0xc0,0,0,0x10,mac,6);
    uint8_t id=dm_read(io,0x2c); *revision=id;
    if(id==1) dm_write(io,0x91,dm_read(io,0x91)&0x7f); /* DM962x in DM9601 frame mode. */
    dm_write(io,0x1e,1); dm_write(io,0x1f,0);
    dm_mii(io,0,true,0x8000); usb_net_sleep(io,100);
    dm_mii(io,4,true,0x01e1); dm_mii(io,0,true,0x1200);
    dm_write(io,5,0x33); /* RX enable and promiscuous; receive broadcast. */
    return !io->failed && mac_valid(mac);
}

/* AX88179/178A register transport. Keep aggregates at 2 KiB so both FS and
 * HS hosts can receive a complete block; never select vendor mode over CDC. */
static uint16_t axg_phy(usb_net_io_t *io,uint8_t reg,bool write,uint16_t n)
{
    uint8_t data[2]; w16(data,n);
    usb_net_control(io,write?0x40:0xc0,2,3,reg,data,2); return r16(data);
}
static void axg_write(usb_net_io_t *io,uint16_t reg,uint32_t n,unsigned size)
{
    uint8_t data[4];w32(data,n);usb_net_control(io,0x40,1,reg,size,data,size);
}
static bool axg_init(usb_net_io_t *io,uint8_t mac[6],uint16_t *revision)
{
    axg_write(io,0x26,0,2);axg_write(io,0x26,0x20,2);usb_net_sleep(io,500);
    axg_write(io,0x33,3,1);usb_net_sleep(io,200);
    usb_net_control(io,0xc0,1,0x10,6,mac,6);
    uint8_t code=0;usb_net_control(io,0xc0,1,3,1,&code,1);*revision=code;
    if(io->failed || !mac_valid(mac)) return false;
    uint8_t queue[5]={7,0x20,3,0,0xff};
    usb_net_control(io,0x40,1,0x2e,5,queue,5);
    axg_write(io,0x55,0x34,1);axg_write(io,0x54,0x52,1);
    axg_write(io,0x34,0,1);axg_write(io,0x35,0,1); /* No checksum/TSO changes to forwarded packets. */
    axg_write(io,0x0b,0x38b,2); /* Two-byte IP alignment header, promiscuous, broadcast/multicast. */
    axg_write(io,0x24,0,1);axg_write(io,0x22,0x10b,2);
    axg_phy(io,4,true,0x1e1);axg_phy(io,9,true,0x200);axg_phy(io,0,true,0x1200);
    s_last_media=UINT32_MAX;
    return !io->failed;
}
static bool axg_link(usb_net_io_t *io,bool *up,uint32_t *speed,bool *full)
{
    uint16_t status=axg_phy(io,0x11,false,0);
    if(status==0xffff) { io->failed=true; io->error="AX88179 PHY returned invalid all-ones status"; return false; }
    *up=(status&0x400)!=0;
    if(!*up) s_last_media=UINT32_MAX;
    *full=(status&0x2000)!=0;
    *speed=*up?((status&0xc000)==0x8000?1000:(status&0xc000)==0x4000?100:10):0;
    uint32_t media=*speed|(*full?0x10000:0);
    if(*up && media!=s_last_media) {
        axg_write(io,0x22,0x100|(*speed==1000?9:*speed==100?0x200:0)|(*full?2:0),2);
        s_last_media=media;
    }
    return !io->failed;
}

bool usb_net_native_init(usb_net_driver_t driver,usb_net_io_t *io,uint8_t mac[6],uint16_t *revision)
{
    if(driver==USB_NET_AX88179) return axg_init(io,mac,revision);
    if(driver==USB_NET_AX88772) return ax_init(io,mac,revision);
    if(driver==USB_NET_LAN95XX) return lan_init(io,mac,revision);
    if(driver==USB_NET_DM9601) return dm_init(io,mac,revision);
    if(driver==USB_NET_RTL8152) return usb_net_rtl8152_init(io,mac,revision);
    return false;
}

bool usb_net_native_link(usb_net_driver_t driver,usb_net_io_t *io,bool *up,uint32_t *speed,bool *full)
{
    if(driver==USB_NET_AX88179) return axg_link(io,up,speed,full);
    if(driver==USB_NET_RTL8152) return usb_net_rtl8152_link(io,up,speed,full);
    uint16_t status,control,adv,partner;
    if(driver==USB_NET_AX88772) {
        ax_mii(io,1,false,0); status=ax_mii(io,1,false,0);
        control=ax_mii(io,0,false,0); adv=ax_mii(io,4,false,0); partner=ax_mii(io,5,false,0);
    } else if(driver==USB_NET_LAN95XX) {
        lan_mii(io,1,false,0); status=lan_mii(io,1,false,0);
        control=lan_mii(io,0,false,0); adv=lan_mii(io,4,false,0); partner=lan_mii(io,5,false,0);
    } else if(driver==USB_NET_DM9601) {
        dm_mii(io,1,false,0); status=dm_mii(io,1,false,0);
        control=dm_mii(io,0,false,0); adv=dm_mii(io,4,false,0); partner=dm_mii(io,5,false,0);
    } else return false;
    if(status==0xffff) { io->failed=true; io->error="PHY returned invalid all-ones status"; return false; }
    *up=(status&4)!=0; *speed=0; *full=false;
    if(io->failed || !*up) { s_last_media=UINT32_MAX; return !io->failed; }
    if(control&0x1000) {
        uint16_t common=adv&partner;
        *speed=(common&0x180)?100:10;
        *full=(common&0x100) || (!(common&0x80) && (common&0x40));
    } else { *speed=(control&0x2000)?100:10; *full=(control&0x100)!=0; }
    uint32_t media=*speed|(*full?0x10000:0);
    if(media!=s_last_media) {
        if(driver==USB_NET_AX88772) usb_net_control(io,0x40,0x1b,0x104|(*speed==100?0x200:0)|(*full?2:0),0,NULL,0);
        else if(driver==USB_NET_LAN95XX) lan_write(io,0x100,0xc000c|(*full?0x100000:0x800000));
        s_last_media=media;
    }
    return !io->failed;
}
