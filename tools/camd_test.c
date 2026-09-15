/* Live CAM server check.
 *
 * Connects to the card server the way the tuner daemon does and reports
 * what happens, with no tuner or transport stream in the way: are the
 * address, the credentials and the DES key right? Given an ECM captured
 * from a real broadcast, it also asks for the control words.
 *
 *   usb-dvbs2-camd-test --server 127.0.0.1:12121 --user Windows --pass Windows \
 *                 --des-key 0102030405060708091011121314
 *   usb-dvbs2-camd-test ... --caid 0x0500 --sid 51202 --ecm 80703c...
 *
 * Option names follow tsdecrypt's, so a working command line copies. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dtv_platform.h"

#include "dtv_camd.h"

static int parse_hex_blob(const char *text, uint8_t *out, size_t capacity,
                          size_t *size)
{
    size_t count = 0;
    unsigned high = 0;
    int have_high = 0;
    while (*text) {
        unsigned value;
        char c = *text++;
        if (c == ' ' || c == ':' || c == '-')
            continue;
        if (c >= '0' && c <= '9') value = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') value = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value = (unsigned)(c - 'A' + 10);
        else return -1;
        if (!have_high) {
            high = value;
            have_high = 1;
        } else {
            if (count >= capacity)
                return -1;
            out[count++] = (uint8_t)((high << 4) | value);
            have_high = 0;
        }
    }
    if (have_high)
        return -1;
    *size = count;
    return 0;
}

static const char *state_name(dtv_camd_state state)
{
    switch (state) {
    case DTV_CAMD_STATE_OFF:         return "off";
    case DTV_CAMD_STATE_CONNECTING:  return "connecting";
    case DTV_CAMD_STATE_CONNECTED:   return "connected";
    case DTV_CAMD_STATE_DECRYPTING:  return "decrypting";
    case DTV_CAMD_STATE_REJECTED:    return "rejected";
    case DTV_CAMD_STATE_ERROR:       return "error";
    }
    return "?";
}

static void print_status(dtv_camd *camd)
{
    dtv_camd_status status;
    dtv_camd_get_status(camd, &status);
    printf("  state: %-11s %s   [ECM %u, CW %u, failed %u]\n",
           state_name(status.state), status.message, status.ecm_sent,
           status.cw_received, status.ecm_failed);
    if (status.last_error[0])
        printf("  last error: %s\n", status.last_error);
}

/* Every state change is printed: a client that connects, fails on the ECM
 * and reconnects looks idle when only the last sample is shown. */
static void print_status_changes(dtv_camd *camd, int *last_state,
                                 char *last_message, size_t message_size)
{
    dtv_camd_status status;
    dtv_camd_get_status(camd, &status);
    if ((int)status.state == *last_state &&
        strcmp(status.message, last_message) == 0)
        return;
    *last_state = (int)status.state;
    snprintf(last_message, message_size, "%s", status.message);
    print_status(camd);
}

