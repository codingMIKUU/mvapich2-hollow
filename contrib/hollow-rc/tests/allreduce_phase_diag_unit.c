/* Exercise the real diagnostic header without MPI processes or RDMA. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long MPI_Aint;
typedef int MPI_Datatype;
typedef int MPI_Op;
typedef double MPID_Time_t;
typedef struct { int handle, rank, local_size; } MPID_Comm;
#define MPI_SUCCESS 0
#define MPI_COMM_WORLD 1
#define MPIR_FINALIZE_CALLBACK_PRIO 5
#define unlikely(x) __builtin_expect(!!(x), 0)
#define MPID_Datatype_get_size_macro(dtype, size) ((size) = (dtype))

static unsigned int clock_reads, finalize_registrations;
static int (*finalize_callback)(void *);
static void MPID_Wtime(MPID_Time_t *time) { *time = ++clock_reads * 1e-6; }
static void MPID_Wtime_todouble(MPID_Time_t *time, double *value) { *value = *time; }
static void MPIR_Add_finalize(int (*callback)(void *), void *extra, int priority)
{
    assert(extra == NULL && priority == 6);
    finalize_callback = callback;
    finalize_registrations++;
}
static void MPIR_Allreduce_pt2pt_rd_MV2(void) {}
static void MPIR_Allreduce_pt2pt_reduce_scatter_allgather_MV2(void) {}
static void ring_requested(void) {}
static void (*MV2_Allreduce_function)(void) = ring_requested;

#include "../../../src/mpi/coll/allreduce_phase_diag.h"

static void call_body(MPID_Comm *comm_ptr, int local_rank, int local_size,
                      int count, int fail_early)
{
    MPI_Datatype datatype = 4;
    MPI_Op op = 1;
    int mpi_errno = fail_early, mpi_errno_ret = 0;
    MV2_AR_PHASE_DECLARE;
    MV2_AR_PHASE_BEGIN();
    if (!fail_early) {
        MV2_AR_PHASE_REDUCED();
        MV2_AR_PHASE_BCAST();
    }
    MV2_AR_PHASE_END();
}

int main(int argc, char **argv)
{
    MPID_Comm comm = {MPI_COMM_WORLD, 0, 4};
    const char *scenario = argc > 1 ? argv[1] : "leader";
    int local_rank = !strcmp(scenario, "nonleader") ? 1 : 0;
    int local_size = !strcmp(scenario, "local") ? 4 : 2;
    int i;
    comm.rank = local_rank;
    if (!strcmp(scenario, "nonworld"))
        comm.handle = 2;
    if (!strcmp(scenario, "rd"))
        MV2_Allreduce_function = MPIR_Allreduce_pt2pt_rd_MV2;
    for (i = 0; i < 10; ++i)
        call_body(&comm, local_rank, local_size, 64, !strcmp(scenario, "error"));
    if (!strcmp(scenario, "sizes"))
        for (i = 0; i < 10; ++i)
            call_body(&comm, local_rank, local_size, 128, 0);
#if MV2_ENABLE_ALLREDUCE_PHASE_DIAG
    for (i = 0; i < (int)mv2_ar_phase_used; ++i) {
        struct mv2_ar_phase_bucket *b = &mv2_ar_phase_buckets[i];
        assert(fabs(b->total.sum - b->reduce.sum - b->exchange.sum - b->bcast.sum) < 1e-12);
        if (local_rank || local_size == comm.local_size)
            assert(b->exchange.sum == 0);
    }
#endif
    printf("clock_reads=%u finalize_registrations=%u\n", clock_reads, finalize_registrations);
    if (finalize_callback)
        finalize_callback(NULL);
    return 0;
}
