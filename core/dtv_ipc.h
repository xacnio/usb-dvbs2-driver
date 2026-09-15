/* dtv_ipc.h - the daemon's command channel.
 *
 * A local, byte-stream connection between the daemon and one client at a
 * time: a named pipe on Windows (\\.\pipe\usb_dvbs2), a Unix domain socket
 * on Linux and macOS ($XDG_RUNTIME_DIR/usb_dvbs2.sock, or
 * /tmp/usb_dvbs2-<uid>.sock without it). The protocol on it is the same
 * text lines everywhere; see README.md. */
#ifndef DTV_IPC_H
#define DTV_IPC_H

#include <stddef.h>

typedef struct dtv_ipc dtv_ipc;
typedef struct dtv_ipc_server dtv_ipc_server;

/* The name the daemon listens on and the client library connects to when
 * none is given. */
const char *dtv_ipc_default_name(void);

/* --- daemon side --------------------------------------------------------- */

/* NULL when the name cannot be listened on. */
dtv_ipc_server *dtv_ipc_listen(const char *name);

/* Waits for the next client. The connection belongs to the server: end the
 * session with dtv_ipc_disconnect(), not dtv_ipc_close(). NULL on error. */
dtv_ipc *dtv_ipc_accept(dtv_ipc_server *server);
void dtv_ipc_disconnect(dtv_ipc *connection);

void dtv_ipc_server_close(dtv_ipc_server *server);

/* Makes a blocked dtv_ipc_accept() or dtv_ipc_read() on this server return
 * at once, so the daemon can end cleanly. Safe to call from a POSIX signal
 * handler; nothing to do on Windows, where the daemon is ended with QUIT. */
void dtv_ipc_server_interrupt(dtv_ipc_server *server);

/* --- client side --------------------------------------------------------- */

/* NULL when nothing is listening. */
dtv_ipc *dtv_ipc_connect(const char *name);
void dtv_ipc_close(dtv_ipc *connection);

/* --- either side --------------------------------------------------------- */

/* Blocks for at least one byte. Returns the count, 0 when the other end has
 * gone, -1 on error. */
int dtv_ipc_read(dtv_ipc *connection, char *buffer, size_t size);

/* Writes all of it; 0 on success. */
int dtv_ipc_write(dtv_ipc *connection, const char *data, size_t size);

/* Bytes that can be read without blocking. */
size_t dtv_ipc_available(dtv_ipc *connection);

#endif
