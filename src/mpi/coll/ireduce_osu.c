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

#include "mpiimpl.h"
#include "collutil.h"

#include "ireduce_tuning.h"

MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_ireduce_sharp);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_ireduce_sharp);

#if defined(CHANNEL_MRAIL) || defined(CHANNEL_PSM)

int (*MV2_Ireduce_function) (const void *sendbuf, void *recvbuf, int count,
                             MPI_Datatype datatype, MPI_Op op, int root,
                             MPID_Comm *comm_ptr, MPID_Sched_t s) = NULL;

int (*MV2_Ireduce_intra_node_function) (const void *sendbuf, void *recvbuf, int count,
                             MPI_Datatype datatype, MPI_Op op, int root,
                             MPID_Comm *comm_ptr, MPID_Sched_t s) = NULL;

#if defined (_SHARP_SUPPORT_)
#undef FUNCNAME
#define FUNCNAME "MPIR_Sharp_Ireduce_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
/* Currently implemented on top of iallreduce. Ideally should use lower level S
 * calls to achieve the same once avaliable*/
int MPIR_Sharp_Ireduce_MV2(const void *sendbuf,
                          void *recvbuf,
                          int count,
                          MPI_Datatype datatype,
                          MPI_Op op,
                          int root,
                          MPID_Comm * comm_ptr,
                          MPIR_Errflag_t *errflag, MPID_Request **req)
{
    MPIR_TIMER_START(coll,ireduce,sharp);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_ireduce_sharp, 1);
    int mpi_errno = MPI_SUCCESS;
    void *new_recvbuf = NULL;
    int rank = comm_ptr->rank;

    if (rank != root) {
        new_recvbuf = (void *) comm_ptr->dev.ch.coll_tmp_buf;
    } else {
        new_recvbuf = (void *) recvbuf;
    }
    mpi_errno = MPIR_Sharp_Iallreduce_MV2(sendbuf, new_recvbuf, count,
                                         datatype, op, comm_ptr, (int *) errflag, req);
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }

fn_exit:
    MPIR_TIMER_END(coll,ireduce,sharp);
    return (mpi_errno);
fn_fail:
    goto fn_exit;
}
#endif /* End of sharp support */

