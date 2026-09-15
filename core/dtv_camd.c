/* CAM client worker: owns the socket, the timing and the key store.
 *
 * The stream thread only uses dtv_camd_ecm_pid(), dtv_camd_submit_ecm()
 * and dtv_camd_get_keys(), none of which block. Everything slow runs on
 * the worker thread:
 *
 *   connect -> login -> [ wait for an ECM -> send it -> read the control
 *              words -> publish them ] -> keepalive while idle
 *
 * A failure closes the socket and starts over after a short pause; a
 * channel the card refuses only rotates to the next CA system. */
#include "dtv_camd_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtv_platform.h"

#define CAMD_CONNECT_TIMEOUT_MS   1500
/* How long to wait for an answer to an ECM. It must outlast the server's
 * own giving-up time (OSCam clienttimeout, often 12 s) or the client tears
 * the connection down before the answer arrives -- a 5 s timeout produced
 * an endless connect/timeout/reconnect cycle. A timeout is therefore not
 * a broken connection: the message id makes a late answer recognisable. */
#define CAMD_IO_TIMEOUT_MS        DTV_CAMD_ECM_TIMEOUT_MS
#define CAMD_RETRY_DELAY_MS       2000
#define CAMD_KEEPALIVE_MS         8000
/* Refusals in a row before another CA system is tried. One is too eager:
 * OSCam says cannot decode once while a network reader wakes up. */
#define CAMD_REJECTS_BEFORE_ROTATE 3
#define CAMD_MAX_ECM_SIZE          512

struct dtv_camd {
    dtv_camd_config config;
    dtv_camd_ops ops;
    dtv_camd_connection connection;

    dtv_mutex lock;
    dtv_thread *thread;
    dtv_event *wake;
    dtv_atomic32 stop;

    /* --- guarded by lock --- */
    dtv_camd_candidate candidates[DTV_CAMD_MAX_CANDIDATES];
    size_t candidate_count;
    size_t candidate_index;
    uint16_t service_id;

    uint8_t pending_ecm[CAMD_MAX_ECM_SIZE];
    size_t pending_size;
    /* Last section sent, so repeats of the same ECM cost nothing. */
    uint8_t last_ecm[CAMD_MAX_ECM_SIZE];
    size_t last_size;

    uint8_t even[8];
    uint8_t odd[8];
    int even_valid;
    int odd_valid;
    unsigned generation;

    dtv_camd_status status;
    /* Read without the lock by dtv_camd_ecm_pid(), on every USB read. */
    dtv_atomic32 active_ecm_pid;
};

/* ---- helpers ------------------------------------------------------- */

static const uint8_t zero_cw[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

void dtv_camd_random(uint8_t *buffer, size_t size)
{
    /* Padding and CBC initialisation vectors for a link that is normally
     * to 127.0.0.1: they must not repeat, but need not resist an attacker,
     * so no cryptographic generator is pulled in. */
    static dtv_atomic32 counter;
    static uint64_t state;
    size_t i;
    if (!state) {
        state = dtv_utc_now_ticks() ^ ((uint64_t)dtv_process_id() << 32) ^
                (uint64_t)dtv_tick_ms();
        if (!state)
            state = 0x9e3779b97f4a7c15ull;
    }
    state ^= (uint64_t)dtv_atomic_inc(&counter) * 0x9e3779b97f4a7c15ull;
    for (i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        buffer[i] = (uint8_t)(state >> 24);
    }
}

void dtv_camd_set_read_timeout(dtv_camd_connection *connection,
                               unsigned milliseconds)
{
    if (!connection || connection->socket_handle == DTV_INVALID_SOCKET)
        return;
    dtv_socket_set_timeouts(connection->socket_handle, milliseconds,
                            CAMD_IO_TIMEOUT_MS);
}

int dtv_camd_read_all(dtv_camd_connection *connection, void *buffer,
                      size_t size)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0;
    while (done < size) {
        int got = recv(connection->socket_handle, (char *)out + done,
                       (int)(size - done), 0);
        if (got == DTV_SOCKET_ERROR && done == 0 &&
            (dtv_socket_error() == DTV_ETIMEDOUT ||
             dtv_socket_error() == DTV_EWOULDBLOCK))
            return DTV_CAMD_IO_TIMEOUT;
        if (got <= 0)
            return -1;
        done += (size_t)got;
    }
    return (int)done;
}

