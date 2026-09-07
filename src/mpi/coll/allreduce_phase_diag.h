/* Optional, sampled diagnostics for the MPI_COMM_WORLD two-level body.
 * Included only by allreduce_osu.c; no MPI collectives or per-call output.
 */
#ifndef MV2_ALLREDUCE_PHASE_DIAG_H
#define MV2_ALLREDUCE_PHASE_DIAG_H

#ifndef MV2_ENABLE_ALLREDUCE_PHASE_DIAG
#define MV2_ENABLE_ALLREDUCE_PHASE_DIAG 1
#endif

#if MV2_ENABLE_ALLREDUCE_PHASE_DIAG
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MV2_AR_PHASE_BUCKETS 64

struct mv2_ar_phase_metric {
    double sum, max;
};

struct mv2_ar_phase_bucket {
    uint64_t bytes, calls, samples, failed;
    unsigned int datatype, op;
    int rank, world, local_rank, local_size, inter;
    const char *algo;
    struct mv2_ar_phase_metric reduce, exchange, bcast, total;
};

struct mv2_ar_phase_sample {
    struct mv2_ar_phase_bucket *bucket;
    double start, reduced, broadcast;
    int stage;
};

/* MPI's global ALLFUNC critical section protects setup and accounting.
 * Progress may temporarily release it; timestamps remain call-local.
 */
static int mv2_ar_phase_enabled = -1;
static uint64_t mv2_ar_phase_every = 64, mv2_ar_phase_skip;
static uint64_t mv2_ar_phase_bytes, mv2_ar_phase_dropped;
static unsigned int mv2_ar_phase_used;
static struct mv2_ar_phase_bucket mv2_ar_phase_buckets[MV2_AR_PHASE_BUCKETS];
static char mv2_ar_phase_host[128];
static const char *mv2_ar_phase_mode;

static double mv2_ar_phase_clock(void)
{
    MPID_Time_t now;
    double seconds;
    MPID_Wtime(&now);
    MPID_Wtime_todouble(&now, &seconds);
    return seconds;
}

static int mv2_ar_phase_report(void *unused)
{
    unsigned int i;
    (void)unused;
    for (i = 0; i < mv2_ar_phase_used; ++i) {
        const struct mv2_ar_phase_bucket *b = &mv2_ar_phase_buckets[i];
        double scale = b->samples ? 1e6 / b->samples : 0;
        /* One bounded line per rank/size, only at MPI_Finalize.  A missing
         * exchange on a nonleader has inter_samples=0, not a zero-latency
         * network operation. No live communicator is used here.
         */
        fprintf(stderr,
                "MV2_ALLREDUCE_PHASE scope=world_two_level_body mode=%s host=%s "
                "rank=%d world=%d local_rank=%d local_size=%d bytes=%" PRIu64 " "
                "datatype=%x op=%x inter_algo=%s calls=%" PRIu64 " "
                "skip=%" PRIu64 " every=%" PRIu64 " samples=%" PRIu64 " "
                "failed=%" PRIu64 " inter_samples=%" PRIu64 " "
                "reduce_avg_us=%.6f inter_avg_us=%.6f bcast_avg_us=%.6f "
                "total_avg_us=%.6f reduce_max_us=%.6f inter_max_us=%.6f "
                "bcast_max_us=%.6f total_max_us=%.6f\n",
                mv2_ar_phase_mode, mv2_ar_phase_host, b->rank, b->world,
                b->local_rank, b->local_size, b->bytes, b->datatype, b->op,
                b->algo, b->calls, mv2_ar_phase_skip, mv2_ar_phase_every,
                b->samples, b->failed, b->inter ? b->samples : 0,
                b->reduce.sum * scale, b->exchange.sum * scale,
                b->bcast.sum * scale, b->total.sum * scale,
                b->reduce.max * 1e6, b->exchange.max * 1e6,
                b->bcast.max * 1e6, b->total.max * 1e6);
    }
    if (mv2_ar_phase_dropped)
        fprintf(stderr, "MV2_ALLREDUCE_PHASE_NOTICE host=%s "
                "bucket_limit=%d dropped_calls=%" PRIu64 "\n",
                mv2_ar_phase_host, MV2_AR_PHASE_BUCKETS, mv2_ar_phase_dropped);
    return MPI_SUCCESS;
}

