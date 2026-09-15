/* dtv_platform.c - Win32 and POSIX behind dtv_platform.h. */
#ifndef _WIN32
#define _GNU_SOURCE /* strcasecmp, clock_gettime and pthread_condattr_setclock */
#endif

#include "dtv_platform.h"

#include <stdlib.h>
#include <string.h>

/* Days from 1601-01-01 to the given civil date (proleptic Gregorian), after
 * Howard Hinnant's days_from_civil, which counts from 1970-01-01. */
static int64_t days_from_1601(int year, int month, int day)
{
    int64_t y = (int64_t)year - (month <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u +
                   (unsigned)day - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int64_t since_1970 = era * 146097 + (int64_t)doe - 719468;
    return since_1970 + 134774; /* 1601-01-01 .. 1970-01-01 */
}

uint64_t dtv_utc_ticks(int year, int month, int day, int hour, int minute,
                       int second)
{
    int64_t seconds = days_from_1601(year, month, day) * 86400 +
                      (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
    return seconds < 0 ? 0 : (uint64_t)seconds * 10000000u;
}

void dtv_utc_split(uint64_t ticks, dtv_utc_time *out)
{
    /* Hinnant's civil_from_days, on days since 1970-01-01. */
    uint64_t seconds = ticks / 10000000u;
    int64_t z = (int64_t)(seconds / 86400u) - 134774 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned second_of_day = (unsigned)(seconds % 86400u);
    if (!out)
        return;
    out->day = (int)(doy - (153u * mp + 2u) / 5u + 1u);
    out->month = (int)(mp < 10u ? mp + 3u : mp - 9u);
    out->year = (int)((int64_t)yoe + era * 400 + (out->month <= 2));
    out->hour = (int)(second_of_day / 3600u);
    out->minute = (int)(second_of_day / 60u % 60u);
    out->second = (int)(second_of_day % 60u);
}

#ifdef _WIN32

#include <windows.h>

_Static_assert(sizeof(CRITICAL_SECTION) <= sizeof(dtv_mutex),
               "dtv_mutex is too small for a CRITICAL_SECTION");
_Static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(dtv_cond),
               "dtv_cond is too small for a CONDITION_VARIABLE");

void dtv_sleep_ms(unsigned milliseconds) { Sleep(milliseconds); }

uint64_t dtv_tick_ms(void) { return GetTickCount64(); }

uint64_t dtv_utc_now_ticks(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
}

void dtv_mutex_init(dtv_mutex *m) { InitializeCriticalSection((CRITICAL_SECTION *)m->storage); }
void dtv_mutex_destroy(dtv_mutex *m) { DeleteCriticalSection((CRITICAL_SECTION *)m->storage); }
void dtv_mutex_lock(dtv_mutex *m) { EnterCriticalSection((CRITICAL_SECTION *)m->storage); }
void dtv_mutex_unlock(dtv_mutex *m) { LeaveCriticalSection((CRITICAL_SECTION *)m->storage); }

void dtv_cond_init(dtv_cond *c) { InitializeConditionVariable((CONDITION_VARIABLE *)c->storage); }
void dtv_cond_destroy(dtv_cond *c) { (void)c; }

int dtv_cond_wait_ms(dtv_cond *c, dtv_mutex *m, unsigned milliseconds)
{
    return SleepConditionVariableCS((CONDITION_VARIABLE *)c->storage,
                                    (CRITICAL_SECTION *)m->storage,
                                    milliseconds == DTV_WAIT_FOREVER
                                        ? INFINITE : milliseconds) ? 0 : 1;
}

void dtv_cond_signal(dtv_cond *c) { WakeConditionVariable((CONDITION_VARIABLE *)c->storage); }
void dtv_cond_broadcast(dtv_cond *c) { WakeAllConditionVariable((CONDITION_VARIABLE *)c->storage); }

struct dtv_event {
    HANDLE handle;
};

dtv_event *dtv_event_create(void)
{
    dtv_event *event = calloc(1, sizeof(*event));
    if (!event)
        return NULL;
    event->handle = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!event->handle) {
        free(event);
        return NULL;
    }
    return event;
}

void dtv_event_destroy(dtv_event *event)
{
    if (!event)
        return;
    CloseHandle(event->handle);
    free(event);
}

void dtv_event_set(dtv_event *event) { if (event) SetEvent(event->handle); }

int dtv_event_wait(dtv_event *event, unsigned milliseconds)
{
    return event && WaitForSingleObject(event->handle,
                                        milliseconds == DTV_WAIT_FOREVER
                                            ? INFINITE : milliseconds) ==
                        WAIT_OBJECT_0;
}

struct dtv_thread {
    HANDLE handle;
    dtv_thread_fn function;
    void *argument;
};

static DWORD WINAPI thread_entry(LPVOID parameter)
{
    dtv_thread *thread = (dtv_thread *)parameter;
    thread->function(thread->argument);
    return 0;
}

