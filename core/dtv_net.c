/* dtv_net.c - see dtv_net.h. */
#include "dtv_net.h"

#ifdef _WIN32

int dtv_net_startup(void)
{
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
}

void dtv_net_cleanup(void) { WSACleanup(); }

int dtv_socket_set_nonblocking(dtv_socket socket_handle, int enabled)
{
    u_long mode = enabled ? 1u : 0u;
    return ioctlsocket(socket_handle, FIONBIO, &mode) == 0 ? 0 : -1;
}

void dtv_socket_set_timeouts(dtv_socket socket_handle, unsigned receive_ms,
                             unsigned send_ms)
{
    DWORD receive = (DWORD)receive_ms, send = (DWORD)send_ms;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&receive,
               sizeof(receive));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send,
               sizeof(send));
}

#else

#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>

int dtv_net_startup(void)
{
    /* Writing to a connection the card server has closed must fail the
     * write, not end the process. Setting it more than once is harmless. */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void dtv_net_cleanup(void) { }

int dtv_socket_set_nonblocking(dtv_socket socket_handle, int enabled)
{
    int flags = fcntl(socket_handle, F_GETFL, 0);
    if (flags < 0)
        return -1;
    flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(socket_handle, F_SETFL, flags) == 0 ? 0 : -1;
}

void dtv_socket_set_timeouts(dtv_socket socket_handle, unsigned receive_ms,
                             unsigned send_ms)
{
    struct timeval receive = { (time_t)(receive_ms / 1000u),
                               (suseconds_t)(receive_ms % 1000u) * 1000 };
    struct timeval send = { (time_t)(send_ms / 1000u),
                            (suseconds_t)(send_ms % 1000u) * 1000 };
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &receive,
               sizeof(receive));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, &send, sizeof(send));
}

#endif
