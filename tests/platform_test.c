/* What dtv_platform, dtv_sha256 and dtv_ipc promise, on the system the test
 * runs on: the calendar arithmetic the programme guide depends on, timed
 * waits that end on time, threads that can be waited for, SHA-256 against
 * the FIPS vectors, and a line going through the daemon's command channel. */
#include <stdio.h>
#include <string.h>

#include "dtv_ipc.h"
#include "dtv_platform.h"
#include "dtv_sha256.h"

static int failures;

static void check(const char *what, int condition)
{
    if (condition) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

static void test_calendar(void)
{
    dtv_utc_time t;
    /* 1970-01-01 is 11644473600 s after 1601-01-01, the FILETIME epoch. */
    check("1970-01-01 in 100 ns ticks since 1601",
          dtv_utc_ticks(1970, 1, 1, 0, 0, 0) == 116444736000000000ull);
    check("2000-03-01 12:34:56",
          dtv_utc_ticks(2000, 3, 1, 12, 34, 56) ==
              (116444736000000000ull + 951914096ull * 10000000ull));
    dtv_utc_split(dtv_utc_ticks(2024, 2, 29, 23, 59, 58), &t);
    check("a leap day survives the round trip",
          t.year == 2024 && t.month == 2 && t.day == 29 && t.hour == 23 &&
          t.minute == 59 && t.second == 58);
    dtv_utc_split(dtv_utc_ticks(2100, 12, 31, 0, 0, 1), &t);
    check("a century that is not a leap year",
          t.year == 2100 && t.month == 12 && t.day == 31 && t.second == 1);
    {
        uint64_t now = dtv_utc_now_ticks();
        dtv_utc_split(now, &t);
        check("the system clock reads as a plausible date",
              t.year >= 2024 && t.year < 2200);
    }
}

static void test_sha256(void)
{
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    static const uint8_t two_blocks[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
        0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
        0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1 };
    static const uint8_t empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
        0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
        0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
    const char *long_text =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[32];

    dtv_sha256((const uint8_t *)"abc", 3, digest);
    check("SHA-256 of abc", memcmp(digest, abc, 32) == 0);
    dtv_sha256((const uint8_t *)long_text, strlen(long_text), digest);
    check("SHA-256 padding into a second block",
          memcmp(digest, two_blocks, 32) == 0);
    dtv_sha256((const uint8_t *)"", 0, digest);
    check("SHA-256 of nothing", memcmp(digest, empty, 32) == 0);
}

typedef struct counter {
    dtv_atomic32 value;
    dtv_event *go;
} counter;

static void count_up(void *argument)
{
    counter *c = (counter *)argument;
    int i;
    for (i = 0; i < 100000; ++i)
        dtv_atomic_inc(&c->value);
}

static void test_threads(void)
{
    counter c;
    dtv_thread *threads[4];
    int i, started = 1;
    uint64_t before;

    memset(&c, 0, sizeof(c));
    c.go = dtv_event_create();
    check("an event can be made", c.go != NULL);

    before = dtv_tick_ms();
    check("an unset event times out", dtv_event_wait(c.go, 120) == 0);
    check("and waits about as long as asked",
          dtv_tick_ms() - before >= 100 && dtv_tick_ms() - before < 2000);
    dtv_event_set(c.go);
    check("a set event is taken by the next wait", dtv_event_wait(c.go, 0) == 1);
    check("and resets itself", dtv_event_wait(c.go, 0) == 0);

    for (i = 0; i < 4; ++i) {
        threads[i] = dtv_thread_start(count_up, &c);
        started = started && threads[i];
    }
    check("threads start", started);
    for (i = 0; i < 4; ++i)
        if (threads[i])
            check("a thread is joined", dtv_thread_join(threads[i], 5000) == 0);
    check("atomic increments from four threads add up",
          dtv_atomic_get(&c.value) == 400000);
    check("compare-and-swap refuses a stale value",
          dtv_atomic_cas(&c.value, 1, 7) == 400000 &&
          dtv_atomic_get(&c.value) == 400000);
    check("and accepts the right one",
          dtv_atomic_cas(&c.value, 400000, 7) == 400000 &&
          dtv_atomic_get(&c.value) == 7);
    dtv_event_destroy(c.go);

    {
        dtv_mutex mutex;
        dtv_cond cond;
        dtv_mutex_init(&mutex);
        dtv_cond_init(&cond);
        dtv_mutex_lock(&mutex);
        /* Taken twice by the same thread, as a CRITICAL_SECTION allows. */
        dtv_mutex_lock(&mutex);
        dtv_mutex_unlock(&mutex);
        before = dtv_tick_ms();
        check("a condition wait times out",
              dtv_cond_wait_ms(&cond, &mutex, 80) == 1);
        check("after about the time asked", dtv_tick_ms() - before >= 60);
        dtv_mutex_unlock(&mutex);
        dtv_cond_destroy(&cond);
        dtv_mutex_destroy(&mutex);
    }
}

typedef struct channel_test {
    dtv_ipc_server *server;
    char received[64];
    int answered;
} channel_test;

static void serve_one(void *argument)
{
    channel_test *test = (channel_test *)argument;
    dtv_ipc *client = dtv_ipc_accept(test->server);
    int got;
    if (!client)
        return;
    got = dtv_ipc_read(client, test->received, sizeof(test->received) - 1);
    if (got > 0) {
        test->received[got] = 0;
        test->answered = dtv_ipc_write(client, "STATUS 1\n", 9) == 0;
    }
    /* Keep the connection until the client has read the answer. */
    dtv_sleep_ms(200);
    dtv_ipc_disconnect(client);
}

static void test_channel(void)
{
    channel_test test;
    char name[128];
    dtv_thread *server_thread;
    dtv_ipc *client = NULL;
    int i;

    memset(&test, 0, sizeof(test));
#ifdef _WIN32
    snprintf(name, sizeof(name), "\\\\.\\pipe\\usb_dvbs2_test_%lu",
             dtv_process_id());
#else
    snprintf(name, sizeof(name), "/tmp/usb_dvbs2_test_%lu.sock",
             dtv_process_id());
#endif
    test.server = dtv_ipc_listen(name);
    check("the command channel can be opened", test.server != NULL);
    if (!test.server)
        return;
    server_thread = dtv_thread_start(serve_one, &test);
    for (i = 0; i < 50 && !client; ++i) {
        client = dtv_ipc_connect(name);
        if (!client)
            dtv_sleep_ms(20);
    }
    check("a client connects", client != NULL);
    if (client) {
        char reply[32];
        int got = 0;
        check("a command is written",
              dtv_ipc_write(client, "STATUS\n", 7) == 0);
        for (i = 0; i < 100 && !dtv_ipc_available(client); ++i)
            dtv_sleep_ms(10);
        check("the answer is waiting", dtv_ipc_available(client) > 0);
        got = dtv_ipc_read(client, reply, sizeof(reply) - 1);
        if (got > 0)
            reply[got] = 0;
        check("the answer arrives whole",
              got == 9 && strcmp(reply, "STATUS 1\n") == 0);
        dtv_ipc_close(client);
    }
    check("the server thread ends", dtv_thread_join(server_thread, 3000) == 0);
    check("the daemon saw the command",
          strcmp(test.received, "STATUS\n") == 0 && test.answered);
    dtv_ipc_server_close(test.server);
}

int main(void)
{
    test_calendar();
    test_sha256();
    test_threads();
    test_channel();
    if (failures)
        printf("%d failed\n", failures);
    else
        printf("all passed\n");
    return failures ? 1 : 0;
}
