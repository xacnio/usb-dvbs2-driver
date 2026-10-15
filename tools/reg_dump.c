/* reg_dump.c - dump the register map of one chip (for identification)
 *
 * In the demod_id scan, bus 3 / addr 0x14 answered for real (not an echo).
 * This confirms the real chip by checking whether the registers vary with
 * the value written, and extracts a chip id or signature. */
#include <stdio.h>
#include <stdlib.h>
#include "usb_backend.h"
#include "it9300.h"
#include "tool_util.h"

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
    const dtv_hardware_profile *profile=open_connected_device(&usb);
    if(!profile) return 2;
    it9300_attach(&b,usb);
    it9300_identify(&b);
    if(it9300_query_fw_version(&b)!=0&&
       load_file_to_bridge(&b,profile->bridge_firmware,0)!=0){fprintf(stderr,"firmware missing or SHA-256 invalid\n");return 3;}
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
