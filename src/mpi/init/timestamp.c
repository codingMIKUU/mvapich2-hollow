/* Copyright (c) 2001-2022, The Ohio State University. All rights
 * reserved.
 *
 * This file is part of the MVAPICH2 software package developed by the
 * team members of The Ohio State University's Network-Based Computing
 * Laboratory (NBCL), headed by Professor Dhabaleswar K. (DK) Panda.
 *
 * For detailed copyright and licensing information, please refer to the
 * copyright file COPYRIGHT in the top level MVAPICH2 directory.
 *
 */
#include <time.h>
#include <stdio.h>
#include <mpiimpl.h>
#include <search.h>
#include <mpichconf.h>

#ifdef PROFILE_STARTUP

#ifndef MAX_TIMESTAMPS
#define MAX_TIMESTAMPS 200
#endif

int mv2_enable_startup_profiling = 0;

/* structs to hold start/end times on one process */
static struct delta_t {
    struct timespec begin;
    struct timespec end;
    const char * label;
    int shift;
} timestamp_deltas[MAX_TIMESTAMPS];

/* offset of last delta added to array */
static size_t current_timestamp_delta = 0;
/* offset of last unfinished timestep */
static size_t current_timestamp_shift = 0;

/* timestamp measures */
static struct timestamp_t {
    struct timespec ts;
    const char * label;
    void * data;
} timestamps[MAX_TIMESTAMPS];

/* location in array */
static size_t current_timestamp = 0;
static double delta[MAX_TIMESTAMPS];

/* reduced values for each function */
static double delta_avg[MAX_TIMESTAMPS];
static double delta_min[MAX_TIMESTAMPS];
static double delta_max[MAX_TIMESTAMPS];

/* indirect addressing from this index to the delta array */
/* delta_index holds the location of the end time */
static size_t delta_index[MAX_TIMESTAMPS];
/* how deep into a function tree we are */
static size_t delta_shift[MAX_TIMESTAMPS];

/* 
 * delta operations - work for functions on single process only
 * for timing functions on multiple processes, use the timestamp functions below 
 */
int mv2_begin_delta (const char * label)
{
    if (!(current_timestamp_delta < MAX_TIMESTAMPS)) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC_RAW,
            &timestamp_deltas[current_timestamp_delta].begin);
    timestamp_deltas[current_timestamp_delta].label = label;
    timestamp_deltas[current_timestamp_delta].shift = current_timestamp_shift++;
    return current_timestamp_delta++;
}

void mv2_end_delta (int delta_id)
{
    clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp_deltas[delta_id].end);
    current_timestamp_shift--;
}

void mv2_print_deltas (FILE * fd)
{
    size_t counter;

    fprintf(fd, "\nMPI Library Timing Info\n%40s%15s\n", "", "Time (sec)");

    for (counter = 0; counter < current_timestamp_delta; counter++) {
        struct timespec temp;

        temp.tv_nsec = timestamp_deltas[counter].end.tv_nsec -
            timestamp_deltas[counter].begin.tv_nsec;
        temp.tv_sec = timestamp_deltas[counter].end.tv_sec -
            timestamp_deltas[counter].begin.tv_sec;

        if (0 > temp.tv_nsec) {
            temp.tv_nsec += 1e9;
            temp.tv_sec -= 1;
        }

        fprintf(fd, "%*s%-*s %4lld.%.9lld\n",
                timestamp_deltas[counter].shift,
                "",
                35 - timestamp_deltas[counter].shift,
                timestamp_deltas[counter].label,
                (long long)temp.tv_sec,
                (long long)temp.tv_nsec);
    }

    fprintf(fd, "\n");
}

/* 
 * takes a single timestamp with a string label
 * 
 * do not use inside of loops - it will rapidly overflow the small 
 * buffer used to store the timestamps, and only the first pair will 
 * be shown. 
 */
int mv2_take_timestamp (const char * label, void * data)
{
    if (!mv2_enable_startup_profiling) {
        return 0;
    }

    if (current_timestamp < MAX_TIMESTAMPS) {
        int clock_error = clock_gettime(CLOCK_MONOTONIC_RAW,
                &timestamps[current_timestamp].ts);
        timestamps[current_timestamp].label = label;
        /* add data field to take size of push operations */
        timestamps[current_timestamp].data = data;

        if (!clock_error) {
            current_timestamp++;
        }

        return clock_error;
    }

    else {
        PRINT_ERROR("MAX_TIMESTAMPS (%d) exceeded. Please set CFLAGS=\"-DMAX_TIMESTAMPS <larger_value>\" to enable further profiling\n", MAX_TIMESTAMPS);
        return -2;
    }
}

/* 
 * basic mpi reduction 
 */
