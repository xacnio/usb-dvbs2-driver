/* dtv_platform.h - what the driver needs from the operating system.
 *
 * Windows, Linux and macOS each spell sleeping, clocks, locks and threads
 * their own way. The drivers, the engine and the tools use these names
 * instead, and dtv_platform.c maps them onto Win32 or POSIX, so nothing
 * above this file includes <windows.h> or <pthread.h> for them. */
#ifndef DTV_PLATFORM_H
#define DTV_PLATFORM_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define DTV_PATH_SEPARATOR '\\'
#define DTV_EXE_SUFFIX ".exe"
#else
#define DTV_PATH_SEPARATOR '/'
#define DTV_EXE_SUFFIX ""
#endif

#define DTV_PATH_MAX 1024

/* --- time ---------------------------------------------------------------- */

void dtv_sleep_ms(unsigned milliseconds);

/* Milliseconds from an arbitrary start, never going backwards. */
uint64_t dtv_tick_ms(void);

/* The same, truncated: for the intervals that were written as 32-bit
 * GetTickCount() arithmetic and rely on its wrap-around. */
static inline uint32_t dtv_tick32(void) { return (uint32_t)dtv_tick_ms(); }

/* Wall-clock UTC as 100-nanosecond ticks since 1601-01-01, the Windows
 * FILETIME scale the engine's programme times are compared in. */
uint64_t dtv_utc_now_ticks(void);

/* A UTC calendar time on the same scale. */
uint64_t dtv_utc_ticks(int year, int month, int day, int hour, int minute,
                       int second);

typedef struct dtv_utc_time {
    int year, month, day, hour, minute, second;
} dtv_utc_time;

/* The calendar time of a tick count on that scale. */
void dtv_utc_split(uint64_t ticks, dtv_utc_time *out);

/* --- atomics ------------------------------------------------------------- */

typedef atomic_int_least32_t dtv_atomic32;

static inline int32_t dtv_atomic_get(const dtv_atomic32 *value)
{
    return (int32_t)atomic_load(value);
}

/* Stores and returns the previous value. */
static inline int32_t dtv_atomic_set(dtv_atomic32 *value, int32_t next)
{
    return (int32_t)atomic_exchange(value, next);
}

/* Stores next only if the value is expected; returns the value found. */
static inline int32_t dtv_atomic_cas(dtv_atomic32 *value, int32_t expected,
                                     int32_t next)
{
    int_least32_t found = expected;
    atomic_compare_exchange_strong(value, &found, next);
    return (int32_t)found;
}

/* Return the new value. */
static inline int32_t dtv_atomic_inc(dtv_atomic32 *value)
{
    return (int32_t)atomic_fetch_add(value, 1) + 1;
}

static inline int32_t dtv_atomic_dec(dtv_atomic32 *value)
{
    return (int32_t)atomic_fetch_sub(value, 1) - 1;
}

/* --- locks --------------------------------------------------------------- */

/* Opaque storage big enough for a CRITICAL_SECTION or a pthread mutex, so a
 * lock can live inside a struct or a static without an allocation. */
typedef struct dtv_mutex {
    void *storage[8];
} dtv_mutex;

typedef struct dtv_cond {
    void *storage[8];
} dtv_cond;

void dtv_mutex_init(dtv_mutex *mutex);
void dtv_mutex_destroy(dtv_mutex *mutex);
void dtv_mutex_lock(dtv_mutex *mutex);
void dtv_mutex_unlock(dtv_mutex *mutex);

void dtv_cond_init(dtv_cond *cond);
void dtv_cond_destroy(dtv_cond *cond);
/* Waits with the mutex held; returns 0 when woken, 1 on timeout. Spurious
 * wake-ups happen, so the caller checks its condition again. */
int  dtv_cond_wait_ms(dtv_cond *cond, dtv_mutex *mutex, unsigned milliseconds);
void dtv_cond_signal(dtv_cond *cond);
void dtv_cond_broadcast(dtv_cond *cond);

/* An auto-reset event: dtv_event_set() wakes one waiter, or the next one to
 * wait if nobody is waiting yet. */
typedef struct dtv_event dtv_event;

dtv_event *dtv_event_create(void);
void dtv_event_destroy(dtv_event *event);
void dtv_event_set(dtv_event *event);
/* 1 when the event was set, 0 on timeout. */
int  dtv_event_wait(dtv_event *event, unsigned milliseconds);

/* --- threads ------------------------------------------------------------- */

typedef struct dtv_thread dtv_thread;
typedef void (*dtv_thread_fn)(void *argument);

/* NULL when the thread could not be started. */
dtv_thread *dtv_thread_start(dtv_thread_fn function, void *argument);

/* Waits for the thread to end and frees it: 0 when it did. On a timeout the
 * thread keeps running and 1 is returned; the handle stays valid, so the
 * caller may wait again or let go of it with dtv_thread_detach(). */
int  dtv_thread_join(dtv_thread *thread, unsigned milliseconds);
#define DTV_WAIT_FOREVER 0xffffffffu

/* Forgets a thread that is left to run to its end on its own. */
void dtv_thread_detach(dtv_thread *thread);

/* --- files and processes ------------------------------------------------- */

/* The folder holding the running executable, without a trailing separator.
 * 0 on success. */
int dtv_executable_dir(char *out, size_t size);

int dtv_file_exists(const char *path);

unsigned long dtv_process_id(void);

int dtv_stricmp(const char *a, const char *b);

#endif /* DTV_PLATFORM_H */
