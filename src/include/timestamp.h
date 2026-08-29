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
#ifndef _TIMESTAMP_H_
#define _TIMESTAMP_H_ 1

#include <stdio.h>
#include <mpichconf.h>

#ifdef PROFILE_STARTUP
/* runtime variable to toggle profiling */
extern int mv2_enable_startup_profiling;

/* adding support for size field */
int mv2_take_timestamp (const char * label, void * data);
int mv2_print_timestamps (FILE * fd);

int mv2_begin_delta (const char * label);
void mv2_end_delta (int delta_id);
void mv2_print_deltas (FILE * fd);
#else

/* dummy macros to avoid a ton of ifdefs */
#define mv2_take_timestamp(label, data)
#define mv2_print_timestamps(fd)

#endif /* PROFILE_STARTUP */

#endif /* _TIMESTAMP_H_ */
