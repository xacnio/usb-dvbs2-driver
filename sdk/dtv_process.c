/* dtv_process.c - see dtv_process.h. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include "dtv_process.h"
#include "dtv_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <tlhelp32.h>

struct dtv_process {
    PROCESS_INFORMATION info;
};

struct dtv_pipe {
    HANDLE handle;
};

dtv_process *dtv_process_spawn(const char *program, const char *const *argv,
                               const char *cwd, dtv_pipe **output)
{
    char command[2048];
    size_t used = 0;
    STARTUPINFOA startup;
    SECURITY_ATTRIBUTES attributes;
    HANDLE read_end = NULL, write_end = NULL;
    dtv_process *process;
    int i;

    if (output)
        *output = NULL;
    /* Every argument quoted; none of the daemon's contains a quote. */
    for (i = 0; argv[i]; ++i) {
        int written = snprintf(command + used, sizeof(command) - used,
                               "%s\"%s\"", i ? " " : "",
                               i ? argv[i] : program);
        if (written < 0 || (size_t)written >= sizeof(command) - used)
            return NULL;
        used += (size_t)written;
    }

    process = calloc(1, sizeof(*process));
    if (!process)
        return NULL;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = NULL;
    attributes.bInheritHandle = TRUE;
    if (output && CreatePipe(&read_end, &write_end, &attributes, 64 * 1024)) {
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write_end;
        startup.hStdError = write_end;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    if (!CreateProcessA(NULL, command, NULL, NULL, write_end ? TRUE : FALSE,
                        CREATE_NO_WINDOW, NULL, cwd, &startup,
                        &process->info)) {
        if (write_end) CloseHandle(write_end);
        if (read_end) CloseHandle(read_end);
        free(process);
        return NULL;
    }
    /* Only the child may keep the write end, or the reads never end. */
    if (write_end)
        CloseHandle(write_end);
    if (read_end && output) {
        *output = calloc(1, sizeof(**output));
        if (*output)
            (*output)->handle = read_end;
        else
            CloseHandle(read_end);
    }
    return process;
}

int dtv_process_wait(dtv_process *process, unsigned milliseconds)
{
    return process && WaitForSingleObject(process->info.hProcess,
                                          milliseconds) == WAIT_OBJECT_0;
}

void dtv_process_kill(dtv_process *process)
{
    if (process)
        TerminateProcess(process->info.hProcess, 1);
}

void dtv_process_free(dtv_process *process)
{
    if (!process)
        return;
    CloseHandle(process->info.hProcess);
    CloseHandle(process->info.hThread);
    free(process);
}

int dtv_process_kill_named(const char *name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry;
    DWORD self = GetCurrentProcessId();
    int killed = 0;
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            HANDLE process;
            if (entry.th32ProcessID == self ||
                _stricmp(entry.szExeFile, name) != 0)
                continue;
            process = OpenProcess(PROCESS_TERMINATE, FALSE,
                                  entry.th32ProcessID);
            if (process) {
                TerminateProcess(process, 0);
                CloseHandle(process);
                ++killed;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return killed;
}

int dtv_pipe_read(dtv_pipe *pipe, char *buffer, size_t size)
{
    DWORD got = 0;
    if (!pipe || !size)
        return -1;
    if (!ReadFile(pipe->handle, buffer, (DWORD)size, &got, NULL))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return (int)got;
}

void dtv_pipe_close(dtv_pipe *pipe)
{
    if (!pipe)
        return;
    CloseHandle(pipe->handle);
    free(pipe);
}

#else /* POSIX */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#endif

struct dtv_process {
    pid_t pid;
    int exited;
};

struct dtv_pipe {
    int fd;
};

dtv_process *dtv_process_spawn(const char *program, const char *const *argv,
                               const char *cwd, dtv_pipe **output)
{
    int fds[2] = { -1, -1 };
    dtv_process *process;
    pid_t pid;

    if (output)
        *output = NULL;
    process = calloc(1, sizeof(*process));
    if (!process)
        return NULL;
    if (output && pipe(fds) != 0)
        fds[0] = fds[1] = -1;

    pid = fork();
    if (pid < 0) {
        if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
        free(process);
        return NULL;
    }
    if (pid == 0) {
        /* The child: only async-signal-safe calls until exec. */
        if (fds[1] >= 0) {
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]);
            close(fds[1]);
        }
        if (cwd && cwd[0] && chdir(cwd) != 0)
            _exit(127);
        execv(program, (char *const *)argv);
        _exit(127);
    }

    process->pid = pid;
    if (fds[1] >= 0)
        close(fds[1]);
    if (fds[0] >= 0) {
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        *output = calloc(1, sizeof(**output));
        if (*output)
            (*output)->fd = fds[0];
        else
            close(fds[0]);
    }
    return process;
}

