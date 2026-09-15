/* strap_test.c - read the GPIO board straps and verify the GPIO table by
 * loopback
 *
 * 1) Read every GPIO as an input (board/demod type straps: GPIO 1/14).
 * 2) Loopback: drive each GPIO OUT+HIGH and read the input, then OUT+LOW
 *    and read again. If HIGH/LOW show up on the input, the GPIO register
 *    table is CORRECT. If they do not, the table is wrong and every GPIO
 *    power attempt was pointless. */
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
    printf("bridge fw=%d.%d.%d.%d\n\n",b.fw_ver[0],b.fw_ver[1],b.fw_ver[2],b.fw_ver[3]);
    it9300_bridge_init(&b);

    printf("=== GPIO input (board strap) ===\n");
    for(int g=0;g<16;g++){
        uint8_t lv=0xff;
        int rc=it9300_gpio_read(&b,g,&lv);
        printf("  GPIO%-2d input = %s\n", g+1, rc==0 ? (lv?"HIGH":"LOW") : "error");
    }

    printf("\n=== Loopback (OUT->input) GPIO table verification ===\n");
    int ok=0;
    for(int g=0;g<16;g++){
        uint8_t hi=0xff, lo=0xff;
        it9300_gpio_set(&b,g,1); dtv_sleep_ms(5); it9300_gpio_read(&b,g,&hi);
        it9300_gpio_set(&b,g,0); dtv_sleep_ms(5); it9300_gpio_read(&b,g,&lo);
        int works = (hi==1 && lo==0);
        printf("  GPIO%-2d: HIGH->%d LOW->%d  %s\n", g+1, hi, lo, works?"OK":"");
        if(works) ok++;
    }
    printf("\nGPIOs where loopback works: %d/16 %s\n", ok,
           ok>0 ? "-> GPIO table is CORRECT" : "-> GPIO table is WRONG or the pins are driven externally");
    dtv_usb_close(usb);
    return 0;
}