int dtv_camd_write_all(dtv_camd_connection *connection, const void *buffer,
                       size_t size)
{
    const uint8_t *in = (const uint8_t *)buffer;
    size_t done = 0;
    while (done < size) {
        int sent = send(connection->socket_handle, (const char *)in + done,
                        (int)(size - done), 0);
        if (sent <= 0)
            return -1;
        done += (size_t)sent;
    }
    return (int)done;
}

static void set_status(dtv_camd *camd, dtv_camd_state state,
                       const char *message)
{
    dtv_mutex_lock(&camd->lock);
    camd->status.state = state;
    if (message) {
        snprintf(camd->status.message, sizeof(camd->status.message), "%s",
                 message);
        if (state == DTV_CAMD_STATE_ERROR ||
            state == DTV_CAMD_STATE_REJECTED)
            snprintf(camd->status.last_error,
                     sizeof(camd->status.last_error), "%s", message);
    }
    dtv_mutex_unlock(&camd->lock);
}

static void close_connection(dtv_camd *camd)
{
    if (camd->connection.socket_handle != DTV_INVALID_SOCKET) {
        dtv_socket handle = camd->connection.socket_handle;
        camd->connection.socket_handle = DTV_INVALID_SOCKET;
#ifndef _WIN32
        /* Closing a descriptor does not wake a recv() blocked on it in
         * another thread on POSIX, as closesocket() does on Winsock;
         * shutting the connection down does. */
        shutdown(handle, SHUT_RDWR);
#endif
        dtv_socket_close(handle);
    }
}

/* Connects with a bounded wait: a card server that is not running must
 * fail fast, since the user is looking at a black picture meanwhile. */
static int open_connection(dtv_camd *camd)
{
    struct addrinfo hints, *result = NULL, *entry;
    char port_text[16];
    dtv_socket handle = DTV_INVALID_SOCKET;

    snprintf(port_text, sizeof(port_text), "%u", camd->config.port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(camd->config.host, port_text, &hints, &result) != 0)
        return -1;

    for (entry = result; entry; entry = entry->ai_next) {
        fd_set writable, failed;
        struct timeval timeout;
        int flag = 1, error = 0;
        socklen_t error_size = sizeof(error);

        handle = socket(entry->ai_family, entry->ai_socktype,
                        entry->ai_protocol);
        if (handle == DTV_INVALID_SOCKET)
            continue;
        dtv_socket_set_nonblocking(handle, 1);
        if (connect(handle, entry->ai_addr, (int)entry->ai_addrlen) != 0 &&
            dtv_socket_error() != DTV_EWOULDBLOCK &&
            dtv_socket_error() != DTV_EINPROGRESS) {
            dtv_socket_close(handle);
            handle = DTV_INVALID_SOCKET;
            continue;
        }
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(handle, &writable);
        FD_SET(handle, &failed);
        timeout.tv_sec = CAMD_CONNECT_TIMEOUT_MS / 1000;
        timeout.tv_usec = (CAMD_CONNECT_TIMEOUT_MS % 1000) * 1000;
        /* Writable with no pending error is a connection; POSIX reports a
         * refused one as writable too, so the error is read either way. */
        if (select((int)handle + 1, NULL, &writable, &failed, &timeout) <= 0 ||
            !FD_ISSET(handle, &writable) ||
            getsockopt(handle, SOL_SOCKET, SO_ERROR, (char *)&error,
                       &error_size) != 0 || error != 0) {
            dtv_socket_close(handle);
            handle = DTV_INVALID_SOCKET;
            continue;
        }
        dtv_socket_set_nonblocking(handle, 0);
        /* Every message is a small request whose answer is awaited;
         * Nagle would add a round trip per control word. */
        setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag,
                   sizeof(flag));
        dtv_socket_set_timeouts(handle, CAMD_IO_TIMEOUT_MS, CAMD_IO_TIMEOUT_MS);
        break;
    }
    freeaddrinfo(result);

    if (handle == DTV_INVALID_SOCKET)
        return -1;
    camd->connection.socket_handle = handle;
    return 0;
}

