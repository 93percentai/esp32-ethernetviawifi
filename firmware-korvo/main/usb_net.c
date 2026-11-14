#include "usb_net.h"
#include <string.h>

static uint16_t r16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t r32(const uint8_t *p) { return r16(p) | (uint32_t)r16(p+2) << 16; }
static void w16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void w32(uint8_t *p, uint32_t v) { w16(p,v); w16(p+2,v>>16); }
static bool frame_size_valid(size_t n) { return n>=14 && n<=BRIDGE_FRAME_MAX; }
static bool range(size_t offset, size_t length, size_t size) { return offset <= size && length <= size-offset; }

const char *usb_net_driver_name(usb_net_driver_t d)
{
    static const char *const names[]={"unsupported","CDC ECM","CDC NCM16","RNDIS","ASIX AX88772","Realtek RTL8152","Davicom DM96xx","Microchip LAN95xx","ASIX AX88179/178A"};
    return (unsigned)d < sizeof(names)/sizeof(*names) ? names[d] : "unknown";
}
int usb_net_driver_priority(usb_net_driver_t d)
{
    return d==USB_NET_ECM ? 40 : d==USB_NET_NCM ? 30 : d==USB_NET_RNDIS ? 20 : d!=USB_NET_NONE ? 10 : 0;
}

usb_net_driver_t usb_net_native_match(uint16_t vid, uint16_t pid)
{
    /* Match exact measured chipset IDs, including OEM IDs documented upstream.
     * Never probe arbitrary vendor-specific interfaces with register writes. */
    static const struct { uint16_t vid,pid; usb_net_driver_t driver; } ids[]={
        {0x0b95,0x7720,USB_NET_AX88772},{0x0b95,0x772a,USB_NET_AX88772},
        {0x0b95,0x772b,USB_NET_AX88772},{0x0b95,0x7e2b,USB_NET_AX88772},
        {0x05ac,0x1402,USB_NET_AX88772},{0x17ef,0x7203,USB_NET_AX88772},
        {0x13b1,0x0018,USB_NET_AX88772},{0x1557,0x7720,USB_NET_AX88772},
        {0x07d1,0x3c05,USB_NET_AX88772},{0x2001,0x3c05,USB_NET_AX88772},
        {0x2001,0x1a02,USB_NET_AX88772},{0x0db0,0xa877,USB_NET_AX88772},
        {0x0b95,0x1790,USB_NET_AX88179},{0x0b95,0x178a,USB_NET_AX88179},
        {0x04b4,0x3610,USB_NET_AX88179},{0x2001,0x4a00,USB_NET_AX88179},
        {0x0df6,0x0072,USB_NET_AX88179},{0x04e8,0xa100,USB_NET_AX88179},
        {0x17ef,0x304b,USB_NET_AX88179},{0x050d,0x0128,USB_NET_AX88179},
        {0x0930,0x0a13,USB_NET_AX88179},{0x0711,0x0179,USB_NET_AX88179},
        {0x07c9,0x000e,USB_NET_AX88179},{0x07c9,0x000f,USB_NET_AX88179},
        {0x07c9,0x0010,USB_NET_AX88179},
        {0x0bda,0x8152,USB_NET_RTL8152},
        {0x07aa,0x9601,USB_NET_DM9601},{0x0a46,0x9601,USB_NET_DM9601},
        {0x0a46,0x6688,USB_NET_DM9601},{0x0a46,0x0268,USB_NET_DM9601},
        {0x0a46,0x8515,USB_NET_DM9601},{0x0a47,0x9601,USB_NET_DM9601},
        {0x0fe6,0x8101,USB_NET_DM9601},{0x0a46,0x9000,USB_NET_DM9601},
        {0x0a46,0x9620,USB_NET_DM9601},{0x0a46,0x9621,USB_NET_DM9601},
        {0x0a46,0x9622,USB_NET_DM9601},{0x0a46,0x0269,USB_NET_DM9601},
        {0x0a46,0x1269,USB_NET_DM9601},
    };
    for (size_t i=0;i<sizeof(ids)/sizeof(*ids);++i)
        if (vid==ids[i].vid && pid==ids[i].pid) return ids[i].driver;
    if (vid==0x0424 && (pid==0x9500 || pid==0x9505 || pid==0x9e00 || pid==0x9e01 ||
        pid==0xec00 || pid==0x9530 || pid==0x9730 || pid==0x9e08 || (pid>=0x9900 && pid<=0x9909))) return USB_NET_LAN95XX;
    return USB_NET_NONE;
}