static int mv2_ar_phase_number(const char *name, uint64_t *number)
{
    const char *value = getenv(name);
    char *end;
    unsigned long long parsed;
    if (!value)
        return 1;
    if (*value < '0' || *value > '9')
        return 0;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno || *end)
        return 0;
    *number = parsed;
    return 1;
}

static void mv2_ar_phase_init(void)
{
    uint64_t enabled = 0;
    mv2_ar_phase_enabled = 0;
    if (!mv2_ar_phase_number("MV2_ALLREDUCE_PHASE_STATS", &enabled) || enabled > 1)
        goto invalid;
    if (!enabled)
        return;
    if (!mv2_ar_phase_number("MV2_ALLREDUCE_PHASE_EVERY", &mv2_ar_phase_every) ||
        !mv2_ar_phase_number("MV2_ALLREDUCE_PHASE_SKIP", &mv2_ar_phase_skip) ||
        !mv2_ar_phase_number("MV2_ALLREDUCE_PHASE_BYTES", &mv2_ar_phase_bytes) ||
        !mv2_ar_phase_every || (mv2_ar_phase_every & (mv2_ar_phase_every - 1)))
        goto invalid;
    if (gethostname(mv2_ar_phase_host, sizeof(mv2_ar_phase_host) - 1))
        strcpy(mv2_ar_phase_host, "unknown");
    mv2_ar_phase_host[sizeof(mv2_ar_phase_host) - 1] = '\0';
#ifdef _ENABLE_HOLLOW_RC_
    mv2_ar_phase_mode = "hollow";
#else
    mv2_ar_phase_mode = "ordinary";
    if (getenv("MV2_TRANSPORT_MODE") &&
        !strcmp(getenv("MV2_TRANSPORT_MODE"), "xrc"))
        mv2_ar_phase_mode = "xrc";
#endif
    MPIR_Add_finalize(mv2_ar_phase_report, NULL, MPIR_FINALIZE_CALLBACK_PRIO + 1);
    mv2_ar_phase_enabled = 1;
    return;
invalid:
    fprintf(stderr, "MV2_ALLREDUCE_PHASE: invalid diagnostic setting; "
            "disabled. STATS must be 0/1, EVERY a positive power of two, "
            "SKIP/BYTES unsigned decimal integers.\n");
}

static const char *mv2_ar_phase_inter_algo(void)
{
    /* Match the dispatch in the two-level helper, not the requested tuning
     * number. In particular Ring's function pointer falls back to pt2pt_rs.
     */
    if (MV2_Allreduce_function == &MPIR_Allreduce_pt2pt_rd_MV2)
        return "rd";
    if (MV2_Allreduce_function == &MPIR_Allreduce_pt2pt_reduce_scatter_allgather_MV2)
        return "rsa_collectives";
#if defined(_SHARP_SUPPORT_)
    if (MV2_Allreduce_function == &MPIR_Sharp_Allreduce_MV2)
        return "sharp_or_rd";
#endif
    return "rs";
}

