/* cs378x protocol (camd35 over TCP).
 *
 * Ported from tsdecrypt's camd-cs378x.c (GPL-2.0, Unix Solutions Ltd.),
 * with OpenSSL replaced by dtv_camd_crypt.c. This is tsdecrypt's default
 * protocol and what OSCam serves from its [camd35] section.
 *
 * A message is a 20 byte header plus payload, padded to whole AES blocks
 * and encrypted with AES-128 in ECB mode keyed with the MD5 of the
 * password. In front goes a four byte plaintext token, the CRC-32 of the
 * MD5 of the user name, which picks the account.
 *
 *   [0]      command (0x00 ECM request, 0x01 answer, 0x05 EMM request,
 *            0x08 no card, 0x44 no control word found)
 *   [1]      payload length
 *   [4..7]   CRC-32 of the payload
 *   [8..9]   service id
 *   [10..11] CA system id
 *   [12..15] provider id
 *   [16..17] message id
 *   [18..19] ECM pid
 *   [20..]   payload (the ECM section)
 *
 * Padding rounds up to a multiple of 16, never 4: tsdecrypt's boundary()
 * takes an exponent, so boundary(4, n) is round up to 2^4. Read as a
 * modulus it truncates the last cipher block and no answer ever comes. */
#include "dtv_camd_internal.h"
#include "dtv_camd_crypt.h"

#include <stdio.h>
#include <string.h>

#define CS378X_HDR_LEN    20
#define CS378X_DATA_SIZE  256

static size_t round_to_block(size_t size)
{
    return (size + 15u) & ~(size_t)15u;
}

static void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

/* Token and AES key both come from the credentials, so they are computed
 * once per connection. */
static void derive_keys(dtv_camd_connection *connection)
{
    uint8_t digest[16];
    dtv_md5(connection->config.user, strlen(connection->config.user),
            digest);
    connection->cs378x.auth_token = dtv_crc32(0, digest, sizeof(digest));
    dtv_md5(connection->config.password, strlen(connection->config.password),
            digest);
    memcpy(connection->cs378x.aes_key, digest, sizeof(digest));
}

static int cs378x_login(dtv_camd_connection *connection)
{
    /* There is no login exchange: the first ECM carries the credentials,
     * so bad ones show up as silence and the read timeout reconnects. */
    derive_keys(connection);
    connection->error[0] = 0;
    return 1;
}

static int cs378x_send(dtv_camd_connection *connection, uint8_t command,
                       const uint8_t *payload, size_t payload_size)
{
    uint8_t message[4 + CS378X_HDR_LEN + CS378X_DATA_SIZE + 16];
    uint8_t *body = message + 4;
    dtv_aes_key schedule;
    size_t body_size, offset;

    if (payload_size > CS378X_DATA_SIZE) {
        snprintf(connection->error, sizeof(connection->error),
                 "cs378x: payload too long (%u)", (unsigned)payload_size);
        return -1;
    }

    body_size = round_to_block(CS378X_HDR_LEN + payload_size);
    memset(body, 0, CS378X_HDR_LEN);
    /* Older camd3 servers expect the unused tail to be 0xff. */
    memset(body + CS378X_HDR_LEN, 0xff, body_size - CS378X_HDR_LEN);
    memcpy(body + CS378X_HDR_LEN, payload, payload_size);

    body[0] = command;
    body[1] = (uint8_t)payload_size;
    put32(body + 4, dtv_crc32(0, payload, payload_size));
    put16(body + 8, connection->service_id);
    put16(body + 10, connection->ca_system_id);
    put32(body + 12, connection->provider_id);
    put16(body + 16, ++connection->cs378x.msg_id);
    put16(body + 18, connection->ecm_pid);

    put32(message, connection->cs378x.auth_token);
    dtv_aes128_set_key(&schedule, connection->cs378x.aes_key);
    for (offset = 0; offset < body_size; offset += 16u)
        dtv_aes128_encrypt_block(&schedule, body + offset, body + offset);

    if (dtv_camd_write_all(connection, message, body_size + 4u) < 0) {
        snprintf(connection->error, sizeof(connection->error),
                 "cs378x: could not send");
        return -1;
    }
    return 1;
}

