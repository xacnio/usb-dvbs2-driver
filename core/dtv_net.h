/* dtv_net.h - BSD sockets on Winsock and POSIX alike.
 *
 * The stream goes out on UDP and the card server client speaks TCP; both
 * use the ordinary sockets API, which differs between the two only in its
 * handle type, how a socket is closed, how errors are read and that
 * Winsock has to be started. */
#ifndef DTV_NET_H
#define DTV_NET_H

#ifdef _WIN32
/* winsock2.h must precede anything that pulls in windows.h. */
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET dtv_socket;
#define DTV_INVALID_SOCKET INVALID_SOCKET
#define DTV_SOCKET_ERROR   SOCKET_ERROR
#define dtv_socket_close   closesocket
#define dtv_socket_error() WSAGetLastError()
#define DTV_EWOULDBLOCK    WSAEWOULDBLOCK
#define DTV_EINPROGRESS    WSAEWOULDBLOCK
#define DTV_ETIMEDOUT      WSAETIMEDOUT
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef int dtv_socket;
#define DTV_INVALID_SOCKET (-1)
#define DTV_SOCKET_ERROR   (-1)
#define dtv_socket_close   close
#define dtv_socket_error() errno
#define DTV_EWOULDBLOCK    EWOULDBLOCK
#define DTV_EINPROGRESS    EINPROGRESS
/* A receive timeout shows as EAGAIN on POSIX, WSAETIMEDOUT on Winsock. */
#define DTV_ETIMEDOUT      EAGAIN
#endif

/* Reference counted; pair every successful call with dtv_net_cleanup().
 * On POSIX it also keeps a peer that closed the connection from ending the
 * process with SIGPIPE. 0 on success. */
int  dtv_net_startup(void);
void dtv_net_cleanup(void);

/* 0 on success. */
int dtv_socket_set_nonblocking(dtv_socket socket_handle, int enabled);

/* Receive and send timeouts. */
void dtv_socket_set_timeouts(dtv_socket socket_handle, unsigned receive_ms,
                             unsigned send_ms);

#endif /* DTV_NET_H */
