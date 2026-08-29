/*RAM
 * Copyright (C) 1999-2001 The Regents of the University of California
 * (through E.O. Lawrence Berkeley National Laboratory), subject to
 * approval by the U.S. Department of Energy.
 *
 * Use of this software is under license. The license agreement is included
 * in the file MVICH_LICENSE.TXT.
 *
 * Developed at Berkeley Lab as part of MVICH.
 *
 * Authors: Bill Saphir      <wcsaphir@lbl.gov>
 *          Michael Welcome  <mlwelcome@lbl.gov>
 */

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

#include <mpichconf.h>
#include <src/pm/mpirun/mpirun_rsh.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <debug_utils.h>
#include "mpispawn_tree.h"
#include "mpirun_util.h"
#include "mpmd.h"
#include "mpirun_dbg.h"
#include "mpirun_params.h"
#include "src/slurm/slurm_startup.h"
#include "src/pbs/pbs_startup.h"
#include <mpirun_environ.h>
#include "mpirun_ckpt.h"

#if defined(_NSIG)
#define NSIG _NSIG
#endif                          /* defined(_NSIG) */

extern int read_hostfile(char const *hostfile_name, int using_pbs);

process *plist = NULL;
int nprocs = 0;
int nprocs_per_node = 0;
int aout_index = 0;

/* xxx need to add checking for string overflow, do this more carefully ... */
char *mpispawn_param_env = NULL;
char *spawnfile = NULL;
char *binary_dirname = NULL;

#if defined(USE_RSH)
int use_rsh = 1;
#else                           /* defined(USE_RSH) */
int use_rsh = 0;
#endif                          /* defined(USE_RSH) */

int xterm_on = 0;
int show_on = 0;
int use_dirname = 1;
int hostfile_on = 0;
int param_count = 0;
int legacy_startup = 0;
int dpm = 0;
extern spawn_info_t spinf;
int USE_LINEAR_SSH = 1;         /* By default, use linear ssh. Enable
                                   -fastssh for tree based ssh */
int USE_SRUN = 0;               /* Enable -srun for using srun instead of
                                   ssh or rsh */
#define LAUNCHER_LEN 4          /* set for srun as the longest name */

char hostfile[HOSTFILE_LEN + 1];
char launcher[LAUNCHER_LEN + 1];

static int using_slurm = 0;
static int using_pbs = 0;
/*
  The group active for mpispawn. NULL if no group change is required.
 */
char *change_group = NULL;

static struct option option_table[] = {
    {"np", required_argument, 0, 0},    // 0
    {"ppn", required_argument, 0, 0}, 
    {"debug", no_argument, 0, 0},
    {"xterm", no_argument, 0, 0},
    {"hostfile", required_argument, 0, 0},
    {"show", no_argument, 0, 0},  
    {"launcher", required_argument, 0, 0},
    {"help", no_argument, 0, 0},
    {"v", no_argument, 0, 0},
    {"tv", no_argument, 0, 0},
    {"legacy", no_argument, 0, 0},
    {"startedByTv", no_argument, 0, 0},
    {"spawnfile", required_argument, 0, 0},
    {"dpm", no_argument, 0, 0},
    {"fastssh", no_argument, 0, 0}, 
    //This option is to activate the mpmd, it requires the configuration file as argument
    {"config", required_argument, 0, 0},
    {"dpmspawn", required_argument, 0, 0},
    // This option enables the group selection for mpispawns
    {"sg", required_argument, 0, 0},
    {"export", no_argument, 0, 0},
    {"export-all", no_argument, 0, 0},
#if defined(CKPT) && defined(CR_FTB)
    {"sparehosts", required_argument, 0, 0},
#endif
    {0, 0, 0, 0}
};

/*
 * option enum for the switch case selection.
 *
 * option names should be added to the same position
 * in the enum list as they are in the option list above 
 */
enum mpirun_option_name {
    mpirun_option_np = 0,
    mpirun_option_ppn, 
    mpirun_option_debug,
    mpirun_option_xterm,
    mpirun_option_hostfile,
    mpirun_option_show,
    mpirun_option_launcher,
    mpirun_option_help,
    mpirun_option_v,
    mpirun_option_tv,
    mpirun_option_legacy,
    mpirun_option_startedByTv,
    mpirun_option_spawnfile,
    mpirun_option_dpm,
    mpirun_option_fastssh,
    mpirun_option_config,
    mpirun_option_dpmspawn,
    mpirun_option_sg,
    mpirun_option_export,
    mpirun_option_export_all,
    mpirun_option_sparehosts
};

