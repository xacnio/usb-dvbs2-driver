/* newcamd protocol.
 *
 * Ported from tsdecrypt's camd-newcamd.c (GPL-2.0, Unix Solutions Ltd.,
 * itself derived from getstream), with three differences:
 *
 *  - OpenSSL is replaced by dtv_camd_crypt.c;
 *  - the CA id and provider id are written into the header. The original
 *    always sent zeros, which works only when the server binds one port
 *    per CA system; OSCam's wildcard configuration takes the CA id from
 *    the client header and rejects a zero;
 *  - reads and writes use the worker's timed socket helpers, so a server
 *    that stops answering mid-message cannot hang the client.
 *
 * Wire layout after the two length bytes:
 *
 *   [0..1]   message id
 *   [2..3]   service id
 *   [4..5]   CA system id
 *   [6..8]   provider id
 *   [9]      unused
 *   [10..]   payload (a DVB section: table id, length, data)
 *
 * Everything from the message id on is encrypted with two-key triple DES
 * in CBC mode, with a random IV appended after the ciphertext. */
#include "dtv_camd_internal.h"
#include "dtv_camd_crypt.h"

#include <stdio.h>
#include <string.h>

#define NEWCAMD_HDR_LEN      8
#define NEWCAMD_FIRST_CMD    0xe0
#define NEWCAMD_CLIENT_ID    0x7878  /* what tsdecrypt announces itself as */
#define NEWCAMD_MSG_MAX      400
/* An answer to a request already given up on. Distinct from
 * DTV_CAMD_IO_TIMEOUT (nothing arrived): this one must be read past. */
#define NEWCAMD_STALE        (-3)

enum {
    MSG_CLIENT_2_SERVER_LOGIN = NEWCAMD_FIRST_CMD,
    MSG_CLIENT_2_SERVER_LOGIN_ACK,
    MSG_CLIENT_2_SERVER_LOGIN_NAK,
    MSG_CARD_DATA_REQ,
    MSG_CARD_DATA,
    MSG_KEEPALIVE = NEWCAMD_FIRST_CMD + 0x1d
};

/* The 14 byte configured key becomes a 16 byte DES key pair by spreading
 * the bits seven at a time; low bits are then set to odd parity. */
static void des_key_spread(uint8_t out[16], const uint8_t in[14])
{
    out[0]  =   in[0] & 0xfe;
    out[1]  = (uint8_t)(((in[0] << 7) | (in[1] >> 1)) & 0xfe);
    out[2]  = (uint8_t)(((in[1] << 6) | (in[2] >> 2)) & 0xfe);
    out[3]  = (uint8_t)(((in[2] << 5) | (in[3] >> 3)) & 0xfe);
    out[4]  = (uint8_t)(((in[3] << 4) | (in[4] >> 4)) & 0xfe);
    out[5]  = (uint8_t)(((in[4] << 3) | (in[5] >> 5)) & 0xfe);
    out[6]  = (uint8_t)(((in[5] << 2) | (in[6] >> 6)) & 0xfe);
    out[7]  = (uint8_t)(in[6] << 1);
    out[8]  =   in[7] & 0xfe;
    out[9]  = (uint8_t)(((in[7] << 7)  | (in[8] >> 1)) & 0xfe);
    out[10] = (uint8_t)(((in[8] << 6)  | (in[9] >> 2)) & 0xfe);
    out[11] = (uint8_t)(((in[9] << 5)  | (in[10] >> 3)) & 0xfe);
    out[12] = (uint8_t)(((in[10] << 4) | (in[11] >> 4)) & 0xfe);
    out[13] = (uint8_t)(((in[11] << 3) | (in[12] >> 5)) & 0xfe);
    out[14] = (uint8_t)(((in[12] << 2) | (in[13] >> 6)) & 0xfe);
    out[15] = (uint8_t)(in[13] << 1);
    dtv_des_set_odd_parity(out, 16);
}

static uint8_t xor_sum(const uint8_t *data, size_t size)
{
    uint8_t sum = 0;
    while (size--)
        sum ^= *data++;
    return sum;
}

