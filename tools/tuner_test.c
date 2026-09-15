/* tuner_test.c - REAL chip detection by write-read-back
 *
 * Ghidra finding: the tuner is an RDA5815M, bus=3, I2C addr 0x0C (7-bit) /
 * 0x18 (8-bit), with a register-write [reg,val] protocol. To tell a real
 * chip from an echo (a read-only bus returns addr<<1|1) it does a write-
 * read-back:
 *   - write a known value into a scratch register
 *   - read it back; if the value stuck the chip is REAL, otherwise it is an
 *     echo or nothing at all
 *
 * Every GPIO is power-cycled and buses 3/1/2 are scanned across all 7-bit
 * addresses. Volatile register writes (user approved). */
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

/* write reg=val, then read reg back. A real chip keeps val. */
static int wrb(it9300 *b, uint8_t addr, uint8_t reg, uint8_t val, uint8_t *back)
{
    uint8_t w[2] = { reg, val };
    if (it9300_i2c_write(b, addr, w, 2) != 0) return -1;
    return it9300_i2c_wr_rd(b, addr, &reg, 1, back, 1);
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
    printf("firmware=%d.%d.%d.%d\n",b.fw_ver[0],b.fw_ver[1],b.fw_ver[2],b.fw_ver[3]);
    it9300_bridge_init(&b);

    uint8_t buses[]={0x03,0x01,0x02};
    /* Focus: RDA5815 0x0C; plus the common demod addresses */
    uint8_t addrs[]={0x0C,0x60,0x6C,0x14,0x18,0x0D,0x61,0x6E,0x08,0x51};
    uint8_t scratch_regs[]={0x0c,0x0d,0x7e};

    printf("=== write-read-back REAL chip scan ===\n");
    int hits=0;
    for(int g=0; g<16; g++){
        it9300_gpio_power_cycle(&b,g);
        for(size_t bi=0; bi<sizeof(buses); bi++){
            b.i2c_bus=buses[bi];
            for(size_t ai=0; ai<sizeof(addrs); ai++){
                uint8_t addr=addrs[ai];
                uint8_t echo=(uint8_t)((addr<<1)|1);
                for(size_t ri=0; ri<sizeof(scratch_regs); ri++){
                    uint8_t reg=scratch_regs[ri];
                    uint8_t v1=0,v2=0;
                    if(wrb(&b,addr,reg,0xA5,&v1)!=0) continue;
                    if(wrb(&b,addr,reg,0x5A,&v2)!=0) continue;
                    /* real chip: 0xA5 then 0x5A come back; echo: both are
                     * constant */
                    if(v1==0xA5 && v2==0x5A){
                        printf("  >>> REAL: GPIO%d bus=0x%02x addr=0x%02x reg=0x%02x "
                               "(A5->%02x, 5A->%02x)\n", g+1,b.i2c_bus,addr,reg,v1,v2);
                        hits++;
                    } else if(v1!=echo && v1==v2 && v1!=0){
                        printf("  ?  partial: GPIO%d bus=0x%02x addr=0x%02x reg=0x%02x "
                               "(A5->%02x 5A->%02x, non-echo constant)\n",
                               g+1,b.i2c_bus,addr,reg,v1,v2);
                    }
                }
            }
        }
    }
    printf("\n%d real chip register(s) found.\n",hits);
    dtv_usb_close(usb);
    return 0;
}