dtv_thread *dtv_thread_start(dtv_thread_fn function, void *argument)
{
    dtv_thread *thread = calloc(1, sizeof(*thread));
    if (!thread)
        return NULL;
    thread->function = function;
    thread->argument = argument;
    thread->handle = CreateThread(NULL, 0, thread_entry, thread, 0, NULL);
    if (!thread->handle) {
        free(thread);
        return NULL;
    }
    return thread;
}

int dtv_thread_join(dtv_thread *thread, unsigned milliseconds)
{
    if (!thread)
        return 0;
    if (WaitForSingleObject(thread->handle, milliseconds == DTV_WAIT_FOREVER
                                                ? INFINITE : milliseconds) !=
        WAIT_OBJECT_0)
        return 1;
    CloseHandle(thread->handle);
    free(thread);
    return 0;
}

void dtv_thread_detach(dtv_thread *thread)
{
    /* The record stays with the running thread, which reads it on entry;
     * a few bytes per abandoned thread. */
    if (thread)
        CloseHandle(thread->handle);
}

int dtv_executable_dir(char *out, size_t size)
{
    char path[MAX_PATH];
    char *slash;
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (!out || !size || length == 0 || length >= sizeof(path))
        return -1;
    slash = strrchr(path, '\\');
    if (!slash)
        return -1;
    *slash = 0;
    if (strlen(path) + 1 > size)
        return -1;
    memcpy(out, path, strlen(path) + 1);
    return 0;
}

int dtv_file_exists(const char *path)
{
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

unsigned long dtv_process_id(void) { return (unsigned long)GetCurrentProcessId(); }

int dtv_stricmp(const char *a, const char *b) { return _stricmp(a, b); }

#else /* POSIX */

#include <errno.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(dtv_mutex),
               "dtv_mutex is too small for a pthread mutex");
_Static_assert(sizeof(pthread_cond_t) <= sizeof(dtv_cond),
               "dtv_cond is too small for a pthread condition");

void dtv_sleep_ms(unsigned milliseconds)
{
    struct timespec wait = { (time_t)(milliseconds / 1000u),
                             (long)(milliseconds % 1000u) * 1000000L };
    while (nanosleep(&wait, &wait) != 0 && errno == EINTR)
        ;
}

uint64_t dtv_tick_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

uint64_t dtv_utc_now_ticks(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return ((uint64_t)now.tv_sec + 11644473600u) * 10000000u +
           (uint64_t)now.tv_nsec / 100u;
}

#define MUTEX(m) ((pthread_mutex_t *)(void *)(m)->storage)
#define COND(c)  ((pthread_cond_t *)(void *)(c)->storage)

void dtv_mutex_init(dtv_mutex *m)
{
    /* Recursive, as a CRITICAL_SECTION is: the code above was written
     * against those and may take a lock it already holds. */
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(MUTEX(m), &attributes);
    pthread_mutexattr_destroy(&attributes);
}

void dtv_mutex_destroy(dtv_mutex *m) { pthread_mutex_destroy(MUTEX(m)); }
void dtv_mutex_lock(dtv_mutex *m) { pthread_mutex_lock(MUTEX(m)); }
void dtv_mutex_unlock(dtv_mutex *m) { pthread_mutex_unlock(MUTEX(m)); }

void dtv_cond_init(dtv_cond *c)
{
#ifdef __APPLE__
    pthread_cond_init(COND(c), NULL);
#else
    /* Timed waits against the monotonic clock, so a clock change does not
     * stretch or cut them short. */
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
    pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    pthread_cond_init(COND(c), &attributes);
    pthread_condattr_destroy(&attributes);
#endif
}

void dtv_cond_destroy(dtv_cond *c) { pthread_cond_destroy(COND(c)); }

int dtv_cond_wait_ms(dtv_cond *c, dtv_mutex *m, unsigned milliseconds)
{
    struct timespec deadline;
    if (milliseconds == DTV_WAIT_FOREVER)
        return pthread_cond_wait(COND(c), MUTEX(m)) == 0 ? 0 : 1;
#ifdef __APPLE__
    deadline.tv_sec = (time_t)(milliseconds / 1000u);
    deadline.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    return pthread_cond_timedwait_relative_np(COND(c), MUTEX(m), &deadline) ==
                   ETIMEDOUT ? 1 : 0;
#else
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(milliseconds / 1000u);
    deadline.tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(COND(c), MUTEX(m), &deadline) == ETIMEDOUT
               ? 1 : 0;
#endif
}

void dtv_cond_signal(dtv_cond *c) { pthread_cond_signal(COND(c)); }
void dtv_cond_broadcast(dtv_cond *c) { pthread_cond_broadcast(COND(c)); }

struct dtv_event {
    dtv_mutex lock;
    dtv_cond changed;
    int set;
};

dtv_event *dtv_event_create(void)
{
    dtv_event *event = calloc(1, sizeof(*event));
    if (!event)
        return NULL;
    dtv_mutex_init(&event->lock);
    dtv_cond_init(&event->changed);
    return event;
}

