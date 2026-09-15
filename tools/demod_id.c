/* demod_id.c - phase A: DS3103B chip id (with the GPIO 2/5/7 power
 * sequence)
 *
 * Ghidra finding: demod/tuner power is controlled through GPIO 2, 5 and 7
 * (native, 1-based) (FUN_002268a0/00226a08/00226a90 -> FUN_002a9334). Our
 * table is 0-based: GPIO2=idx1, GPIO5=idx4, GPIO7=idx6. It powers these
 * GPIOs on together and reads the DS3103 chip id (reg 0x00). */
#include <stdio.h>
#include <stdlib.h>
#include "dtv_platform.h"
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

static const char *chip_name(uint8_t id){
    switch(id){case 0x70:return "DS3103/DS3103B";case 0x71:return "DS3103C";
               case 0x74:return "RS6000";default:return "?";}
}

/* demod chip id: bus 2/3 x addr 0x68/0x34, reg 0x00 (consistent and not an
 * echo) */
static int try_chipid(it9300 *b){
    uint8_t addrs[]={0x68,0x34}; uint8_t buses[]={0x02,0x03};
    int hit=0;
    for(size_t bi=0;bi<sizeof(buses);bi++){
        b->i2c_bus=buses[bi];
        for(size_t ai=0;ai<sizeof(addrs);ai++){
            uint8_t addr=addrs[ai], echo=(uint8_t)((addr<<1)|1);
            uint8_t reg=0x00,v1=0,v2=0;
            if(it9300_i2c_wr_rd(b,addr,&reg,1,&v1,1)!=0) continue;
            if(it9300_i2c_wr_rd(b,addr,&reg,1,&v2,1)!=0) continue;
            if(v1!=v2||v1==echo||v1==0||v1==0xff) continue;
            printf("    >>> bus=0x%02x addr=0x%02x reg0=0x%02x chip_id=0x%02x (%s)\n",
                   b->i2c_bus,addr,v1,v1>>1,chip_name(v1>>1));
            hit++;
        }
    }
    return hit;
}

/* GPIO 2,5,7 (idx 1,4,6) to a given level */
static void power_gpios(it9300 *b, int lv2, int lv5, int lv7){
    it9300_gpio_set(b,1,lv2);   /* GPIO2 */
    it9300_gpio_set(b,4,lv5);   /* GPIO5 */
    it9300_gpio_set(b,6,lv7);   /* GPIO7 */
}

/* Board reset sequence, identical to libudtv.so FUN_0022569c(0xff).
 * FUN_002268a0 drives native GPIO2 on every call. The core GPIO table is
 * 0-based, so GPIO2 is index 1 here. */
static int native_gpio2_reset(it9300 *b){
    if(it9300_gpio_set(b,1,1)!=0) return -1;
    dtv_sleep_ms(100);
    if(it9300_gpio_set(b,1,0)!=0) return -1;
    dtv_sleep_ms(200);
    if(it9300_gpio_set(b,1,1)!=0) return -1;
    dtv_sleep_ms(100);
    return 0;
}

/* RDA5815M: bus 3, 7-bit address 0x0c in the native driver. */
static int try_tuner_ack(it9300 *b){
    const uint8_t probe[]={0x04,0xe1};
    b->i2c_bus=3;
    int ret=it9300_i2c_write(b,0x0c,probe,(int)sizeof(probe));
    printf("    RDA5815M bus=3 addr=0x0c write ACK: %s (ret=%d)\n",
           ret==0?"YES":"NO",ret);
    return ret==0;
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
    printf("bridge fw=%d.%d.%d.%d\n",b.fw_ver[0],b.fw_ver[1],b.fw_ver[2],b.fw_ver[3]);
    it9300_bridge_init(&b);

    int total=0;
    printf("[0] Native GPIO2 reset: HIGH(100ms)->LOW(200ms)->HIGH(100ms)\n");
    if(native_gpio2_reset(&b)!=0){
        fprintf(stderr,"native GPIO2 reset failed\n");
    }else{
        try_tuner_ack(&b);
        total+=try_chipid(&b);
    }

    /* Combination 1: GPIO 2,5,7 all HIGH */
    printf("[1] GPIO2/5/7 = HIGH\n");
    power_gpios(&b,1,1,1); dtv_sleep_ms(250);
    total+=try_chipid(&b);

    /* Combination 2: reset sequence (LOW->HIGH) */
    printf("[2] GPIO2/5/7 reset: LOW(100ms)->HIGH(250ms)\n");
    power_gpios(&b,0,0,0); dtv_sleep_ms(100);
    power_gpios(&b,1,1,1); dtv_sleep_ms(250);
    total+=try_chipid(&b);

    /* Combination 3: reset one by one (on some boards demod_reset is
     * separate) */
    int combos[][3]={{1,1,0},{1,0,1},{0,1,1},{1,0,0},{0,1,0},{0,0,1}};
    for(int c=0;c<6;c++){
        printf("[3.%d] GPIO2=%d GPIO5=%d GPIO7=%d\n",c,combos[c][0],combos[c][1],combos[c][2]);
        power_gpios(&b,combos[c][0],combos[c][1],combos[c][2]); dtv_sleep_ms(200);
        total+=try_chipid(&b);
    }

    printf("\nTotal demod responses: %d\n",total);
    if(!total) printf("DS3103 is still silent. The GPIO index/table or the reset sequence may differ.\n");
    dtv_usb_close(usb);
    return 0;
}
