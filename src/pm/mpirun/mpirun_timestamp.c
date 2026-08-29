#include <time.h>
#include <stdio.h>
#include <search.h>

#ifndef MPIRUN_MAX_TIMESTAMPS
#define MPIRUN_MAX_TIMESTAMPS 100
#endif

static struct timestamp_t {
    struct timespec ts;
    const char * label;
} timestamps[MPIRUN_MAX_TIMESTAMPS];

static size_t current_timestamp = 0;
static double delta[MPIRUN_MAX_TIMESTAMPS];

static size_t delta_index[MPIRUN_MAX_TIMESTAMPS];
static size_t delta_shift[MPIRUN_MAX_TIMESTAMPS];

int mv2_take_timestamp_mpirun (const char * label)
{
    if (current_timestamp < MPIRUN_MAX_TIMESTAMPS) {
        int clock_error = clock_gettime(CLOCK_MONOTONIC_RAW,
                &timestamps[current_timestamp].ts);
        timestamps[current_timestamp].label = label;

        if (!clock_error) {
            current_timestamp++;
        }

        return clock_error;
    }

    else {
        PRINT_ERROR("MPIRUN_MAX_TIMESTAMPS (%d) exceeded. Please set CFLAGS=\"-DMPIRUN_MAX_TIMESTAMPS <larger_value>\" to enable further profiling\n", MPIRUN_MAX_TIMESTAMPS);
        return -2;
    }
}

static int process_timestamps (void)
{
    unsigned i, shift = 0;

    if (!hcreate(current_timestamp * 1.25)) {
        return -1;
    }

    for (i = 0; i < current_timestamp; i++) {
        ENTRY e, * ep;

        e.key = timestamps[i].label;
        e.data = (void *)i;

        if (NULL != (ep = hsearch(e, FIND))) {
            if (delta_index[(unsigned)ep->data]) {
                /*
                 * Only allow one pair for timing.  Maybe there should be a
                 * stack so that multiple pairs can be timed (maybe for timing
                 * within loops?)
                 */
                continue;
            }

            shift = delta_shift[(unsigned)ep->data];
            delta_index[(unsigned)ep->data] = i;
        }

        else {
            delta_shift[i] = shift++;
            delta_index[i] = 0;

            if (NULL == hsearch(e, ENTER)) {
                return -1;
            };
        }
    }

    return 0;
}

int mv2_print_timestamps (FILE * fd)
{
    size_t counter = 1;

    if (process_timestamps()) {
        return -1;
    }

    for (counter = 0; counter < current_timestamp; counter++) {
        struct timespec temp;

        temp.tv_nsec = timestamps[delta_index[counter]].ts.tv_nsec -
            timestamps[counter].ts.tv_nsec;
        temp.tv_sec = timestamps[delta_index[counter]].ts.tv_sec -
            timestamps[counter].ts.tv_sec;

        if (0 > temp.tv_nsec) {
            temp.tv_nsec += 1e9;
            temp.tv_sec -= 1;
        }

        delta[counter] = temp.tv_sec * 1e9 + temp.tv_nsec;
    }

    fprintf(fd, "\nMPI Launcher State Timing Info\n%40s%15s\n",
            "", "Time (sec)");
    for (counter = 0; counter < current_timestamp; counter++) {
        if (0 == delta_index[counter]) continue;

        fprintf(fd, "%*s%-*s %4lld.%.9lld\n",
                delta_shift[counter], "", 40 -
                delta_shift[counter], timestamps[counter].label,
                ((long long)delta[counter]) / (long long)1e9,
                ((long long)delta[counter]) % (long long)1e9);
    }

    fprintf(fd, "\n");
    hdestroy();

    return 0;
}
