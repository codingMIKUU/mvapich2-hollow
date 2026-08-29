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

#include "ibarrier_tuning.h"

MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_ibarrier_sharp);

#if defined(CHANNEL_MRAIL) || defined(CHANNEL_PSM)

int (*MV2_Ibarrier_function) (MPID_Comm *comm_ptr, MPID_Sched_t s) = NULL;

int (*MV2_Ibarrier_intra_node_function) (MPID_Comm *comm_ptr, MPID_Sched_t s) = NULL;

#if defined (_SHARP_SUPPORT_)
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_ibarrier_sharp);

#undef FUNCNAME
#define FUNCNAME "MPIR_Sharp_Ibarrier_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Sharp_Ibarrier_MV2(MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag,
                            MPID_Request **req)
{
    MPIR_TIMER_START(coll, ibarrier,sharp);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_ibarrier_sharp, 1);
    int mpi_errno = MPI_SUCCESS;
    SHARP_REQ_HANDLE * sharp_req = NULL;

    struct sharp_coll_comm * sharp_comm =
        ((sharp_info_t *)comm_ptr->dev.ch.sharp_coll_info)
        ->sharp_comm_module
        ->sharp_coll_comm;

    /* Ensure that all messages in non-sharp channels are progressed first
     * to prevent deadlocks in subsequent blocking sharp API calls */
    while (rdma_global_ext_sendq_size) {
        MPIDI_CH3_Progress_test();
    }

    mpi_errno = sharp_ops.coll_do_barrier_nb(sharp_comm, &sharp_req);

    if (mpi_errno != SHARP_COLL_SUCCESS) {
        MPIR_ERR_SETANDJUMP2(mpi_errno, MPI_ERR_INTERN, "**sharpcoll", 
                             "**sharpcoll %s %d", 
                             sharp_ops.coll_strerror(mpi_errno), mpi_errno);
    }
    /* now create and populate the request */
    *req = MPID_Request_create();
    if(*req == NULL) {
        MPIR_ERR_SETANDJUMP2(mpi_errno, MPI_ERR_INTERN, "**sharpcoll",
                             "**sharpcoll %s %d", 
                             sharp_ops.coll_strerror(mpi_errno), mpi_errno);
    }
    (*req)->sharp_req = sharp_req;
    (*req)->kind = MPID_COLL_REQUEST;
    mpi_errno = MPI_SUCCESS;

  fn_exit:
    MPIR_TIMER_END(coll,ibarrier,sharp);
    return (mpi_errno);

  fn_fail:
    PRINT_DEBUG(DEBUG_Sharp_verbose, "Continuing without SHArP: %s \n",
                sharp_ops.coll_strerror(mpi_errno));
    goto fn_exit;
}
#endif /* end of defined (_SHARP_SUPPORT_) */

#undef FUNCNAME
#define FUNCNAME MPIR_Ibarrier_tune_helper_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static int MPIR_Ibarrier_tune_helper_MV2(MPID_Comm *comm_ptr, MPID_Sched_t s)
{
    int mpi_errno = MPI_SUCCESS;
    int is_homogeneous ATTRIBUTE((unused));

    MPIU_Assert(comm_ptr->comm_kind == MPID_INTRACOMM);

    is_homogeneous = 1;
#ifdef MPID_HAS_HETERO
    if (comm_ptr->is_hetero)
        is_homogeneous = 0;
#endif
    MPIU_Assert(is_homogeneous);

    mpi_errno = MV2_Ibarrier_function(comm_ptr, s);
    if (mpi_errno) MPIR_ERR_POP(mpi_errno);
    
  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIR_Ibarrier_intra_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Ibarrier_intra_MV2(MPID_Comm *comm_ptr, MPID_Sched_t s)
{
    int mpi_errno = MPI_SUCCESS;
    int comm_size, is_homogeneous ATTRIBUTE((unused));
    
    int two_level_ibarrier = 1;
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
    comm_size = comm_ptr->local_size;;

    // Search for some parameters regardless of whether subsequent selected
    // algorithm is 2-level or not
    
    // Search for the corresponding system size inside the tuning table
    while ((range < (mv2_size_ibarrier_tuning_table - 1)) &&
           (comm_size > mv2_ibarrier_thresholds_table[range].numproc)) {
        range++;
    }
    
    /*
    // Search for corresponding inter-leader function
    while ((range_threshold < (mv2_ibarrier_thresholds_table[range].size_inter_table - 1))
           && (nbytes >
               mv2_ibarrier_thresholds_table[range].inter_leader[range_threshold].max)
           && (mv2_ibarrier_thresholds_table[range].inter_leader[range_threshold].max != -1)) {
        range_threshold++;
    }

    // Search for corresponding intra-node function
    
    // Commenting this for the time being as none of
    // the algorithms are 2-level
    while ((range_threshold_intra <
            (mv2_ibarrier_thresholds_table[range].size_intra_table - 1))
           && (nbytes >
               mv2_ibarrier_thresholds_table[range].intra_node[range_threshold_intra].max)
           && (mv2_ibarrier_thresholds_table[range].intra_node[range_threshold_intra].max !=
               -1)) {
        range_threshold_intra++;
    }
    */

    MV2_Ibarrier_function =
        mv2_ibarrier_thresholds_table[range].inter_leader[range_threshold].
        MV2_pt_Ibarrier_function;

    MV2_Ibarrier_intra_node_function =
        mv2_ibarrier_thresholds_table[range].
        intra_node[range_threshold_intra].MV2_pt_Ibarrier_function;

    /* There are currently no two-level nb-barrier functions hence
       setting to 0 by default */
    two_level_ibarrier = 
        mv2_ibarrier_thresholds_table[range].is_two_level_ibarrier[range_threshold]; 
    if (1 != two_level_ibarrier) {
        mpi_errno = MPIR_Ibarrier_tune_helper_MV2(comm_ptr, s);
    }
    else {
        /* Code path should not enter this with the current algorithms*/
    }

    return mpi_errno;
}
#endif                          /*#if defined(CHANNEL_MRAIL) || defined(CHANNEL_PSM) */

#undef FUNCNAME
#define FUNCNAME MPIR_Ibarrier_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Ibarrier_MV2(MPID_Comm *comm_ptr, MPID_Sched_t s)
{
    int mpi_errno = MPI_SUCCESS;

    if (comm_ptr->comm_kind == MPID_INTRACOMM) {    
#if defined(CHANNEL_MRAIL) || defined(CHANNEL_PSM)
        mpi_errno = MPIR_Ibarrier_intra_MV2(comm_ptr, s);
#else
        mpi_errno = MPIR_Ibarrier_intra(comm_ptr, s);
#endif                          /*#if defined(CHANNEL_MRAIL) || defined(CHANNEL_PSM) */
    }
    else {
        mpi_errno = MPIR_Ibarrier_inter(comm_ptr, s);
    }

    return mpi_errno;
}
