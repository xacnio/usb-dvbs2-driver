/* dtv_ipc.c - see dtv_ipc.h. */
#include "dtv_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>

struct dtv_ipc {
    HANDLE handle;
};

struct dtv_ipc_server {
    struct dtv_ipc connection;
};

const char *dtv_ipc_default_name(void)
{
    return "\\\\.\\pipe\\usb_dvbs2";
}

dtv_ipc_server *dtv_ipc_listen(const char *name)
{
    dtv_ipc_server *server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    /* One instance: a second client is refused until the first leaves. */
    server->connection.handle = CreateNamedPipeA(
        name, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0,
        NULL);
    if (server->connection.handle == INVALID_HANDLE_VALUE) {
        free(server);
        return NULL;
    }
    return server;
}

dtv_ipc *dtv_ipc_accept(dtv_ipc_server *server)
{
    if (!server)
        return NULL;
    if (!ConnectNamedPipe(server->connection.handle, NULL) &&
        GetLastError() != ERROR_PIPE_CONNECTED)
        return NULL;
    return &server->connection;
}

void dtv_ipc_disconnect(dtv_ipc *connection)
{
    if (connection)
        DisconnectNamedPipe(connection->handle);
}

void dtv_ipc_server_close(dtv_ipc_server *server)
{
    if (!server)
        return;
    DisconnectNamedPipe(server->connection.handle);
    CloseHandle(server->connection.handle);
    free(server);
}

void dtv_ipc_server_interrupt(dtv_ipc_server *server)
{
    (void)server;
}

dtv_ipc *dtv_ipc_connect(const char *name)
{
    dtv_ipc *connection;
    HANDLE handle = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return NULL;
    connection = calloc(1, sizeof(*connection));
    if (!connection) {
        CloseHandle(handle);
        return NULL;
    }
    connection->handle = handle;
    return connection;
}

void dtv_ipc_close(dtv_ipc *connection)
{
    if (!connection)
        return;
    CloseHandle(connection->handle);
    free(connection);
}

int dtv_ipc_read(dtv_ipc *connection, char *buffer, size_t size)
{
    DWORD got = 0;
    if (!connection || !size)
        return -1;
    if (!ReadFile(connection->handle, buffer, (DWORD)size, &got, NULL))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return (int)got;
}

int dtv_ipc_write(dtv_ipc *connection, const char *data, size_t size)
{
    DWORD written = 0;
    /* Deliberately not followed by FlushFileBuffers: on a named pipe it
     * waits for the other end to read everything, and the daemon does not
     * read while it retunes. */
    if (!connection ||
        !WriteFile(connection->handle, data, (DWORD)size, &written, NULL))
        return -1;
    return written == size ? 0 : -1;
}

size_t dtv_ipc_available(dtv_ipc *connection)
{
    DWORD available = 0;
    if (!connection ||
        !PeekNamedPipe(connection->handle, NULL, 0, NULL, &available, NULL))
        return 0;
    return available;
}

#else /* POSIX: a Unix domain socket */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct dtv_ipc {
    int fd;
    /* Set by dtv_ipc_server_interrupt() on the daemon's side; NULL for a
     * client. */
    volatile sig_atomic_t *interrupted;
};

struct dtv_ipc_server {
    int fd;
    volatile sig_atomic_t interrupted;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct dtv_ipc connection;
};

const char *dtv_ipc_default_name(void)
{
    static char name[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (!name[0]) {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        if (runtime && runtime[0] &&
            strlen(runtime) + sizeof("/usb_dvbs2.sock") <= sizeof(name))
            snprintf(name, sizeof(name), "%s/usb_dvbs2.sock", runtime);
        else
            snprintf(name, sizeof(name), "/tmp/usb_dvbs2-%lu.sock",
                     (unsigned long)getuid());
    }
    return name;
}

static int make_address(const char *name, struct sockaddr_un *address)
{
    if (!name || strlen(name) >= sizeof(address->sun_path))
        return -1;
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, name, strlen(name) + 1);
    return 0;
}

static void no_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

