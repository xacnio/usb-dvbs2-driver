/* Shared between the CAM worker and the individual protocols. As in
 * tsdecrypt, one worker owns the socket, the state machine and the timing,
 * and a small ops table implements the wire protocol; no protocol file
 * knows about threads or the transport stream. */
#ifndef DTV_CAMD_INTERNAL_H
#define DTV_CAMD_INTERNAL_H

#include "dtv_camd.h"

#include "dtv_net.h"

/* newcamd caps messages at 400 bytes; cs378x adds a 20 byte header to at
 * most a 256 byte payload. One size covers both. */
#define DTV_CAMD_MSG_SIZE 1024

typedef struct dtv_camd_connection {
    dtv_socket socket_handle;
    dtv_camd_config config;

    /* Service and CA system of the next ECM. cs378x carries them in its
     * header, and newcamd needs them when the server uses a wildcard port
     * (OSCam port = 12121@0000 reads the CA id from the client header). */
    uint16_t service_id;
    uint16_t ca_system_id;
    uint32_t provider_id;
    uint16_t ecm_pid;         /* cs378x reports it back to the server */

    /* Filled in by the protocol on a failed exchange; shown in the status
     * line. */
    char error[128];

    struct {
        uint8_t login_key[16];    /* triple DES key after the spread */
        uint8_t session_key[16];  /* same, re-derived after login */
        const uint8_t *active_key;
        uint16_t msg_id;
        uint16_t ca_system_id;    /* what the card reported */
        uint8_t buffer[DTV_CAMD_MSG_SIZE];
    } newcamd;

    struct {
        uint32_t auth_token;
        uint8_t aes_key[16];
        uint16_t msg_id;
        uint8_t buffer[DTV_CAMD_MSG_SIZE];
    } cs378x;
} dtv_camd_connection;

/* Return values shared by the protocol entry points:
 *   1  success
 *   0  answered negatively (card cannot decode this) -- connection is
 *      still good and must not be torn down
 *  -1  fatal: reconnect */
typedef struct dtv_camd_ops {
    const char *name;
    /* Does login() prove the server speaks this protocol? newcamd
     * exchanges a handshake and a password, so it does. cs378x has no
     * login -- the credentials ride on the first ECM -- so connect() only
     * proves something accepted TCP, and reporting connected there hides a
     * client pointed at the wrong port. */
    int verifies_login;
    int (*login)(dtv_camd_connection *connection);
    int (*send_ecm)(dtv_camd_connection *connection, const uint8_t *section,
                    size_t size);
    int (*recv_cw)(dtv_camd_connection *connection, uint8_t control_word[16]);
    int (*keepalive)(dtv_camd_connection *connection);
} dtv_camd_ops;

void dtv_camd_ops_newcamd(dtv_camd_ops *ops);
void dtv_camd_ops_cs378x(dtv_camd_ops *ops);

/* How long a reply may take. The ECM timeout has to outlast the server's
 * own giving-up time (see dtv_camd.c). The short one is for informative
 * exchanges where no answer is an acceptable answer: a card server with no
 * card does not reply to a card query at all. */
#define DTV_CAMD_ECM_TIMEOUT_MS   14000
#define DTV_CAMD_INFO_TIMEOUT_MS   2000

/* Changes the receive timeout of the open connection. */
void dtv_camd_set_read_timeout(dtv_camd_connection *connection,
                               unsigned milliseconds);

/* Returned by dtv_camd_read_all() when the server said nothing within the
 * timeout, as opposed to closing the connection. Protocols treat it as no
 * answer to this ECM, not a broken link: servers go quiet when no reader
 * can answer, and reconnecting over it costs another lost ECM. */
#define DTV_CAMD_IO_TIMEOUT (-2)

/* Socket helpers with the worker's timeouts applied. Both return the byte
 * count, DTV_CAMD_IO_TIMEOUT, or -1; a short read is an error, since every
 * message in these protocols is length prefixed. */
int dtv_camd_read_all(dtv_camd_connection *connection, void *buffer,
                      size_t size);
int dtv_camd_write_all(dtv_camd_connection *connection, const void *buffer,
                       size_t size);

/* Unpredictable bytes for padding and initialisation vectors. */
void dtv_camd_random(uint8_t *buffer, size_t size);

#endif