static bool endpoint_pair(const uint8_t *d, size_t end, size_t start, ecm_descriptor_t *e)
{
    e->bulk_in=e->bulk_out=0;
    for (size_t p=start+d[start];p<end && d[p+1]!=4;p+=d[p]) {
        if (d[p+1]!=5 || d[p]<7) continue;
        uint8_t ep=d[p+2], kind=d[p+3]&3; uint16_t mps=r16(d+p+4)&0x7ff;
        if (!(ep&15) || (ep&0x70) || !mps || mps>1024) return false;
        if (kind==2) {
            if (mps>512 || (mps&(mps-1))) return false;
            if (ep&0x80) { if(e->bulk_in) return false; e->bulk_in=ep; e->in_mps=mps; }
            else { if(e->bulk_out) return false; e->bulk_out=ep; e->out_mps=mps; }
        } else if (kind==3 && (ep&0x80)) { e->notification=ep; e->notification_mps=mps; }
    }
    return e->bulk_in && e->bulk_out;
}

bool usb_net_parse_config(const uint8_t *d, size_t n, uint16_t vid, uint16_t pid, usb_net_descriptor_t *out)
{
    if (!d || !out || n<9 || d[0]<9 || d[1]!=2) return false;
    size_t end=r16(d+2);
    if(end<d[0] || end>n) return false;
    for(size_t p=0;p<end;) {
        if(end-p<2 || d[p]<2 || d[p]>end-p) return false;
        p+=d[p];
    }
    usb_net_descriptor_t best={0};
    for(size_t c=d[0];c<end;c+=d[c]) {
        if(d[c+1]!=4 || d[c]<9 || d[c+3]) continue;
        uint8_t cls=d[c+5], sub=d[c+6], proto=d[c+7];
        usb_net_descriptor_t candidate={.configuration=d[5]};
        ecm_descriptor_t *e=&candidate.endpoints;
        e->control_interface=d[c+2]; e->data_interface=e->control_interface;
        e->max_segment=BRIDGE_FRAME_MAX;
        if(cls==2 && sub==6 && !proto) candidate.driver=USB_NET_ECM;
        else if(cls==2 && sub==0x0d && !proto) candidate.driver=USB_NET_NCM;
        else if((cls==0xe0 && sub==1 && proto==3) || (cls==0xef && sub==4 && proto==1) ||
                (cls==2 && sub==2 && proto==0xff)) candidate.driver=USB_NET_RNDIS;
        else if(cls==0xff) candidate.driver=usb_net_native_match(vid,pid);
        if(!candidate.driver) continue;
        if(candidate.driver>=USB_NET_AX88772) {
            e->data_alt=d[c+3];
            if(!endpoint_pair(d,end,c,e)) continue;
        } else {
            bool paired=false;
            for(size_t p=c+d[c];p<end && d[p+1]!=4;p+=d[p]) {
                if(d[p+1]==0x24 && d[p]>=3) {
                    if(d[p+2]==6 && d[p]==5 && d[p+3]==e->control_interface) { e->data_interface=d[p+4]; paired=true; }
                    else if(d[p+2]==0xf && d[p]>=13) { e->mac_string=d[p+3]; e->max_segment=r16(d+p+8); }
                    else if(d[p+2]==0x1a && d[p]>=6) candidate.ncm_capabilities=d[p+5];
                } else if(d[p+1]==5 && d[p]>=7 && (d[p+2]&0x80) && (d[p+3]&3)==3) {
                    e->notification=d[p+2]; e->notification_mps=r16(d+p+4)&0x7ff;
                }
            }
            /* RNDIS often omits Union: pair only through an explicit two-interface IAD. */
            if(!paired && candidate.driver==USB_NET_RNDIS) {
                for(size_t p=d[0];p<end;p+=d[p]) if(d[p+1]==11 && d[p]>=8 && d[p+2]==e->control_interface && d[p+3]==2) {
                    e->data_interface=e->control_interface+1; paired=true;
                }
            }
            if(!paired || e->data_interface==e->control_interface || e->max_segment<BRIDGE_FRAME_MAX) continue;
            if(candidate.driver!=USB_NET_RNDIS && !e->mac_string) continue;
            bool found=false;
            for(size_t p=d[0];p<end;p+=d[p]) if(d[p+1]==4 && d[p]>=9 && d[p+2]==e->data_interface && d[p+5]==10) {
                e->data_alt=d[p+3]; if(endpoint_pair(d,end,p,e)) { found=true; break; }
            }
            if(!found) continue;
        }
        if(e->notification && (!(e->notification&15) || (e->notification&0x70) || e->notification_mps<8 || e->notification_mps>1024)) continue;
        if(usb_net_driver_priority(candidate.driver)>usb_net_driver_priority(best.driver)) best=candidate;
    }
    if(!best.driver) return false;
    *out=best; return true;
}

