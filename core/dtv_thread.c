#include "dtv_thread.h"

#ifdef _WIN32

#include <windows.h>
#include <string.h>

/* avrt.dll is loaded by name rather than linked: MMCSS is a convenience,
 * and a system without it must still stream. */
typedef HANDLE (WINAPI *av_set_characteristics)(LPCWSTR, LPDWORD);
typedef BOOL   (WINAPI *av_set_priority)(HANDLE, int);
typedef BOOL   (WINAPI *av_revert)(HANDLE);

static av_set_characteristics g_av_begin;
static av_set_priority        g_av_priority;
static av_revert              g_av_end;
static LONG                   g_avrt_loaded;

static void load_avrt(void)
{
    HMODULE module;
    if (InterlockedCompareExchange(&g_avrt_loaded, 1, 0) != 0)
        return;
    module = LoadLibraryA("avrt.dll");
    if (!module)
        return;
    g_av_begin = (av_set_characteristics)(void *)GetProcAddress(
        module, "AvSetMmThreadCharacteristicsW");
    g_av_priority = (av_set_priority)(void *)GetProcAddress(
        module, "AvSetMmThreadPriority");
    g_av_end = (av_revert)(void *)GetProcAddress(
        module, "AvRevertMmThreadCharacteristics");
    /* Never freed on purpose: the pointers outlive this call and the
     * process keeps one reference for its whole run. */
}

void *dtv_thread_realtime_begin(void)
{
    DWORD task = 0;
    HANDLE token = NULL;
    /* Above the desktop but below kernel work. TIME_CRITICAL is worse: it
     * outranks the threads that drain our socket and draw the picture, so
     * the stream arrived on time into a starved player. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    load_avrt();
    if (!g_av_begin)
        return NULL;
    /* Pro Audio is the strongest standard MMCSS task and the one whose
     * deadline matches ours: a stream that cannot be paused or retried. */
    token = g_av_begin(L"Pro Audio", &task);
    if (token == INVALID_HANDLE_VALUE || !token)
        return NULL;
    if (g_av_priority)
        g_av_priority(token, 1 /* AVRT_PRIORITY_HIGH */);
    return token;
}

void dtv_thread_realtime_end(void *token)
{
    if (token && g_av_end)
        g_av_end((HANDLE)token);
}

/* Declared locally: this toolchain does not carry them, and the call is
 * resolved by name, so an older Windows simply does nothing. */
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
typedef struct dtv_power_throttling_state {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} dtv_power_throttling_state;
typedef BOOL (WINAPI *set_process_information)(HANDLE, int, LPVOID, DWORD);

void dtv_thread_process_no_throttle(void)
{
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    set_process_information set_information;
    dtv_power_throttling_state state;
    if (!kernel)
        return;
    set_information = (set_process_information)(void *)GetProcAddress(
        kernel, "SetProcessInformation");
    if (!set_information)
        return;
    memset(&state, 0, sizeof(state));
    state.Version = 1; /* PROCESS_POWER_THROTTLING_CURRENT_VERSION */
    /* Name the execution-speed policy and leave its bit clear: that is how
     * the API spells decide this one for me, and the answer is no. */
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    set_information(GetCurrentProcess(), 4 /* ProcessPowerThrottling */,
                    &state, (DWORD)sizeof(state));
}

#elif defined(__APPLE__)

#include <pthread.h>

/* macOS has no user-settable real-time class without a time-constraint
 * policy; the interactive quality-of-service class keeps the thread on the
 * performance cores and ahead of background work, which is the problem
 * MMCSS solves on Windows. */
void *dtv_thread_realtime_begin(void)
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return NULL;
}

void dtv_thread_realtime_end(void *token) { (void)token; }
void dtv_thread_process_no_throttle(void) { }

#else /* Linux and other POSIX systems */

#include <pthread.h>
#include <sched.h>

/* A low real-time priority, above every ordinary thread. Only a process
 * allowed to (root, CAP_SYS_NICE or an rtprio limit) gets it; everywhere
 * else the call fails and the thread simply runs as before. The previous
 * policy is the token, so the thread can go back to it. */
typedef struct realtime_token {
    int policy;
    struct sched_param param;
} realtime_token;

void *dtv_thread_realtime_begin(void)
{
    static realtime_token tokens[16];
    static int next;
    struct sched_param param;
    int policy;
    if (pthread_getschedparam(pthread_self(), &policy, &param) != 0)
        return NULL;
    {
        struct sched_param raised;
        raised.sched_priority = sched_get_priority_min(SCHED_RR) + 10;
        if (pthread_setschedparam(pthread_self(), SCHED_RR, &raised) != 0)
            return NULL;
    }
    /* A handful of streaming threads per process; a slot each is plenty,
     * and a token is only an undo record. */
    {
        realtime_token *token = &tokens[next++ % 16];
        token->policy = policy;
        token->param = param;
        return token;
    }
}

void dtv_thread_realtime_end(void *token)
{
    realtime_token *previous = (realtime_token *)token;
    if (previous)
        pthread_setschedparam(pthread_self(), previous->policy,
                              &previous->param);
}

void dtv_thread_process_no_throttle(void) { }

#endif