/* Copies the CA system the worker is asking about into the connection and
 * publishes its pid for the stream thread. */
static void apply_candidate(dtv_camd *camd)
{
    uint16_t pid = 0x1fff;
    dtv_mutex_lock(&camd->lock);
    if (camd->candidate_index < camd->candidate_count) {
        const dtv_camd_candidate *candidate =
            &camd->candidates[camd->candidate_index];
        camd->connection.service_id = camd->service_id;
        camd->connection.ca_system_id = candidate->ca_system_id;
        camd->connection.provider_id = candidate->provider_id;
        camd->connection.ecm_pid = candidate->ecm_pid;
        pid = candidate->ecm_pid;
        camd->status.ca_system_id = candidate->ca_system_id;
        camd->status.ecm_pid = pid;
    } else {
        camd->connection.ca_system_id = 0;
        camd->connection.provider_id = 0;
        camd->status.ecm_pid = 0x1fff;
    }
    dtv_mutex_unlock(&camd->lock);
    dtv_atomic_set(&camd->active_ecm_pid, pid);
}

static void rotate_candidate(dtv_camd *camd)
{
    int rotated = 0;
    dtv_mutex_lock(&camd->lock);
    if (camd->candidate_count > 1) {
        camd->candidate_index =
            (camd->candidate_index + 1u) % camd->candidate_count;
        rotated = 1;
    }
    /* The pending ECM belongs to the pid we are leaving behind. */
    camd->pending_size = 0;
    camd->last_size = 0;
    dtv_mutex_unlock(&camd->lock);
    if (rotated)
        apply_candidate(camd);
}

static void publish_control_words(dtv_camd *camd, const uint8_t cw[16],
                                  unsigned latency_ms)
{
    dtv_mutex_lock(&camd->lock);
    /* A server that only knows one parity sends zeros for the other; the
     * previous key for that parity still stands. */
    if (memcmp(cw, zero_cw, 8) != 0) {
        memcpy(camd->even, cw, 8);
        camd->even_valid = 1;
    }
    if (memcmp(cw + 8, zero_cw, 8) != 0) {
        memcpy(camd->odd, cw + 8, 8);
        camd->odd_valid = 1;
    }
    ++camd->generation;
    ++camd->status.cw_received;
    camd->status.last_latency_ms = latency_ms;
    camd->status.state = DTV_CAMD_STATE_DECRYPTING;
    snprintf(camd->status.message, sizeof(camd->status.message),
             "Descrambling (%u ms)", latency_ms);
    dtv_mutex_unlock(&camd->lock);
}

/* Takes the ECM waiting to be sent, if any. Returns its size. */
static size_t take_pending_ecm(dtv_camd *camd, uint8_t *out)
{
    size_t size;
    dtv_mutex_lock(&camd->lock);
    size = camd->pending_size;
    if (size) {
        memcpy(out, camd->pending_ecm, size);
        memcpy(camd->last_ecm, camd->pending_ecm, size);
        camd->last_size = size;
        camd->pending_size = 0;
    }
    dtv_mutex_unlock(&camd->lock);
    return size;
}