/* Reads one whole message into connection->cs378x.buffer; returns its
 * length or -1. The length is only known after the first block has been
 * decrypted, hence the block by block read. */
static int cs378x_recv(dtv_camd_connection *connection)
{
    uint8_t *data = connection->cs378x.buffer;
    dtv_aes_key schedule;
    uint32_t token;
    size_t expected = CS378X_DATA_SIZE, offset;
    uint8_t header[4];

    {
        int rc = dtv_camd_read_all(connection, header, sizeof(header));
        if (rc == DTV_CAMD_IO_TIMEOUT) {
            snprintf(connection->error, sizeof(connection->error),
                     "The CAM server did not answer");
            return DTV_CAMD_IO_TIMEOUT;
        }
        if (rc < 0) {
            snprintf(connection->error, sizeof(connection->error),
                     "cs378x: no reply");
            return -1;
        }
    }
    token = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
            ((uint32_t)header[2] << 8) | header[3];
    if (token != connection->cs378x.auth_token) {
        /* Not fatal -- some servers echo their own token -- but recorded,
         * since a mismatch usually means an unknown user name. */
        snprintf(connection->error, sizeof(connection->error),
                 "cs378x: authentication token mismatch");
    }

    dtv_aes128_set_key(&schedule, connection->cs378x.aes_key);
    for (offset = 0; offset < expected; offset += 16u) {
        if (offset + 16u > DTV_CAMD_MSG_SIZE) {
            snprintf(connection->error, sizeof(connection->error),
                     "cs378x: reply overflows its buffer");
            return -1;
        }
        if (dtv_camd_read_all(connection, data + offset, 16) < 0) {
            snprintf(connection->error, sizeof(connection->error),
                     "cs378x: reply cut short");
            return -1;
        }
        dtv_aes128_decrypt_block(&schedule, data + offset, data + offset);
        if (offset == 0)
            expected = round_to_block((size_t)data[1] + CS378X_HDR_LEN);
    }
    return (int)expected;
}

static int cs378x_send_ecm(dtv_camd_connection *connection,
                           const uint8_t *section, size_t size)
{
    return cs378x_send(connection, 0x00, section, size);
}

static int cs378x_recv_cw(dtv_camd_connection *connection,
                          uint8_t control_word[16])
{
    const uint8_t *data = connection->cs378x.buffer;
    int attempts = 0;

    for (;;) {
        int length = cs378x_recv(connection);
        if (length == DTV_CAMD_IO_TIMEOUT)
            return 0;   /* silence: keep the connection, count a miss */
        if (length < 0)
            return -1;
        /* The server pushes EMM requests down the same socket, sometimes
         * right after a control word; they answer nothing we asked. */
        if (data[0] == 0x05) {
            if (++attempts > 8) {
                snprintf(connection->error, sizeof(connection->error),
                         "cs378x: only EMM requests arrive");
                return -1;
            }
            continue;
        }
        if (data[0] != 0x01) {
            snprintf(connection->error, sizeof(connection->error),
                     "cs378x: %s",
                     data[0] == 0x08 ? "no card"
                     : data[0] == 0x44 ? "control word not found"
                                       : "unknown server reply");
            return 0;
        }
        if (length < 48 || data[1] < 0x10) {
            snprintf(connection->error, sizeof(connection->error),
                     "cs378x: control word packet incomplete (%d)", length);
            return 0;
        }
        memcpy(control_word, data + 20, 16);
        return 1;
    }
}

void dtv_camd_ops_cs378x(dtv_camd_ops *ops)
{
    ops->name = "cs378x";
    ops->verifies_login = 0;   /* nothing is exchanged until the first ECM */
    ops->login = cs378x_login;
    ops->send_ecm = cs378x_send_ecm;
    ops->recv_cw = cs378x_recv_cw;
    /* camd35 has no idle message; the server simply keeps the socket. */
    ops->keepalive = NULL;
}