int dtv_process_wait(dtv_process *process, unsigned milliseconds)
{
    uint64_t deadline;
    if (!process)
        return 1;
    deadline = dtv_tick_ms() + milliseconds;
    for (;;) {
        pid_t result;
        if (process->exited)
            return 1;
        result = waitpid(process->pid, NULL, WNOHANG);
        if (result == process->pid || (result < 0 && errno == ECHILD)) {
            process->exited = 1;
            return 1;
        }
        if (dtv_tick_ms() >= deadline)
            return 0;
        dtv_sleep_ms(20);
    }
}

void dtv_process_kill(dtv_process *process)
{
    if (process && !process->exited)
        kill(process->pid, SIGKILL);
}

void dtv_process_free(dtv_process *process)
{
    if (!process)
        return;
    /* Reaped when it has already gone, so no zombie is left behind. */
    if (!process->exited)
        waitpid(process->pid, NULL, WNOHANG);
    free(process);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int dtv_process_kill_named(const char *name)
{
    int killed = 0;
    pid_t self = getpid();
#ifdef __APPLE__
    int count = proc_listallpids(NULL, 0);
    pid_t *pids;
    int i;
    if (count <= 0)
        return 0;
    pids = calloc((size_t)count + 16u, sizeof(*pids));
    if (!pids)
        return 0;
    count = proc_listallpids(pids, (count + 16) * (int)sizeof(*pids));
    for (i = 0; i < count; ++i) {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (pids[i] <= 0 || pids[i] == self ||
            proc_pidpath(pids[i], path, sizeof(path)) <= 0 ||
            strcmp(base_name(path), name) != 0)
            continue;
        if (kill(pids[i], SIGKILL) == 0)
            ++killed;
    }
    free(pids);
#else
    DIR *proc = opendir("/proc");
    struct dirent *entry;
    if (!proc)
        return 0;
    while ((entry = readdir(proc)) != NULL) {
        char link[64], path[DTV_PATH_MAX];
        ssize_t length;
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0 || (pid_t)pid == self)
            continue;
        snprintf(link, sizeof(link), "/proc/%ld/exe", pid);
        /* Only our own processes can be read here, and only those could be
         * ended anyway. */
        length = readlink(link, path, sizeof(path) - 1);
        if (length <= 0)
            continue;
        path[length] = 0;
        if (strcmp(base_name(path), name) != 0)
            continue;
        if (kill((pid_t)pid, SIGKILL) == 0)
            ++killed;
    }
    closedir(proc);
#endif
    return killed;
}

int dtv_pipe_read(dtv_pipe *pipe, char *buffer, size_t size)
{
    ssize_t got;
    if (!pipe || !size)
        return -1;
    do {
        got = read(pipe->fd, buffer, size);
    } while (got < 0 && errno == EINTR);
    return got < 0 ? -1 : (int)got;
}

void dtv_pipe_close(dtv_pipe *pipe)
{
    if (!pipe)
        return;
    close(pipe->fd);
    free(pipe);
}

#endif