static void camd_worker(void *argument)
{
    dtv_camd *camd = (dtv_camd *)argument;
    int logged_in = 0;
    int rejects = 0;
    uint64_t last_traffic = dtv_tick_ms();

    while (!dtv_atomic_get(&camd->stop)) {
        uint8_t ecm[CAMD_MAX_ECM_SIZE];
        uint8_t control_word[16];
        size_t ecm_size;
        uint64_t started;
        int rc;

        if (!logged_in) {
            char text[192];
            set_status(camd, DTV_CAMD_STATE_CONNECTING,
                       "Connecting to the CAM server");
            camd->connection.error[0] = 0;
            if (open_connection(camd) != 0) {
                snprintf(text, sizeof(text),
                         "Could not connect to the CAM server (%s:%u)",
                         camd->config.host, camd->config.port);
                set_status(camd, DTV_CAMD_STATE_ERROR, text);
                if (dtv_event_wait(camd->wake, CAMD_RETRY_DELAY_MS) &&
                    dtv_atomic_get(&camd->stop))
                    break;
                continue;
            }
            if (camd->ops.login(&camd->connection) != 1) {
                snprintf(text, sizeof(text), "%s",
                         camd->connection.error[0]
                             ? camd->connection.error
                             : "CAM login failed");
                set_status(camd, DTV_CAMD_STATE_ERROR, text);
                close_connection(camd);
                if (dtv_event_wait(camd->wake, CAMD_RETRY_DELAY_MS) &&
                    dtv_atomic_get(&camd->stop))
                    break;
                continue;
            }
            logged_in = 1;
            rejects = 0;
            last_traffic = dtv_tick_ms();
            if (camd->ops.verifies_login) {
                snprintf(text, sizeof(text), "CAM connected (%s)",
                         camd->ops.name);
                set_status(camd, DTV_CAMD_STATE_CONNECTED, text);
            } else {
                /* The socket opened, nothing more. The state advances the
                 * moment the server answers anything. */
                snprintf(text, sizeof(text),
                         "Connected to the CAM server, waiting for a reply (%s)",
                         camd->ops.name);
                set_status(camd, DTV_CAMD_STATE_CONNECTING, text);
            }
            /* An ECM may have been waiting through the whole login. */
            dtv_event_set(camd->wake);
        }

        ecm_size = take_pending_ecm(camd, ecm);
        if (!ecm_size) {
            int woken = dtv_event_wait(camd->wake, 500);
            if (dtv_atomic_get(&camd->stop))
                break;
            if (!woken &&
                dtv_tick_ms() - last_traffic > CAMD_KEEPALIVE_MS) {
                if (camd->ops.keepalive &&
                    camd->ops.keepalive(&camd->connection) < 0) {
                    close_connection(camd);
                    logged_in = 0;
                    set_status(camd, DTV_CAMD_STATE_ERROR,
                               "CAM connection dropped");
                    continue;
                }
                last_traffic = dtv_tick_ms();
            }
            continue;
        }

        camd->connection.error[0] = 0;
        started = dtv_tick_ms();
        dtv_mutex_lock(&camd->lock);
        ++camd->status.ecm_sent;
        dtv_mutex_unlock(&camd->lock);

        if (camd->ops.send_ecm(&camd->connection, ecm, ecm_size) < 0) {
            close_connection(camd);
            logged_in = 0;
            set_status(camd, DTV_CAMD_STATE_ERROR,
                       camd->connection.error[0] ? camd->connection.error
                                                 : "ECM could not be sent");
            continue;
        }

        rc = camd->ops.recv_cw(&camd->connection, control_word);
        last_traffic = dtv_tick_ms();
        if (rc == 1) {
            rejects = 0;
            publish_control_words(camd, control_word,
                                  (unsigned)(dtv_tick_ms() - started));
            continue;
        }

        dtv_mutex_lock(&camd->lock);
        ++camd->status.ecm_failed;
        dtv_mutex_unlock(&camd->lock);

        if (rc == 0) {
            /* The server is healthy, the card just cannot decode this
             * one; the answer is often not yet rather than no. */
            set_status(camd, DTV_CAMD_STATE_REJECTED,
                       camd->connection.error[0]
                           ? camd->connection.error
                           : "The card cannot decrypt this channel");
            if (++rejects >= CAMD_REJECTS_BEFORE_ROTATE) {
                rejects = 0;
                rotate_candidate(camd);
            }
            continue;
        }

        close_connection(camd);
        logged_in = 0;
        set_status(camd, DTV_CAMD_STATE_ERROR,
                   camd->connection.error[0] ? camd->connection.error
                                             : "The CAM server did not answer");
    }

    close_connection(camd);
}