void dtv_event_destroy(dtv_event *event)
{
    if (!event)
        return;
    dtv_cond_destroy(&event->changed);
    dtv_mutex_destroy(&event->lock);
    free(event);
}

void dtv_event_set(dtv_event *event)
{
    if (!event)
        return;
    dtv_mutex_lock(&event->lock);
    event->set = 1;
    dtv_cond_signal(&event->changed);
    dtv_mutex_unlock(&event->lock);
}

int dtv_event_wait(dtv_event *event, unsigned milliseconds)
{
    uint64_t deadline;
    int result;
    if (!event)
        return 0;
    deadline = dtv_tick_ms() + milliseconds;
    dtv_mutex_lock(&event->lock);
    while (!event->set) {
        uint64_t now = dtv_tick_ms();
        if (milliseconds != DTV_WAIT_FOREVER && now >= deadline)
            break;
        dtv_cond_wait_ms(&event->changed, &event->lock,
                         milliseconds == DTV_WAIT_FOREVER
                             ? DTV_WAIT_FOREVER : (unsigned)(deadline - now));
    }
    result = event->set;
    event->set = 0;
    dtv_mutex_unlock(&event->lock);
    return result;
}

/* pthread_join() has no timeout, so the thread says when it has finished
 * and a timed join waits for that before joining. */
struct dtv_thread {
    pthread_t handle;
    dtv_thread_fn function;
    void *argument;
    dtv_mutex lock;
    dtv_cond done_changed;
    int done;
    int detached;
};

static void thread_free(dtv_thread *thread)
{
    dtv_cond_destroy(&thread->done_changed);
    dtv_mutex_destroy(&thread->lock);
    free(thread);
}

static void *thread_entry(void *parameter)
{
    dtv_thread *thread = (dtv_thread *)parameter;
    int detached;
    thread->function(thread->argument);
    dtv_mutex_lock(&thread->lock);
    thread->done = 1;
    detached = thread->detached;
    dtv_cond_broadcast(&thread->done_changed);
    dtv_mutex_unlock(&thread->lock);
    /* A detached thread is nobody's to free but its own. */
    if (detached)
        thread_free(thread);
    return NULL;
}

dtv_thread *dtv_thread_start(dtv_thread_fn function, void *argument)
{
    dtv_thread *thread = calloc(1, sizeof(*thread));
    if (!thread)
        return NULL;
    thread->function = function;
    thread->argument = argument;
    dtv_mutex_init(&thread->lock);
    dtv_cond_init(&thread->done_changed);
    if (pthread_create(&thread->handle, NULL, thread_entry, thread) != 0) {
        thread_free(thread);
        return NULL;
    }
    return thread;
}

int dtv_thread_join(dtv_thread *thread, unsigned milliseconds)
{
    uint64_t deadline;
    int done;
    if (!thread)
        return 0;
    deadline = dtv_tick_ms() + milliseconds;
    dtv_mutex_lock(&thread->lock);
    while (!thread->done) {
        uint64_t now = dtv_tick_ms();
        if (milliseconds != DTV_WAIT_FOREVER && now >= deadline)
            break;
        dtv_cond_wait_ms(&thread->done_changed, &thread->lock,
                         milliseconds == DTV_WAIT_FOREVER
                             ? DTV_WAIT_FOREVER : (unsigned)(deadline - now));
    }
    done = thread->done;
    dtv_mutex_unlock(&thread->lock);
    if (!done)
        return 1;
    pthread_join(thread->handle, NULL);
    thread_free(thread);
    return 0;
}

void dtv_thread_detach(dtv_thread *thread)
{
    int done;
    if (!thread)
        return;
    pthread_detach(thread->handle);
    dtv_mutex_lock(&thread->lock);
    done = thread->done;
    thread->detached = 1;
    dtv_mutex_unlock(&thread->lock);
    if (done)
        thread_free(thread);
}

int dtv_executable_dir(char *out, size_t size)
{
    char path[DTV_PATH_MAX];
    char *slash;
#ifdef __APPLE__
    uint32_t length = (uint32_t)sizeof(path);
    char resolved[DTV_PATH_MAX];
    if (_NSGetExecutablePath(path, &length) != 0)
        return -1;
    if (realpath(path, resolved))
        memcpy(path, resolved, strlen(resolved) + 1);
#else
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0)
        return -1;
    path[length] = 0;
#endif
    if (!out || !size)
        return -1;
    slash = strrchr(path, '/');
    if (!slash)
        return -1;
    *slash = 0;
    if (strlen(path) + 1 > size)
        return -1;
    memcpy(out, path, strlen(path) + 1);
    return 0;
}

int dtv_file_exists(const char *path)
{
    struct stat info;
    return path && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

unsigned long dtv_process_id(void) { return (unsigned long)getpid(); }

int dtv_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }

#endif