#undef FUNCNAME
#define FUNCNAME MPIR_Ireduce_tune_helper_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static int MPIR_Ireduce_tune_helper_MV2(const void *sendbuf, void *recvbuf, int count,
                             MPI_Datatype datatype, MPI_Op op, int root,
                             MPID_Comm *comm_ptr, MPID_Sched_t s)
{
    int mpi_errno = MPI_SUCCESS;
    int is_homogeneous ATTRIBUTE((unused)), pof2, comm_size;

    MPIU_Assert(comm_ptr->comm_kind == MPID_INTRACOMM);

    is_homogeneous = 1;
#ifdef MPID_HAS_HETERO
    if (comm_ptr->is_hetero)
        is_homogeneous = 0;
#endif
    MPIU_Assert(is_homogeneous);
    comm_size = comm_ptr->local_size;
    
    pof2 = 1;
    while (pof2 <= comm_size) pof2 <<= 1;
    pof2 >>=1;

    if ((MV2_Ireduce_function == MPIR_Ireduce_redscat_gather) &&
        (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) && (count >= pof2))
    {
        mpi_errno = MPIR_Ireduce_redscat_gather(sendbuf, recvbuf, count, datatype, op, root, comm_ptr, s);
    }
    else {
        mpi_errno = MV2_Ireduce_function(sendbuf, recvbuf, count, datatype,
                                         op, root, comm_ptr, s);
    }
    if (mpi_errno) MPIR_ERR_POP(mpi_errno);
    
  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIR_Ireduce_intra_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Ireduce_intra_MV2(const void *sendbuf, void *recvbuf, int count,
                             MPI_Datatype datatype, MPI_Op op, int root,
                             MPID_Comm *comm_ptr, MPID_Sched_t s)
{
    int mpi_errno = MPI_SUCCESS;
    int comm_size, is_homogeneous ATTRIBUTE((unused));
    MPI_Aint sendtype_size, nbytes;
    
    int two_level_ireduce = 1;
    int range = 0;
    int range_threshold = 0;
    int range_threshold_intra = 0;

    MPIU_Assert(comm_ptr->comm_kind == MPID_INTRACOMM);

    is_homogeneous = 1;
#ifdef MPID_HAS_HETERO
    if (comm_ptr->is_hetero)
        is_homogeneous = 0;
#endif
    MPIU_Assert(is_homogeneous); /* we don't handle the hetero case right now */
    comm_size = comm_ptr->local_size;
    MPID_Datatype_get_size_macro(datatype, sendtype_size);
    nbytes = count * sendtype_size;

    // Search for some parameters regardless of whether subsequent selected
    // algorithm is 2-level or not
    
    // Search for the corresponding system size inside the tuning table
    while ((range < (mv2_size_ireduce_tuning_table - 1)) &&
           (comm_size > mv2_ireduce_thresholds_table[range].numproc)) {
        range++;
    }
    
    // Search for corresponding inter-leader function
    while ((range_threshold < (mv2_ireduce_thresholds_table[range].size_inter_table - 1))
           && (nbytes >
               mv2_ireduce_thresholds_table[range].inter_leader[range_threshold].max)
           && (mv2_ireduce_thresholds_table[range].inter_leader[range_threshold].max != -1)) {
        range_threshold++;
    }

    // Search for corresponding intra-node function
    
    // Commenting this for the time being as none of
    // the algorithms are 2-level
    /*
    while ((range_threshold_intra <
            (mv2_ireduce_thresholds_table[range].size_intra_table - 1))
           && (nbytes >
               mv2_ireduce_thresholds_table[range].intra_node[range_threshold_intra].max)
           && (mv2_ireduce_thresholds_table[range].intra_node[range_threshold_intra].max !=
               -1)) {
        range_threshold_intra++;
    }
    */

    MV2_Ireduce_function =
        mv2_ireduce_thresholds_table[range].inter_leader[range_threshold].
        MV2_pt_Ireduce_function;

    MV2_Ireduce_intra_node_function =
        mv2_ireduce_thresholds_table[range].
        intra_node[range_threshold_intra].MV2_pt_Ireduce_function;

    /* There are currently no two-level nb-reduce functions hence
       setting to 0 by default */
    two_level_ireduce = 
        mv2_ireduce_thresholds_table[range].is_two_level_ireduce[range_threshold]; 
    if (1 != two_level_ireduce) {
        mpi_errno = MPIR_Ireduce_tune_helper_MV2(sendbuf, recvbuf, count, datatype,
                                     op, root, comm_ptr, s);
    }
    else {
        /* Code path should not enter this with the current algorithms*/
    }

    return mpi_errno;
}
#endif                          /*#if defined(CHANNEL_MRAIL) || defined(CHANNEL_PSM) */

#undef FUNCNAME
#define FUNCNAME MPIR_Ireduce_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Ireduce_MV2(const void *sendbuf, void *recvbuf, int count,
                             MPI_Datatype datatype, MPI_Op op, int root,
                             MPID_Comm *comm_ptr, MPID_Sched_t s)
{
    int mpi_errno = MPI_SUCCESS;

    if (comm_ptr->comm_kind == MPID_INTRACOMM) {    
#if defined(CHANNEL_MRAIL) || defined(CHANNEL_PSM)
      mpi_errno = MPIR_Ireduce_intra_MV2(sendbuf, recvbuf, count, datatype,
					 op, root, comm_ptr, s);
#else
      mpi_errno = MPIR_Ireduce_intra(sendbuf, recvbuf, count, datatype,
                                     op, root, comm_ptr, s);
#endif                          /*#if defined(CHANNEL_MRAIL) || defined(CHANNEL_PSM) */
    }
    else {
      mpi_errno = MPIR_Ireduce_inter(sendbuf, recvbuf, count, datatype,
                                     op, root, comm_ptr, s);
    }

    return mpi_errno;
}