bool usb_net_ncm_parameters(const uint8_t *d, size_t n, usb_net_parameters_t *out)
{
    if(!d || !out || n<28 || r16(d)<28 || !(r16(d+2)&1)) return false;
    uint32_t in=r32(d+4), max=r32(d+16);
    uint16_t divisor=r16(d+20), rem=r16(d+22), alignment=r16(d+24);
    if(in<2048 || max<2048 || !divisor || divisor>4096 || rem>=divisor || !alignment || alignment>4096 || (alignment&(alignment-1))) return false;
    out->in_max=in>16384 ? 16384 : in;
    out->out_max=max>16384 ? 16384 : max;
    out->out_divisor=divisor; out->out_remainder=(rem+divisor-(14%divisor))%divisor; out->out_alignment=alignment;
    size_t payload=((12+alignment-1)&~((size_t)alignment-1))+16;
    while(payload%divisor!=out->out_remainder) ++payload;
    return payload+BRIDGE_FRAME_MAX<=out->out_max;
}

static int decode_block(usb_net_driver_t driver, const uint8_t *d, size_t n, usb_net_frame_cb_t cb, void *ctx)
{
    int count=0;
    if(driver==USB_NET_ECM) {
        if(!frame_size_valid(n)) return -1;
        if(cb && !cb(d,n,ctx)) return -1;
        return 1;
    }
    if(driver==USB_NET_AX88179) {
        if(n<8) return -1;
        uint32_t tail=r32(d+n-4); size_t entries=tail&0xffff, table=tail>>16, offset=0;
        if(!entries || entries>512 || (table&3) || !range(table,entries*4,n-4)) return -1;
        for(size_t i=0;i<entries;++i) {
            uint32_t meta=r32(d+table+i*4); size_t size=(meta>>16)&0x1fff;
            if(!size) continue; /* Optional dummy metadata entry. */
            size_t padded=(size+7)&~7u;
            if(size<16 || !range(offset,padded,table) || (meta&0xa0000000) || !frame_size_valid(size-2)) return -1;
            offset+=padded; ++count;
        }
        offset=0;
        for(size_t i=0;i<entries;++i) {
            size_t size=(r32(d+table+i*4)>>16)&0x1fff;
            if(!size) continue;
            if(cb && !cb(d+offset+2,size-2,ctx)) return -1;
            offset+=(size+7)&~7u;
        }
        return count;
    }
    if(driver==USB_NET_NCM) {
        if(n<12 || r32(d)!=0x484d434e || r16(d+4)!=12) return -1;
        size_t block=r16(d+8), ndp=r16(d+10);
        if(block<12 || block>n || !ndp) return -1;
        /* An NDP16 chain and all datagrams must fit; bounded iterations reject cycles. */
        size_t visited[16]; uint16_t table_lengths[16], offsets[256], sizes[256]; unsigned tables=0;
        while(ndp) {
            if(tables==16 || ndp<12 || (ndp&3) || !range(ndp,12,block)) return -1;
            for(unsigned i=0;i<tables;++i) if(visited[i]==ndp) return -1;
            visited[tables++]=ndp;
            if(r32(d+ndp)!=0x304d434e) return -1; /* NCM0: CRC mode explicitly disabled. */
            size_t length=r16(d+ndp+4); bool terminated=false;
            if(length<12 || (length&3) || !range(ndp,length,block)) return -1;
            for(size_t p=ndp+8;p<ndp+length;p+=4) {
                size_t offset=r16(d+p), size=r16(d+p+2);
                if(!offset && !size) { terminated=true; break; }
                if(!offset || !size || offset<12 || !range(offset,size,block) ||
                   (offset<ndp+length && ndp<offset+size) || !frame_size_valid(size)) return -1;
                if(count==256) return -1;
                offsets[count]=offset; sizes[count]=size; ++count;
            }
            if(!terminated) return -1;
            table_lengths[tables-1]=length;
            ndp=r16(d+ndp+6);
        }
        for(int i=0;i<count;++i) {
            for(unsigned j=0;j<tables;++j)
                if(offsets[i]<visited[j]+table_lengths[j] && visited[j]<(size_t)offsets[i]+sizes[i]) return -1;
            for(int j=0;j<i;++j)
                if(offsets[i]<(size_t)offsets[j]+sizes[j] && offsets[j]<(size_t)offsets[i]+sizes[i]) return -1;
        }
        for(int i=0;i<count;++i) if(cb && !cb(d+offsets[i],sizes[i],ctx)) return -1;
        return count;
    }
    for(size_t p=0;p<n;) {
        size_t size=0, offset=0, total=0, alignment=1;
        if(driver==USB_NET_RNDIS) {
            if(n-p<44 || r32(d+p)!=1) return -1;
            total=r32(d+p+4); offset=(size_t)r32(d+p+8)+8; size=r32(d+p+12);
            if(total<44 || total>n-p || offset<44 || !range(offset,size,total) ||
               r32(d+p+16) || r32(d+p+20) || r32(d+p+24) || r32(d+p+28) || r32(d+p+32)) return -1;
        } else if(driver==USB_NET_AX88772) {
            if(n-p<4) return -1;
            size=r16(d+p)&0x7ff; offset=4; total=offset+size; alignment=2;
            if(size!=(uint16_t)(~r16(d+p+2)&0x7ff)) return -1;
            if(!size && n-p==4) return count; /* ASIX termination word. */
        } else if(driver==USB_NET_RTL8152) {
            if(n-p<24) return -1;
            uint32_t status=r32(d+p); size=status&0x7fff;
            if((status&0x8000) || size<4) return -1;
            offset=24; total=offset+size; size-=4; alignment=8; /* Strip Ethernet FCS. */
        } else if(driver==USB_NET_DM9601) {
            if(n-p<3 || (d[p]&0xbf)) return -1;
            size=r16(d+p+1); if(size<4) return -1;
            offset=3; total=offset+size; size-=4;
        } else if(driver==USB_NET_LAN95XX) {
            if(n-p<4) return -1;
            uint32_t status=r32(d+p); size=(status>>16)&0x3fff;
            if((status&0x8000) || size<4) return -1;
            offset=4; total=offset+size; size-=4; alignment=4;
        } else return -1;
        if(!range(p,total,n) || !frame_size_valid(size)) return -1;
        /* DM96xx uses one frame per transfer; DM962x can append up to four
         * padding bytes after its declared payload/FCS. They are not another frame. */
        if(driver==USB_NET_DM9601) {
            if(n-p-total>4) return -1;
            if(cb && !cb(d+p+offset,size,ctx)) return -1;
            return count+1;
        }
        if(cb && !cb(d+p+offset,size,ctx)) return -1;
        ++count;
        size_t padded=(total+alignment-1)&~(alignment-1);
        if(padded>n-p) p=n;
        else p+=padded;
    }
    return count;
}