static int reduce_timestamps (MPID_Comm * comm_ptr, int pg_rank, int pg_size)
{
    if (!mv2_enable_startup_profiling) {
        return 0;
    }

    MPIR_Errflag_t errflag = 0;

    if (current_timestamp < 1) {
        return current_timestamp;
    }

    MPIR_Reduce((void *)delta, (void *)delta_avg, current_timestamp,
            MPI_DOUBLE, MPI_SUM, 0, comm_ptr, &errflag);

    MPIR_Reduce((void *)delta, (void *)delta_min, current_timestamp,
            MPI_DOUBLE, MPI_MIN, 0, comm_ptr, &errflag);

    MPIR_Reduce((void *)delta, (void *)delta_max, current_timestamp,
            MPI_DOUBLE, MPI_MAX, 0, comm_ptr, &errflag);
    /* 
     * TODO: currently data field just uses the value on rank 1
     * to add support for other arbitrary data some kind of reduction will 
     * be needed
     */

    if (0 == pg_rank) {
        size_t counter;

        for (counter = 1; counter < current_timestamp; counter++) {
            delta_avg[counter - 1] /= pg_size;
        }
    }

    return (int)errflag;
}

/* 
 * creates hash search table of timestamps
 */
static int process_timestamps (void)
{
    unsigned shift = 0;
    unsigned long i; 
    if (!mv2_enable_startup_profiling) {
        return 0;
    }

    if (!hcreate(current_timestamp * 1.25)) {
        return -1;
    }

    for (i = 0; i < current_timestamp; i++) {
        ENTRY e, * ep;

        /* hsearch uses strcmp on the key */
        e.key = (char *)timestamps[i].label;
        /* going to put delta into this index */
        e.data = (void *)i;

        /*
         * find an existing hash table entry with the same key
         * ep will have the first timestamp with this key
         * looks for a start time and marks e the corresponding end time 
         */
        if (NULL != (ep = hsearch(e, FIND))) {
            if (delta_index[(unsigned long)ep->data]) {
                /*
                 * Only allow one pair for timing.  Maybe there should be a
                 * stack so that multiple pairs can be timed (maybe for timing
                 * within loops?)
                 *
                 * skip if an index is already assigned to an end time
                 */
                continue;
            }

            /* returning from function, reset shift to that level */
            shift = delta_shift[(unsigned long)ep->data];
            /* set this entry as the end timestamp for ep */
            delta_index[(unsigned long)ep->data] = i;
        }
        else {
            /* first entry with this key - the start time */
            /* shift is the tree level or number of spaces to shift the output */
            delta_shift[i] = shift++;
            /* clear out the corresponding delta_index for an end time */
            delta_index[i] = 0;

            /* add start entry to hash table */
            if (NULL == hsearch(e, ENTER)) {
                return -1;
            };
        }
    }

    return 0;
}

/* 
 * hash and pair timesteps, reduce, and print
 */
int mv2_print_timestamps (FILE * fd)
{
    MPID_Comm * comm_ptr = NULL;
    size_t counter = 1;
    int pg_rank = 0;

    if (!mv2_enable_startup_profiling) {
        return 0;
    }

    PMPI_Comm_rank(MPI_COMM_WORLD, &pg_rank);
    MPID_Comm_get_ptr(MPI_COMM_WORLD, comm_ptr);

    /* feel like we should destroy the hash table if this fails */
    if (process_timestamps()) {
        return -1;
    }
    /* get deltas */    
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

        /* delta stores all differences in timestamps */
        delta[counter] = temp.tv_sec * 1e9 + temp.tv_nsec;
    }

    if (reduce_timestamps(comm_ptr, pg_rank, comm_ptr->local_size)) {
        return -1;
    }
    /* print timestamp deltas */
    if (0 == pg_rank) {
        /* adding support for data field */
        fprintf(fd, "\nMPI Library Timing Info\n%45s%15s%15s%15s%15s\n",
                "", "MIN", "AVG", "MAX", "Data");

        for (counter = 0; counter < current_timestamp; counter++) {
            if (0 == delta_index[counter]) continue;

            fprintf(fd, "%*s%-*s %4lld.%.9lld %4lld.%.9lld %4lld.%.9lld %14lld\n",
                    (int)delta_shift[counter], "", 45 -
                    (int)delta_shift[counter], timestamps[counter].label,
                    ((long long)delta_min[counter]) / (long long)1e9,
                    ((long long)delta_min[counter]) % (long long)1e9,
                    ((long long)delta_avg[counter]) / (long long)1e9,
                    ((long long)delta_avg[counter]) % (long long)1e9,
                    ((long long)delta_max[counter]) / (long long)1e9,
                    ((long long)delta_max[counter]) % (long long)1e9, 
                    (long long)timestamps[counter].data);
        }

        fprintf(fd, "\n");
    }

    hdestroy();

    return 0;
}
#endif /*PROFILE_STARTUP*/
