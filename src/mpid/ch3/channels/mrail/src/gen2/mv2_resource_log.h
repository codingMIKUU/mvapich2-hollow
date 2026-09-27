/* Optional, bounded resource-pressure diagnostics.  No MPI calls in emit(). */
#ifndef MV2_RESOURCE_LOG_H
#define MV2_RESOURCE_LOG_H

#ifndef MV2_ENABLE_RESOURCE_LOG
#define MV2_ENABLE_RESOURCE_LOG 1
#endif

enum mv2_resource_event {
    MV2_RES_SRQ_LOW_WATER,
    MV2_RES_SRQ_POST_SHORT,
    MV2_RES_SRQ_REFILL_WAIT,
    MV2_RES_VBUF_GROW,
    MV2_RES_SEND_WQE_EMPTY,
    MV2_RES_FORCED_RNDV,
    MV2_RES_EVENT_COUNT
};

#if MV2_ENABLE_RESOURCE_LOG
extern int mv2_resource_log_enabled;
void mv2_resource_log_init(int rank);
void mv2_resource_log_emit(enum mv2_resource_event event, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#define MV2_RESOURCE_LOG_IF(condition, event, ...) do { \
    if (__builtin_expect(mv2_resource_log_enabled, 0) && (condition)) \
        mv2_resource_log_emit((event), __VA_ARGS__); \
} while (0)
#define MV2_RESOURCE_LOG(event, ...) MV2_RESOURCE_LOG_IF(1, event, __VA_ARGS__)
#else
static inline void mv2_resource_log_init(int rank) { (void)rank; }
#define MV2_RESOURCE_LOG_IF(condition, event, ...) do { } while (0)
#define MV2_RESOURCE_LOG(event, ...) do { } while (0)
#endif

#if MV2_ENABLE_RESOURCE_LOG && defined(MV2_RESOURCE_LOG_IMPLEMENTATION)
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int mv2_resource_log_enabled;
static int mv2_res_initialized, mv2_res_rank;
static unsigned int mv2_res_max_lines = 8;
static unsigned long long mv2_res_counts[MV2_RES_EVENT_COUNT];
static char mv2_res_host[96];
static const char *mv2_res_mode;
static double mv2_res_started;
static const char *const mv2_res_names[MV2_RES_EVENT_COUNT] = {
    "srq_low_water", "srq_post_short", "srq_refill_wait", "vbuf_grow",
    "send_wqe_empty", "forced_rndv"
};

static double mv2_res_clock_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

/* A single bounded write avoids holding a stdio stream lock in the async
 * SRQ thread.  Output can still block on a slow stderr reader: diagnostic
 * runs are not uninstrumented performance measurements.
 */
static void mv2_res_write(const char *line, int size)
{
    if (size > 0) {
        size_t bytes = (size_t)size < 768 ? (size_t)size : 767;
        ssize_t written = write(STDERR_FILENO, line, bytes);
        (void)written;
    }
}

static void mv2_res_report(void)
{
    int i, saved_errno = errno;
    for (i = 0; i < MV2_RES_EVENT_COUNT; ++i) {
        unsigned long long count = __atomic_load_n(&mv2_res_counts[i], __ATOMIC_RELAXED);
        if (count) {
            char line[768];
            int n = snprintf(line, sizeof(line),
                "MV2_RESOURCE_SUMMARY host=%s rank=%d mode=%s event=%s count=%llu\n",
                mv2_res_host, mv2_res_rank, mv2_res_mode, mv2_res_names[i], count);
            mv2_res_write(line, n);
        }
    }
    errno = saved_errno;
}

static int mv2_res_number(const char *name, long *value)
{
    const char *text = getenv(name);
    char *end;
    if (!text)
        return 1;
    if (!*text)
        return 0;
    errno = 0;
    *value = strtol(text, &end, 10);
    return !errno && end != text && !*end;
}

void mv2_resource_log_init(int rank)
{
    long enabled = 0, selected_rank = -1, max_lines = 8;
    const char *mode;
    int saved_errno = errno;
    char line[768];
    int n;
    /* Called by single-threaded RDMA parameter setup, before resource
     * allocation and before starting async event threads. Never reconfigure
     * the logger from a posting/polling path.
     */
    if (mv2_res_initialized)
        return;
    mv2_res_initialized = 1;
    if (!mv2_res_number("MV2_RESOURCE_LOG", &enabled) || enabled < 0 || enabled > 1)
        goto invalid;
    if (!enabled)
        goto done;
    if (!mv2_res_number("MV2_RESOURCE_LOG_RANK", &selected_rank) ||
        selected_rank < -1 || selected_rank > INT_MAX ||
        !mv2_res_number("MV2_RESOURCE_LOG_MAX", &max_lines) ||
        max_lines < 1 || max_lines > 64)
        goto invalid;
    if (selected_rank != -1 && selected_rank != rank)
        goto done;
    mv2_res_rank = rank;
    mv2_res_max_lines = (unsigned int)max_lines;
    if (gethostname(mv2_res_host, sizeof(mv2_res_host) - 1))
        strcpy(mv2_res_host, "unknown");
    mv2_res_host[sizeof(mv2_res_host) - 1] = '\0';
    mode = getenv("MV2_TRANSPORT_MODE");
    mv2_res_mode = !mode ? "mrail" : !strcmp(mode, "ordinary") ? "ordinary" :
        !strcmp(mode, "xrc") ? "xrc" : !strcmp(mode, "hollow") ? "hollow" : "mrail";
    if (atexit(mv2_res_report))
        goto invalid;
    mv2_res_started = mv2_res_clock_ms();
    mv2_resource_log_enabled = 1;
    n = snprintf(line, sizeof(line),
        "MV2_RESOURCE_CONFIG host=%s rank=%d mode=%s max_lines_per_event=%u sampling=powers_of_two\n",
        mv2_res_host, rank, mv2_res_mode, mv2_res_max_lines);
    mv2_res_write(line, n);
    goto done;
invalid:
    n = snprintf(line, sizeof(line),
        "MV2_RESOURCE_CONFIG rank=%d invalid_configuration=1 diagnostics_disabled=1\n", rank);
    mv2_res_write(line, n);
done:
    errno = saved_errno;
}

void mv2_resource_log_emit(enum mv2_resource_event event, const char *fmt, ...)
{
    unsigned long long count;
    int saved_errno = errno, n;
    char detail[320], line[768];
    va_list ap;
    if (!mv2_resource_log_enabled || event < 0 || event >= MV2_RES_EVENT_COUNT)
        return;
    /* MPI progress and the async SRQ thread may report concurrently. No new
     * mutex/spinlock is added; only the enabled event path updates counters.
     */
    count = __atomic_add_fetch(&mv2_res_counts[event], 1, __ATOMIC_RELAXED);
    if (!count || (count & (count - 1)) ||
        count > (1ULL << (mv2_res_max_lines - 1)))
        return;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    n = snprintf(line, sizeof(line),
        "MV2_RESOURCE_EVENT host=%s rank=%d mode=%s since_init_ms=%.3f event=%s count=%llu %s\n",
        mv2_res_host, mv2_res_rank, mv2_res_mode,
        mv2_res_clock_ms() - mv2_res_started, mv2_res_names[event], count, detail);
    mv2_res_write(line, n);
    errno = saved_errno;
}
#endif /* implementation */
#endif /* MV2_RESOURCE_LOG_H */
