/* parser for hostfile reader */

%{
/*
 * Copyright (c) 2001-2022, The Ohio State University. All rights
 * reserved.
 *
 * This file is part of the MVAPICH2 software package developed by the
 * team members of The Ohio State University's Network-Based Computing
 * Laboratory (NBCL), headed by Professor Dhabaleswar K. (DK) Panda.
 *
 * For detailed copyright and licensing information, please refer to the
 * copyright file COPYRIGHT in the top level MVAPICH2 directory.
 */

#include <process.h>
#include <debug_utils.h>
#include <mpirun_util.h>
#include <db/text.h>
#include <unistd.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>

extern int yylex();

void yyerror (char const *s);
static int commit(void);
static void free_memory(void);
static void print_memory(void);

static struct rank_s {
    char const * hostname;
    char const * hca;
    int port;
} current = {NULL, NULL, -1};

static size_t multiplier = 1;
static size_t n_ranks = 0;
static size_t n_alloc = 0;
static struct rank_s * rank = NULL;
static char const * hostfile = NULL;
static int lineno = 1;


%}

%union {
    size_t decimal;
    char * text;
}

%token <decimal> DECIMAL
%token <text> TEXT

%%

hostfile:   /* empty */
        |   hostfile line
;

line:   '\n'                            { lineno++; }
    |   hostname '\n'                   { lineno++; if(commit()) YYERROR; }
    |   error '\n'                      { lineno++; YYERROR; }
;

hostname:   TEXT                        { current.hostname = $1; }
        |   hostname ':' specifiers
;

specifiers: multiplier
          | multiplier ':' hca
          | hca
;

multiplier: DECIMAL                     { multiplier = $1; }
;

hca:    TEXT                            { current.hca = $1; }
   |    TEXT ':' DECIMAL                { current.hca = $1; current.port = $3; }
;

%%

extern FILE * hostfile_yyin;
extern process * plist;
extern int nprocs;
extern int nprocs_per_node;
extern int dpm;

static void
print_memory(void)
{
#if DEBUG_HOSTFILE_READER
    size_t i;

    for (i = 0; i < n_ranks; i++) {
        PRINT_DEBUG(DEBUG_HOSTFILE_READER, "(%s, %s, %d)\n",
            rank[i].hostname,
            rank[i].hca ? rank[i].hca : "(NULL)",
            rank[i].port);
    }
#endif
}

static int
commit(void)
{
    extern size_t multiplier;
    extern size_t n_ranks;
    extern size_t n_alloc;
    extern struct rank_s * rank;

    if ((multiplier + n_ranks) > n_alloc) {
        size_t p_alloc = n_alloc;
        void * ptr;

        do {
            n_alloc = (n_alloc) ? (n_alloc << 1) : 64;

            /* Handle overflow */
            if (p_alloc > n_alloc) {
                PRINT_ERROR("Error allocating space for parsing hostfile\n");
                n_alloc = p_alloc;
                return -1;
            }
        } while ((multiplier + n_ranks) > n_alloc);

        ptr = realloc(rank, sizeof(struct rank_s) * n_alloc);

        /* Handle allocation failure */
        if (ptr == NULL) {
            PRINT_ERROR_ERRNO("Error allocating space for parsing hostfile", errno);
            n_alloc = p_alloc;
            return -1;
        }

        rank = ptr;
    }

    while (multiplier--) {
        rank[n_ranks].hostname = db_add_text(current.hostname);
        rank[n_ranks].hca = db_add_text(current.hca);
        rank[n_ranks].port = current.port;

        ++n_ranks;
    }

    if (current.hostname) free((void *)current.hostname);
    if (current.hca) free((void *)current.hca);

    current.hostname = NULL;
    current.hca = NULL;
    current.port = -1;
    multiplier = 1;

    return 0;
}

extern int
read_hostfile(char const * pathname, int using_pbs)
{
    int rv;
    int i, offset = dpm ? env2int("TOTALPROCS") : 0;
    int n = offset + nprocs;
    int host_index = 0;
    int host_num = 0;
    pid_t cpid;
    int status = 0;

    multiplier = 1;
    lineno = 1;

    hostfile = pathname;
    /* create uniq'ed version of hostfile */
    if (using_pbs && env2int("PBS_NUM_PPN") != nprocs_per_node) {
        char const * src_hostfile = hostfile;
        char * homedir = env2str("HOME");
        int jobid = env2int("PBS_JOBID");
        pid_t pid = getpid();
        sigset_t tmp_mask, old_mask;

        hostfile = mkstr("%s/.mpirun_hosts_%d_%d.tmp", homedir, jobid, pid);

        sigemptyset(&tmp_mask);
        sigprocmask(SIG_SETMASK, &tmp_mask, &old_mask);

        if (!(cpid = fork())) {
            /* child process to call uniq */
            const char *const args[4] = {"uniq", src_hostfile, hostfile, NULL};
            execvp(args[0], (char *const *)args); 

            perror("execvp");
            PRINT_ERROR("Failed to create unique hostfile, "
                        "startup failed\n");
            exit(EXIT_FAILURE);
        }
        /* wait for child process to complete */
        waitpid(cpid, &status, 0);
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
#ifdef DEBUG_HOSTFILE_READER
        PRINT_DEBUG(DEBUG_HOSTFILE_READER, "Temporary hostfile created: %s\n",
                    hostfile);
#endif
    }

    hostfile_yyin = fopen(hostfile, "r");

    if (hostfile_yyin == NULL) {
	PRINT_ERROR_ERRNO("Can't open hostfile `%s'", errno, hostfile);
	exit(EXIT_FAILURE);
    }

    rv = yyparse();

    if (rv) {
        print_memory();
        free_memory();
        fclose(hostfile_yyin);
        
        exit(EXIT_FAILURE);
    }

    if (n_ranks == 0) {
        PRINT_ERROR("No host found in hostfile `%s'\n", hostfile);
        print_memory();
        free_memory();
        fclose(hostfile_yyin);
        exit(EXIT_FAILURE);
    }
    
    for (i = offset; i < n; i++) {
        /* 
         * if the user set the ppn value we only assign the next host every
         * ppn interations. Otherwise we change it for each iteration for 
         * cyclic distribution 
         */
        if (!nprocs_per_node || !(i % nprocs_per_node)) {
            host_index = host_num % n_ranks;
            host_num++;
        }
        plist[i - offset].hostname = rank[host_index].hostname;

        if (rank[i % n_ranks].hca) {
            plist[i - offset].device = rank[host_index].hca;
        }

        if (rank[i % n_ranks].port >= 0) {
            plist[i - offset].port = rank[host_index].port;
        }
    }

    print_memory();
    free_memory();
    fclose(hostfile_yyin);

    /* remove temporary hostfile */
    if (hostfile != pathname) {
        if (unlink(hostfile)) {
            PRINT_ERROR("Failed to remove temporary hostfile %s\n", hostfile);
        }
    }

    return rv;
}
    
static void
free_memory(void)
{
    if (rank) {
        free(rank);
        rank = NULL;
    }

    n_ranks = 0;
    n_alloc = 0;
}

void
yyerror (char const * s)
{
    extern char const * hostfile;
    extern int lineno;

    PRINT_ERROR("Error parsing hostfile `%s' line %d - %s\n", hostfile, lineno, s);
}