/* ---- public interface ---------------------------------------------- */

int dtv_camd_available(void)
{
    return 1;
}

dtv_camd *dtv_camd_start(const dtv_camd_config *config)
{
    dtv_camd *camd;

    if (!config || !config->host[0] || !config->port)
        return NULL;

    camd = (dtv_camd *)calloc(1, sizeof(*camd));
    if (!camd)
        return NULL;

    /* Reference counted; the stream thread starts it too. */
    dtv_net_startup();

    camd->config = *config;
    camd->connection.config = *config;
    camd->connection.socket_handle = DTV_INVALID_SOCKET;
    camd->active_ecm_pid = 0x1fff;
    camd->status.ecm_pid = 0x1fff;
    switch (config->proto) {
    case DTV_CAMD_PROTO_CS378X:
        dtv_camd_ops_cs378x(&camd->ops);
        break;
    case DTV_CAMD_PROTO_NEWCAMD:
    default:
        dtv_camd_ops_newcamd(&camd->ops);
        break;
    }

    dtv_mutex_init(&camd->lock);
    camd->wake = dtv_event_create();
    if (!camd->wake) {
        dtv_mutex_destroy(&camd->lock);
        free(camd);
        dtv_net_cleanup();
        return NULL;
    }
    snprintf(camd->status.message, sizeof(camd->status.message),
             "Starting the CAM client");
    camd->status.state = DTV_CAMD_STATE_CONNECTING;
    camd->thread = dtv_thread_start(camd_worker, camd);
    if (!camd->thread) {
        dtv_event_destroy(camd->wake);
        dtv_mutex_destroy(&camd->lock);
        free(camd);
        dtv_net_cleanup();
        return NULL;
    }
    return camd;
}

void dtv_camd_stop(dtv_camd *camd)
{
    if (!camd)
        return;
    dtv_atomic_set(&camd->stop, 1);
    dtv_event_set(camd->wake);
    /* The worker may be part way through a fourteen second wait and a
     * channel change must not queue behind it. Closing the socket makes
     * the blocked recv() fail at once. */
    if (dtv_thread_join(camd->thread, 200) != 0) {
        close_connection(camd);
        if (dtv_thread_join(camd->thread, 3000) != 0 &&
            dtv_thread_join(camd->thread, CAMD_IO_TIMEOUT_MS) != 0) {
            /* Still inside a call that never returned: it owns the client,
             * so the client is left to it rather than freed under it. */
            dtv_thread_detach(camd->thread);
            return;
        }
    }
    dtv_event_destroy(camd->wake);
    dtv_mutex_destroy(&camd->lock);
    free(camd);
    dtv_net_cleanup();
}

void dtv_camd_set_service(dtv_camd *camd, uint16_t service_id,
                          const dtv_camd_candidate *candidates,
                          size_t count)
{
    if (!camd)
        return;
    if (count > DTV_CAMD_MAX_CANDIDATES)
        count = DTV_CAMD_MAX_CANDIDATES;
    dtv_mutex_lock(&camd->lock);
    camd->service_id = service_id;
    camd->candidate_count = count;
    camd->candidate_index = 0;
    if (count)
        memcpy(camd->candidates, candidates,
               count * sizeof(camd->candidates[0]));
    /* Keys from the previous service would turn a scrambled picture into
     * noise instead of leaving it visibly scrambled. */
    camd->even_valid = camd->odd_valid = 0;
    ++camd->generation;
    camd->pending_size = camd->last_size = 0;
    camd->status.ecm_sent = camd->status.cw_received = 0;
    camd->status.ecm_failed = 0;
    camd->status.last_latency_ms = 0;
    if (!count) {
        camd->status.state = DTV_CAMD_STATE_CONNECTED;
        /* An empty candidate list also means a newly selected service is
         * waiting for its live PMT, so calling it free-to-air would
         * contradict the scanner. A neutral waiting message fits both. */
        snprintf(camd->status.message, sizeof(camd->status.message),
                 "Waiting for channel information");
    } else {
        camd->status.state = DTV_CAMD_STATE_CONNECTED;
        snprintf(camd->status.message, sizeof(camd->status.message),
                 "Waiting for an ECM");
    }
    dtv_mutex_unlock(&camd->lock);
    apply_candidate(camd);
}