/* Pads to a triple DES block boundary with random bytes and appends the
 * running xor as a check byte. Returns the new length or -1. */
static int pad_message(uint8_t *data, int length)
{
    uint8_t padding[8];
    int pad_count = (8 - ((length - 1) % 8)) % 8;
    if (length + pad_count + 1 >= NEWCAMD_MSG_MAX - 8)
        return -1;
    dtv_camd_random(padding, sizeof(padding));
    memcpy(data + length, padding, (size_t)pad_count);
    length += pad_count;
    data[length] = xor_sum(data + 2, (size_t)length - 2u);
    return length + 1;
}

static void set_active_key(dtv_camd_connection *connection,
                           const uint8_t *key)
{
    connection->newcamd.active_key = key;
}

static int send_message(dtv_camd_connection *connection, const uint8_t *data,
                        int data_length, uint16_t service_id,
                        uint16_t ca_system_id, uint32_t provider_id,
                        int use_msg_id)
{
    uint8_t message[DTV_CAMD_MSG_SIZE];
    uint8_t iv[8];
    int length;

    if (data_length < 3 ||
        data_length + NEWCAMD_HDR_LEN + 4 > NEWCAMD_MSG_MAX) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: invalid message length (%d)", data_length);
        return -1;
    }

    memset(message, 0, NEWCAMD_HDR_LEN + 4);
    memcpy(message + NEWCAMD_HDR_LEN + 4, data, (size_t)data_length);

    /* The section length is re-encoded from the real payload size; the
     * section syntax flags in the top nibble are kept. */
    message[NEWCAMD_HDR_LEN + 5] =
        (uint8_t)((data[1] & 0xf0u) | (((data_length - 3) >> 8) & 0x0fu));
    message[NEWCAMD_HDR_LEN + 6] = (uint8_t)((data_length - 3) & 0xff);

    message[4] = (uint8_t)(service_id >> 8);
    message[5] = (uint8_t)service_id;
    message[6] = (uint8_t)(ca_system_id >> 8);
    message[7] = (uint8_t)ca_system_id;
    message[8] = (uint8_t)(provider_id >> 16);
    message[9] = (uint8_t)(provider_id >> 8);
    message[10] = (uint8_t)provider_id;

    if (use_msg_id) {
        ++connection->newcamd.msg_id;
        message[2] = (uint8_t)(connection->newcamd.msg_id >> 8);
        message[3] = (uint8_t)connection->newcamd.msg_id;
    }

    length = pad_message(message, data_length + NEWCAMD_HDR_LEN + 4);
    if (length < 0) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: padding failed");
        return -1;
    }

    /* The IV goes on the wire after the ciphertext, in the clear. */
    dtv_camd_random(iv, sizeof(iv));
    memcpy(message + length, iv, sizeof(iv));
    dtv_des_ede2_cbc(connection->newcamd.active_key,
                     connection->newcamd.active_key + 8, iv,
                     message + 2, message + 2, (size_t)length - 2u, 1);
    length += (int)sizeof(iv);

    message[0] = (uint8_t)((length - 2) >> 8);
    message[1] = (uint8_t)((length - 2) & 0xff);

    if (dtv_camd_write_all(connection, message, (size_t)length) < 0) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: could not send");
        return -1;
    }
    return 1;
}

/* Reads one message and copies its payload into data. Returns the payload
 * length, 0 on a protocol error, DTV_CAMD_IO_TIMEOUT on silence, and
 * NEWCAMD_STALE when the message id did not match the request. */