int usb_net_decode(usb_net_driver_t driver, const uint8_t *data, size_t size, usb_net_frame_cb_t cb, void *ctx)
{
    if(!data || !size) return -1;
    int valid=decode_block(driver,data,size,NULL,NULL);
    return valid<0 || !cb ? valid : decode_block(driver,data,size,cb,ctx);
}

size_t usb_net_encode(usb_net_driver_t driver, usb_net_parameters_t *params, uint16_t mps,
                      const void *frame, size_t n, uint8_t *out, size_t capacity)
{
    if(driver<=USB_NET_NONE || driver>USB_NET_AX88179) return 0;
    if(!frame || !out || n<14 || n>BRIDGE_FRAME_MAX || !mps) return 0;
    size_t offset=0, total=n;
    if(driver==USB_NET_NCM) {
        if(!params || !params->out_divisor || !params->out_alignment) return 0;
        offset=28;
        while(offset % params->out_divisor != params->out_remainder) ++offset;
        total=offset+n;
        if(total>capacity || total>params->out_max || total>65535) return 0;
        memset(out,0,total); w32(out,0x484d434e); w16(out+4,12); w16(out+6,params->sequence++);
        w16(out+8,total); w16(out+10,12); w32(out+12,0x304d434e); w16(out+16,16);
        w16(out+20,offset); w16(out+22,n);
        /* NDP starts at 12; move it if the device requires a larger alignment. */
        if(12 % params->out_alignment) {
            size_t ndp=(12+params->out_alignment-1)&~((size_t)params->out_alignment-1);
            offset=ndp+16; while(offset % params->out_divisor != params->out_remainder) ++offset;
            total=offset+n;
            if(total>capacity || total>params->out_max || total>65535) return 0;
            memset(out,0,total); w32(out,0x484d434e); w16(out+4,12); w16(out+6,params->sequence-1);
            w16(out+8,total); w16(out+10,ndp); w32(out+ndp,0x304d434e); w16(out+ndp+4,16);
            w16(out+ndp+8,offset); w16(out+ndp+10,n);
        }
    } else {
        offset=driver==USB_NET_RNDIS ? 44 : driver==USB_NET_AX88772 ? 4 :
               (driver==USB_NET_RTL8152 || driver==USB_NET_LAN95XX || driver==USB_NET_AX88179) ? 8 : driver==USB_NET_DM9601 ? 2 : 0;
        total=offset+n;
        if(driver==USB_NET_RNDIS && params && params->out_alignment) {
            total=(total+params->out_alignment-1)&~((size_t)params->out_alignment-1);
            if(params->out_max && total>params->out_max) return 0;
        }
        if(driver==USB_NET_AX88772 && total%mps==0) total+=4;
        if(driver==USB_NET_DM9601) while((total&1) || total%mps==0) ++total;
        if(total>capacity) return 0;
        memset(out,0,total);
        if(driver==USB_NET_RNDIS) { w32(out,1); w32(out+4,total); w32(out+8,36); w32(out+12,n); }
        else if(driver==USB_NET_AX88772) { w16(out,n); w16(out+2,~n); if(total>offset+n) w16(out+total-2,0xffff); }
        else if(driver==USB_NET_RTL8152) w32(out,n|0xc0000000);
        else if(driver==USB_NET_AX88179) { w32(out,n); if((n+8)%mps==0) w32(out+4,0x80008000); }
        else if(driver==USB_NET_DM9601) w16(out,total-2);
        else if(driver==USB_NET_LAN95XX) { w32(out,n|0x3000); w32(out+4,n); }
    }
    memcpy(out+offset,frame,n); return total;
}

int usb_net_control(usb_net_io_t *io, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
    if(io->failed) { if((type&0x80) && data) memset(data,0,len); return -1; }
    int count=io->control(io->context,type,req,val,idx,data,len);
    if(count<0 || ((type&0x80) && count!=len)) { io->failed=true; io->error=count<0?"USB control transfer failed":"Short USB control reply"; return -1; }
    return count;
}
void usb_net_sleep(usb_net_io_t *io, unsigned ms) { if(!io->failed) io->sleep(io->context,ms); }