uint16_t dtv_camd_ecm_pid(dtv_camd *camd)
{
    if (!camd)
        return 0x1fff;
    return (uint16_t)dtv_atomic_get(&camd->active_ecm_pid);
}

void dtv_camd_submit_ecm(dtv_camd *camd, const uint8_t *section, size_t size)
{
    int wake = 0;
    if (!camd || !section || size < 3 || size > CAMD_MAX_ECM_SIZE)
        return;
    /* Only the two ECM table ids; anything else on the pid (EMM, or the
     * CA system's own bookkeeping) is not a control word request. */
    if (section[0] != 0x80 && section[0] != 0x81)
        return;

    dtv_mutex_lock(&camd->lock);
    if (camd->last_size != size ||
        memcmp(camd->last_ecm, section, size) != 0) {
        memcpy(camd->pending_ecm, section, size);
        camd->pending_size = size;
        wake = 1;
    }
    dtv_mutex_unlock(&camd->lock);
    if (wake)
        dtv_event_set(camd->wake);
}

int dtv_camd_get_keys(dtv_camd *camd, uint8_t even[8], uint8_t odd[8],
                      unsigned *generation)
{
    int valid;
    if (!camd)
        return 0;
    dtv_mutex_lock(&camd->lock);
    memcpy(even, camd->even, 8);
    memcpy(odd, camd->odd, 8);
    valid = camd->even_valid || camd->odd_valid;
    if (generation)
        *generation = camd->generation;
    dtv_mutex_unlock(&camd->lock);
    return valid;
}

void dtv_camd_get_status(dtv_camd *camd, dtv_camd_status *status)
{
    if (!status)
        return;
    if (!camd) {
        memset(status, 0, sizeof(*status));
        status->state = DTV_CAMD_STATE_OFF;
        snprintf(status->message, sizeof(status->message), "CAM off");
        status->ecm_pid = 0x1fff;
        return;
    }
    dtv_mutex_lock(&camd->lock);
    *status = camd->status;
    dtv_mutex_unlock(&camd->lock);
}

size_t dtv_camd_build_biss_ecm(uint16_t service_id, uint16_t pid,
                               uint8_t *out)
{
    /* A BISS key is stored under a four byte identifier of service id +
     * one elementary pid (F C41D1455 for service 0xC41D on pid 0x1455),
     * and the emulator builds it from the first four ECM payload bytes.
     * So the ECM is exactly those four bytes: sent with two, the server
     * looked for F 14550000 and found nothing. Table id 0x80, then the
     * section length in the usual place. */
    if (!out)
        return 0;
    out[0] = 0x80;
    out[1] = 0x70;
    out[2] = 0x04;
    out[3] = (uint8_t)(service_id >> 8);
    out[4] = (uint8_t)service_id;
    out[5] = (uint8_t)(pid >> 8);
    out[6] = (uint8_t)pid;
    return DTV_CAMD_BISS_ECM_SIZE;
}

int dtv_camd_parse_des_key(const char *text,
                           uint8_t key[DTV_CAMD_DES_KEY_SIZE])
{
    size_t digits = 0;
    if (!text || !key)
        return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    memset(key, 0, DTV_CAMD_DES_KEY_SIZE);
    while (*text) {
        unsigned value;
        char c = *text++;
        if (c == ' ' || c == ':' || c == '-')
            continue;
        if (c >= '0' && c <= '9') value = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') value = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value = (unsigned)(c - 'A' + 10);
        else return -1;
        if (digits >= DTV_CAMD_DES_KEY_SIZE * 2u)
            return -1;
        if (digits & 1u)
            key[digits / 2u] |= (uint8_t)value;
        else
            key[digits / 2u] = (uint8_t)(value << 4);
        ++digits;
    }
    return digits == DTV_CAMD_DES_KEY_SIZE * 2u ? 0 : -1;
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
