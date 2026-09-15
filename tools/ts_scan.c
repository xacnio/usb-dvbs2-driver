/* Offline MPEG-TS PAT/PMT/SDT service listing tool. */
#include <stdio.h>
#include "ts_services.h"

int main(int argc, char **argv)
{
    static dtv_ts_scan_result scan; /* too large for the stack */
    size_t i;
    if (argc != 2) {
        fprintf(stderr, "Usage: %s capture.ts\n", argv[0]);
        return 2;
    }
    int rc = dtv_ts_scan_file(argv[1], &scan);
    if (rc != 0) {
        fprintf(stderr, "TS scan failed: %d\n", rc);
        return 1;
    }
    printf("packets=%lu sync_errors=%lu services=%lu\n",
           (unsigned long)scan.packet_count,
           (unsigned long)scan.sync_errors,
           (unsigned long)scan.service_count);
    for (i = 0; i < scan.service_count; ++i) {
        const dtv_ts_service *s = &scan.services[i];
        printf("%5u  %-5s  %-28s PMT=0x%04x V=0x%04x A=0x%04x%s\n",
               s->service_id, s->has_video ? "TV" :
               (s->has_audio ? "RADIO" : "DATA"), s->name,
               s->pmt_pid, s->video_pid, s->audio_pid,
               s->scrambled ? " [CA]" : "");
    }
    return 0;
}