static int recv_message(dtv_camd_connection *connection, uint8_t *data,
                        int use_msg_id)
{
    uint8_t *message = connection->newcamd.buffer;
    uint8_t iv[8];
    int length, payload, rc;

    rc = dtv_camd_read_all(connection, message, 2);
    if (rc == DTV_CAMD_IO_TIMEOUT) {
        snprintf(connection->error, sizeof(connection->error),
                 "The CAM server did not answer");
        return DTV_CAMD_IO_TIMEOUT;
    }
    if (rc < 0) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: no reply");
        return -1;
    }
    length = ((message[0] << 8) | message[1]) & 0xffff;
    if (length < 16 || length > DTV_CAMD_MSG_SIZE - 2) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: invalid reply length (%d)", length);
        return -1;
    }
    if (dtv_camd_read_all(connection, message + 2, (size_t)length) < 0) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: reply cut short");
        return -1;
    }
    length += 2;

    if ((length - 2) % 8 || (length - 2) < 16) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: reply not aligned to the cipher block");
        return -1;
    }
    length -= (int)sizeof(iv);
    memcpy(iv, message + length, sizeof(iv));
    dtv_des_ede2_cbc(connection->newcamd.active_key,
                     connection->newcamd.active_key + 8, iv,
                     message + 2, message + 2, (size_t)length - 2u, 0);

    if (xor_sum(message + 2, (size_t)length - 2u)) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: checksum error (the DES key may be wrong)");
        return -1;
    }

    if (use_msg_id) {
        uint16_t answered =
            (uint16_t)(((message[2] << 8) | message[3]) & 0xffff);
        if (answered != connection->newcamd.msg_id)
            return NEWCAMD_STALE;
    }

    payload = (((message[5 + NEWCAMD_HDR_LEN] << 8) |
                message[6 + NEWCAMD_HDR_LEN]) & 0x0fff) + 3;
    if (payload > length - (4 + NEWCAMD_HDR_LEN)) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: reply overflows its buffer");
        return -1;
    }
    memmove(data, message + 4 + NEWCAMD_HDR_LEN, (size_t)payload);
    return payload;
}

static int send_command(dtv_camd_connection *connection, uint8_t command)
{
    uint8_t data[3] = { 0, 0, 0 };
    data[0] = command;
    return send_message(connection, data, sizeof(data), 0, 0, 0, 0);
}

static void read_card_data(dtv_camd_connection *connection,
                           const uint8_t *data)
{
    connection->newcamd.ca_system_id =
        (uint16_t)((data[4] << 8) | data[5]);
}

static int newcamd_login(dtv_camd_connection *connection)
{
    uint8_t *buffer = connection->newcamd.buffer;
    uint8_t random_key[14], mixed[14];
    char crypted_password[DTV_MD5_CRYPT_SIZE];
    size_t user_length, password_length;
    unsigned i;
    int answer;

    connection->newcamd.msg_id = 0;
    connection->newcamd.ca_system_id = 0;

    /* The server opens with 14 random bytes; the login key is those xored
     * with the configured key. */
    if (dtv_camd_read_all(connection, random_key, sizeof(random_key)) < 0) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: the handshake could not be read");
        return -1;
    }
    for (i = 0; i < sizeof(mixed); ++i)
        mixed[i] = (uint8_t)(random_key[i] ^ connection->config.des_key[i]);
    des_key_spread(connection->newcamd.login_key, mixed);
    set_active_key(connection, connection->newcamd.login_key);

    dtv_md5_crypt(connection->config.password, "$1$abcdefgh$",
                  crypted_password, sizeof(crypted_password));

    user_length = strlen(connection->config.user) + 1u;
    password_length = strlen(crypted_password) + 1u;
    if (user_length + password_length + 3u > NEWCAMD_MSG_MAX) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: user name too long");
        return -1;
    }

    buffer[0] = MSG_CLIENT_2_SERVER_LOGIN;
    buffer[1] = 0;
    buffer[2] = (uint8_t)(user_length + password_length);
    memcpy(buffer + 3, connection->config.user, user_length);
    memcpy(buffer + 3 + user_length, crypted_password, password_length);

    if (send_message(connection, buffer, (int)(buffer[2] + 3),
                     NEWCAMD_CLIENT_ID, 0, 0, 1) < 0)
        return -1;

    {
        uint8_t reply[DTV_CAMD_MSG_SIZE];
        int length = recv_message(connection, reply, 0);
        if (length != 3) {
            if (connection->error[0] == 0)
                snprintf(connection->error, sizeof(connection->error),
                         "newcamd: unexpected login reply (%d)", length);
            return -1;
        }
        answer = reply[0];
    }
    if (answer != MSG_CLIENT_2_SERVER_LOGIN_ACK) {
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: login refused (user, password or DES key)");
        return -1;
    }

    /* After login the traffic key changes: the configured key xored with
     * the crypt(3) password, spread the same way. */
    memcpy(mixed, connection->config.des_key, sizeof(mixed));
    for (i = 0; i + 1u < password_length; ++i)
        mixed[i % 14u] ^= (uint8_t)crypted_password[i];
    des_key_spread(connection->newcamd.session_key, mixed);
    set_active_key(connection, connection->newcamd.session_key);

    /* Asking what card the server holds is a courtesy, not part of the
     * login: a server with no reader answers nothing, and failing over
     * that would call a working card server broken. Short wait, optional
     * answer. */
    if (send_command(connection, MSG_CARD_DATA_REQ) >= 0) {
        uint8_t reply[DTV_CAMD_MSG_SIZE];
        int length;
        dtv_camd_set_read_timeout(connection, DTV_CAMD_INFO_TIMEOUT_MS);
        length = recv_message(connection, reply, 0);
        dtv_camd_set_read_timeout(connection, DTV_CAMD_ECM_TIMEOUT_MS);
        if (length > 0 && reply[0] == MSG_CARD_DATA)
            read_card_data(connection, reply);
        else if (length < 0 && length != DTV_CAMD_IO_TIMEOUT)
            return -1;      /* the connection itself is gone */
    }

    connection->error[0] = 0;
    return 1;
}