dtv_ipc_server *dtv_ipc_listen(const char *name)
{
    struct sockaddr_un address;
    dtv_ipc_server *server;
    if (make_address(name, &address) != 0)
        return NULL;
    server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    server->connection.fd = -1;
    server->connection.interrupted = &server->interrupted;
    server->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->fd < 0) {
        free(server);
        return NULL;
    }
    /* A socket file left by a daemon that died is in the way of bind(); one
     * that still answers belongs to a running daemon and is left alone. */
    {
        dtv_ipc *running = dtv_ipc_connect(name);
        if (running) {
            dtv_ipc_close(running);
            close(server->fd);
            free(server);
            return NULL;
        }
        unlink(name);
    }
    if (bind(server->fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->fd, 1) != 0) {
        close(server->fd);
        free(server);
        return NULL;
    }
    /* Only this user may drive the receiver. */
    chmod(name, S_IRUSR | S_IWUSR);
    snprintf(server->path, sizeof(server->path), "%s", name);
    return server;
}

/* Waits until fd can be read, checking now and then whether the daemon is
 * being stopped: accept() and recv() are not woken by a signal or by
 * shutdown() on every system. 1 when readable, 0 when interrupted. */
static int wait_readable(int fd, volatile sig_atomic_t *interrupted)
{
    for (;;) {
        struct pollfd entry;
        int ready;
        if (interrupted && *interrupted)
            return 0;
        entry.fd = fd;
        entry.events = POLLIN;
        entry.revents = 0;
        ready = poll(&entry, 1, interrupted ? 250 : -1);
        if (ready > 0)
            return 1;
        if (ready < 0 && errno != EINTR)
            return 1; /* let the call itself report the error */
    }
}

dtv_ipc *dtv_ipc_accept(dtv_ipc_server *server)
{
    int fd;
    if (!server)
        return NULL;
    do {
        if (!wait_readable(server->fd, &server->interrupted))
            return NULL;
        fd = accept(server->fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return NULL;
    no_sigpipe(fd);
    server->connection.fd = fd;
    return &server->connection;
}

void dtv_ipc_disconnect(dtv_ipc *connection)
{
    if (connection && connection->fd >= 0) {
        close(connection->fd);
        connection->fd = -1;
    }
}

void dtv_ipc_server_close(dtv_ipc_server *server)
{
    if (!server)
        return;
    dtv_ipc_disconnect(&server->connection);
    close(server->fd);
    unlink(server->path);
    free(server);
}

void dtv_ipc_server_interrupt(dtv_ipc_server *server)
{
    /* Only a flag: the waits above see it within a quarter of a second. */
    if (server)
        server->interrupted = 1;
}


dtv_ipc *dtv_ipc_connect(const char *name)
{
    struct sockaddr_un address;
    dtv_ipc *connection;
    int fd;
    if (make_address(name, &address) != 0)
        return NULL;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return NULL;
    }
    no_sigpipe(fd);
    connection = calloc(1, sizeof(*connection));
    if (!connection) {
        close(fd);
        return NULL;
    }
    connection->fd = fd;
    return connection;
}

void dtv_ipc_close(dtv_ipc *connection)
{
    if (!connection)
        return;
    if (connection->fd >= 0)
        close(connection->fd);
    free(connection);
}

int dtv_ipc_read(dtv_ipc *connection, char *buffer, size_t size)
{
    ssize_t got;
    if (!connection || connection->fd < 0 || !size)
        return -1;
    do {
        if (!wait_readable(connection->fd, connection->interrupted))
            return 0;
        got = recv(connection->fd, buffer, size, 0);
    } while (got < 0 && errno == EINTR);
    return got < 0 ? -1 : (int)got;
}

int dtv_ipc_write(dtv_ipc *connection, const char *data, size_t size)
{
    size_t done = 0;
    if (!connection || connection->fd < 0)
        return -1;
    while (done < size) {
        ssize_t sent = send(connection->fd, data + done, size - done,
                            MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        done += (size_t)sent;
    }
    return 0;
}

size_t dtv_ipc_available(dtv_ipc *connection)
{
    int available = 0;
    if (!connection || connection->fd < 0 ||
        ioctl(connection->fd, FIONREAD, &available) != 0 || available < 0)
        return 0;
    return (size_t)available;
}

#endif
