#ifndef MPIRUN_TIMESTAMP_H
#define MPIRUN_TIMESTAMP_H 1

#include <stdio.h>

int mv2_take_timestamp_mpirun (const char * label);
int mv2_print_timestamps (FILE * fd);

#endif