static int newcamd_send_ecm(dtv_camd_connection *connection,
                            const uint8_t *section, size_t size)
{
    return send_message(connection, section, (int)size,
                        connection->service_id, connection->ca_system_id,
                        connection->provider_id, 1);
}

static int newcamd_recv_cw(dtv_camd_connection *connection,
                           uint8_t control_word[16])
{
    uint8_t reply[DTV_CAMD_MSG_SIZE];
    int length, attempts = 0;

    /* Not every message is an answer to our ECM. In extended (mgcamd)
     * mode the server announces its cards on its own, one message per CA
     * system, with their own message ids; reading one as a control word
     * looks exactly like an id mismatch, and the first version of this
     * client reconnected forever over it. So: read until an ECM answer
     * (table id 0x80 or 0x81) turns up, skipping stale answers too. */
    for (;;) {
        length = recv_message(connection, reply, 1);
        if (length == DTV_CAMD_IO_TIMEOUT)
            break;
        if (length == NEWCAMD_STALE ||
            (length > 0 && reply[0] != 0x80 && reply[0] != 0x81)) {
            if (++attempts > 64) {
                snprintf(connection->error, sizeof(connection->error),
                         "newcamd: no answer to the ECM");
                return -1;
            }
            continue;
        }
        break;
    }

    if (length == 19) {
        memcpy(control_word, reply + 3, 16);
        return 1;
    }
    if (length == 3) {
        /* The server answered properly: this card cannot decode it. */
        snprintf(connection->error, sizeof(connection->error),
                 "The card cannot decrypt this channel");
        return 0;
    }
    if (length == DTV_CAMD_IO_TIMEOUT) {
        /* Silence, not a broken link. The next request gets a new message
         * id and a stale answer to this one is skipped. */
        return 0;
    }
    if (connection->error[0] == 0)
        snprintf(connection->error, sizeof(connection->error),
                 "newcamd: unexpected reply (%d bytes)", length);
    return -1;
}

static int newcamd_keepalive(dtv_camd_connection *connection)
{
    uint8_t reply[DTV_CAMD_MSG_SIZE];
    if (send_command(connection, MSG_KEEPALIVE) < 0)
        return -1;
    /* The server echoes the keepalive back; leaving it in the socket would
     * desynchronise the next ECM exchange. */
    if (recv_message(connection, reply, 0) <= 0)
        return -1;
    return 1;
}

void dtv_camd_ops_newcamd(dtv_camd_ops *ops)
{
    ops->name = "newcamd";
    ops->verifies_login = 1;
    ops->login = newcamd_login;
    ops->send_ecm = newcamd_send_ecm;
    ops->recv_cw = newcamd_recv_cw;
    ops->keepalive = newcamd_keepalive;
}