static inline int mpirun_get_launcher(char *launcher_arg) 
{ 
    const char* const mpirun_launcher_options[] = { "ssh", "rsh", "srun" };
    int launchers = 3;
    int i, ret = 0;
    for (i = 0; i < launchers; i++) {
        if (!strcmp(launcher_arg, mpirun_launcher_options[i])) {
            break;
        }
    }
    switch (i) {
        case 0:
            use_rsh = 0;
            USE_SRUN = 0;
            DBG(fprintf(stderr, "ssh => use_rsh: %d, USE_SRUN: %d\n", use_rsh, USE_SRUN));
            break;
        case 1:
            use_rsh = 1;
            USE_SRUN = 0;
            DBG(fprintf(stderr, "rsh => use_rsh: %d, USE_SRUN: %d\n", use_rsh, USE_SRUN));
            break;
        case 2:
            use_rsh = 0;
            USE_SRUN = 1;
            DBG(fprintf(stderr, "srun => use_rsh: %d, USE_SRUN: %d\n", use_rsh, USE_SRUN));
            break;
        default:
            fprintf(stderr, "Invalid launcher selected. Launcher must be one of:\n");
            for (i = 0; i < launchers; i++) {
                fprintf(stderr, "\t%s\n", mpirun_launcher_options[i]);
            }
            ret = -1;
            break;
    }
    return ret;
}

#if !defined(HAVE_GET_CURRENT_DIR_NAME)
char *get_current_dir_name()
{
    struct stat64 dotstat;
    struct stat64 pwdstat;
    char *pwd = getenv("PWD");

    if (pwd != NULL && stat64(".", &dotstat) == 0 && stat64(pwd, &pwdstat) == 0 && pwdstat.st_dev == dotstat.st_dev && pwdstat.st_ino == dotstat.st_ino) {
        /* The PWD value is correct. */
        return strdup(pwd);
    }

    size_t size = 1;
    char *buffer;

    for (;; ++size) {
        buffer = malloc(size);

        if (!buffer) {
            return NULL;
        }

        if (getcwd(buffer, size) == buffer) {
            break;
        }

        free(buffer);

        if (errno != ERANGE) {
            return NULL;
        }
    }

    return buffer;
}
#endif                          /* !defined(HAVE_GET_CURRENT_DIR_NAME) */

#if !defined(HAVE_STRNDUP)
char *strndup(const char *s, size_t n)
{
    size_t len = strlen(s);

    if (n < len) {
        len = n;
    }

    char *result = malloc(len + 1);

    if (!result) {
        return NULL;
    }

    result[len] = '\0';
    return memcpy(result, s, len);
}
#endif                          /* !defined(HAVE_STRNDUP) */

#define PARAMFILE_LEN 256

/**
 * Command line analysis function.
 *
 * mpirun [-debug] [-xterm] -np N [-hostfile hfile | h1 h2 h3 ... hN] a.out [args]
 */
