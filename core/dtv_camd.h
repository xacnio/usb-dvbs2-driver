/* CAM (card server) client: ECM in, control words out.
 *
 * This is the tsdecrypt side of the project, minus the stream: only the
 * protocol that carries an ECM to the card server and brings back the two
 * control words. The protocols are tsdecrypt's own (newcamd and
 * cs378x/camd35), so any server that works with tsdecrypt works here.
 *
 * Everything is asynchronous: the stream thread hands over ECM sections
 * and reads back whatever control words have arrived. A missing key means
 * a moment of scrambled picture, never a stalled stream.
 *
 * The descrambling itself is in dtv_csa.h. */
#ifndef DTV_CAMD_H
#define DTV_CAMD_H

#include <stddef.h>
#include <stdint.h>

typedef enum dtv_camd_proto {
    DTV_CAMD_PROTO_NEWCAMD = 0,
    DTV_CAMD_PROTO_CS378X = 1
} dtv_camd_proto;

/* newcamd keys are 14 bytes (0102030405060708091011121314 by default);
 * the login key is derived from them. */
#define DTV_CAMD_DES_KEY_SIZE 14

typedef struct dtv_camd_config {
    dtv_camd_proto proto;
    char host[96];
    unsigned port;
    char user[64];
    char password[64];
    uint8_t des_key[DTV_CAMD_DES_KEY_SIZE];
} dtv_camd_config;

/* What a front end shows. Ordered by progress, so at least connected is a
 * comparison. */
typedef enum dtv_camd_state {
    DTV_CAMD_STATE_OFF = 0,
    DTV_CAMD_STATE_CONNECTING,
    DTV_CAMD_STATE_CONNECTED,   /* logged in, no control word yet */
    DTV_CAMD_STATE_DECRYPTING,  /* control words arriving */
    DTV_CAMD_STATE_REJECTED,    /* server answered: card cannot decode */
    DTV_CAMD_STATE_ERROR        /* connection or login failure */
} dtv_camd_state;

typedef struct dtv_camd_status {
    dtv_camd_state state;
    char message[128];        /* human readable, in English */
    /* The last failure, kept after a reconnect: without it a client that
     * fails and reconnects looks healthy and the one clue as to why
     * nothing decrypts is lost. */
    char last_error[128];
    uint16_t ca_system_id;    /* what the server reported, when it does */
    unsigned ecm_sent;
    unsigned cw_received;
    unsigned ecm_failed;
    unsigned last_latency_ms; /* ECM -> control word round trip */
    uint16_t ecm_pid;         /* which pid is being asked right now */
} dtv_camd_status;

/* One CA candidate from the PMT: a CA system, its ECM pid and the
 * provider the descriptor named (0 when unknown, which is usual). */
typedef struct dtv_camd_candidate {
    uint16_t ca_system_id;
    uint16_t ecm_pid;
    uint32_t provider_id;
} dtv_camd_candidate;

#define DTV_CAMD_MAX_CANDIDATES 8

/* BISS (CA system 0x26xx): no ECM stream and often no ECM pid, since the
 * payload is scrambled with a key known in advance. The card server holds
 * it, so it is asked with an ECM that never went on air, built from the
 * service id and an elementary pid -- how the key is indexed. */
#define DTV_CA_IS_BISS(caid) (((caid) >> 8) == 0x26u)
/* Fills out such an ECM; returns its length (always
 * DTV_CAMD_BISS_ECM_SIZE) for dtv_camd_submit_ecm. */
#define DTV_CAMD_BISS_ECM_SIZE 7
size_t dtv_camd_build_biss_ecm(uint16_t service_id, uint16_t pid,
                               uint8_t *out);

typedef struct dtv_camd dtv_camd;

/* 0 when the library was built without the card server client
 * (USB_DVBS2_CAMD=OFF): every call below is then a no-op and
 * dtv_camd_start() returns NULL. */
int dtv_camd_available(void);

/* Starts the client and its worker. The connection is made lazily, so
 * this succeeds with the server down; NULL only on bad config or OOM. */
dtv_camd *dtv_camd_start(const dtv_camd_config *config);
void dtv_camd_stop(dtv_camd *camd);

/* Channel change: forget the old keys, take the new candidates. Zero
 * candidates parks the client but keeps the connection. */
void dtv_camd_set_service(dtv_camd *camd, uint16_t service_id,
                          const dtv_camd_candidate *candidates,
                          size_t count);

/* Which pid to filter for ECMs right now, 0x1fff for none. The client
 * changes it when a CA system turns out not to work, so re-read it
 * periodically rather than caching across a channel. */
uint16_t dtv_camd_ecm_pid(dtv_camd *camd);

/* Hands over a complete ECM section (table id 0x80 or 0x81). Non-blocking
 * and drops repeats of the section in flight, so it is safe per packet. */
void dtv_camd_submit_ecm(dtv_camd *camd, const uint8_t *section,
                         size_t size);

/* Reads the current control words. generation increases when they change,
 * so the caller can skip re-keying. Returns 1 when a key is valid. */
int dtv_camd_get_keys(dtv_camd *camd, uint8_t even[8], uint8_t odd[8],
                      unsigned *generation);

void dtv_camd_get_status(dtv_camd *camd, dtv_camd_status *status);

/* Parses 0102030405060708091011121314 into 14 bytes; 0 on success.
 * Spaces and a leading 0x are tolerated. */
int dtv_camd_parse_des_key(const char *text,
                           uint8_t key[DTV_CAMD_DES_KEY_SIZE]);

/* "newcamd" / "cs378x" (also accepts "camd35"). Returns 0 on success. */
int dtv_camd_parse_proto(const char *text, dtv_camd_proto *proto);
const char *dtv_camd_proto_name(dtv_camd_proto proto);

#endif