static void mv2_ar_phase_begin(struct mv2_ar_phase_sample *s,
                               MPID_Comm *comm, int local_rank, int local_size,
                               int count, MPI_Datatype datatype, MPI_Op op)
{
    struct mv2_ar_phase_bucket *b = NULL;
    MPI_Aint type_size;
    uint64_t bytes;
    unsigned int i;
    const char *algo;
    if (mv2_ar_phase_enabled < 0)
        mv2_ar_phase_init();
    if (!mv2_ar_phase_enabled || comm->handle != MPI_COMM_WORLD)
        return;
    MPID_Datatype_get_size_macro(datatype, type_size);
    bytes = (uint64_t)count * (uint64_t)type_size;
    if (mv2_ar_phase_bytes && bytes != mv2_ar_phase_bytes)
        return;
    algo = local_size == comm->local_size ? "none" : mv2_ar_phase_inter_algo();
    for (i = 0; i < mv2_ar_phase_used; ++i) {
        b = &mv2_ar_phase_buckets[i];
        if (b->bytes == bytes && b->datatype == (unsigned int)datatype &&
            b->op == (unsigned int)op && b->algo == algo)
            break;
    }
    if (i == mv2_ar_phase_used) {
        if (i == MV2_AR_PHASE_BUCKETS) {
            mv2_ar_phase_dropped++;
            return;
        }
        b = &mv2_ar_phase_buckets[mv2_ar_phase_used++];
        b->bytes = bytes;
        b->datatype = (unsigned int)datatype;
        b->op = (unsigned int)op;
        b->algo = algo;
        b->rank = comm->rank;
        b->world = comm->local_size;
        b->local_rank = local_rank;
        b->local_size = local_size;
        b->inter = local_rank == 0 && local_size != comm->local_size;
    }
    b->calls++;
    if (b->calls <= mv2_ar_phase_skip ||
        ((b->calls - mv2_ar_phase_skip - 1) & (mv2_ar_phase_every - 1)))
        return;
    s->bucket = b;
    s->stage = 0;
    s->start = mv2_ar_phase_clock();
}

static void mv2_ar_phase_add(struct mv2_ar_phase_metric *m, double elapsed)
{
    m->sum += elapsed;
    if (elapsed > m->max)
        m->max = elapsed;
}

static void mv2_ar_phase_end(struct mv2_ar_phase_sample *s, int failed)
{
    struct mv2_ar_phase_bucket *b = s->bucket;
    double end = mv2_ar_phase_clock();
    if (failed || s->stage != 2 || s->reduced < s->start ||
        s->broadcast < s->reduced || end < s->broadcast) {
        b->failed++;
        return;
    }
    b->samples++;
    mv2_ar_phase_add(&b->reduce, s->reduced - s->start);
    mv2_ar_phase_add(&b->exchange, s->broadcast - s->reduced);
    mv2_ar_phase_add(&b->bcast, end - s->broadcast);
    mv2_ar_phase_add(&b->total, end - s->start);
}

#define MV2_AR_PHASE_DECLARE struct mv2_ar_phase_sample phase_sample = { .bucket = NULL }
#define MV2_AR_PHASE_BEGIN() do { \
    if (unlikely(mv2_ar_phase_enabled)) \
        mv2_ar_phase_begin(&phase_sample, comm_ptr, local_rank, local_size, \
                           count, datatype, op); \
} while (0)
#define MV2_AR_PHASE_REDUCED() do { \
    if (unlikely(phase_sample.bucket != NULL)) { \
        phase_sample.reduced = mv2_ar_phase_clock(); \
        phase_sample.stage = 1; \
    } \
} while (0)
#define MV2_AR_PHASE_BCAST() do { \
    if (unlikely(phase_sample.bucket != NULL)) { \
        phase_sample.broadcast = phase_sample.bucket->inter ? \
            mv2_ar_phase_clock() : phase_sample.reduced; \
        phase_sample.stage = 2; \
    } \
} while (0)
#define MV2_AR_PHASE_END() do { \
    if (unlikely(phase_sample.bucket != NULL)) \
        mv2_ar_phase_end(&phase_sample, mpi_errno || mpi_errno_ret); \
} while (0)
#else
/* CPPFLAGS=-DMV2_ENABLE_ALLREDUCE_PHASE_DIAG=0 removes even the branches. */
#define MV2_AR_PHASE_DECLARE
#define MV2_AR_PHASE_BEGIN() do {} while (0)
#define MV2_AR_PHASE_REDUCED() do {} while (0)
#define MV2_AR_PHASE_BCAST() do {} while (0)
#define MV2_AR_PHASE_END() do {} while (0)
#endif
#endif