int main(int argc, char **argv)
{
    dtv_camd_config config;
    dtv_camd_candidate candidate;
    dtv_camd *camd;
    uint8_t ecm[512];
    size_t ecm_size = 0;
    unsigned wait_seconds = 10;
    uint16_t service_id = 0;
    int i;

    memset(&config, 0, sizeof(config));
    memset(&candidate, 0, sizeof(candidate));
    config.proto = DTV_CAMD_PROTO_NEWCAMD;
    snprintf(config.host, sizeof(config.host), "127.0.0.1");
    config.port = 12121;
    snprintf(config.user, sizeof(config.user), "user");
    snprintf(config.password, sizeof(config.password), "pass");
    dtv_camd_parse_des_key("0102030405060708091011121314", config.des_key);
    candidate.ecm_pid = 0x1fff;

    for (i = 1; i < argc; ++i) {
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--server") == 0 && value) {
            char *colon;
            snprintf(config.host, sizeof(config.host), "%s", value);
            colon = strrchr(config.host, ':');
            if (colon) {
                *colon = 0;
                config.port = (unsigned)strtoul(colon + 1, NULL, 10);
            }
            ++i;
        } else if (strcmp(argv[i], "--user") == 0 && value) {
            snprintf(config.user, sizeof(config.user), "%s", value);
            ++i;
        } else if (strcmp(argv[i], "--pass") == 0 && value) {
            snprintf(config.password, sizeof(config.password), "%s", value);
            ++i;
        } else if (strcmp(argv[i], "--des-key") == 0 && value) {
            if (dtv_camd_parse_des_key(value, config.des_key) != 0) {
                fprintf(stderr, "The DES key must be 28 hexadecimal digits.\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--proto") == 0 && value) {
            if (dtv_camd_parse_proto(value, &config.proto) != 0) {
                fprintf(stderr, "The protocol must be newcamd or cs378x.\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--caid") == 0 && value) {
            candidate.ca_system_id =
                (uint16_t)strtoul(value, NULL, 0);
            ++i;
        } else if (strcmp(argv[i], "--provider") == 0 && value) {
            candidate.provider_id = (uint32_t)strtoul(value, NULL, 0);
            ++i;
        } else if (strcmp(argv[i], "--pid") == 0 && value) {
            candidate.ecm_pid = (uint16_t)strtoul(value, NULL, 0);
            ++i;
        } else if (strcmp(argv[i], "--sid") == 0 && value) {
            service_id = (uint16_t)strtoul(value, NULL, 0);
            ++i;
        } else if (strcmp(argv[i], "--ecm") == 0 && value) {
            if (parse_hex_blob(value, ecm, sizeof(ecm), &ecm_size) != 0) {
                fprintf(stderr, "The ECM hex string could not be parsed.\n");
                return 2;
            }
            ++i;
        } else if (strcmp(argv[i], "--wait") == 0 && value) {
            wait_seconds = (unsigned)strtoul(value, NULL, 10);
            ++i;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    printf("CAM: %s %s:%u user=%s\n", dtv_camd_proto_name(config.proto),
           config.host, config.port, config.user);

    camd = dtv_camd_start(&config);
    if (!camd) {
        fprintf(stderr, "The CAM client could not be started.\n");
        return 1;
    }

    /* Even with no ECM the login has to happen before anything can be
     * said, so give the worker a moment either way. */
    if (ecm_size) {
        dtv_camd_candidate list[1];
        list[0] = candidate;
        dtv_camd_set_service(camd, service_id, list, 1);
    }

    {
        unsigned elapsed;
        int reported = 0;
        int last_state = -1;
        char last_message[128] = "";
        for (elapsed = 0; elapsed < wait_seconds * 10u; ++elapsed) {
            dtv_camd_status status;
            dtv_sleep_ms(100);
            print_status_changes(camd, &last_state, last_message,
                                 sizeof(last_message));
            dtv_camd_get_status(camd, &status);
            if (!reported && status.state >= DTV_CAMD_STATE_CONNECTED) {
                printf("Login complete.\n");
                print_status(camd);
                reported = 1;
                if (ecm_size) {
                    printf("Sending ECM (%u bytes, table 0x%02x)...\n",
                           (unsigned)ecm_size, ecm[0]);
                    dtv_camd_submit_ecm(camd, ecm, ecm_size);
                }
                if (!ecm_size)
                    break;
            }
            if (status.state == DTV_CAMD_STATE_DECRYPTING) {
                uint8_t even[8], odd[8];
                unsigned generation, byte;
                dtv_camd_get_keys(camd, even, odd, &generation);
                printf("Control words received (%u ms):\n  even: ",
                       status.last_latency_ms);
                for (byte = 0; byte < 8u; ++byte)
                    printf("%02x", even[byte]);
                printf("\n  odd : ");
                for (byte = 0; byte < 8u; ++byte)
                    printf("%02x", odd[byte]);
                printf("\n");
                dtv_camd_stop(camd);
                return 0;
            }
            if (status.state == DTV_CAMD_STATE_REJECTED) {
                print_status(camd);
                dtv_camd_stop(camd);
                return 1;
            }
        }
    }

    print_status(camd);
    {
        dtv_camd_status status;
        dtv_camd_get_status(camd, &status);
        dtv_camd_stop(camd);
        return status.state >= DTV_CAMD_STATE_CONNECTED ? 0 : 1;
    }
}
