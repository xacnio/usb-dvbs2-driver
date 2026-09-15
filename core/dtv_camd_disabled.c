/* The card server client and the descrambler, left out.
 *
 * Built instead of dtv_camd*.c and dtv_csa.c when USB_DVBS2_CAMD is OFF:
 * the engine keeps calling the same functions, and they do nothing. Free
 * services stream as before; a scrambled one stays scrambled. */
#include "dtv_camd.h"
#include "dtv_csa.h"

#include <stdio.h>
#include <string.h>

int dtv_camd_available(void) { return 0; }

dtv_camd *dtv_camd_start(const dtv_camd_config *config)
{
    (void)config;
    return NULL;
}

void dtv_camd_stop(dtv_camd *camd) { (void)camd; }

void dtv_camd_set_service(dtv_camd *camd, uint16_t service_id,
                          const dtv_camd_candidate *candidates, size_t count)
{
    (void)camd; (void)service_id; (void)candidates; (void)count;
}

uint16_t dtv_camd_ecm_pid(dtv_camd *camd)
{
    (void)camd;
    return 0x1fff;
}

void dtv_camd_submit_ecm(dtv_camd *camd, const uint8_t *section, size_t size)
{
    (void)camd; (void)section; (void)size;
}

int dtv_camd_get_keys(dtv_camd *camd, uint8_t even[8], uint8_t odd[8],
                      unsigned *generation)
{
    (void)camd;
    memset(even, 0, 8);
    memset(odd, 0, 8);
    if (generation)
        *generation = 0;
    return 0;
}

void dtv_camd_get_status(dtv_camd *camd, dtv_camd_status *status)
{
    (void)camd;
    if (!status)
        return;
    memset(status, 0, sizeof(*status));
    status->state = DTV_CAMD_STATE_OFF;
    status->ecm_pid = 0x1fff;
    snprintf(status->message, sizeof(status->message), "CAM off");
}

size_t dtv_camd_build_biss_ecm(uint16_t service_id, uint16_t pid, uint8_t *out)
{
    (void)service_id; (void)pid; (void)out;
    return 0;
}

int dtv_camd_parse_des_key(const char *text, uint8_t key[DTV_CAMD_DES_KEY_SIZE])
{
    (void)text;
    memset(key, 0, DTV_CAMD_DES_KEY_SIZE);
    return 0;
}

int dtv_camd_parse_proto(const char *text, dtv_camd_proto *proto)
{
    if (!text || !proto)
        return -1;
    if (strcmp(text, "newcamd") == 0) {
        *proto = DTV_CAMD_PROTO_NEWCAMD;
        return 0;
    }
    if (strcmp(text, "cs378x") == 0 || strcmp(text, "camd35") == 0) {
        *proto = DTV_CAMD_PROTO_CS378X;
        return 0;
    }
    return -1;
}

const char *dtv_camd_proto_name(dtv_camd_proto proto)
{
    return proto == DTV_CAMD_PROTO_CS378X ? "cs378x" : "newcamd";
}

/* A descrambler that holds no key: never NULL, so the engine does not
 * report a start failure for something that was left out on purpose. */
struct dtv_csa {
    int unused;
};

static struct dtv_csa g_no_csa;

dtv_csa *dtv_csa_create(void) { return &g_no_csa; }
void dtv_csa_destroy(dtv_csa *csa) { (void)csa; }

void dtv_csa_set_keys(dtv_csa *csa, const uint8_t even[8], int even_valid,
                      const uint8_t odd[8], int odd_valid)
{
    (void)csa; (void)even; (void)even_valid; (void)odd; (void)odd_valid;
}

void dtv_csa_clear_keys(dtv_csa *csa) { (void)csa; }

size_t dtv_csa_decrypt_run(dtv_csa *csa, uint8_t *packets, size_t count)
{
    (void)csa; (void)packets; (void)count;
    return 0;
}

int dtv_csa_has_key(const dtv_csa *csa)
{
    (void)csa;
    return 0;
}