void commandLine(int argc, char *argv[], char *totalview_cmd, char **env)
{
    int i;
    int c, option_index;
    int cmd_line_hosts = 0;
    char *env_name = NULL;

    /* RM check */
    /* TODO: have one function return a constant or mask */
    using_slurm = check_for_slurm();
    using_pbs = check_for_pbs();

    do {
        c = getopt_long_only(argc, argv, "+", option_table, &option_index);
        switch (c) {
        case '?':
        case ':':
            usage(argv[0]);
            exit(EXIT_FAILURE);
            break;
        case EOF:
            break;
        case 0:
            switch ((enum mpirun_option_name)option_index) {
            case mpirun_option_np:            /* -np */
                nprocs = atoi(optarg);
                if (nprocs < 1) {
                    usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case mpirun_option_ppn:
                nprocs_per_node = atoi(optarg);
                if (nprocs_per_node < 1) {
                    usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case mpirun_option_debug:            /* -debug */
                debug_on = 1;
                xterm_on = 1;
                break;
            case mpirun_option_xterm:            /* -xterm */
                xterm_on = 1;
                break;
            case mpirun_option_hostfile:            /* -hostfile */
                hostfile_on = 1;
                strncpy(hostfile, optarg, HOSTFILE_LEN);
                if (strlen(optarg) >= HOSTFILE_LEN - 1)
                    hostfile[HOSTFILE_LEN] = '\0';
                break;
            case mpirun_option_show:
                show_on = 1;
                break;
            case mpirun_option_launcher:
                strncpy(launcher, optarg, LAUNCHER_LEN);
                if (mpirun_get_launcher(launcher)) {
                    usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                break;
            case mpirun_option_help:
                usage(argv[0]);
                exit(EXIT_SUCCESS);
                break;
            case mpirun_option_v:
                PRINT_MVAPICH2_VERSION();
                exit(EXIT_SUCCESS);
                break;
            case mpirun_option_tv:
                {
                    /* -tv */
                    char *tv_env;
                    int count, idx;
                    char **new_argv;
                    tv_env = getenv("TOTALVIEW");
                    if (tv_env != NULL) {
                        strncpy(totalview_cmd, tv_env, TOTALVIEW_CMD_LEN);
                    } else {
                        fprintf(stderr, "TOTALVIEW env is NULL, use default: %s\n", TOTALVIEW_CMD);
                        sprintf(totalview_cmd, "%s", TOTALVIEW_CMD);
                    }
                    new_argv = (char **) malloc(sizeof(char **) * argc + 3);
                    new_argv[0] = totalview_cmd;
                    new_argv[1] = argv[0];
                    new_argv[2] = "-a";
                    new_argv[3] = "-startedByTv";
                    idx = 4;
                    for (count = 1; count < argc; count++) {
                        if (strcmp(argv[count], "-tv"))
                            new_argv[idx++] = argv[count];
                    }
                    new_argv[idx] = NULL;
                    if (execv(new_argv[0], new_argv)) {
                        perror("execv");
                        exit(EXIT_FAILURE);
                    }

                }
                break;
            case mpirun_option_legacy:
                legacy_startup = 1;
                break;
            case mpirun_option_startedByTv:
                /* -startedByTv */
                use_totalview = 1;
                debug_on = 1;
                break;
            case mpirun_option_spawnfile:           /* spawnspec given */
                spawnfile = strdup(optarg);
                DBG(fprintf(stderr, "spawn spec file = %s\n", spawnfile));
                break;
            case mpirun_option_dpm:
                dpm = 1;
                break;
            case mpirun_option_fastssh:           /* -fastssh */
#if !defined(CR_FTB)
                /* disable hierarchical SSH if migration is enabled */
                USE_LINEAR_SSH = 0;
#endif 
                break;
                //With this option the user want to activate the mpmd
            case mpirun_option_config:
                configfile_on = 1;
                strncpy(configfile, optarg, CONFILE_LEN);
                if (strlen(optarg) >= CONFILE_LEN - 1)
                    configfile[CONFILE_LEN] = '\0';
                break;
            case mpirun_option_dpmspawn:
                spinf.totspawns = atoi(optarg);
                break;
            case mpirun_option_sg:
                /* sg: change the active group */
                change_group = optarg;
                DBG(printf("Group change requested: '%s'\n", change_group));
                break;
            case mpirun_option_export:
                enable_send_environ(0);
                break;
            case mpirun_option_export_all:
                enable_send_environ(1);
                break;
#if defined(CKPT) && defined(CR_FTB)
            case mpirun_option_sparehosts:
                sparehosts_on = 1;
                strncpy(sparehostfile, optarg, HOSTFILE_LEN);
                if (strlen(optarg) >= HOSTFILE_LEN - 1) {
                    sparehostfile[HOSTFILE_LEN] = 0;
                }
                break;
#endif
            default:
                fprintf(stderr, "Unknown option\n");
                usage(argv[0]);
                exit(EXIT_FAILURE);
                break;
            }
            break;
        default:
            fprintf(stderr, "Unreachable statement!\n");
            usage(argv[0]);
            exit(EXIT_FAILURE);
            break;
        }
    }
    while (c != EOF);

    if (!nprocs && !configfile_on) {
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    binary_dirname = dirname(strdup(argv[0]));
    if (strlen(binary_dirname) == 1 && argv[0][0] != '.') {
        use_dirname = 0;
    }
    //If the mpmd is active we need to parse the configuration file
    if (configfile_on) {
        /*TODO In the future the user can add the nprocs on the command line. Now the
         * number of processes is defined in the configfile */
        nprocs = 0;
        plist = parse_config(configfile, &nprocs);
        DBG(fprintf(stderr, "PARSED CONFIG FILE\n"));

    }

    if (!hostfile_on && !nprocs_per_node) {
        /* get hostnames from argument list */
        /* TODO: this line is a bad hack and assumes that args < np will
         * always be hostnames. However, consider the case of np=2 and the
         * benchmark './osu_latency -m 256'. In this instance we would
         * consider './osu_latency' and '-m' to be hosts. It would be much
         * better practice to delimit the host list with a flag
         */
        if (strchr(argv[optind], '=') || argc - optind < nprocs + 1) {
            /* only fall back to default host file if no RM detected */
            if (using_slurm || using_pbs) {
                aout_index = optind;
                goto cont;
            }
            env_name = env2str("HOME");
            sprintf(hostfile, "%s/.mpirun_hosts", env_name);
            if (env_name) {
                free(env_name);
                env_name = NULL;
            }
            if (file_exists(hostfile)) {
                hostfile_on = 1;
                aout_index = optind;
                goto cont;
            } else {
                PRINT_ERROR("No hostfile specified and no supported RM "
                            "detected.\n");
                PRINT_ERROR("Without hostfile option, hostnames must be " 
                                "specified on command line.\n");
                usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } 
        /* hosts on command line */
        cmd_line_hosts = 1;
        aout_index = nprocs + optind;
    } else {                    /* if (!hostfile_on) */
        aout_index = optind;
    }

  cont:
    if (!configfile_on) {
        plist = malloc(nprocs * sizeof(process));
        if (plist == NULL) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }

        for (i = 0; i < nprocs; i++) {
            plist[i].state = P_NOTSTARTED;
            plist[i].device = NULL;
            plist[i].port = -1;
            plist[i].remote_pid = 0;
            //TODO ADD EXECNAME AND ARGS

        }
    }

    /* grab hosts from command line or file */
    if (hostfile_on) {
        read_hostfile(hostfile, 0);
    } else if (cmd_line_hosts)  {
        for (i = 0; i < nprocs; i++) {
            plist[i].hostname = argv[optind + i];
        }
    }
    /* check if we are under an RM with a supported node list */
    else if (using_pbs) {
        /* job allocated via pbs - node file created by RM */
        if (read_hostfile(pbs_nodefile(), 1)) {
            PRINT_ERROR("Unable to parse PBS_NODEFILE [%s]", pbs_nodefile());
            exit(EXIT_FAILURE);
        }
    }
    else if (using_slurm) {
        /* job allocated via slurm - a node list exists */
        if (slurm_startup(nprocs, nprocs_per_node)) {
            PRINT_ERROR("Slurm startup failed");
            exit(EXIT_FAILURE);
        }
    }
}

void usage(const char * arg0)
{
    fprintf(stderr, "usage: mpirun_rsh [-v] [-sg group] [-launcher rsh|ssh|srun] "
            "[-debug] -[tv] [-xterm] [-show] [-legacy] [-export|-export-all] "
            "-np N "
#if defined(CKPT) && defined(CR_FTB)
            "[-sparehosts sparehosts_file] "
#endif
            "(-hostfile hfile | h1 h2 ... hN) a.out args | -config configfile (-hostfile hfile | h1 h2 ... hN)]\n");
    fprintf(stderr, "Where:\n");
    fprintf(stderr, "\tsg         => " "execute the processes as different group ID\n");
    fprintf(stderr, "\tlauncher   => " "one of rsh, ssh, or srun for connecing (ssh is default)\n"); 
    fprintf(stderr, "\tdebug      => " "run each process under the control of gdb\n");
    fprintf(stderr, "\ttv         => " "run each process under the control of totalview\n");
    fprintf(stderr, "\txterm      => " "run remote processes under xterm\n");
    fprintf(stderr, "\tshow       => " "show command for remote execution but don't run it\n");
    fprintf(stderr, "\tlegacy     => " "use old startup method (1 ssh/process)\n");
    fprintf(stderr, "\texport     => " "automatically export environment to remote processes\n");
    fprintf(stderr, "\texport-all => " "automatically export environment to remote processes even if already set remotely\n");
    fprintf(stderr, "\tnp         => " "specify the number of processes\n");
    fprintf(stderr, "\tppn        => " "specify the number of processes to allocate to eachnode\n");
    fprintf(stderr, "\th1 h2...   => " "names of hosts where processes should run\n");
    fprintf(stderr, "or\thostfile   => " "name of file containing hosts, one per line\n");
    fprintf(stderr, "\ta.out      => " "name of MPI binary\n");
    fprintf(stderr, "\targs       => " "arguments for MPI binary\n");
    fprintf(stderr, "\tconfig     => " "name of file containing the exe information: each line has the form -n numProc : exe args\n");
#if defined(CKPT) && defined(CR_FTB)
    fprintf(stderr, "\tsparehosts => " "file containing the spare hosts for migration, one per line\n");
#endif
    fprintf(stderr, "\n");
}

int file_exists(char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
}

/* vi:set sw=4 sts=4 tw=76 expandtab: */
