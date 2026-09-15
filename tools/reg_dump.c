/* reg_dump.c - dump the register map of one chip (for identification)
 *
 * In the demod_id scan, bus 3 / addr 0x14 answered for real (not an echo).
 * This confirms the real chip by checking whether the registers vary with
 * the value written, and extracts a chip id or signature. */
#include <stdio.h>
#include <stdlib.h>
#include "usb_backend.h"
#include "it9300.h"
#include "firmware_verify.h"

#define UDTV_VID 0x048D
#define UDTV_PID 0xF036
#define DEFAULT_FW "../firmware/hiremco-it9303.fw"

static uint8_t *read_file(const char *p, size_t *sz){
    FILE *f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<=0){fclose(f);return NULL;}
    uint8_t *b=malloc(n);
    if(b&&fread(b,1,n,f)!=(size_t)n){free(b);b=NULL;}
    fclose(f); if(b)*sz=(size_t)n; return b;
}

static void dump(it9300 *b, uint8_t bus, uint8_t addr){
    b->i2c_bus = bus;
    printf("--- bus=0x%02x addr=0x%02x (echo=0x%02x) reg[0x00..0x1f] ---\n",
           bus, addr, (addr<<1)|1);
    printf("   ");
    int varying=0; uint8_t first=0xAA;
    for(int r=0;r<0x20;r++){
        uint8_t rp=(uint8_t)r, v=0;
        int rc=it9300_i2c_wr_rd(b,addr,&rp,1,&v,1);
        if(rc!=0){ printf("-- "); continue; }
        if(r==0) first=v;
        else if(v!=first) varying=1;
        printf("%02x ", v);
    }
    printf("\n   -> %s\n\n", varying ? "REGISTERS VARY = REAL CHIP" : "constant = echo/empty");
}

int main(void)
{
    dtv_usb *usb=NULL; it9300 b;
    if(dtv_usb_open(&usb,UDTV_VID,UDTV_PID)!=0){fprintf(stderr,"could not open device\n");return 2;}
    it9300_attach(&b,usb);
    it9300_identify(&b);
    if(it9300_query_fw_version(&b)!=0){
        size_t sz=0; uint8_t *fw=read_file(DEFAULT_FW,&sz);
        if(!fw||dtv_firmware_verify(DTV_FIRMWARE_IT9303,fw,sz)!=0||
           it9300_download_firmware(&b,fw,sz)!=0){fprintf(stderr,"firmware missing or SHA-256 invalid\n");free(fw);return 3;}
        free(fw);
    }
    it9300_bridge_init(&b);
    it9300_gpio_power_cycle(&b,0);

    dump(&b, 0x03, 0x14);   /* demod_id candidate */
    dump(&b, 0x03, 0x0C);   /* Ghidra tuner RDA5815 */
    dump(&b, 0x02, 0x14);
    dump(&b, 0x03, 0x68);
    dump(&b, 0x03, 0x34);
    dtv_usb_close(usb);
    return 0;
}
