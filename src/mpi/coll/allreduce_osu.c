/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 *  (C) 2001 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
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

#include "mpiimpl.h"
#include "coll_shmem.h"
#include "common_tuning.h"
#include "allreduce_tuning.h"
#include "bcast_tuning.h"
#include "reduce_tuning.h"
#if defined(CKPT)
#include <cr.h>
#endif

#if defined (_SHARP_SUPPORT_)
#include "api/sharp_coll.h"
#include "ibv_sharp.h"
extern int mv2_sharp_tuned_msg_size;
#endif

MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_sharp);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_shm_rd);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_shm_rs);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_shm_intra);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_intra_p2p);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_2lvl);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_shmem);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_mcast);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_reduce_scatter_allgather_colls);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_subcomm);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_topo_aware_hierarchical);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_pt2pt_ring);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_pt2pt_ring_wrapper);
MPIR_T_PVAR_DOUBLE_TIMER_DECL_EXTERN(MV2, mv2_coll_timer_allreduce_pt2pt_ring_inplace);

MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_sharp);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_shm_rd);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_shm_rs);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_shm_intra);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_intra_p2p);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_2lvl);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_shmem);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_mcast);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_reduce_scatter_allgather_colls);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_subcomm);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_topo_aware_hierarchical);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_wrapper);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_inplace);

MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_rd_bytes_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_rs_bytes_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_bytes_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_wrapper_bytes_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_inplace_bytes_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_rd_bytes_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_rs_bytes_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_bytes_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_wrapper_bytes_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_inplace_bytes_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_rd_count_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_rs_count_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_count_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_wrapper_count_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_inplace_count_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_rd_count_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_rs_count_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_count_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_wrapper_count_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_pt2pt_ring_inplace_count_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_bytes_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_bytes_recv);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_count_send);
MPIR_T_PVAR_ULONG2_COUNTER_DECL_EXTERN(MV2, mv2_coll_allreduce_count_recv);


int (*MV2_Allreduce_function)(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)=NULL;


int (*MV2_Allreduce_intra_function)(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)=NULL;

static int (*MPIR_Rank_list_mapper)(MPID_Comm *, int)=NULL;

#define RING_ALLREDUCE_FALLBACK(_sendbuf,_recvbuf,_count,_datatype,_op,_comm_ptr,_errflag,_nbytes,_comm_size)       \
    do {                                                                                                            \
    if (64*1024 <= _nbytes/_comm_size) {                                                                            \
        mpi_errno = MPIR_Allreduce_pt2pt_ring_wrapper_MV2(_sendbuf, _recvbuf,                                       \
                _count, _datatype, _op, _comm_ptr, _errflag);                                                       \
    } else {                                                                                                        \
        mpi_errno = MPIR_Allreduce_pt2pt_rs_MV2(_sendbuf, _recvbuf, _count,                                         \
                _datatype, _op, _comm_ptr, _errflag);                                                               \
    }                                                                                                               \
    } while(0);


#define ALLREDUCE_SKIP_SMALL_MESSAGE_TUNING_TABLES                                              \
do {                                                                                            \
    /* skip the tables for small messages using topo aware collectives  */                      \
    if(nbytes <= mv2_topo_aware_allreduce_max_msg && nbytes >=                                  \
            mv2_topo_aware_allreduce_min_msg &&                                                 \
            mv2_enable_topo_aware_collectives &&                                                \
            mv2_use_topo_aware_allreduce &&                                                     \
            comm_ptr->dev.ch.topo_coll_ok == 1 && is_commutative)                               \
    {                                                                                           \
        if (local_size < mv2_topo_aware_allreduce_ppn_threshold) {                              \
            goto use_tables;                                                                    \
        }                                                                                       \
        mpi_errno = MPIR_Allreduce_topo_aware_hierarchical_MV2(sendbuf,                         \
                recvbuf, count, datatype, op, comm_ptr, errflag);                               \
        return mpi_errno;                                                                       \
    }                                                                                           \
    /* skip the tables for small messages using sock aware collectives  */                      \
    if(nbytes <= mv2_socket_aware_allreduce_max_msg && nbytes >=                                \
            mv2_socket_aware_allreduce_min_msg &&                                               \
            mv2_enable_socket_aware_collectives &&                                              \
            mv2_use_socket_aware_allreduce &&                                                   \
            comm_ptr->dev.ch.use_intra_sock_comm == 1 && is_commutative)                        \
    {                                                                                           \
        if (local_size < mv2_socket_aware_allreduce_ppn_threshold) {                            \
            goto use_tables;                                                                    \
        }                                                                                       \
        mpi_errno = MPIR_Allreduce_socket_aware_two_level_MV2(sendbuf,                          \
                recvbuf, count, datatype, op, comm_ptr, errflag);                               \
        return mpi_errno;                                                                       \
    }                                                                                           \
                                                                                                \
    /* skip the tables for small messages using single leader shmem collectives */              \
    if (likely(mv2_enable_shmem_allreduce &&                                                    \
                mv2_enable_skip_tuning_table_search && (nbytes <=                               \
                    mv2_coll_skip_table_threshold))) {                                          \
        /* for small messages, force Shmem + RD */                                              \
        MV2_Allreduce_intra_function = MPIR_Allreduce_reduce_shmem_MV2;                         \
        MV2_Allreduce_function = MPIR_Allreduce_pt2pt_rd_MV2;                                   \
        is_two_level = 1;                                                                       \
        goto skip_tuning_tables;                                                                \
    }                                                                                           \
                                                                                                \
} while(0);


#define ALLREDUCE_SKIP_LARGE_MESSAGE_TUNING_TABLES                                              \
do {                                                                                            \
    /* skip the tables for large messages with low PPN using ring collective */                 \
    if (mv2_allred_use_ring == 1 && mv2_allreduce_ring_algo_threshold <=                        \
            nbytes && local_size <= mv2_allreduce_ring_algo_ppn_threshold) {                    \
        /* flat ring-based Allreduce */                                                         \
        return MPIR_Allreduce_pt2pt_ring_wrapper_MV2(sendbuf, recvbuf,                          \
                count, datatype, op, comm_ptr, errflag);                                        \
    }                                                                                           \
                                                                                                \
                                                                                                \
    /* skip the tables for large messages with high PPN using ring-based
     * reduce_scatter followed by Allgather  collective */                                      \
    if ((comm_ptr->dev.ch.allgather_comm_ok != 0 &&                                             \
                comm_ptr->dev.ch.is_blocked == 0 &&                                             \
                mv2_allreduce_cyclic_algo_threshold <= nbytes) ||                               \
            (mv2_allreduce_red_scat_allgather_algo_threshold <= nbytes                          \
             && local_size >                                                                    \
             mv2_allreduce_red_scat_allgather_algo_ppn_threshold)) {                            \
                                                                                                \
        /* for large messages or cyclic hostfiles for medium messages, use
         * red-scat-allgather algorithm  */                                                     \
        return MPIR_Allreduce_pt2pt_reduce_scatter_allgather_MV2(sendbuf, recvbuf, count,       \
                datatype, op, comm_ptr, errflag);                                               \
    }                                                                                           \
} while(0);


#if defined (_SHARP_SUPPORT_)
#undef FCNAME
#define FCNAME "MPIR_Sharp_Allreduce_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Sharp_Allreduce_MV2 (const void *sendbuf, void *recvbuf, int count,
                        MPI_Datatype datatype, MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,sharp);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_sharp, 1);

    int mpi_errno = MPI_SUCCESS;
    int is_contig = 0;
    int is_inplace = 0;
    struct sharp_coll_reduce_spec reduce_spec;
    struct sharp_reduce_datatyepe_size * dt_size = NULL;
    struct sharp_coll_comm * sharp_comm = NULL;
    MPID_Datatype * dtp = NULL;

    if (HANDLE_GET_KIND(datatype) == HANDLE_KIND_BUILTIN) {
        is_contig = 1;
    } else {
        MPID_Datatype_get_ptr(datatype, dtp);
        is_contig = dtp->is_contig;
    }

    mv2_get_sharp_datatype(datatype, &dt_size);
    reduce_spec.dtype = dt_size->sharp_data_type;
    if (reduce_spec.dtype == SHARP_DTYPE_NULL) {
        mpi_errno = SHARP_COLL_ENOT_SUPP;
        goto fn_fail;
    }

    reduce_spec.op = mv2_get_sharp_reduce_op(op);
    if (reduce_spec.op == SHARP_OP_NULL) {
        mpi_errno = SHARP_COLL_ENOT_SUPP;
        goto fn_fail;
    }

    if (is_contig == 1) {
        reduce_spec.sbuf_desc.buffer.length = count * dt_size->size;
        if (sendbuf != MPI_IN_PLACE) {
            reduce_spec.sbuf_desc.buffer.ptr    = (void *)sendbuf;
        } else {
            is_inplace = 1;
            reduce_spec.sbuf_desc.buffer.ptr    = MPIU_Malloc(reduce_spec.sbuf_desc.buffer.length);
            MPIU_Memcpy(reduce_spec.sbuf_desc.buffer.ptr, recvbuf, reduce_spec.sbuf_desc.buffer.length);
        }
        reduce_spec.sbuf_desc.type              = SHARP_DATA_BUFFER;
        reduce_spec.sbuf_desc.mem_type          = SHARP_MEM_TYPE_HOST;
        reduce_spec.sbuf_desc.buffer.mem_handle = NULL;
        reduce_spec.rbuf_desc.buffer.ptr    = recvbuf;
        reduce_spec.rbuf_desc.buffer.length = count * dt_size->size;
        reduce_spec.rbuf_desc.type          = SHARP_DATA_BUFFER;
        reduce_spec.rbuf_desc.mem_type      = SHARP_MEM_TYPE_HOST;
        reduce_spec.aggr_mode               = SHARP_AGGREGATION_NONE;
        reduce_spec.rbuf_desc.buffer.mem_handle = NULL;    
    } else {
        /* NOT implemented in Sharp */
        mpi_errno = SHARP_COLL_ENOT_SUPP;
        goto fn_fail;
    }

    reduce_spec.length = count;     
    sharp_comm = ((sharp_info_t *)comm_ptr->dev.ch.sharp_coll_info)->sharp_comm_module->sharp_coll_comm;

    /* Ensure that all messages in non-sharp channels are progressed first
     * to prevent deadlocks in subsequent blocking sharp API calls */
    while (rdma_global_ext_sendq_size) {
        MPIDI_CH3_Progress_test();
    }

    mpi_errno = sharp_ops.coll_do_allreduce(sharp_comm, &reduce_spec);
    if (mpi_errno != SHARP_COLL_SUCCESS) {
        goto fn_fail;
    }
    
    mpi_errno = MPI_SUCCESS;

fn_exit:
    MPIU_Free(dt_size);
    if (is_inplace)
        MPIU_Free(reduce_spec.sbuf_desc.buffer.ptr);

    MPIR_TIMER_END(coll,allreduce,sharp);  
    return (mpi_errno);

fn_fail:
    PRINT_DEBUG(DEBUG_Sharp_verbose, "Continue without SHArP: %s \n", sharp_ops.coll_strerror(mpi_errno));
    mpi_errno = MPI_ERR_INTERN;
    goto fn_exit;
}

#endif /* end of defined (_SHARP_SUPPORT_) */


/* This is the default implementation of allreduce. The algorithm is:
   
   Algorithm: MPI_Allreduce

   For the heterogeneous case, we call MPI_Reduce followed by MPI_Bcast
   in order to meet the requirement that all processes must have the
   same result. For the homogeneous case, we use the following algorithms.

   For long messages and for builtin ops and if count >= pof2 (where
   pof2 is the nearest power-of-two less than or equal to the number
   of processes), we use Rabenseifner's algorithm (see 
   http://www.hlrs.de/organization/par/services/models/mpi/myreduce.html ).
   This algorithm implements the allreduce in two steps: first a
   reduce-scatter, followed by an allgather. A recursive-halving
   algorithm (beginning with processes that are distance 1 apart) is
   used for the reduce-scatter, and a recursive doubling 
   algorithm is used for the allgather. The non-power-of-two case is
   handled by dropping to the nearest lower power-of-two: the first
   few even-numbered processes send their data to their right neighbors
   (rank+1), and the reduce-scatter and allgather happen among the remaining
   power-of-two processes. At the end, the first few even-numbered
   processes get the result from their right neighbors.

   For the power-of-two case, the cost for the reduce-scatter is 
   lgp.alpha + n.((p-1)/p).beta + n.((p-1)/p).gamma. The cost for the
   allgather lgp.alpha + n.((p-1)/p).beta. Therefore, the
   total cost is:
   Cost = 2.lgp.alpha + 2.n.((p-1)/p).beta + n.((p-1)/p).gamma

   For the non-power-of-two case, 
   Cost = (2.floor(lgp)+2).alpha + (2.((p-1)/p) + 2).n.beta + n.(1+(p-1)/p).gamma

   
   For short messages, for user-defined ops, and for count < pof2 
   we use a recursive doubling algorithm (similar to the one in
   MPI_Allgather). We use this algorithm in the case of user-defined ops
   because in this case derived datatypes are allowed, and the user
   could pass basic datatypes on one process and derived on another as
   long as the type maps are the same. Breaking up derived datatypes
   to do the reduce-scatter is tricky. 

   Cost = lgp.alpha + n.lgp.beta + n.lgp.gamma

   Possible improvements: 

   End Algorithm: MPI_Allreduce
*/

int MPIR_Allreduce_mcst_reduce_two_level_helper_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{ 
    return 0;
}

int MPIR_Allreduce_mcst_reduce_redscat_gather_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    return 0;
}

/* not declared static because a machine-specific function may call this one 
   in some cases */
/* This is flat p2p recursive-doubling allreduce */
#undef FCNAME
#define FCNAME "MPIR_Allreduce_pt2pt_rd_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_pt2pt_rd_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,shm_rd);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_shm_rd, 1);
    int comm_size, rank;
    int mpi_errno = MPI_SUCCESS;
    int mpi_errno_ret = MPI_SUCCESS;
    int mask, dst, is_commutative, pof2, newrank = 0, rem, newdst;
    MPI_Aint true_lb, true_extent, extent;
    void *tmp_buf;
    MPI_User_function *uop;
    MPID_Op *op_ptr;
    MPIU_THREADPRIV_DECL;
#ifdef HAVE_CXX_BINDING
    int is_cxx_uop = 0;
#endif
    MPIU_CHKLMEM_DECL(3);

    if (count == 0) {
        MPIR_TIMER_END(coll,allreduce,shm_rd);
        return MPI_SUCCESS;
    }

    MPIU_THREADPRIV_GET;

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

    /* homogeneous */

    /* set op_errno to 0. stored in perthread structure */
    MPIU_THREADPRIV_FIELD(op_errno) = 0;

    comm_size = comm_ptr->local_size;
    rank = comm_ptr->rank;

    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
        /* get the function by indexing into the op table */
        uop = MPIR_Op_table[op % 16 - 1];
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }
#ifdef HAVE_CXX_BINDING
        if (op_ptr->language == MPID_LANG_CXX) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
            is_cxx_uop = 1;
        } else {
#endif
           if (op_ptr->language == MPID_LANG_C) {
               uop = (MPI_User_function *) op_ptr->function.c_function;
           } else {
               uop = (MPI_User_function *) op_ptr->function.f77_function;
           }
#ifdef HAVE_CXX_BINDING
        }
#endif
    }

    /* need to allocate temporary buffer to store incoming data */
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);

    if ((comm_ptr->dev.ch.shmem_coll_ok == 1) &&
        ((count * (MPIR_MAX(extent, true_extent)) <  mv2_coll_tmp_buf_size))) {
        tmp_buf = (void *) ((char *)comm_ptr->dev.ch.coll_tmp_buf);
    } else {
        MPIU_CHKLMEM_MALLOC(tmp_buf, void *, count * (MPIR_MAX(extent, true_extent)),
                            mpi_errno, "temporary buffer");
    }
    /* adjust for potential negative lower bound in datatype */
    tmp_buf = (void *) ((char *) tmp_buf - true_lb);

    /* copy local data into recvbuf */
    if (sendbuf != MPI_IN_PLACE) {
        mpi_errno =
            MPIR_Localcopy(sendbuf, count, datatype, recvbuf, count,
                           datatype);
        MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                            "**fail");
    }

    /* find nearest power-of-two less than or equal to comm_size */
    pof2 = comm_ptr->dev.ch.gpof2;

    rem = comm_size - pof2;

    /* In the non-power-of-two case, all even-numbered
       processes of rank < 2*rem send their data to
       (rank+1). These even-numbered processes no longer
       participate in the algorithm until the very end. The
       remaining processes form a nice power-of-two. */

    if (rank < 2 * rem) {
        if (rank % 2 == 0) {
            /* even */
            MPIR_PVAR_INC(allreduce, pt2pt_rd, send, count, datatype);
            mpi_errno = MPIC_Send(recvbuf, count, datatype, rank + 1,
                                     MPIR_ALLREDUCE_TAG, comm_ptr, errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }

            /* temporarily set the rank to -1 so that this
               process does not pariticipate in recursive
               doubling */
            newrank = -1;
        } else {
            /* odd */
            MPIR_PVAR_INC(allreduce, pt2pt_rd, recv, count, datatype);
            mpi_errno = MPIC_Recv(tmp_buf, count, datatype, rank - 1,
                                     MPIR_ALLREDUCE_TAG, comm_ptr,
                                     MPI_STATUS_IGNORE, errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }

            /* do the reduction on received data. since the
               ordering is right, it doesn't matter whether
               the operation is commutative or not. */
#ifdef HAVE_CXX_BINDING
            if (is_cxx_uop) {
                (*MPIR_Process.cxx_call_op_fn) (tmp_buf, recvbuf, count,
                                                datatype, uop);
            } else {
#endif
               (*uop) (tmp_buf, recvbuf, &count, &datatype);

               /* change the rank */
               newrank = rank / 2;
#ifdef HAVE_CXX_BINDING
            }
#endif
        }
    } else {                /* rank >= 2*rem */
        newrank = rank - rem;
    }

    /* If op is user-defined or count is less than pof2, use
       recursive doubling algorithm. Otherwise do a reduce-scatter
       followed by allgather. (If op is user-defined,
       derived datatypes are allowed and the user could pass basic
       datatypes on one process and derived on another as long as
       the type maps are the same. Breaking up derived
       datatypes to do the reduce-scatter is tricky, therefore
       using recursive doubling in that case.) */

    if (newrank != -1) {
        mask = 0x1;
        while (mask < pof2) {
            newdst = newrank ^ mask;
            /* find real rank of dest */
            dst = (newdst < rem) ? newdst * 2 + 1 : newdst + rem;

            /* Send the most current data, which is in recvbuf. Recv
               into tmp_buf */
            MPIR_PVAR_INC(allreduce, pt2pt_rd, send, count, datatype);
            MPIR_PVAR_INC(allreduce, pt2pt_rd, recv, count, datatype);
            mpi_errno = MPIC_Sendrecv(recvbuf, count, datatype,
                                         dst, MPIR_ALLREDUCE_TAG,
                                         tmp_buf, count, datatype, dst,
                                         MPIR_ALLREDUCE_TAG, comm_ptr,
                                         MPI_STATUS_IGNORE, errflag);

            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }

            /* tmp_buf contains data received in this step.
               recvbuf contains data accumulated so far */

            if (is_commutative || (dst < rank)) {
                /* op is commutative OR the order is already right */
#ifdef HAVE_CXX_BINDING
                if (is_cxx_uop) {
                    (*MPIR_Process.cxx_call_op_fn) (tmp_buf, recvbuf,
                                                    count, datatype,
                                                    uop);
                } else {
#endif
                    (*uop) (tmp_buf, recvbuf, &count, &datatype);
#ifdef HAVE_CXX_BINDING
                }
#endif
            } else {
                /* op is noncommutative and the order is not right */
#ifdef HAVE_CXX_BINDING
                if (is_cxx_uop) {
                    (*MPIR_Process.cxx_call_op_fn) (recvbuf, tmp_buf,
                                                    count, datatype,
                                                    uop);
                } else {
#endif
                    (*uop) (recvbuf, tmp_buf, &count, &datatype);
#ifdef HAVE_CXX_BINDING
                }
#endif
                    /* copy result back into recvbuf */
                    mpi_errno = MPIR_Localcopy(tmp_buf, count, datatype,
                                                   recvbuf, count, datatype);
                    MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno,
                                            MPI_ERR_OTHER, "**fail");
                }
                    mask <<= 1;
            }
        }

    /* In the non-power-of-two case, all odd-numbered
       processes of rank < 2*rem send the result to
       (rank-1), the ranks who didn't participate above. */
    if (rank < 2 * rem) {
        if (rank % 2) {     /* odd */
            MPIR_PVAR_INC(allreduce, pt2pt_rd, send, count, datatype);
            mpi_errno = MPIC_Send(recvbuf, count,
                                     datatype, rank - 1,
                                     MPIR_ALLREDUCE_TAG, comm_ptr, errflag);
        } else {            /* even */
            MPIR_PVAR_INC(allreduce, pt2pt_rd, recv, count, datatype);
            mpi_errno = MPIC_Recv(recvbuf, count,
                                  datatype, rank + 1,
                                  MPIR_ALLREDUCE_TAG, comm_ptr,
                                  MPI_STATUS_IGNORE, errflag);
        }
        if (mpi_errno) {
            /* for communication errors, just record the error but continue */
            *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }
    }

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

  fn_exit:
    MPIU_CHKLMEM_FREEALL();
    MPIR_TIMER_END(coll,allreduce,shm_rd);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;


}

/* not declared static because a machine-specific function may call this one 
   in some cases */
/* This is flat reduce-scatter-allgather allreduce */
#undef FCNAME
#define FCNAME "MPIR_Allreduce_pt2pt_rs_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_pt2pt_rs_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,shm_rs);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_shm_rs, 1);
    int comm_size, rank;
    int mpi_errno = MPI_SUCCESS;
    int mpi_errno_ret = MPI_SUCCESS;
    int mask, dst, is_commutative, pof2, newrank = 0, rem, newdst, i,
        send_idx, recv_idx, last_idx, send_cnt, recv_cnt, *cnts, *disps;
    MPI_Aint true_lb, true_extent, extent;
    void *tmp_buf;
    MPI_User_function *uop;
    MPID_Op *op_ptr;
    MPIU_THREADPRIV_DECL;
#ifdef HAVE_CXX_BINDING
    int is_cxx_uop = 0;
#endif
    MPIU_CHKLMEM_DECL(3);

    if (count == 0) {
        MPIR_TIMER_END(coll,allreduce,shm_rs);
        return MPI_SUCCESS;
    }

    MPIU_THREADPRIV_GET;

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

    /* homogeneous */

    /* set op_errno to 0. stored in perthread structure */
    MPIU_THREADPRIV_FIELD(op_errno) = 0;

    comm_size = comm_ptr->local_size;

    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
        /* get the function by indexing into the op table */
        uop = MPIR_Op_table[op % 16 - 1];
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }
#ifdef HAVE_CXX_BINDING
        if (op_ptr->language == MPID_LANG_CXX) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
            is_cxx_uop = 1;
        } else {
#endif
            if (op_ptr->language == MPID_LANG_C) {
                uop = (MPI_User_function *) op_ptr->function.c_function;
            } else {
                uop = (MPI_User_function *) op_ptr->function.f77_function;
            }
#ifdef HAVE_CXX_BINDING
        }
#endif
    }

    if (comm_ptr->dev.ch.rank_list == NULL || is_commutative != 1) {
        rank = comm_ptr->rank;
        MPIR_Rank_list_mapper = &Bunch_Rank_list_mapper;
    } else {
        /* my index on rank_list */
        rank = comm_ptr->dev.ch.rank_list_index;
        MPIU_Assert(rank >= 0);
        MPIR_Rank_list_mapper = &Cyclic_Rank_list_mapper;
    }

    /* need to allocate temporary buffer to store incoming data */
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);

    MPIU_CHKLMEM_MALLOC(tmp_buf, void *,
                        count * (MPIR_MAX(extent, true_extent)), mpi_errno,
                        "temporary buffer");

    /* adjust for potential negative lower bound in datatype */
    tmp_buf = (void *) ((char *) tmp_buf - true_lb);

    /* copy local data into recvbuf */
    if (sendbuf != MPI_IN_PLACE) {
        mpi_errno =
            MPIR_Localcopy(sendbuf, count, datatype, recvbuf, count,
                           datatype);
        MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                            "**fail");
    }

    /* find nearest power-of-two less than or equal to comm_size */
    pof2 = comm_ptr->dev.ch.gpof2;

    rem = comm_size - pof2;

    /* In the non-power-of-two case, all even-numbered
       processes of rank < 2*rem send their data to
       (rank+1). These even-numbered processes no longer
       participate in the algorithm until the very end. The
       remaining processes form a nice power-of-two. */

    if (rank < 2 * rem) {
        if (rank % 2 == 0) {
            /* even */
            MPIR_PVAR_INC(allreduce, pt2pt_rs, send, count, datatype);
            mpi_errno = MPIC_Send(recvbuf, count, datatype, MPIR_Rank_list_mapper(comm_ptr, rank + 1),
                                     MPIR_ALLREDUCE_TAG, comm_ptr, errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }

            /* temporarily set the rank to -1 so that this
               process does not pariticipate in recursive
               doubling */
            newrank = -1;
        } else {
            /* odd */
            MPIR_PVAR_INC(allreduce, pt2pt_rs, recv, count, datatype);
            mpi_errno = MPIC_Recv(tmp_buf, count, datatype, MPIR_Rank_list_mapper(comm_ptr, rank - 1),
                                     MPIR_ALLREDUCE_TAG, comm_ptr,
                                     MPI_STATUS_IGNORE, errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }

            /* do the reduction on received data. since the
               ordering is right, it doesn't matter whether
               the operation is commutative or not. */
#ifdef HAVE_CXX_BINDING
            if (is_cxx_uop) {
                (*MPIR_Process.cxx_call_op_fn) (tmp_buf, recvbuf, count,
                                                datatype, uop);
            } else {
#endif
                (*uop) (tmp_buf, recvbuf, &count, &datatype);

                /* change the rank */
                newrank = rank / 2;
#ifdef HAVE_CXX_BINDING
            }
#endif
        }
    } else {                /* rank >= 2*rem */
        newrank = rank - rem;
    }

    /* If op is user-defined or count is less than pof2, use
       recursive doubling algorithm. Otherwise do a reduce-scatter
       followed by allgather. (If op is user-defined,
       derived datatypes are allowed and the user could pass basic
       datatypes on one process and derived on another as long as
       the type maps are the same. Breaking up derived
       datatypes to do the reduce-scatter is tricky, therefore
       using recursive doubling in that case.) */

    if (newrank != -1) {
        if ((HANDLE_GET_KIND(op) != HANDLE_KIND_BUILTIN) || (count < pof2)) {  /* use recursive doubling */
            mask = 0x1;
            while (mask < pof2) {
                newdst = newrank ^ mask;
                /* find real rank of dest */
                dst = (newdst < rem) ? newdst * 2 + 1 : newdst + rem;

                /* Send the most current data, which is in recvbuf. Recv
                   into tmp_buf */
                MPIR_PVAR_INC(allreduce, pt2pt_rs, send, count, datatype);
                MPIR_PVAR_INC(allreduce, pt2pt_rs, recv, count, datatype);
                mpi_errno = MPIC_Sendrecv(recvbuf, count, datatype,
                                             MPIR_Rank_list_mapper(comm_ptr, dst), MPIR_ALLREDUCE_TAG,
                                             tmp_buf, count, datatype, MPIR_Rank_list_mapper(comm_ptr, dst),
                                             MPIR_ALLREDUCE_TAG, comm_ptr,
                                             MPI_STATUS_IGNORE, errflag);

                if (mpi_errno) {
                    /* for communication errors, just record the error but continue */
                    *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                    MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                    MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                }

                /* tmp_buf contains data received in this step.
                   recvbuf contains data accumulated so far */

                if (is_commutative || (dst < rank)) {
                    /* op is commutative OR the order is already right */
#ifdef HAVE_CXX_BINDING
                    if (is_cxx_uop) {
                        (*MPIR_Process.cxx_call_op_fn) (tmp_buf, recvbuf,
                                                        count, datatype,
                                                        uop);
                    } else {
#endif
                        (*uop) (tmp_buf, recvbuf, &count, &datatype);
#ifdef HAVE_CXX_BINDING
                    }
#endif
                } else {
                    /* op is noncommutative and the order is not right */
#ifdef HAVE_CXX_BINDING
                    if (is_cxx_uop) {
                        (*MPIR_Process.cxx_call_op_fn) (recvbuf, tmp_buf,
                                                        count, datatype,
                                                        uop);
                    } else {
#endif
                        (*uop) (recvbuf, tmp_buf, &count, &datatype);
#ifdef HAVE_CXX_BINDING
                    }
#endif
                    /* copy result back into recvbuf */
                    mpi_errno = MPIR_Localcopy(tmp_buf, count, datatype,
                                               recvbuf, count, datatype);
                    MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno,
                                        MPI_ERR_OTHER, "**fail");
                }
                mask <<= 1;
            }
        } else {

            /* do a reduce-scatter followed by allgather */

            /* for the reduce-scatter, calculate the count that
               each process receives and the displacement within
               the buffer */

            MPIU_CHKLMEM_MALLOC(cnts, int *, pof2 * sizeof (int), mpi_errno,
                                "counts");
            MPIU_CHKLMEM_MALLOC(disps, int *, pof2 * sizeof (int),
                                mpi_errno, "displacements");

            for (i = 0; i < (pof2 - 1); i++) {
                cnts[i] = count / pof2;
            }
            cnts[pof2 - 1] = count - (count / pof2) * (pof2 - 1);

            disps[0] = 0;
            for (i = 1; i < pof2; i++) {
                disps[i] = disps[i - 1] + cnts[i - 1];
            }

            mask = 0x1;
            send_idx = recv_idx = 0;
            last_idx = pof2;
            while (mask < pof2) {
                newdst = newrank ^ mask;
                /* find real rank of dest */
                dst = (newdst < rem) ? newdst * 2 + 1 : newdst + rem;

                send_cnt = recv_cnt = 0;
                if (newrank < newdst) {
                    send_idx = recv_idx + pof2 / (mask * 2);
                    for (i = send_idx; i < last_idx; i++)
                        send_cnt += cnts[i];
                    for (i = recv_idx; i < send_idx; i++)
                        recv_cnt += cnts[i];
                } else {
                    recv_idx = send_idx + pof2 / (mask * 2);
                    for (i = send_idx; i < recv_idx; i++)
                        send_cnt += cnts[i];
                    for (i = recv_idx; i < last_idx; i++)
                        recv_cnt += cnts[i];
                }

                /* Send data from recvbuf. Recv into tmp_buf */
                MPIR_PVAR_INC(allreduce, pt2pt_rs, send, send_cnt, datatype);
                MPIR_PVAR_INC(allreduce, pt2pt_rs, recv, recv_cnt, datatype);
                mpi_errno = MPIC_Sendrecv((char *) recvbuf +
                                             disps[send_idx] * extent,
                                             send_cnt, datatype,
                                             MPIR_Rank_list_mapper(comm_ptr, dst), MPIR_ALLREDUCE_TAG,
                                             (char *) tmp_buf +
                                             disps[recv_idx] * extent,
                                             recv_cnt, datatype, MPIR_Rank_list_mapper(comm_ptr, dst),
                                             MPIR_ALLREDUCE_TAG, comm_ptr,
                                             MPI_STATUS_IGNORE, errflag);
                if (mpi_errno) {
                    /* for communication errors, just record the error but continue */
                    *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                    MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                    MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                }

                /* tmp_buf contains data received in this step.
                   recvbuf contains data accumulated so far */

                /* This algorithm is used only for predefined ops
                   and predefined ops are always commutative. */

                (*uop) ((char *) tmp_buf + disps[recv_idx] * extent,
                        (char *) recvbuf + disps[recv_idx] * extent,
                        &recv_cnt, &datatype);

                /* update send_idx for next iteration */
                send_idx = recv_idx;
                mask <<= 1;

                /* update last_idx, but not in last iteration
                   because the value is needed in the allgather
                   step below. */
                if (mask < pof2)
                    last_idx = recv_idx + pof2 / mask;
            }

            /* now do the allgather */

            mask >>= 1;
            while (mask > 0) {
                newdst = newrank ^ mask;
                /* find real rank of dest */
                dst = (newdst < rem) ? newdst * 2 + 1 : newdst + rem;

                send_cnt = recv_cnt = 0;
                if (newrank < newdst) {
                    /* update last_idx except on first iteration */
                    if (mask != pof2 / 2) {
                        last_idx = last_idx + pof2 / (mask * 2);
                    }

                    recv_idx = send_idx + pof2 / (mask * 2);
                    for (i = send_idx; i < recv_idx; i++) {
                        send_cnt += cnts[i];
                    }
                    for (i = recv_idx; i < last_idx; i++) {
                        recv_cnt += cnts[i];
                    }
                } else {
                    recv_idx = send_idx - pof2 / (mask * 2);
                    for (i = send_idx; i < last_idx; i++) {
                        send_cnt += cnts[i];
                    }
                    for (i = recv_idx; i < send_idx; i++) {
                        recv_cnt += cnts[i];
                    }
                }
                MPIR_PVAR_INC(allreduce, pt2pt_rs, send, send_cnt, datatype);
                MPIR_PVAR_INC(allreduce, pt2pt_rs, recv, recv_cnt, datatype);
                mpi_errno = MPIC_Sendrecv((char *) recvbuf +
                                             disps[send_idx] * extent,
                                             send_cnt, datatype,
                                             MPIR_Rank_list_mapper(comm_ptr, dst), MPIR_ALLREDUCE_TAG,
                                             (char *) recvbuf +
                                             disps[recv_idx] * extent,
                                             recv_cnt, datatype, MPIR_Rank_list_mapper(comm_ptr, dst),
                                             MPIR_ALLREDUCE_TAG, comm_ptr,
                                             MPI_STATUS_IGNORE, errflag);
                if (mpi_errno) {
                    /* for communication errors, just record the error but continue */
                    *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                    MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                    MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                }

                if (newrank > newdst) {
                    send_idx = recv_idx;
                }

                mask >>= 1;
            }
        }
    }

    /* In the non-power-of-two case, all odd-numbered
       processes of rank < 2*rem send the result to
       (rank-1), the ranks who didn't participate above. */
    if (rank < 2 * rem) {
        if (rank % 2) {     /* odd */
            MPIR_PVAR_INC(allreduce, pt2pt_rs, send, count, datatype);
            mpi_errno = MPIC_Send(recvbuf, count,
                                     datatype, MPIR_Rank_list_mapper(comm_ptr, rank - 1),
                                     MPIR_ALLREDUCE_TAG, comm_ptr, errflag);
        } else {            /* even */
            MPIR_PVAR_INC(allreduce, pt2pt_rs, recv, count, datatype);
            mpi_errno = MPIC_Recv(recvbuf, count,
                                  datatype, MPIR_Rank_list_mapper(comm_ptr, rank + 1),
                                  MPIR_ALLREDUCE_TAG, comm_ptr,
                                  MPI_STATUS_IGNORE, errflag);
        }
        if (mpi_errno) {
            /* for communication errors, just record the error but continue */
            *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }
    }

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

  fn_exit:
    MPIU_CHKLMEM_FREEALL();
    MPIR_TIMER_END(coll,allreduce,shm_rs);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}



/* not declared static because a machine-specific function may call this one 
   in some cases */
#undef FCNAME
#define FCNAME "MPIR_Allreduce_pt2pt_old_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_pt2pt_old_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
#ifdef MPID_HAS_HETERO
    int rc;
    int is_homogeneous = 1;
#endif
    int comm_size, rank;
    MPI_Aint type_size;
    int mpi_errno = MPI_SUCCESS;
    int mpi_errno_ret = MPI_SUCCESS;
    int mask, dst, is_commutative, pof2, newrank = 0, rem, newdst, i,
        send_idx, recv_idx, last_idx, send_cnt, recv_cnt, *cnts, *disps;
    MPI_Aint true_lb, true_extent, extent;
    void *tmp_buf;
    MPI_User_function *uop;
    MPID_Op *op_ptr;
    MPIU_THREADPRIV_DECL;
#ifdef HAVE_CXX_BINDING
    int is_cxx_uop = 0;
#endif
    MPIU_CHKLMEM_DECL(3);

    if (count == 0) {
        return MPI_SUCCESS;
    }

    MPIU_THREADPRIV_GET;

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

#ifdef MPID_HAS_HETERO
    if (comm_ptr->is_hetero) {
        is_homogeneous = 0;
    }

    if (!is_homogeneous) {
        /* heterogeneous. To get the same result on all processes, we
           do a reduce to 0 and then broadcast. */
        mpi_errno = MPIR_Reduce_MV2(sendbuf, recvbuf, count, datatype,
                                    op, 0, comm_ptr, errflag);
        /* 
           FIXME: mpi_errno is error CODE, not necessarily the error
           class MPI_ERR_OP.  In MPICH2, we can get the error class 
           with errorclass = mpi_errno & ERROR_CLASS_MASK;
         */
        if (mpi_errno == MPI_ERR_OP || mpi_errno == MPI_SUCCESS) {
            /* Allow MPI_ERR_OP since we can continue from this error */
            rc = MPIR_Bcast_impl(recvbuf, count, datatype, 0, comm_ptr,
                                 errflag);
            if (rc)
                mpi_errno = rc;
        }
    } else
#endif                          /* MPID_HAS_HETERO */
    {
        /* homogeneous */

        /* set op_errno to 0. stored in perthread structure */
        MPIU_THREADPRIV_FIELD(op_errno) = 0;

        comm_size = comm_ptr->local_size;
        rank = comm_ptr->rank;

        if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
            is_commutative = 1;
            /* get the function by indexing into the op table */
            uop = MPIR_Op_table[op % 16 - 1];
        } else {
            MPID_Op_get_ptr(op, op_ptr);
            if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
                is_commutative = 0;
            } else {
                is_commutative = 1;
            }
#ifdef HAVE_CXX_BINDING
            if (op_ptr->language == MPID_LANG_CXX) {
                uop = (MPI_User_function *) op_ptr->function.c_function;
                is_cxx_uop = 1;
            } else {
#endif
                if (op_ptr->language == MPID_LANG_C) {
                    uop = (MPI_User_function *) op_ptr->function.c_function;
                } else {
                    uop = (MPI_User_function *) op_ptr->function.f77_function;
                }
#ifdef HAVE_CXX_BINDING
            }
#endif
        }

        /* need to allocate temporary buffer to store incoming data */
        MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
        MPID_Datatype_get_extent_macro(datatype, extent);

        MPIU_CHKLMEM_MALLOC(tmp_buf, void *,
                            count * (MPIR_MAX(extent, true_extent)), mpi_errno,
                            "temporary buffer");

        /* adjust for potential negative lower bound in datatype */
        tmp_buf = (void *) ((char *) tmp_buf - true_lb);

        /* copy local data into recvbuf */
        if (sendbuf != MPI_IN_PLACE) {
            mpi_errno =
                MPIR_Localcopy(sendbuf, count, datatype, recvbuf, count,
                               datatype);
            MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                "**fail");
        }

        MPID_Datatype_get_size_macro(datatype, type_size);

        /* find nearest power-of-two less than or equal to comm_size */
        pof2 = comm_ptr->dev.ch.gpof2;

        rem = comm_size - pof2;

        /* In the non-power-of-two case, all even-numbered
           processes of rank < 2*rem send their data to
           (rank+1). These even-numbered processes no longer
           participate in the algorithm until the very end. The
           remaining processes form a nice power-of-two. */

        if (rank < 2 * rem) {
            if (rank % 2 == 0) {
                /* even */
                mpi_errno = MPIC_Send(recvbuf, count, datatype, rank + 1,
                                         MPIR_ALLREDUCE_TAG, comm_ptr, errflag);
                if (mpi_errno) {
                    /* for communication errors, just record the error but continue */
                    *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                    MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                    MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                }

                /* temporarily set the rank to -1 so that this
                   process does not pariticipate in recursive
                   doubling */
                newrank = -1;
            } else {
                /* odd */
                mpi_errno = MPIC_Recv(tmp_buf, count, datatype, rank - 1,
                                         MPIR_ALLREDUCE_TAG, comm_ptr,
                                         MPI_STATUS_IGNORE, errflag);
                if (mpi_errno) {
                    /* for communication errors, just record the error but continue */
                    *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                    MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                    MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                }

                /* do the reduction on received data. since the
                   ordering is right, it doesn't matter whether
                   the operation is commutative or not. */
#ifdef HAVE_CXX_BINDING
                if (is_cxx_uop) {
                    (*MPIR_Process.cxx_call_op_fn) (tmp_buf, recvbuf, count,
                                                    datatype, uop);
                } else {
#endif
                    (*uop) (tmp_buf, recvbuf, &count, &datatype);

                    /* change the rank */
                    newrank = rank / 2;
#ifdef HAVE_CXX_BINDING
                }
#endif
            }
        } else {                /* rank >= 2*rem */
            newrank = rank - rem;
        }

        /* If op is user-defined or count is less than pof2, use
           recursive doubling algorithm. Otherwise do a reduce-scatter
           followed by allgather. (If op is user-defined,
           derived datatypes are allowed and the user could pass basic
           datatypes on one process and derived on another as long as
           the type maps are the same. Breaking up derived
           datatypes to do the reduce-scatter is tricky, therefore
           using recursive doubling in that case.) */

        if (newrank != -1) {
            if ((count * type_size <= mv2_coll_param.allreduce_short_msg) || (HANDLE_GET_KIND(op) != HANDLE_KIND_BUILTIN) || (count < pof2)) {  /* use recursive doubling */
                mask = 0x1;
                while (mask < pof2) {
                    newdst = newrank ^ mask;
                    /* find real rank of dest */
                    dst = (newdst < rem) ? newdst * 2 + 1 : newdst + rem;

                    /* Send the most current data, which is in recvbuf. Recv
                       into tmp_buf */
                    mpi_errno = MPIC_Sendrecv(recvbuf, count, datatype,
                                                 dst, MPIR_ALLREDUCE_TAG,
                                                 tmp_buf, count, datatype, dst,
                                                 MPIR_ALLREDUCE_TAG, comm_ptr,
                                                 MPI_STATUS_IGNORE, errflag);

                    if (mpi_errno) {
                        /* for communication errors, just record the error but continue */
                        *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                        MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                        MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                    }

                    /* tmp_buf contains data received in this step.
                       recvbuf contains data accumulated so far */

                    if (is_commutative || (dst < rank)) {
                        /* op is commutative OR the order is already right */
#ifdef HAVE_CXX_BINDING
                        if (is_cxx_uop) {
                            (*MPIR_Process.cxx_call_op_fn) (tmp_buf, recvbuf,
                                                            count, datatype,
                                                            uop);
                        } else {
#endif
                            (*uop) (tmp_buf, recvbuf, &count, &datatype);
#ifdef HAVE_CXX_BINDING
                        }
#endif
                    } else {
                        /* op is noncommutative and the order is not right */
#ifdef HAVE_CXX_BINDING
                        if (is_cxx_uop) {
                            (*MPIR_Process.cxx_call_op_fn) (recvbuf, tmp_buf,
                                                            count, datatype,
                                                            uop);
                        } else {
#endif
                            (*uop) (recvbuf, tmp_buf, &count, &datatype);
#ifdef HAVE_CXX_BINDING
                        }
#endif
                        /* copy result back into recvbuf */
                        mpi_errno = MPIR_Localcopy(tmp_buf, count, datatype,
                                                   recvbuf, count, datatype);
                        MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno,
                                            MPI_ERR_OTHER, "**fail");
                    }
                    mask <<= 1;
                }
            } else {

                /* do a reduce-scatter followed by allgather */

                /* for the reduce-scatter, calculate the count that
                   each process receives and the displacement within
                   the buffer */

                MPIU_CHKLMEM_MALLOC(cnts, int *, pof2 * sizeof (int), mpi_errno,
                                    "counts");
                MPIU_CHKLMEM_MALLOC(disps, int *, pof2 * sizeof (int),
                                    mpi_errno, "displacements");

                for (i = 0; i < (pof2 - 1); i++) {
                    cnts[i] = count / pof2;
                }
                cnts[pof2 - 1] = count - (count / pof2) * (pof2 - 1);

                disps[0] = 0;
                for (i = 1; i < pof2; i++) {
                    disps[i] = disps[i - 1] + cnts[i - 1];
                }

                mask = 0x1;
                send_idx = recv_idx = 0;
                last_idx = pof2;
                while (mask < pof2) {
                    newdst = newrank ^ mask;
                    /* find real rank of dest */
                    dst = (newdst < rem) ? newdst * 2 + 1 : newdst + rem;

                    send_cnt = recv_cnt = 0;
                    if (newrank < newdst) {
                        send_idx = recv_idx + pof2 / (mask * 2);
                        for (i = send_idx; i < last_idx; i++)
                            send_cnt += cnts[i];
                        for (i = recv_idx; i < send_idx; i++)
                            recv_cnt += cnts[i];
                    } else {
                        recv_idx = send_idx + pof2 / (mask * 2);
                        for (i = send_idx; i < recv_idx; i++)
                            send_cnt += cnts[i];
                        for (i = recv_idx; i < last_idx; i++)
                            recv_cnt += cnts[i];
                    }

                    /* Send data from recvbuf. Recv into tmp_buf */
                    mpi_errno = MPIC_Sendrecv((char *) recvbuf +
                                                 disps[send_idx] * extent,
                                                 send_cnt, datatype,
                                                 dst, MPIR_ALLREDUCE_TAG,
                                                 (char *) tmp_buf +
                                                 disps[recv_idx] * extent,
                                                 recv_cnt, datatype, dst,
                                                 MPIR_ALLREDUCE_TAG, comm_ptr,
                                                 MPI_STATUS_IGNORE, errflag);
                    if (mpi_errno) {
                        /* for communication errors, just record the error but continue */
                        *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                        MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                        MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                    }

                    /* tmp_buf contains data received in this step.
                       recvbuf contains data accumulated so far */

                    /* This algorithm is used only for predefined ops
                       and predefined ops are always commutative. */

                    (*uop) ((char *) tmp_buf + disps[recv_idx] * extent,
                            (char *) recvbuf + disps[recv_idx] * extent,
                            &recv_cnt, &datatype);

                    /* update send_idx for next iteration */
                    send_idx = recv_idx;
                    mask <<= 1;

                    /* update last_idx, but not in last iteration
                       because the value is needed in the allgather
                       step below. */
                    if (mask < pof2)
                        last_idx = recv_idx + pof2 / mask;
                }

                /* now do the allgather */

                mask >>= 1;
                while (mask > 0) {
                    newdst = newrank ^ mask;
                    /* find real rank of dest */
                    dst = (newdst < rem) ? newdst * 2 + 1 : newdst + rem;

                    send_cnt = recv_cnt = 0;
                    if (newrank < newdst) {
                        /* update last_idx except on first iteration */
                        if (mask != pof2 / 2) {
                            last_idx = last_idx + pof2 / (mask * 2);
                        }

                        recv_idx = send_idx + pof2 / (mask * 2);
                        for (i = send_idx; i < recv_idx; i++) {
                            send_cnt += cnts[i];
                        }
                        for (i = recv_idx; i < last_idx; i++) {
                            recv_cnt += cnts[i];
                        }
                    } else {
                        recv_idx = send_idx - pof2 / (mask * 2);
                        for (i = send_idx; i < last_idx; i++) {
                            send_cnt += cnts[i];
                        }
                        for (i = recv_idx; i < send_idx; i++) {
                            recv_cnt += cnts[i];
                        }
                    }

                    mpi_errno = MPIC_Sendrecv((char *) recvbuf +
                                                 disps[send_idx] * extent,
                                                 send_cnt, datatype,
                                                 dst, MPIR_ALLREDUCE_TAG,
                                                 (char *) recvbuf +
                                                 disps[recv_idx] * extent,
                                                 recv_cnt, datatype, dst,
                                                 MPIR_ALLREDUCE_TAG, comm_ptr,
                                                 MPI_STATUS_IGNORE, errflag);
                    if (mpi_errno) {
                        /* for communication errors, just record the error but continue */
                        *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                        MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                        MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                    }

                    if (newrank > newdst) {
                        send_idx = recv_idx;
                    }

                    mask >>= 1;
                }
            }
        }

        /* In the non-power-of-two case, all odd-numbered
           processes of rank < 2*rem send the result to
           (rank-1), the ranks who didn't participate above. */
        if (rank < 2 * rem) {
            if (rank % 2) {     /* odd */
                mpi_errno = MPIC_Send(recvbuf, count,
                                         datatype, rank - 1,
                                         MPIR_ALLREDUCE_TAG, comm_ptr, errflag);
            } else {            /* even */

                mpi_errno = MPIC_Recv(recvbuf, count,
                                      datatype, rank + 1,
                                      MPIR_ALLREDUCE_TAG, comm_ptr,
                                      MPI_STATUS_IGNORE, errflag);
            }
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }

    }
    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

  fn_exit:
    MPIU_CHKLMEM_FREEALL();
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}

/* intra-node shm reduce as the first reduce in allreduce */
int MPIR_Allreduce_reduce_shmem_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,shm_intra);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_shm_intra, 1);
    int mpi_errno = MPI_SUCCESS;
    int i = 0, is_commutative = 0;
    MPI_Aint true_lb, true_extent, extent;
    MPI_User_function *uop;
    MPID_Op *op_ptr;
#ifdef HAVE_CXX_BINDING
    int is_cxx_uop = 0;
#endif
    char *shmem_buf = NULL;
    MPI_Comm shmem_comm = MPI_COMM_NULL;
    MPID_Comm *shmem_commptr = NULL;
    int local_rank = -1, local_size = 0;
    void *local_buf = NULL;
    int stride = 0;
    is_commutative = 0;
    int shmem_comm_rank;

    if (count == 0) {
        MPIR_TIMER_END(coll,allreduce,shm_intra);
        return MPI_SUCCESS;
    }

    
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_num_shmem_coll_calls, 1);
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);
    stride = count * MPIR_MAX(extent, true_extent);
 
    shmem_comm = comm_ptr->dev.ch.shmem_comm;
    PMPI_Comm_size(shmem_comm, &local_size);
    MPID_Comm_get_ptr(shmem_comm, shmem_commptr);   
    if (count * (MPIR_MAX(extent, true_extent)) >= mv2_g_shmem_coll_max_msg_size) {
                mpi_errno =
                    MPIR_Reduce_intra(sendbuf, recvbuf, count, datatype, op, 0,
                                      shmem_commptr, errflag);
        goto fn_exit;
    }

    /* Get the operator and check whether it is commutative or not */
    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
        /* get the function by indexing into the op table */
        uop = MPIR_Op_table[op % 16 - 1];
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }

#if defined(HAVE_CXX_BINDING)
        if (op_ptr->language == MPID_LANG_CXX) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
            is_cxx_uop = 1;
        } else
#endif                          /* defined(HAVE_CXX_BINDING) */
        if (op_ptr->language == MPID_LANG_C) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
        } else {
            uop = (MPI_User_function *) op_ptr->function.f77_function;
        }
    }

    local_rank = shmem_commptr->rank;
    local_size = shmem_commptr->local_size;
    shmem_comm_rank = shmem_commptr->dev.ch.shmem_comm_rank;

    /* Doing the shared memory gather and reduction by the leader */
    if (local_rank == 0) {
        /* Message size is smaller than the shmem_reduce threshold. 
         * The intra-node communication is done through shmem */
        if (local_size > 1) {
            /* Node leader waits till all the non-leaders have written 
             * the data into the shmem buffer */
            MPIDI_CH3I_SHMEM_COLL_GetShmemBuf(local_size, local_rank,
                                              shmem_comm_rank,
                                              (void *) &shmem_buf);
            if (is_commutative) {
                for (i = 1; i < local_size; i++) {
                    local_buf = (char *) shmem_buf + stride * i;
#if defined(HAVE_CXX_BINDING)
                    if (is_cxx_uop) {
                        (*MPIR_Process.cxx_call_op_fn) (local_buf, recvbuf,
                                                        count, datatype,
                                                        uop);
                    } else {
#endif                          /* defined(HAVE_CXX_BINDING) */
                        (*uop) (local_buf, recvbuf, &count, &datatype);
#if defined(HAVE_CXX_BINDING)
                    }
#endif                          /* defined(HAVE_CXX_BINDING) */

                }
                MPIDI_CH3I_SHMEM_COLL_SetGatherComplete(local_size,
                                                        local_rank,
                                                        shmem_comm_rank);
            }
        }
    } else {
        MPIDI_CH3I_SHMEM_COLL_GetShmemBuf(local_size, local_rank,
                                          shmem_comm_rank,
                                          (void *) &shmem_buf);
        local_buf = (char *) shmem_buf + stride * local_rank;
        if (sendbuf != MPI_IN_PLACE) {
            mpi_errno = MPIR_Localcopy(sendbuf, count, datatype, local_buf,
                                       count, datatype);
        } else {
            mpi_errno = MPIR_Localcopy(recvbuf, count, datatype, local_buf,
                                       count, datatype);
        }
        MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                            "**fail");
        MPIDI_CH3I_SHMEM_COLL_SetGatherComplete(local_size, local_rank,
                                                    shmem_comm_rank);
    }

  fn_exit:
    MPIR_TIMER_END(coll,allreduce,shm_intra);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}

/* intra-node p2p reduce as the first reduce in allreduce */
int MPIR_Allreduce_reduce_p2p_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,intra_p2p);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_intra_p2p, 1);
    int mpi_errno = MPI_SUCCESS;
    int mpi_errno_ret = MPI_SUCCESS;
    MPI_Aint true_lb, true_extent;
    MPI_Comm shmem_comm = MPI_COMM_NULL;
    MPID_Comm *shmem_commptr = NULL;
    int local_rank = -1, local_size = 0;

    if (count == 0) {
        MPIR_TIMER_END(coll,allreduce,intra_p2p);
        return MPI_SUCCESS;
    }

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);

    shmem_comm = comm_ptr->dev.ch.shmem_comm;
    PMPI_Comm_size(shmem_comm, &local_size);
    MPID_Comm_get_ptr(shmem_comm, shmem_commptr);
    local_rank = shmem_commptr->rank;
    local_size = shmem_commptr->local_size;

    /* Doing the shared memory gather and reduction by the leader */
    if (local_rank == 0) {
        /* Message size is larger than the shmem_reduce threshold. 
         * The leader will spend too much time doing the math operation
         * for messages that are larger. So, we use a point-to-point
         * based reduce to balance the load across all the processes within
         * the same node*/
        mpi_errno =
            MPIR_Reduce_MV2(sendbuf, recvbuf, count, datatype, op, 0,
                            shmem_commptr, errflag);
        if (mpi_errno) {
            /* for communication errors, just record the error but continue */
            *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }

    } else {
        if (sendbuf != MPI_IN_PLACE) {
            mpi_errno =
                MPIR_Reduce_MV2(sendbuf, recvbuf, count, datatype, op, 0,
                                shmem_commptr, errflag);
        } else {
            /* MPI_Allreduce was called with MPI_IN_PLACE as the sendbuf.
             * Since we are doing Reduce now, we need to be careful. In
             * MPI_Reduce, only the root can use MPI_IN_PLACE as sendbuf.
             * Also, the recvbuf is not relevant at all non-root processes*/
            mpi_errno = MPIR_Reduce_MV2(recvbuf, NULL, count, datatype, op,
                                        0, shmem_commptr, errflag);
        }
        if (mpi_errno) {
            /* for communication errors, just record the error but continue */
            *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }
    }

    MPIR_TIMER_END(coll,allreduce,intra_p2p);
    return (mpi_errno);
}

/* general two level allreduce helper function */
int MPIR_Allreduce_two_level_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,2lvl);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_2lvl, 1);
    int mpi_errno = MPI_SUCCESS;
    int mpi_errno_ret = MPI_SUCCESS;
    int total_size = 0;
    MPI_Aint true_lb, true_extent;
    MPI_Comm shmem_comm = MPI_COMM_NULL, leader_comm = MPI_COMM_NULL;
    MPID_Comm *shmem_commptr = NULL, *leader_commptr = NULL;
    int local_rank = -1, local_size = 0;

    if (count == 0) {
        MPIR_TIMER_END(coll,allreduce,2lvl);
        return MPI_SUCCESS;
    }

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);

    total_size = comm_ptr->local_size;
    shmem_comm = comm_ptr->dev.ch.shmem_comm;
    PMPI_Comm_size(shmem_comm, &local_size);
    MPID_Comm_get_ptr(shmem_comm, shmem_commptr);
    local_rank = shmem_commptr->rank;
    local_size = shmem_commptr->local_size;

    leader_comm = comm_ptr->dev.ch.leader_comm;
    MPID_Comm_get_ptr(leader_comm, leader_commptr);

    if (local_rank == 0) {
        if (sendbuf != MPI_IN_PLACE) {
            mpi_errno = MPIR_Localcopy(sendbuf, count, datatype, recvbuf,
                                       count, datatype);
            MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                "**fail");
        }
    }

    /* Doing the shared memory gather and reduction by the leader */
    if (local_rank == 0) {
        if ((MV2_Allreduce_intra_function == &MPIR_Allreduce_reduce_shmem_MV2) || 
              (MV2_Allreduce_intra_function == &MPIR_Allreduce_reduce_p2p_MV2) ) {
        mpi_errno =
        MV2_Allreduce_intra_function(sendbuf, recvbuf, count, datatype,
                                     op, comm_ptr, errflag);
        }
        else {
        mpi_errno =
        MV2_Allreduce_intra_function(sendbuf, recvbuf, count, datatype,
                                     op, shmem_commptr, errflag);
        }

        if (mpi_errno) {
            /* for communication errors, just record the error but continue */
            *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }

        if (local_size != total_size) {
            /* inter-node allreduce */
            
            if(MV2_Allreduce_function == &MPIR_Allreduce_pt2pt_rd_MV2){
                mpi_errno =
                    MPIR_Allreduce_pt2pt_rd_MV2(MPI_IN_PLACE, recvbuf, count, datatype, op,
                                      leader_commptr, errflag);
            } else if (MV2_Allreduce_function == &MPIR_Allreduce_pt2pt_reduce_scatter_allgather_MV2) {
                mpi_errno =
                    MPIR_Allreduce_pt2pt_reduce_scatter_allgather_MV2(MPI_IN_PLACE,
                            recvbuf, count, datatype, op, leader_commptr,
                            errflag); 
            } else 
#if defined (_SHARP_SUPPORT_)
            if (MV2_Allreduce_function == &MPIR_Sharp_Allreduce_MV2) {
                mpi_errno =
                    MPIR_Sharp_Allreduce_MV2(MPI_IN_PLACE, recvbuf, count, datatype, op,
                                  comm_ptr , errflag);
                if (mpi_errno != MPI_SUCCESS) {
                    /* fall back to RD algorithm if SHArP is not supported */
                    mpi_errno =
                        MPIR_Allreduce_pt2pt_rd_MV2(MPI_IN_PLACE, recvbuf, count, datatype, op,
                                          leader_commptr, errflag); 
                }
            } else
#endif 
            {
                mpi_errno =
                    MPIR_Allreduce_pt2pt_rs_MV2(MPI_IN_PLACE, recvbuf, count, datatype, op,
                                      leader_commptr, errflag);
            }
            

            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }
    } else {
        /* insert the first reduce here */
        if ((MV2_Allreduce_intra_function == &MPIR_Allreduce_reduce_shmem_MV2) || 
              (MV2_Allreduce_intra_function == &MPIR_Allreduce_reduce_p2p_MV2) ) {
        mpi_errno =
        MV2_Allreduce_intra_function(sendbuf, recvbuf, count, datatype,
                                     op, comm_ptr, errflag);
        }
        else {
        mpi_errno =
        MV2_Allreduce_intra_function(sendbuf, recvbuf, count, datatype,
                                     op, shmem_commptr, errflag);
        }
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
    }

    /* Broadcasting the message from leader to the rest */
    mpi_errno = MPIR_Shmem_Bcast_MV2(recvbuf, count, datatype, 0, shmem_commptr, errflag);
    if (mpi_errno) {
        /* for communication errors, just record the error but continue */
        *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
        MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
        MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
    }

  fn_exit:
    MPIR_TIMER_END(coll,allreduce,2lvl);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}

int MPIR_Allreduce_socket_aware_two_level_old_MV2(const void *sendbuf,
                                          void *recvbuf,
                                          int count,
                                          MPI_Datatype datatype,
                                          MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIU_Assert(comm_ptr->dev.ch.use_intra_sock_comm && comm_ptr->dev.ch.shmem_coll_ok == 1);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_2lvl, 1);
    int mpi_errno = MPI_SUCCESS;
    int mpi_errno_ret = MPI_SUCCESS;
    MPI_Aint true_lb, true_extent;
    MPI_Comm shmem_comm = MPI_COMM_NULL, leader_comm = MPI_COMM_NULL;
    MPID_Comm *shmem_commptr = NULL, *leader_commptr = NULL;
    int local_rank = -1;
    if (count == 0) {
        return MPI_SUCCESS;
    }
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);

    shmem_comm = comm_ptr->dev.ch.shmem_comm;
    MPID_Comm_get_ptr(shmem_comm, shmem_commptr);
    local_rank = shmem_commptr->rank;

    leader_comm = comm_ptr->dev.ch.leader_comm;
    MPID_Comm_get_ptr(leader_comm, leader_commptr);

    int intra_sock_rank = -1;
    MPID_Comm * intra_sock_commptr;
    MPID_Comm_get_ptr(shmem_commptr->dev.ch.intra_sock_comm, intra_sock_commptr);
    intra_sock_rank = intra_sock_commptr->rank;

    /* Step 1. Socket leaders do an intra-socket reduce using shared memory*/

    if (intra_sock_rank == 0) {
        /* root ranks */
	if(mv2_use_optimized_release_allreduce == 1) {
	    mpi_errno = MPIR_Reduce_shmem_MV2_optrels(sendbuf, recvbuf, count, datatype, op, 0, intra_sock_commptr, errflag);
	} else {
	    mpi_errno = MPIR_Reduce_shmem_MV2(sendbuf, recvbuf, count, datatype, op, 0, intra_sock_commptr, errflag);
	}
        if(mpi_errno)
        {
        /* for communication errors, just record the error but continue */
        *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
        MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
        MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }

    } else {
        /* non-root ranks */
        if (sendbuf != MPI_IN_PLACE) {
	    if(mv2_use_optimized_release_allreduce == 1) {
		mpi_errno = MPIR_Reduce_shmem_MV2_optrels(sendbuf, recvbuf, count, datatype, op, 0, intra_sock_commptr, errflag);
	    } else {
		mpi_errno = MPIR_Reduce_shmem_MV2(sendbuf, recvbuf, count, datatype, op, 0, intra_sock_commptr, errflag);
	    }
        } else {
	    if(mv2_use_optimized_release_allreduce == 1) {
		mpi_errno = MPIR_Reduce_shmem_MV2_optrels(recvbuf, NULL,  count, datatype, op, 0, intra_sock_commptr, errflag);
	    } else {
		mpi_errno = MPIR_Reduce_shmem_MV2(recvbuf, NULL,  count, datatype, op, 0, intra_sock_commptr, errflag);
	    }
        }

        if (mpi_errno) {
            /* for communication errors, just record the error but continue */
            *errflag = TRUE;
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }
    }

    /* Step 2. Socket level leaders within the node do an intra-node reduce to rank 0*/
    
    MPID_Comm *shmem_leader_commptr = NULL;
    MPID_Comm_get_ptr(shmem_commptr->dev.ch.intra_sock_leader_comm,shmem_leader_commptr);

    if(intra_sock_rank == 0 && shmem_leader_commptr->local_size > 1)
    {
        // Fall back to binomial if shmem coll not ok for shmem leaders
        if(shmem_leader_commptr->dev.ch.shmem_coll_ok != 1)
        {
            mpi_errno = MPIR_Reduce_binomial_MV2(MPI_IN_PLACE,recvbuf,count,datatype,op,0,shmem_leader_commptr,errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }
        else
        {
            MPID_Comm *shmem_leader_shmemcomm = NULL;
            MPID_Comm_get_ptr(shmem_leader_commptr->dev.ch.shmem_comm, shmem_leader_shmemcomm);
            int i, stride, leader_rank, leader_size, leader_shmem_comm_rank;
            MPI_User_function *uop;
            MPID_Op *op_ptr;
            char *shmem_buf = NULL;
            void *local_buf = NULL;
            MPI_Aint true_lb, true_extent, extent;
#ifdef HAVE_CXX_BINDING
            int is_cxx_uop = 0;
#endif

            MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
            MPID_Datatype_get_extent_macro(datatype, extent);
            stride = count * MPIR_MAX(extent, true_extent);

            leader_rank = shmem_leader_shmemcomm->rank;
            leader_size = shmem_leader_shmemcomm->local_size;
            leader_shmem_comm_rank = shmem_leader_shmemcomm->dev.ch.shmem_comm_rank;

            /* Get the operator and check whether it is commutative or not */
            if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
                /* get the function by indexing into the op table */
                uop = MPIR_Op_table[op % 16 - 1];
            } else {
                MPID_Op_get_ptr(op, op_ptr);
#if defined(HAVE_CXX_BINDING)
                if (op_ptr->language == MPID_LANG_CXX) {
                    uop = (MPI_User_function *) op_ptr->function.c_function;
                    is_cxx_uop = 1;
                } else {
#endif                          /* defined(HAVE_CXX_BINDING) */
                    if (op_ptr->language == MPID_LANG_C) {
                        uop = (MPI_User_function *) op_ptr->function.c_function;
                    } else {
                        uop = (MPI_User_function *) op_ptr->function.f77_function;
                    }
#if defined(HAVE_CXX_BINDING)
                }
#endif                          /* defined(HAVE_CXX_BINDING) */
            }

            if (leader_rank == 0) {
                MPIDI_CH3I_SHMEM_COLL_GetShmemBuf(leader_size, leader_rank,
                        leader_shmem_comm_rank, (void *) &shmem_buf);
                for (i = 1; i < leader_size; i++) {
                    local_buf = (char *) shmem_buf + stride * i;
#if defined(HAVE_CXX_BINDING)
                    if (is_cxx_uop) {
                        (*MPIR_Process.cxx_call_op_fn) (local_buf, recvbuf,
                                count, datatype, uop);
                    } else {
#endif                          /* defined(HAVE_CXX_BINDING) */
                        (*uop) (local_buf, recvbuf, &count, &datatype);
#if defined(HAVE_CXX_BINDING)
                    }
#endif
                }
                MPIDI_CH3I_SHMEM_COLL_SetGatherComplete(leader_size,
                        leader_rank, leader_shmem_comm_rank);
            } else {
                MPIDI_CH3I_SHMEM_COLL_GetShmemBuf(leader_size, leader_rank,
                        leader_shmem_comm_rank, (void *) &shmem_buf);
                local_buf = (char *) shmem_buf + stride * leader_rank;
                mpi_errno = MPIR_Localcopy(recvbuf, count, datatype,
                        local_buf, count, datatype);
                if (mpi_errno) {
                    MPIR_ERR_POP(mpi_errno);
                }
                MPIDI_CH3I_SHMEM_COLL_SetGatherComplete(leader_size, leader_rank,
                        leader_shmem_comm_rank);
            }
        }
    }

    /* Step 3. Leaders across nodes do an inter-node allreduce */

    if(local_rank == 0 && leader_commptr->local_size > 1)
    {
        mpi_errno = MPIR_Allreduce_pt2pt_rd_MV2(MPI_IN_PLACE, recvbuf, count, datatype, op,
                                                       leader_commptr, errflag);
        if (mpi_errno) {
             /* for communication errors, just record the error but continue */
             *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
             MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
             MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }
    }

    /* Step 4. Intra-node Bcast on shmem comm from root (rank 0) */

    mpi_errno = MPIR_Shmem_Bcast_MV2(recvbuf, count, datatype, 0, shmem_commptr, errflag);

    if (mpi_errno) {
        *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
        MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
        MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
    }

    fn_exit:
        return (mpi_errno);

    fn_fail:
        goto fn_exit;
}

int MPIR_Allreduce_socket_aware_two_level_MV2(const void *sendbuf,
                                          void *recvbuf,
                                          int count,
                                          MPI_Datatype datatype,
                                          MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIU_Assert(comm_ptr->dev.ch.use_intra_sock_comm && comm_ptr->dev.ch.shmem_coll_ok == 1);
    
	if(!mv2_use_slot_shmem_coll) {
		return MPIR_Allreduce_socket_aware_two_level_old_MV2(sendbuf, recvbuf, count, datatype, op,
												comm_ptr, errflag);
	}

	MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_2lvl, 1);
    shmem_info_t *shmem = NULL;
    int mpi_errno = MPI_SUCCESS, mpi_errno_ret = MPI_SUCCESS;
    MPI_Aint true_lb = 0, true_extent = 0, extent = 0;
    void *in_buf = NULL, *buf = NULL;
    int local_rank = -1, is_cxx_uop = 0, rindex = 0, len = 0;
    MPI_User_function *uop = NULL;
    MPID_Op *op_ptr = NULL;
    MPI_Comm shmem_comm = MPI_COMM_NULL, leader_comm = MPI_COMM_NULL;
    MPID_Comm *shmem_commptr = NULL, *leader_commptr = NULL;
    MPID_Comm *intra_sock_commptr = NULL, *shmem_leader_commptr = NULL;

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);
    len = count * MPIR_MAX(extent, true_extent);

    if (count == 0) {
        return MPI_SUCCESS;
    }
    /* Get details shmem_comm */
    shmem_comm = comm_ptr->dev.ch.shmem_comm;
    MPID_Comm_get_ptr(shmem_comm, shmem_commptr);
    local_rank = shmem_commptr->rank;
    /* Get details leader_comm */
    leader_comm = comm_ptr->dev.ch.leader_comm;
    MPID_Comm_get_ptr(leader_comm, leader_commptr);
    /* Get details intra_sock_comm */
    MPID_Comm_get_ptr(shmem_commptr->dev.ch.intra_sock_comm, intra_sock_commptr);
    /* Get the operator and check whether it is commutative or not */
    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        /* get the function by indexing into the op table */
        uop = MPIR_Op_table[op % 16 - 1];
    } else {
        MPID_Op_get_ptr(op, op_ptr);
#if defined(HAVE_CXX_BINDING)
        if (op_ptr->language == MPID_LANG_CXX) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
            is_cxx_uop = 1;
        } else
#endif /* defined(HAVE_CXX_BINDING) */
        {
            if (op_ptr->language == MPID_LANG_C) {
                uop = (MPI_User_function *) op_ptr->function.c_function;
            } else {
                uop = (MPI_User_function *) op_ptr->function.f77_function;
            }
        }
    }

    /* Get details of the datatype */
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);
    
    if (sendbuf != MPI_IN_PLACE) {
        in_buf = (void *)sendbuf;
    } else {
        in_buf = recvbuf;
    }

    /* Step 1. intra-socket reduce using shared memory */
    /* Get shmem region */
    shmem = intra_sock_commptr->dev.ch.shmem_info;
    /* Get rindex */
    rindex = shmem->read % mv2_shm_window_size;
    buf = shmem->queue[shmem->local_rank].shm_slots[rindex]->buf;
    mv2_shm_tree_reduce(shmem, in_buf, len, count, 0, uop,
            datatype, is_cxx_uop);
    shmem->write++;
    shmem->read++;
    if (IS_SHMEM_WINDOW_FULL(shmem->write, shmem->tail)) {
        PRINT_DEBUG(DEBUG_SHM_verbose > 1, "shmem window full: %llu \n", shmem->write);
        mv2_shm_barrier(shmem);
        shmem->tail = shmem->read;
    }
    /* in_buf contains the result of the operation */
    in_buf = buf;

    /* Step 2. Socket level leaders within the node do an intra-node reduce to rank 0*/
    MPID_Comm_get_ptr(shmem_commptr->dev.ch.intra_sock_leader_comm,shmem_leader_commptr);

    if (intra_sock_commptr->rank == 0 && shmem_leader_commptr != NULL && shmem_leader_commptr->local_size > 1) {
        /* Fall back to binomial if shmem coll not ok for shmem leaders */
        if (shmem_leader_commptr->dev.ch.shmem_coll_ok != 1) {
            mpi_errno = MPIR_Reduce_binomial_MV2(MPI_IN_PLACE, in_buf, count, datatype,
                    op, 0, shmem_leader_commptr, errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        } else {
            /* Get shmem region */
            shmem = shmem_leader_commptr->dev.ch.shmem_info;
            /* Get rindex */
            rindex = shmem->read % mv2_shm_window_size;
            buf = shmem->queue[shmem->local_rank].shm_slots[rindex]->buf;
            mv2_shm_tree_reduce(shmem, in_buf, len, count, 0, uop,
                    datatype, is_cxx_uop);
            shmem->write++;
            shmem->read++;
            if (IS_SHMEM_WINDOW_FULL(shmem->write, shmem->tail)) {
                PRINT_DEBUG(DEBUG_SHM_verbose > 1, "shmem window full: %llu \n", shmem->write);
                mv2_shm_barrier(shmem);
                shmem->tail = shmem->read;
            }
            in_buf = buf;
        }
    }

    /* Step 3. Leaders across nodes do an inter-node allreduce */
    if (local_rank == 0 && leader_commptr->local_size > 1) {
#if defined (_SHARP_SUPPORT_)
        MPI_Aint type_size = 0;
        /* Get size of data */
        MPID_Datatype_get_size_macro(datatype, type_size);
        MPIDI_msg_sz_t nbytes = (MPIDI_msg_sz_t) (count) * (type_size);
        if (comm_ptr->dev.ch.is_sharp_ok == 1 && nbytes <=
                mv2_sharp_tuned_msg_size && mv2_enable_sharp_coll == 1 &&
                mv2_enable_sharp_allreduce) {
            mpi_errno = MPIR_Sharp_Allreduce_MV2(MPI_IN_PLACE, in_buf, count,
                    datatype, op, comm_ptr , errflag);
            if (mpi_errno != MPI_SUCCESS) {
                /* fall back to RD algorithm if SHArP is not supported */
                mpi_errno = MPIR_Allreduce_pt2pt_rd_MV2(MPI_IN_PLACE, in_buf, count,
                        datatype, op, leader_commptr,
                        errflag);
                if (mpi_errno) {
                    /* for communication errors, just record the error but continue */
                    *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                    MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                    MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
                }
            }
        } else
#endif /* #if defined (_SHARP_SUPPORT_) */  
        {
            mpi_errno = MPIR_Allreduce_pt2pt_rd_MV2(MPI_IN_PLACE, in_buf, count,
                    datatype, op, leader_commptr,
                    errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }
    }

    /* Step 4. Inter-socket-leader Bcast on shmem_leader_comm */
    if (intra_sock_commptr->rank == 0 && shmem_leader_commptr != NULL && shmem_leader_commptr->local_size > 1) {
        /* Fall back to binomial if shmem coll not ok for shmem leaders */
        if (shmem_leader_commptr->dev.ch.shmem_coll_ok != 1) {
            mpi_errno = MPIR_Bcast_binomial_MV2(in_buf, count, datatype, 0, shmem_leader_commptr, errflag);
            if (mpi_errno) {
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        } else {
            MPIR_Shmem_Bcast_MV2(in_buf, count, datatype, 0, shmem_leader_commptr, errflag);
        }
    }

    /* Step 5. Intra-socket Bcast on intra_sock_comm */
    MPIR_Shmem_Bcast_MV2(in_buf, count, datatype, 0, intra_sock_commptr, errflag);

    MPIR_Localcopy(in_buf, count, datatype, recvbuf, count, datatype);

    fn_exit:
        return (mpi_errno);

    fn_fail:
        goto fn_exit;
}

/* Given comm_ptr, MPIR_Allreduce_topo_aware_inter_node_helper_MV2 performs an
 * inter-node Allreduce operation using SHARP/rd over leader_comm (in comm_ptr) */
int MPIR_Allreduce_topo_aware_inter_node_helper_MV2(const void *sendbuf,
                                          void *recvbuf,
                                          int count,
                                          MPI_Datatype datatype,
                                          MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    int mpi_errno = MPI_SUCCESS, mpi_errno_ret = MPI_SUCCESS;
    MPI_Comm leader_comm = MPI_COMM_NULL;
    MPID_Comm *leader_commptr = NULL;   
    leader_comm = comm_ptr->dev.ch.leader_comm;
    MPID_Comm_get_ptr(leader_comm, leader_commptr);
#if defined (_SHARP_SUPPORT_)
    MPI_Aint type_size = 0;
    /* Get size of data */
    MPID_Datatype_get_size_macro(datatype, type_size);
    MPIDI_msg_sz_t nbytes = (MPIDI_msg_sz_t) (count) * (type_size);
    if (comm_ptr->dev.ch.is_sharp_ok == 1 && nbytes <=
            mv2_sharp_tuned_msg_size && mv2_enable_sharp_coll == 1 &&
            mv2_enable_sharp_allreduce) {
        mpi_errno = MPIR_Sharp_Allreduce_MV2(sendbuf, recvbuf, count,
                datatype, op, comm_ptr , errflag);
        if (mpi_errno != MPI_SUCCESS) {
            /* fall back to RD algorithm if SHArP is not supported */
            mpi_errno = MPIR_Allreduce_pt2pt_rd_MV2(sendbuf, recvbuf, count,
                    datatype, op, leader_commptr,
                    errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }
    } else
#endif /* #if defined (_SHARP_SUPPORT_) */  
    {
        mpi_errno = MPIR_Allreduce_pt2pt_rd_MV2(sendbuf, recvbuf, count,
                datatype, op, leader_commptr,
                errflag);
        if (mpi_errno) {
            /* for communication errors, just record the error but continue */
            *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }
    }

fn_exit:
    return (mpi_errno);

fn_fail:
    goto fn_exit;

}

int MPIR_Allreduce_topo_aware_hierarchical_MV2(const void *sendbuf,
                                          void *recvbuf,
                                          int count,
                                          MPI_Datatype datatype,
                                          MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,topo_aware_hierarchical);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_topo_aware_hierarchical, 1);
    MPIU_Assert(comm_ptr->dev.ch.topo_coll_ok == 1 && comm_ptr->dev.ch.shmem_coll_ok == 1);
    int i = 0;
    shmem_info_t *shmem = NULL;
    int mpi_errno = MPI_SUCCESS, mpi_errno_ret = MPI_SUCCESS;
    MPI_Aint true_lb = 0, true_extent = 0, extent = 0;
    void *in_buf = NULL, *buf = NULL;
    int local_rank = -1, is_cxx_uop = 0, rindex = 0, len = 0;
    MPI_User_function *uop = NULL;
    MPID_Op *op_ptr = NULL;
    MPI_Comm shmem_comm = MPI_COMM_NULL, leader_comm = MPI_COMM_NULL;
    MPID_Comm *shmem_commptr = NULL, *leader_commptr = NULL;
    MPID_Comm *topo_comm_ptr = NULL;

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);
    len = count * MPIR_MAX(extent, true_extent);

    if (count == 0) {
        return MPI_SUCCESS;
    }
    
    /* Get leader and shared memory communicators/related attributes */
    shmem_comm = comm_ptr->dev.ch.shmem_comm;
    MPID_Comm_get_ptr(shmem_comm, shmem_commptr);
    local_rank = shmem_commptr->rank;
    leader_comm = comm_ptr->dev.ch.leader_comm;
    MPID_Comm_get_ptr(leader_comm, leader_commptr);
    
    /* Get the operator and check whether it is commutative or not */
    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        /* get the function by indexing into the op table */
        uop = MPIR_Op_table[op % 16 - 1];
    } else {
        MPID_Op_get_ptr(op, op_ptr);
#if defined(HAVE_CXX_BINDING)
        if (op_ptr->language == MPID_LANG_CXX) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
            is_cxx_uop = 1;
        } else
#endif /* defined(HAVE_CXX_BINDING) */
        {
            if (op_ptr->language == MPID_LANG_C) {
                uop = (MPI_User_function *) op_ptr->function.c_function;
            } else {
                uop = (MPI_User_function *) op_ptr->function.f77_function;
            }
        }
    }

    /* Get details of the datatype */
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);

    if (sendbuf != MPI_IN_PLACE) {
        in_buf = (void *)sendbuf;
    } else {
        in_buf = recvbuf;
    }

    /* Step 1. intra-node topo-aware reduce using shared memory */
    for (i = 0; i <= mv2_num_intra_node_comm_levels; ++i) {
        /* Get next topo_comm pointer */
        MPID_Comm_get_ptr(shmem_commptr->dev.ch.topo_comm[i], topo_comm_ptr);
        if (topo_comm_ptr && topo_comm_ptr->local_size > 1) {
            /* Get shmem region */
            shmem = topo_comm_ptr->dev.ch.shmem_info;
            /* Get rindex */
            rindex = shmem->read % mv2_shm_window_size;
            buf = shmem->queue[shmem->local_rank].shm_slots[rindex]->buf;
            mv2_shm_tree_reduce(shmem, in_buf, len, count, 0, uop,
                    datatype, is_cxx_uop);
            shmem->write++;
            shmem->read++;
            if (IS_SHMEM_WINDOW_FULL(shmem->write, shmem->tail)) {
                PRINT_DEBUG(DEBUG_SHM_verbose > 1, "shmem window full: %llu \n", shmem->write);
                mv2_shm_barrier(shmem);
                shmem->tail = shmem->read;
            }
            /* in_buf contains the result of the operation */
            in_buf = buf;
        }
    }

    /* Step 2. Leaders across nodes do an inter-node allreduce */
    if (local_rank == 0 && leader_commptr->local_size > 1) {
        /* in_buf == sendbuf if and only if no intra-node reduction occurs
         * (example : local size is one). This check exists to avoid overwriting
         * the application's sendbuf in the inter-node step*/
        if (in_buf == sendbuf) {
            mpi_errno = MPIR_Allreduce_topo_aware_inter_node_helper_MV2(sendbuf, 
                            recvbuf, count, datatype, op, comm_ptr, errflag);
        } else {
            mpi_errno = MPIR_Allreduce_topo_aware_inter_node_helper_MV2(MPI_IN_PLACE,
                            in_buf, count, datatype, op, comm_ptr, errflag);
        }

        if (mpi_errno) {
            *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }
    }

    /* Step 3. Intra-node Bcast */
    for (i = mv2_num_intra_node_comm_levels; i >= 0; --i) {
        MPID_Comm_get_ptr(shmem_commptr->dev.ch.topo_comm[i], topo_comm_ptr);
        if (topo_comm_ptr && topo_comm_ptr->local_size > 1) {
            mpi_errno = MPIR_Shmem_Bcast_MV2(in_buf, count, datatype, 0, topo_comm_ptr, errflag);
            if (mpi_errno) {
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }
    }
    
    /* Copy in_buf into recvbuf only if it has been modified */
    if (in_buf != recvbuf && in_buf != sendbuf) {
        MPIR_Localcopy(in_buf, count, datatype, recvbuf, count, datatype);
    }

fn_exit:
    MPIR_TIMER_END(coll,allreduce,topo_aware_hierarchical);
    return (mpi_errno);

fn_fail:
    goto fn_exit;
}

int MPIR_Allreduce_shmem_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,shmem);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_shmem, 1);
    int mpi_errno = MPI_SUCCESS;
    int mpi_errno_ret = MPI_SUCCESS;
    int i = 0, is_commutative = 0;
    MPI_Aint true_lb, true_extent, extent;
    MPI_User_function *uop;
    MPID_Op *op_ptr;
#ifdef HAVE_CXX_BINDING
    int is_cxx_uop = 0;
#endif
    char *shmem_buf = NULL;
    MPI_Comm shmem_comm = MPI_COMM_NULL, leader_comm = MPI_COMM_NULL;
    MPID_Comm *shmem_commptr = NULL, *leader_commptr = NULL;
    int local_rank = -1, local_size = 0;
    void *local_buf = NULL;
    int stride = 0;
    is_commutative = 0;
    int total_size, shmem_comm_rank;

    if (count == 0) {
        return MPI_SUCCESS;
    }

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);
    stride = count * MPIR_MAX(extent, true_extent);

    /* Get the operator and check whether it is commutative or not */
    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
        /* get the function by indexing into the op table */
        uop = MPIR_Op_table[op % 16 - 1];
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }

#if defined(HAVE_CXX_BINDING)
        if (op_ptr->language == MPID_LANG_CXX) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
            is_cxx_uop = 1;
        } else
#endif                          /* defined(HAVE_CXX_BINDING) */
        if (op_ptr->language == MPID_LANG_C) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
        } else {
            uop = (MPI_User_function *) op_ptr->function.f77_function;
        }
    }

    total_size = comm_ptr->local_size;
    shmem_comm = comm_ptr->dev.ch.shmem_comm;
    PMPI_Comm_size(shmem_comm, &local_size);
    MPID_Comm_get_ptr(shmem_comm, shmem_commptr);
    local_rank = shmem_commptr->rank;
    local_size = shmem_commptr->local_size;
    shmem_comm_rank = shmem_commptr->dev.ch.shmem_comm_rank;

    leader_comm = comm_ptr->dev.ch.leader_comm;
    MPID_Comm_get_ptr(leader_comm, leader_commptr);

    if (local_rank == 0) {
        if (sendbuf != MPI_IN_PLACE) {
            mpi_errno = MPIR_Localcopy(sendbuf, count, datatype, recvbuf,
                                       count, datatype);
            MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                "**fail");
        }
    }

    /* Doing the shared memory gather and reduction by the leader */
    if (local_rank == 0) {
        if (stride <= mv2_coll_param.shmem_allreduce_msg) {
            /* Message size is smaller than the shmem_reduce threshold. 
             * The intra-node communication is done through shmem */
            if (local_size > 1) {
                /* Node leader waits till all the non-leaders have written 
                 * the data into the shmem buffer */
                MPIDI_CH3I_SHMEM_COLL_GetShmemBuf(local_size, local_rank,
                                                  shmem_comm_rank,
                                                  (void *) &shmem_buf);
                if (is_commutative) {
                    for (i = 1; i < local_size; i++) {
                        local_buf = (char *) shmem_buf + stride * i;
#if defined(HAVE_CXX_BINDING)
                        if (is_cxx_uop) {
                            (*MPIR_Process.cxx_call_op_fn) (local_buf, recvbuf,
                                                            count, datatype,
                                                            uop);
                        } else {
#endif                          /* defined(HAVE_CXX_BINDING) */
                            (*uop) (local_buf, recvbuf, &count, &datatype);
#if defined(HAVE_CXX_BINDING)
                        }
#endif                          /* defined(HAVE_CXX_BINDING) */

                    }
                    MPIDI_CH3I_SHMEM_COLL_SetGatherComplete(local_size,
                                                            local_rank,
                                                            shmem_comm_rank);
                }
            }
        } else {
            /* Message size is larger than the shmem_reduce threshold. 
             * The leader will spend too much time doing the math operation
             * for messages that are larger. So, we use a point-to-point
             * based reduce to balance the load across all the processes within
             * the same node*/
            mpi_errno =
                MPIR_Reduce_MV2(sendbuf, recvbuf, count, datatype, op, 0,
                                shmem_commptr, errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }

        }
        if (local_size != total_size) {
            mpi_errno =
                MPIR_Allreduce_MV2(MPI_IN_PLACE, recvbuf, count, datatype, op,
                                   leader_commptr, errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }
    } else {
        if (stride <= mv2_coll_param.shmem_allreduce_msg) {
            MPIDI_CH3I_SHMEM_COLL_GetShmemBuf(local_size, local_rank,
                                              shmem_comm_rank,
                                              (void *) &shmem_buf);
            local_buf = (char *) shmem_buf + stride * local_rank;
            if (sendbuf != MPI_IN_PLACE) {
                mpi_errno = MPIR_Localcopy(sendbuf, count, datatype, local_buf,
                                           count, datatype);
            } else {
                mpi_errno = MPIR_Localcopy(recvbuf, count, datatype, local_buf,
                                           count, datatype);
            }
            MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                "**fail");
            MPIDI_CH3I_SHMEM_COLL_SetGatherComplete(local_size, local_rank,
                                                    shmem_comm_rank);
        } else {
            if (sendbuf != MPI_IN_PLACE) {
                mpi_errno =
                    MPIR_Reduce_MV2(sendbuf, recvbuf, count, datatype, op, 0,
                                    shmem_commptr, errflag);
            } else {
                /* MPI_Allreduce was called with MPI_IN_PLACE as the sendbuf.
                 * Since we are doing Reduce now, we need to be careful. In
                 * MPI_Reduce, only the root can use MPI_IN_PLACE as sendbuf.
                 * Also, the recvbuf is not relevant at all non-root processes*/
                mpi_errno = MPIR_Reduce_MV2(recvbuf, NULL, count, datatype, op,
                                            0, shmem_commptr, errflag);
            }
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }
    }

    /* Broadcasting the message from leader to the rest */
    /* Note: shared memory broadcast could improve the performance */
    if (local_size > 1) {
        MPIR_Bcast_MV2(recvbuf, count, datatype, 0, shmem_commptr, errflag);
        if (mpi_errno) {
            /* for communication errors, just record the error but continue */
            *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
            MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
            MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
        }
    }

  fn_exit:
    MPIR_TIMER_END(coll,allreduce,shmem);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}


#if defined(_MCST_SUPPORT_)
#undef FCNAME
#define FCNAME "MPIR_Allreduce_mcst_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_mcst_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,mcast);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_mcast, 1);
    MPI_Aint true_lb, true_extent;
   /*We use reduce (at rank =0) followed by mcst-bcast to implement the 
    * allreduce operation */
    int root=0;
    MPI_Aint nbytes=0;
    MPI_Aint type_size=0, position=0; 
    int mpi_errno=MPI_SUCCESS;
    int mpi_errno_ret=MPI_SUCCESS;
    int rank = comm_ptr->rank, is_contig=0, is_commutative=0;
    MPIU_CHKLMEM_DECL(1);
    MPID_Datatype *dtp=NULL;
    void *tmp_buf=NULL; 
    MPID_Op *op_ptr=NULL;
    MPID_Datatype_get_size_macro(datatype, type_size);
    nbytes = type_size * count;

 
    if (HANDLE_GET_KIND(datatype) == HANDLE_KIND_BUILTIN) { 
        is_contig = 1;
    } else {
        MPID_Datatype_get_ptr(datatype, dtp);
        is_contig = dtp->is_contig;
    }

    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }
    }

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);

   if(is_commutative == 0) { 
       reduce_fn = &MPIR_Reduce_binomial_MV2; 
   } else { 
       if(MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_two_level_helper_MV2) {
            reduce_fn = &MPIR_Reduce_MV2;
       } else {
            reduce_fn = &MPIR_Reduce_redscat_gather_MV2;
       } 
   } 

    /* First do a reduction at rank = 0 */
    if(rank == root) {
        mpi_errno = reduce_fn(sendbuf, recvbuf, count, datatype,
                                op, root, comm_ptr, errflag);
    } else {
        if(sendbuf != MPI_IN_PLACE) {
            mpi_errno = reduce_fn(sendbuf, recvbuf, count, datatype,
                                op, root, comm_ptr, errflag);
        } else {
            mpi_errno = reduce_fn(recvbuf, NULL, count, datatype,
                                op, root, comm_ptr, errflag);
        }
    }
    if (mpi_errno) {
        /* for communication errors, just record the error but continue */
        *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
        MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
        MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
    }

    /* Now do a mcst-bcast operation with rank0 as the root */
    if(!is_contig) {
        /* Mcast cannot handle non-regular datatypes. We need to pack
         * as bytes before sending it*/ 
        MPIU_CHKLMEM_MALLOC(tmp_buf, void *, nbytes, mpi_errno, "tmp_buf");

        position = 0;
        if (rank == root) {
            mpi_errno = MPIR_Pack_impl(recvbuf, count, datatype, tmp_buf, nbytes,
                                       &position);
            if (mpi_errno)
                MPIR_ERR_POP(mpi_errno);
        }
        mpi_errno = MPIR_Mcast_inter_node_MV2(tmp_buf, nbytes, MPI_BYTE,
                                     root, comm_ptr, errflag);
    } else { 
        mpi_errno = MPIR_Mcast_inter_node_MV2(recvbuf, count, datatype,
                                     root, comm_ptr, errflag);
    } 
   
    if (mpi_errno) {
        /* for communication errors, just record the error but continue */
        *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
        MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
        MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
    }
    
    if (!is_contig) {
        /* We are done, lets pack the data back the way the user 
         * needs it */ 
        if (rank != root) {
            position = 0;
            mpi_errno = MPIR_Unpack_impl(tmp_buf, nbytes, &position, recvbuf,
                                         count, datatype);
            if (mpi_errno)
                MPIR_ERR_POP(mpi_errno);
        }
    }

    /* check to see if the intra-node mcast is not done. 
     * if this is the case, do it either through shmem or knomial */ 
    if(comm_ptr->dev.ch.intra_node_done == 0) { 
        MPID_Comm *shmem_commptr=NULL; 
        MPID_Comm_get_ptr(comm_ptr->dev.ch.shmem_comm, shmem_commptr); 
        int local_size = shmem_commptr->local_size; 
        if (local_size > 1) {
            MPIR_Bcast_MV2(recvbuf, count, datatype, 0, shmem_commptr, errflag);
            if (mpi_errno) {
                /* for communication errors, just record the error but continue */
                *errflag = MPIR_ERR_GET_CLASS(mpi_errno);
                MPIR_ERR_SET(mpi_errno, MPI_ERR_OTHER, "**fail");
                MPIR_ERR_ADD(mpi_errno_ret, mpi_errno);
            }
        }
    } 


  fn_exit:
    MPIU_CHKLMEM_FREEALL();
    MPIR_TIMER_END(coll,allreduce,mcast);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}
#endif /*  #if defined(_MCST_SUPPORT_) */ 

#undef FUNCNAME
#define FUNCNAME MPIR_Allreduce_new_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_new_MV2(const void *sendbuf,
                       void *recvbuf,
                       int count,
                       MPI_Datatype datatype,
                       MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
#ifdef MPID_HAS_HETERO
    int rc;
    int is_homogeneous = 1;
#endif

    int mpi_errno = MPI_SUCCESS;
    int rank = 0, comm_size = 0;
   
    mpi_errno = PMPI_Comm_size(comm_ptr->handle, &comm_size);
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }
    mpi_errno = PMPI_Comm_rank(comm_ptr->handle, &rank);
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }

    if (count == 0) {
        return MPI_SUCCESS;
    }

    MPIU_THREADPRIV_DECL;
    MPIU_THREADPRIV_GET;
    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

    MPI_Aint sendtype_size = 0;
    MPI_Aint nbytes = 0;
    int range = 0, range_threshold = 0, range_threshold_intra = 0;
    int is_two_level = 0;
    int is_commutative = 0;
    MPI_Aint true_lb, true_extent;

    MPID_Datatype_get_size_macro(datatype, sendtype_size);
    nbytes = count * sendtype_size;

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Op *op_ptr;

    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }
    }

#ifdef _ENABLE_CUDA_
    MPI_Aint extent;
    MPID_Datatype_get_extent_macro(datatype, extent);
    int stride = 0;
    stride = count * MPIR_MAX(extent, true_extent);
    int recv_mem_type = 0;
    int send_mem_type = 0;
    char *recv_host_buf = NULL;
    char *send_host_buf = NULL;
    char *temp_recvbuf = recvbuf;
    const char *temp_sendbuf = sendbuf;

    if (mv2_enable_device) {
       recv_mem_type = is_device_buffer(recvbuf);
       if ( sendbuf != MPI_IN_PLACE ){
           send_mem_type = is_device_buffer(sendbuf);
       }
    }

    if(mv2_enable_device && send_mem_type){
        send_host_buf = (char*) MPIU_Malloc(stride);
        MPIU_Memcpy_Device((void *)send_host_buf,
                            (void *)sendbuf, 
                            stride, 
                            deviceMemcpyDeviceToHost);
        sendbuf = send_host_buf;
    }

    if(mv2_enable_device && recv_mem_type){
        recv_host_buf = (char*) MPIU_Malloc(stride);
        MPIU_Memcpy_Device((void *)recv_host_buf,
                            (void *)recvbuf, 
                            stride, 
                            deviceMemcpyDeviceToHost);
        recvbuf = recv_host_buf;
    }
#endif

#ifdef MPID_HAS_HETERO
    if (comm_ptr->is_hetero) {
        is_homogeneous = 0;
    }

    if (!is_homogeneous) {
        /* heterogeneous. To get the same result on all processes, we
           do a reduce to 0 and then broadcast. */
        mpi_errno = MPIR_Reduce_MV2(sendbuf, recvbuf, count, datatype,
                                    op, 0, comm_ptr, errflag);
        /* 
           FIXME: mpi_errno is error CODE, not necessarily the error
           class MPI_ERR_OP.  In MPICH2, we can get the error class 
           with errorclass = mpi_errno & ERROR_CLASS_MASK;
         */
        if (mpi_errno == MPI_ERR_OP || mpi_errno == MPI_SUCCESS) {
            /* Allow MPI_ERR_OP since we can continue from this error */
            rc = MPIR_Bcast_impl(recvbuf, count, datatype, 0, comm_ptr,
                                 errflag);
            if (rc)
                mpi_errno = rc;
        }
    } else
#endif /* MPID_HAS_HETERO */
    {
        /* Search for the corresponding system size inside the tuning table */
        while ((range < (mv2_size_allreduce_tuning_table - 1)) &&
               (comm_size > mv2_allreduce_thresholds_table[range].numproc)) {
            range++;
        }
        /* Search for corresponding inter-leader function */
        /* skip mcast pointers if mcast is not available */
        if(mv2_allreduce_thresholds_table[range].mcast_enabled != 1){
            while ((range_threshold < (mv2_allreduce_thresholds_table[range].size_inter_table - 1)) 
                    && ((mv2_allreduce_thresholds_table[range].
                    inter_leader[range_threshold].MV2_pt_Allreduce_function 
                    == &MPIR_Allreduce_mcst_reduce_redscat_gather_MV2) ||
                    (mv2_allreduce_thresholds_table[range].
                    inter_leader[range_threshold].MV2_pt_Allreduce_function
                    == &MPIR_Allreduce_mcst_reduce_two_level_helper_MV2)
                    )) {
                    range_threshold++;
            }
        }
        while ((range_threshold < (mv2_allreduce_thresholds_table[range].size_inter_table - 1))
               && (nbytes >
               mv2_allreduce_thresholds_table[range].inter_leader[range_threshold].max)
               && (mv2_allreduce_thresholds_table[range].inter_leader[range_threshold].max != -1)) {
               range_threshold++;
        }
        if(mv2_allreduce_thresholds_table[range].is_two_level_allreduce[range_threshold] == 1){
               is_two_level = 1;    
        }
        /* Search for corresponding intra-node function */
        while ((range_threshold_intra <
               (mv2_allreduce_thresholds_table[range].size_intra_table - 1))
                && (nbytes >
                mv2_allreduce_thresholds_table[range].intra_node[range_threshold_intra].max)
                && (mv2_allreduce_thresholds_table[range].intra_node[range_threshold_intra].max !=
                -1)) {
                range_threshold_intra++;
        }

        MV2_Allreduce_function = mv2_allreduce_thresholds_table[range].inter_leader[range_threshold]
                                .MV2_pt_Allreduce_function;

        MV2_Allreduce_intra_function = mv2_allreduce_thresholds_table[range].intra_node[range_threshold_intra]
                                .MV2_pt_Allreduce_function;

        /* check if mcast is ready, otherwise replace mcast with other algorithm */
        if((MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_redscat_gather_MV2)||
          (MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_two_level_helper_MV2)){
#if defined(_MCST_SUPPORT_)
            /* fall back to RD algorithm if:
             *  1) two level is disabled
             *  2) mcast is not ready or supported */
            if(is_two_level != 1 ||
                mv2_use_mcast_allreduce != 1 ||
                comm_ptr->dev.ch.is_mcast_ok != 1 ||
                comm_ptr->dev.ch.shmem_coll_ok !=1)
#endif  /* #if defined(_MCST_SUPPORT_) */
            {
                MV2_Allreduce_function = &MPIR_Allreduce_pt2pt_rd_MV2;
            }
        } 

        if(is_two_level == 1){
#if defined(_MCST_SUPPORT_)
            if((MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_redscat_gather_MV2)||
            (MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_two_level_helper_MV2)){ 

                mpi_errno = MPIR_Allreduce_mcst_MV2(sendbuf, recvbuf, count,
                                               datatype, op, comm_ptr, errflag);
            } else
#endif  /* #if defined(_MCST_SUPPORT_) */
            { 
                /* check if shm is ready, if not use other algorithm first */
                if ((comm_ptr->dev.ch.shmem_coll_ok == 1)
                    && (mv2_enable_shmem_allreduce)
                    && (is_commutative)
                    && (mv2_enable_shmem_collectives)) {
                    mpi_errno = MPIR_Allreduce_two_level_MV2(sendbuf, recvbuf, count,
                                                     datatype, op, comm_ptr, errflag);
                } else {
                    mpi_errno = MPIR_Allreduce_pt2pt_rd_MV2(sendbuf, recvbuf, count,
                                                     datatype, op, comm_ptr, errflag);
                }
            }
        } else { 
            mpi_errno = MV2_Allreduce_function(sendbuf, recvbuf, count,
                                           datatype, op, comm_ptr, errflag);
        }
    } 

#ifdef _ENABLE_CUDA_
    if(mv2_enable_device && recv_mem_type){
        recvbuf = temp_recvbuf;
        MPIU_Memcpy_Device((void *)recvbuf,
                            (void *)recv_host_buf, 
                            stride, 
                            deviceMemcpyHostToDevice);
    }
    if(mv2_enable_device && recv_mem_type){
        if(recv_host_buf){
            MPIU_Free(recv_host_buf);
            recv_host_buf = NULL;
        }
    }
    if(mv2_enable_device && send_mem_type){
        sendbuf = temp_sendbuf;
        if(send_host_buf){
            MPIU_Free(send_host_buf);
            send_host_buf = NULL;
        }
    }
#endif
	comm_ptr->dev.ch.intra_node_done=0;
	
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }
    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

  fn_exit:
    return (mpi_errno);

  fn_fail:
    goto fn_exit;

}


#undef FUNCNAME
#define FUNCNAME MPIR_Allreduce_index_tuned_intra_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_index_tuned_intra_MV2(const void *sendbuf,
                       void *recvbuf,
                       int count,
                       MPI_Datatype datatype,
                       MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
#ifdef MPID_HAS_HETERO
    int rc;
    int is_homogeneous = 1;
#endif

    int mpi_errno = MPI_SUCCESS;
    int comm_size_index = 0;
    int inter_node_algo_index = 0;
    int intra_node_algo_index = 0;
    int local_size = 0;
    int partial_sub_ok = 0;
    int conf_index = 0;
    int table_min_comm_size = 0;
    int table_max_comm_size = 0;
    int table_min_inter_size = 0;
    int table_max_inter_size = 0;
    int table_min_intra_size = 0;
    int table_max_intra_size = 0;
    int last_inter;
    int last_intra;
    int lp2ltn; // largest power of 2 less than n
    int lp2ltn_min;
    MPID_Comm *shmem_commptr = NULL;
    MPI_Comm shmem_comm;
    int rank = 0, comm_size = 0;
    MPI_Aint sendtype_size = 0;
    MPI_Aint nbytes = 0;
    int is_two_level = 0;
    int is_commutative = 0;
    MPI_Aint true_lb, true_extent;
   
    mpi_errno = PMPI_Comm_size(comm_ptr->handle, &comm_size);
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }
    mpi_errno = PMPI_Comm_rank(comm_ptr->handle, &rank);
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }

    if (count == 0) {
        return MPI_SUCCESS;
    }

    MPIU_THREADPRIV_DECL;
    MPIU_THREADPRIV_GET;
    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

    MPID_Datatype_get_size_macro(datatype, sendtype_size);
    nbytes = count * sendtype_size;

#if defined (_SHARP_SUPPORT_)
    int is_socket_aware = mv2_enable_socket_aware_collectives
                          && mv2_use_socket_aware_allreduce
                          && mv2_use_socket_aware_sharp_allreduce;

    //If is_socket_aware == 1 then Sharp allreduce will be called from within the socket-aware 
    //allreduce function
    if (comm_ptr->dev.ch.is_sharp_ok == 1 && nbytes <= mv2_sharp_tuned_msg_size
        && mv2_enable_sharp_coll == 2 && !is_socket_aware && mv2_enable_sharp_allreduce) {
        /* Direct flat algorithm in which every process calls Sharp
         * MV2_ENABLE_SHARP should be set to 2 */
        mpi_errno = MPIR_Sharp_Allreduce_MV2(sendbuf, recvbuf, count,
                                             datatype, op, comm_ptr, errflag);
        if (mpi_errno == MPI_SUCCESS) {
            goto fn_exit;
        }
        /* SHArP collective is not supported, continue without using SHArP */
    }
#endif /* end of defined (_SHARP_SUPPORT_) */

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Op *op_ptr;

    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }
    }

#ifdef _ENABLE_CUDA_
    MPI_Aint extent;
    MPID_Datatype_get_extent_macro(datatype, extent);
    int stride = 0;
    stride = count * MPIR_MAX(extent, true_extent);
    int recv_mem_type = 0;
    int send_mem_type = 0;
    char *recv_host_buf = NULL;
    char *send_host_buf = NULL;
    char *temp_recvbuf = recvbuf;

    if (mv2_enable_device) {
	recv_mem_type = is_device_buffer(recvbuf);
	if ( sendbuf != MPI_IN_PLACE ){
	    send_mem_type = is_device_buffer(sendbuf);
	}
    }

    if(mv2_enable_device && send_mem_type){
        send_host_buf = (char*) MPIU_Malloc(stride);
        MPIU_Memcpy_Device((void *)send_host_buf,
			 (void *)sendbuf, 
			 stride, 
			 deviceMemcpyDeviceToHost);
        sendbuf = send_host_buf;
    }

    if(mv2_enable_device && recv_mem_type){
        recv_host_buf = (char*) MPIU_Malloc(stride);
        MPIU_Memcpy_Device((void *)recv_host_buf,
			 (void *)recvbuf, 
			 stride, 
			 deviceMemcpyDeviceToHost);
        recvbuf = recv_host_buf;
    }
#endif
    
    if (comm_ptr->dev.ch.shmem_coll_ok == 1 && comm_ptr->dev.ch.is_uniform) {
    
        shmem_comm = comm_ptr->dev.ch.shmem_comm;
        MPID_Comm_get_ptr(shmem_comm, shmem_commptr);
        local_size = shmem_commptr->local_size;
        if (mv2_allreduce_indexed_table_ppn_conf[0] == -1) {
            // Indicating user defined tuning
            conf_index = 0;
            goto conf_check_end;
        }
   
        if (mv2_enable_allreduce_skip_small_message_tuning_table_search) {
            ALLREDUCE_SKIP_SMALL_MESSAGE_TUNING_TABLES
        }
        if (mv2_enable_allreduce_skip_large_message_tuning_table_search) {
            ALLREDUCE_SKIP_LARGE_MESSAGE_TUNING_TABLES
        }

use_tables: 
        FIND_PPN_INDEX  (allreduce, local_size,conf_index, partial_sub_ok)

    }

    if (partial_sub_ok != 1) {
        conf_index = mv2_allreduce_indexed_num_ppn_conf/2;
    }

 
conf_check_end:


#ifdef MPID_HAS_HETERO
    if (comm_ptr->is_hetero) {
        is_homogeneous = 0;
    }

    if (!is_homogeneous) {
        /* heterogeneous. To get the same result on all processes, we
           do a reduce to 0 and then broadcast. */
        mpi_errno = MPIR_Reduce_MV2(sendbuf, recvbuf, count, datatype,
                                    op, 0, comm_ptr, errflag);
        /* 
           FIXME: mpi_errno is error CODE, not necessarily the error
           class MPI_ERR_OP.  In MPICH2, we can get the error class 
           with errorclass = mpi_errno & ERROR_CLASS_MASK;
	*/
        if (mpi_errno == MPI_ERR_OP || mpi_errno == MPI_SUCCESS) {
            /* Allow MPI_ERR_OP since we can continue from this error */
            rc = MPIR_Bcast_impl(recvbuf, count, datatype, 0, comm_ptr,
                                 errflag);
            if (rc)
                mpi_errno = rc;
        }
    } else
#endif /* MPID_HAS_HETERO */
	{
    
	    /* Search for the corresponding system size inside the tuning table */
	    /*
	     * Comm sizes progress in powers of 2. Therefore comm_size can just be indexed instead
	     */
	    table_min_comm_size = mv2_allreduce_indexed_thresholds_table[conf_index][0].numproc;
	    table_max_comm_size =
		mv2_allreduce_indexed_thresholds_table[conf_index][mv2_size_allreduce_indexed_tuning_table[conf_index] - 1].
		numproc;
    
	    if (comm_size < table_min_comm_size) {
		/* Comm size smaller than smallest configuration in table: use smallest available */
		comm_size_index = 0;
	    }
	    else if (comm_size > table_max_comm_size) {
		/* Comm size larger than largest configuration in table: use largest available */
		comm_size_index = mv2_size_allreduce_indexed_tuning_table[conf_index] - 1;
	    }
	    else {
		/* Comm size in between smallest and largest configuration: find closest match */
        lp2ltn_min = pow(2, (int)log2(table_min_comm_size));
		if (comm_ptr->dev.ch.is_pof2) {
		    comm_size_index = log2( comm_size / lp2ltn_min );
		}
		else {
		    lp2ltn = pow(2, (int)log2(comm_size));
            comm_size_index = (lp2ltn < lp2ltn_min) ? 0 : log2( lp2ltn / lp2ltn_min );
		}
	    }
	    /* Search for corresponding inter-leader function */
	    /* skip mcast pointers if mcast is not available */
    
	    last_inter = mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].size_inter_table - 1;
	    table_min_inter_size = mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].inter_leader[0].msg_sz;
	    table_max_inter_size = mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].
		inter_leader[last_inter].msg_sz;
	    last_intra = mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].size_intra_table - 1;
	    table_min_intra_size = mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].intra_node[0].msg_sz;
	    table_max_intra_size = mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].
		intra_node[last_intra].msg_sz;
    
	    if (nbytes < table_min_inter_size) {
		/* Msg size smaller than smallest configuration in table: use smallest available */
		inter_node_algo_index = 0;
	    }
	    else if (nbytes > table_max_inter_size) {
		/* Msg size larger than largest configuration in table: use largest available */
		inter_node_algo_index = last_inter;
	    }
	    else {
		/* Msg size in between smallest and largest configuration: find closest match */
		if (pow(2, (int)log2(nbytes)) == nbytes) {
		    inter_node_algo_index = log2( nbytes / table_min_inter_size );
		}
		else {
		    lp2ltn = pow(2, (int)log2(nbytes));
		    inter_node_algo_index = (lp2ltn < table_min_inter_size) ? 0 : log2( lp2ltn / table_min_inter_size );
		}
	    }
    
	    if (nbytes < table_min_intra_size) {
		/* Msg size smaller than smallest configuration in table: use smallest available */
		intra_node_algo_index = 0;
	    }
	    else if (nbytes > table_max_intra_size) {
		/* Msg size larger than largest configuration in table: use largest available */
		intra_node_algo_index = last_intra;
	    }
	    else {
		/* Msg size in between smallest and largest configuration: find closest match */
		if (pow(2, (int)log2(nbytes)) == nbytes) {
		    intra_node_algo_index = log2(nbytes / table_min_intra_size );
		}
		else {
		    lp2ltn = pow(2, (int)log2(nbytes));
		    intra_node_algo_index = (lp2ltn < table_min_intra_size) ? 0 : log2(lp2ltn / table_min_intra_size );
		}
	    }
	    
	    if(mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].
	       is_two_level_allreduce[inter_node_algo_index] == 1){
		is_two_level = 1;    
	    }

	    MV2_Allreduce_function = mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].
		inter_leader[inter_node_algo_index].MV2_pt_Allreduce_function;

	    MV2_Allreduce_intra_function = mv2_allreduce_indexed_thresholds_table[conf_index][comm_size_index].
		intra_node[intra_node_algo_index].MV2_pt_Allreduce_function;

skip_tuning_tables:
        if (MV2_Allreduce_function == &MPIR_Allreduce_topo_aware_hierarchical_MV2) {
            if(nbytes <= mv2_topo_aware_allreduce_max_msg && nbytes >=            
                    mv2_topo_aware_allreduce_min_msg &&                           
                    mv2_enable_topo_aware_collectives &&                          
                    mv2_use_topo_aware_allreduce &&                               
                    comm_ptr->dev.ch.topo_coll_ok == 1 && is_commutative
                    && local_size >= mv2_topo_aware_allreduce_ppn_threshold) {

                return MPIR_Allreduce_topo_aware_hierarchical_MV2(sendbuf,   
                        recvbuf, count, datatype, op, comm_ptr, errflag);         
            }
            else {
                //Fall back to recursive doubling in case the above isn't satisfied.
                return MPIR_Allreduce_pt2pt_rd_MV2(sendbuf, recvbuf, count,
                        datatype, op, comm_ptr, errflag);
            }
        }
        if (MV2_Allreduce_function == &MPIR_Allreduce_socket_aware_two_level_MV2) {
            if(nbytes <= mv2_socket_aware_allreduce_max_msg && nbytes >= mv2_socket_aware_allreduce_min_msg
               && mv2_enable_socket_aware_collectives
               && mv2_use_socket_aware_allreduce && comm_ptr->dev.ch.shmem_coll_ok == 1
               && comm_ptr->dev.ch.use_intra_sock_comm == 1
               && is_commutative && local_size >= mv2_socket_aware_allreduce_ppn_threshold) {

                return MPIR_Allreduce_socket_aware_two_level_MV2(sendbuf, recvbuf, count, datatype, op,
                                                                      comm_ptr, errflag);
            } else {
                return MPIR_Allreduce_pt2pt_rd_MV2(sendbuf, recvbuf, count,
                        datatype, op, comm_ptr, errflag);
            }
        }

	    /* check if mcast is ready, otherwise replace mcast with other algorithm */
	    if((MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_redscat_gather_MV2)||
	       (MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_two_level_helper_MV2)){
#if defined(_MCST_SUPPORT_)
            /* fall back to RD algorithm if:
             *  1) two level is disabled
             *  2) mcast is not ready or supported */
            if(is_two_level != 1 ||
                mv2_use_mcast_allreduce != 1 ||
                comm_ptr->dev.ch.is_mcast_ok != 1 ||
                comm_ptr->dev.ch.shmem_coll_ok != 1)
#endif  /* #if defined(_MCST_SUPPORT_) */
            {
                MV2_Allreduce_function = &MPIR_Allreduce_pt2pt_rd_MV2;
            }
	    }
#if defined (_SHARP_SUPPORT_)
        if (comm_ptr->dev.ch.is_sharp_ok == 1 && nbytes <= mv2_sharp_tuned_msg_size
                && mv2_enable_sharp_coll == 1 && mv2_enable_sharp_allreduce) {
            is_two_level = 1;
            MV2_Allreduce_function = &MPIR_Sharp_Allreduce_MV2;

        }
#endif 
                
	    if(is_two_level == 1) {
#if defined(_MCST_SUPPORT_)
		if((MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_redscat_gather_MV2)||
		   (MV2_Allreduce_function == &MPIR_Allreduce_mcst_reduce_two_level_helper_MV2)){ 

		    mpi_errno = MPIR_Allreduce_mcst_MV2(sendbuf, recvbuf, count,
							datatype, op, comm_ptr, errflag);
		} else
#endif  /* #if defined(_MCST_SUPPORT_) */
		    { 
			/* check if shm is ready, if not use other algorithm first */
			if ((comm_ptr->dev.ch.shmem_coll_ok == 1)
			    && (mv2_enable_shmem_allreduce)
			    && (is_commutative)
			    && (mv2_enable_shmem_collectives)) {
			    mpi_errno = MPIR_Allreduce_two_level_MV2(sendbuf, recvbuf, count,
								     datatype, op, comm_ptr, errflag);
			} else {
			    mpi_errno = MPIR_Allreduce_pt2pt_rd_MV2(sendbuf, recvbuf, count,
								    datatype, op, comm_ptr, errflag);
			}
		    }
	    } else { 
		mpi_errno = MV2_Allreduce_function(sendbuf, recvbuf, count,
						   datatype, op, comm_ptr, errflag);
	    }
	}

#ifdef _ENABLE_CUDA_
    if(mv2_enable_device && recv_mem_type){
        recvbuf = temp_recvbuf;
        MPIU_Memcpy_Device((void *)recvbuf,
			 (void *)recv_host_buf, 
			 stride, 
			 deviceMemcpyHostToDevice);
    }
    if(mv2_enable_device && recv_mem_type){
        if(recv_host_buf){
            MPIU_Free(recv_host_buf);
            recv_host_buf = NULL;
        }
    }
    if(mv2_enable_device && send_mem_type){
        if(send_host_buf){
            MPIU_Free(send_host_buf);
            send_host_buf = NULL;
        }
    }
#endif
    comm_ptr->dev.ch.intra_node_done=0;
	
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }
    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

 fn_exit:
    return (mpi_errno);

 fn_fail:
    goto fn_exit;

}



/* This is flat reduce-scatter-allgather allreduce */
/* This function uses reduce_scatter and allgather colls */
#undef FCNAME
#define FCNAME "MPIR_Allreduce_pt2pt_reduce_scatter_allgather_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_pt2pt_reduce_scatter_allgather_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_TIMER_START(coll,allreduce,reduce_scatter_allgather_colls);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_reduce_scatter_allgather_colls, 1);
    int comm_size, rank;
    int mpi_errno = MPI_SUCCESS;
    int i, *cnts, *disps;
    MPI_Aint true_lb, true_extent, extent;
    void *tmp_buf;
    MPIU_THREADPRIV_DECL;
    MPIU_CHKLMEM_DECL(3);

    if (count == 0) {
        MPIR_TIMER_END(coll,allreduce,reduce_scatter_allgather_colls);
        return MPI_SUCCESS;
    }

    MPIU_THREADPRIV_GET;

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

    /* homogeneous */

    /* set op_errno to 0. stored in perthread structure */
    MPIU_THREADPRIV_FIELD(op_errno) = 0;

    comm_size = comm_ptr->local_size;
    rank = comm_ptr->rank;

    if (count < comm_size) {
        mpi_errno = MPIR_Allreduce_pt2pt_rs_MV2(sendbuf, recvbuf, count, datatype,
                op, comm_ptr, errflag); 
        return mpi_errno;
    } 

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);
 
    {
        /* do a reduce-scatter followed by allgather */

        /* for the reduce-scatter, calculate the count that
           each process receives and the displacement within
           the buffer */

       MPIU_CHKLMEM_MALLOC(cnts, int *, comm_size * sizeof (int), mpi_errno,
                           "counts");
       for (i = 0; i < (comm_size - 1); i++) {
           cnts[i] = count / comm_size;
       }
       cnts[comm_size - 1] = count - (count / comm_size) * (comm_size - 1);

       MPIU_CHKLMEM_MALLOC(tmp_buf, void *,
                           cnts[rank] * (MPIR_MAX(extent, true_extent)), mpi_errno,
                           "temporary buffer");
        
        /* adjust for potential negative lower bound in datatype */
       tmp_buf = (void *) ((char *) tmp_buf - true_lb);
       MPIU_CHKLMEM_MALLOC(disps, int *, comm_size * sizeof (int),
                           mpi_errno, "displacements");
       disps[0] = 0;
       for (i = 1; i < comm_size; i++) {
           disps[i] = disps[i - 1] + cnts[i - 1];
       }

       void * tmp_recvbuf = recvbuf + disps[rank] * (MPIR_MAX(extent, true_extent));
       if (sendbuf != MPI_IN_PLACE) {
            mpi_errno =
               MPIR_Reduce_scatter_MV2(sendbuf, tmp_recvbuf, cnts, datatype, op, comm_ptr,
                   errflag); 
            MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                "**fail");
       } else {
            mpi_errno =
               MPIR_Reduce_scatter_MV2(MPI_IN_PLACE, recvbuf, cnts, datatype, op, comm_ptr,
                   errflag); 
            MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                "**fail");

            if (recvbuf != tmp_recvbuf) {
                /* need to shift the resutls to the location pointed by
                 * tmp_recbuf so that the following Allgather IN_PLACE works properly */
                if (disps[rank] >= cnts[rank]) {
                    /* make sure that there is no overlap between src and dst */
                    mpi_errno =
                       MPIR_Localcopy(recvbuf, cnts[rank], datatype, tmp_recvbuf, cnts[rank],
                                       datatype);
                    MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                        "**fail");
                } else {
                    /* there is overlap between src and dst of shift */
                    void * shift_recvbuf = tmp_recvbuf + disps[rank] *
                        (MPIR_MAX(extent, true_extent)); 
                    MPI_Aint overlapped_count = cnts[rank] - disps[rank];
                    MPIU_Assert(overlapped_count > 0);

                    /* first shift the overlapped data */
                    mpi_errno =
                       MPIR_Localcopy(tmp_recvbuf, overlapped_count, datatype,
                               shift_recvbuf, overlapped_count, datatype);
                    MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                        "**fail");
                    /* now copy the non overlapped data */
                    mpi_errno =
                       MPIR_Localcopy(recvbuf, disps[rank], datatype, tmp_recvbuf, disps[rank],
                                       datatype);
                    MPIR_ERR_CHKANDJUMP((mpi_errno), mpi_errno, MPI_ERR_OTHER,
                                        "**fail");
                }
            }
       }

       if (count % comm_size == 0) {
           MPIR_Allgather_MV2(MPI_IN_PLACE, cnts[rank], datatype, recvbuf, cnts[rank],
                   datatype, comm_ptr, errflag);
       } else {
           MPIR_Allgatherv_MV2(MPI_IN_PLACE, cnts[rank], datatype, recvbuf, cnts,
                disps, datatype, comm_ptr, errflag);
       }
    }
    

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

  fn_exit:
    MPIU_CHKLMEM_FREEALL();
    MPIR_TIMER_END(coll,allreduce,reduce_scatter_allgather_colls);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}







#undef FUNCNAME
#define FUNCNAME MPIR_Allreduce_old_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_old_MV2(const void *sendbuf,
                       void *recvbuf,
                       int count,
                       MPI_Datatype datatype,
                       MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    int mpi_errno = MPI_SUCCESS;

    if (count == 0) {
        return MPI_SUCCESS;
    }

    MPIU_THREADPRIV_DECL;
    MPIU_THREADPRIV_GET;
    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

    int stride = 0, is_commutative = 0;
    MPI_Aint true_lb, true_extent, extent;
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);
    stride = count * MPIR_MAX(extent, true_extent);
    MPID_Op *op_ptr;

    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }
    }

#ifdef _ENABLE_CUDA_
    int recv_mem_type = 0;
    int send_mem_type = 0;
    char *recv_host_buf = NULL;
    char *send_host_buf = NULL;
    char *temp_recvbuf = recvbuf;
    const char *temp_sendbuf = sendbuf;

    if (mv2_enable_device) {
       recv_mem_type = is_device_buffer(recvbuf);
       if ( sendbuf != MPI_IN_PLACE ){
           send_mem_type = is_device_buffer(sendbuf);
       }
    }

    if(mv2_enable_device && send_mem_type){
        send_host_buf = (char*) MPIU_Malloc(stride);
        MPIU_Memcpy_Device((void *)send_host_buf,
                            (void *)sendbuf, 
                            stride, 
                            deviceMemcpyDeviceToHost);
        sendbuf = send_host_buf;
    }

    if(mv2_enable_device && recv_mem_type){
        recv_host_buf = (char*) MPIU_Malloc(stride);
        MPIU_Memcpy_Device((void *)recv_host_buf,
                            (void *)recvbuf, 
                            stride, 
                            deviceMemcpyDeviceToHost);
        recvbuf = recv_host_buf;
    }
#endif

#if defined(_MCST_SUPPORT_)
    if(comm_ptr->dev.ch.is_mcast_ok == 1
       && comm_ptr->dev.ch.shmem_coll_ok == 1
       && mv2_use_mcast_allreduce == 1
       && stride >= mv2_mcast_allreduce_small_msg_size 
       && stride <= mv2_mcast_allreduce_large_msg_size){
        mpi_errno = MPIR_Allreduce_mcst_MV2(sendbuf, recvbuf, count, datatype,
                                     op, comm_ptr, errflag);
    } else
#endif /* #if defined(_MCST_SUPPORT_) */ 
    {
        if ((comm_ptr->dev.ch.shmem_coll_ok == 1)
            && (stride < mv2_coll_param.allreduce_2level_threshold)
            && (mv2_enable_shmem_allreduce)
            && (is_commutative)
            && (mv2_enable_shmem_collectives)) {
            mpi_errno = MPIR_Allreduce_shmem_MV2(sendbuf, recvbuf, count, datatype,
                                                 op, comm_ptr, errflag);

        } else {
       
            mpi_errno = MPIR_Allreduce_pt2pt_old_MV2(sendbuf, recvbuf, count, 
                                            datatype, op, comm_ptr, errflag);

        }
    } 

#ifdef _ENABLE_CUDA_
    if(mv2_enable_device && recv_mem_type){
        recvbuf = temp_recvbuf;
        MPIU_Memcpy_Device((void *)recvbuf,
                            (void *)recv_host_buf, 
                            stride, 
                            deviceMemcpyHostToDevice);
    }
    if(mv2_enable_device && recv_mem_type){
        if(recv_host_buf){
            MPIU_Free(recv_host_buf);
            recv_host_buf = NULL;
        }
    }
    if(mv2_enable_device && send_mem_type){
        sendbuf = temp_sendbuf;
        if(send_host_buf){
            MPIU_Free(send_host_buf);
            send_host_buf = NULL;
        }
    }
#endif
	comm_ptr->dev.ch.intra_node_done=0;
	
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }
    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

  fn_exit:
    return (mpi_errno);

  fn_fail:
    goto fn_exit;

}

#undef FUNCNAME
#define FUNCNAME MPIR_Allreduce_MV2
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_MV2(const void *sendbuf,
                       void *recvbuf,
                       int count,
                       MPI_Datatype datatype,
                       MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    MPIR_T_PVAR_COMM_COUNTER_INC(MV2, mv2_coll_allreduce_subcomm, 1, comm_ptr);
    MPIR_T_PVAR_COMM_TIMER_START(MV2, mv2_coll_timer_allreduce_subcomm, comm_ptr);

    int mpi_errno = MPI_SUCCESS;
    if (count == 0) {
        MPIR_T_PVAR_COMM_TIMER_END(MV2, mv2_coll_timer_allreduce_subcomm, comm_ptr);
        return MPI_SUCCESS;
    }

    if(mv2_use_old_allreduce == 1){
        mpi_errno = MPIR_Allreduce_old_MV2(sendbuf, recvbuf, count,
                                        datatype, op, comm_ptr, errflag);
    } else {
        if (mv2_use_indexed_tuning || mv2_use_indexed_allreduce_tuning) {
            mpi_errno = MPIR_Allreduce_index_tuned_intra_MV2(sendbuf, recvbuf, count,
                    datatype, op, comm_ptr, errflag);
        }
        else {
            mpi_errno = MPIR_Allreduce_new_MV2(sendbuf, recvbuf, count,
                    datatype, op, comm_ptr, errflag);
        }
    }

    MPIR_T_PVAR_COMM_TIMER_END(MV2, mv2_coll_timer_allreduce_subcomm, comm_ptr);

    return (mpi_errno);
}

#undef FCNAME
#define FCNAME "MPIR_Allreduce_pt2pt_ring_wrapper_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_pt2pt_ring_wrapper_MV2(const void *sendbuf, void *recvbuf,
        int count, MPI_Datatype datatype, MPI_Op op, MPID_Comm * comm_ptr,
        MPIR_Errflag_t *errflag) {
    MPIR_TIMER_START(coll,allreduce,pt2pt_ring_wrapper);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_pt2pt_ring_wrapper, 1);

    /* TODO make this algo cyclic process mapping aware */
    int mpi_errno           = MPI_SUCCESS;
    int comm_size           = 0;
    int chunk               = 0;
    int new_count           = 0;
    int remaining_count     = 0;
    MPI_Aint sendtype_size  = 0;

    comm_size       = comm_ptr->local_size;
    mpi_errno       = MPI_SUCCESS;
    chunk           = count / comm_size;
    new_count       = chunk * comm_size;
    remaining_count = count - new_count;

    MPI_Aint true_lb, true_extent, extent;
    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);
    sendtype_size = MPIR_MAX(extent, true_extent);

    if((comm_ptr->local_size == 1) || (comm_ptr->dev.ch.allgather_comm_ok != 0 && comm_ptr->dev.ch.is_blocked == 0)) {
        /* for local size 1 or cyclic hostfiles use red-scat-allgather algorithm */
        mpi_errno = MPIR_Allreduce_pt2pt_rs_MV2(sendbuf, recvbuf, count, datatype,
                op, comm_ptr, errflag);    
        MPIR_TIMER_END(coll,allreduce,pt2pt_ring_wrapper);
        return mpi_errno;
    }

    if (sendbuf != MPI_IN_PLACE)
        mpi_errno = MPIR_Allreduce_pt2pt_ring_MV2(sendbuf, recvbuf, new_count, datatype,
            op, comm_ptr, errflag);
    else  
        mpi_errno = MPIR_Allreduce_pt2pt_ring_inplace_MV2(sendbuf, recvbuf, new_count, datatype,
            op, comm_ptr, errflag);

    if (mpi_errno != MPI_SUCCESS || (remaining_count == 0)) {
        MPIR_TIMER_END(coll,allreduce,pt2pt_ring_wrapper);
        return mpi_errno;
    }

    /* now Allreduce the remaining count */ 
    if (sendbuf != MPI_IN_PLACE) {
        mpi_errno = MPIR_Allreduce_pt2pt_rs_MV2(sendbuf+new_count*sendtype_size,
                recvbuf+new_count*sendtype_size, remaining_count, datatype,
                op, comm_ptr, errflag);
        MPIR_TIMER_END(coll,allreduce,pt2pt_ring_wrapper);
        return mpi_errno;
    } else {
        mpi_errno = MPIR_Allreduce_pt2pt_rs_MV2(MPI_IN_PLACE,
                recvbuf+new_count*sendtype_size, remaining_count, datatype,
                op, comm_ptr, errflag);
        MPIR_TIMER_END(coll,allreduce,pt2pt_ring_wrapper);
        return mpi_errno;
    }
}

/* This is flat ring-allreduce  allreduce */
#undef FCNAME
#define FCNAME "MPIR_Allreduce_pt2pt_ring_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_pt2pt_ring_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    int comm_size, rank;
    int mpi_errno = MPI_SUCCESS;
    MPI_Aint true_lb, true_extent, extent;
    MPI_User_function *uop;
    MPID_Op *op_ptr;
    int is_commutative;
#ifdef HAVE_CXX_BINDING
    int is_cxx_uop = 0;
#endif
    MPIU_THREADPRIV_DECL;

    if (count == 0) {
        mpi_errno = MPI_SUCCESS;
        goto fn_exit;
    }

    MPIU_THREADPRIV_GET;

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

    /* homogeneous */

    /* set op_errno to 0. stored in perthread structure */
    MPIU_THREADPRIV_FIELD(op_errno) = 0;

    comm_size = comm_ptr->local_size;
    rank = comm_ptr->rank;

    /* Get the operator and check whether it is commutative or not */
    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
        /* get the function by indexing into the op table */
        uop = MPIR_Op_table[op % 16 - 1];
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }

#if defined(HAVE_CXX_BINDING)
        if (op_ptr->language == MPID_LANG_CXX) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
            is_cxx_uop = 1;
        } else
#endif                          /* defined(HAVE_CXX_BINDING) */
        if (op_ptr->language == MPID_LANG_C) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
        } else {
            uop = (MPI_User_function *) op_ptr->function.f77_function;
        }
    }
    int context_id = (comm_ptr->comm_kind == MPID_INTRACOMM) ?
                MPID_CONTEXT_INTRA_COLL : MPID_CONTEXT_INTER_COLL;

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);

    MPI_Aint type_size = MPIR_MAX(extent, true_extent);


    if (count % comm_size != 0 || sendbuf == MPI_IN_PLACE ||
            count < comm_size || !is_commutative) {
        return MPIR_Allreduce_pt2pt_rs_MV2(sendbuf, recvbuf, count, datatype,
                op, comm_ptr, errflag);
    }
    MPIR_TIMER_START(coll,allreduce,pt2pt_ring);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_pt2pt_ring, 1);
    //memset(recvbuf, 0, type_size*count);

    MPIU_Assert((count * type_size) % comm_size == 0);
    MPI_Aint chunk_size  = (count * type_size) / comm_size;
    int chunk_count = count / comm_size;

    {
        /* do a reduce-scatter followed by allgather */
        MPIU_Assert(sendbuf != MPI_IN_PLACE);

        int i = 0;
        int left    = (rank - 1 + comm_size) % comm_size;
        int right   = (rank + 1) % comm_size;
        void * recv_chunk = NULL, * send_chunk = NULL, * comp_chunk = NULL;
        MPID_Request *recv_req_ptr = NULL, *send_req_ptr = NULL;

        for (i = 1; i < comm_size; i++) {

            MPID_Request *recv_req_ptr = NULL, *send_req_ptr = NULL;

            /* iteration 1: read chunk murank - 1 from myrank - 1 and do compute locally, at the
             * same time, send chunk myrank + 1 to myrank + 1 */

            if (i == 1 && sendbuf != MPI_IN_PLACE)
                send_chunk  = (void*) sendbuf + ((rank - (i-1) + comm_size) % comm_size)*chunk_size;
            else 
                send_chunk  = recvbuf + ((rank - (i-1) + comm_size) % comm_size)*chunk_size;

            recv_chunk  = recvbuf + ((rank - i + comm_size) % comm_size)*chunk_size;

            comp_chunk  = (void*) sendbuf + ((rank - i + comm_size) % comm_size)*chunk_size;

            /*
            PRINT_DEBUG(1, "i = %d, sending chunk=%d to rank=%d and receiving chunk=%d from rank=%d and doing compute on chunk=%d \n",
                   i, ((rank - (i-1) + comm_size) % comm_size), right, ((rank -
                            i + comm_size) % comm_size), left, ((rank - i +
                                comm_size) % comm_size));
            */
            MPIR_PVAR_INC(allreduce, pt2pt_ring, recv, chunk_count, datatype);
            mpi_errno = MPID_Irecv(recv_chunk, chunk_count, datatype, left, 11, comm_ptr,
                    context_id, &recv_req_ptr);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            MPIR_PVAR_INC(allreduce, pt2pt_ring, send, chunk_count, datatype);
            mpi_errno = MPID_Isend(send_chunk, chunk_count, datatype, right, 11,
                                   comm_ptr, context_id, &send_req_ptr);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            mpi_errno = MPIC_Wait(recv_req_ptr, errflag);
            if (mpi_errno) MPIR_ERR_POPFATAL(mpi_errno);

            /* now do the computation with some MPI_Test (TODO) in between */
#if defined(HAVE_CXX_BINDING)
            if (is_cxx_uop) {
                (*MPIR_Process.cxx_call_op_fn) (comp_chunk, recv_chunk,
                                                chunk_count, datatype, uop);
            } else {
#endif                          /* defined(HAVE_CXX_BINDING) */
                (*uop) (comp_chunk, recv_chunk, &chunk_count, &datatype);
#if defined(HAVE_CXX_BINDING)
            }
#endif

            mpi_errno = MPIC_Wait(send_req_ptr, errflag);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            MPID_Request_release(send_req_ptr);
            MPID_Request_release(recv_req_ptr);
        }



        /*
        if (rank == 0) {
        PRINT_DEBUG(1, "Final Results count = %d\n", count);
        for (i = 0; i < count; i++) {
            fprintf(stderr, "%d ", ((float*)recvbuf)[i]);
        }
        PRINT_DEBUG(1, "\n");
        }
        */
           /* Allgather on recvbuf with  one elemented  shifted to right */

        for (i = 1; i < comm_size; i++) {
        
            recv_chunk = ((rank - (i-1) + comm_size)%comm_size)*chunk_size + recvbuf;
            MPIR_PVAR_INC(allreduce, pt2pt_ring, recv, chunk_count, datatype);
            mpi_errno = MPID_Irecv(recv_chunk, chunk_count, datatype, left, 11, comm_ptr,
                    context_id, &recv_req_ptr);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            send_chunk = ((rank - (i-2) + comm_size)%comm_size)*chunk_size + recvbuf;
            MPIR_PVAR_INC(allreduce, pt2pt_ring, send, chunk_count, datatype);
            mpi_errno = MPID_Isend(send_chunk, chunk_count, datatype, right, 11,
                                   comm_ptr, context_id, &send_req_ptr);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            mpi_errno = MPIC_Wait(recv_req_ptr, errflag);
            if (mpi_errno) MPIR_ERR_POPFATAL(mpi_errno);
            mpi_errno = MPIC_Wait(send_req_ptr, errflag);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            MPID_Request_release(send_req_ptr);
            MPID_Request_release(recv_req_ptr);
        }
    }

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

  fn_exit:
    MPIR_TIMER_END(coll,allreduce,pt2pt_ring);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}


/* This is flat ring-allreduce  allreduce with MPI_INPLACE for sendbuf */
#undef FCNAME
#define FCNAME "MPIR_Allreduce_pt2pt_ring_MV2"
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Allreduce_pt2pt_ring_inplace_MV2(const void *sendbuf,
                             void *recvbuf,
                             int count,
                             MPI_Datatype datatype,
                             MPI_Op op, MPID_Comm * comm_ptr, MPIR_Errflag_t *errflag)
{
    int comm_size, rank;
    int mpi_errno = MPI_SUCCESS;
    MPI_Aint true_lb, true_extent, extent;
    MPI_User_function *uop;
    MPID_Op *op_ptr;
    int is_commutative;
#ifdef HAVE_CXX_BINDING
    int is_cxx_uop = 0;
#endif
    MPIU_THREADPRIV_DECL;
    MPIU_CHKLMEM_DECL(3);

    if (count == 0) {
        return MPI_SUCCESS;
    }

    MPIU_THREADPRIV_GET;

    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_ENTER(comm_ptr);

    /* homogeneous */

    /* set op_errno to 0. stored in perthread structure */
    MPIU_THREADPRIV_FIELD(op_errno) = 0;

    comm_size = comm_ptr->local_size;
    rank = comm_ptr->rank;

    /* Get the operator and check whether it is commutative or not */
    if (HANDLE_GET_KIND(op) == HANDLE_KIND_BUILTIN) {
        is_commutative = 1;
        /* get the function by indexing into the op table */
        uop = MPIR_Op_table[op % 16 - 1];
    } else {
        MPID_Op_get_ptr(op, op_ptr);
        if (op_ptr->kind == MPID_OP_USER_NONCOMMUTE) {
            is_commutative = 0;
        } else {
            is_commutative = 1;
        }

#if defined(HAVE_CXX_BINDING)
        if (op_ptr->language == MPID_LANG_CXX) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
            is_cxx_uop = 1;
        } else
#endif                          /* defined(HAVE_CXX_BINDING) */
        if (op_ptr->language == MPID_LANG_C) {
            uop = (MPI_User_function *) op_ptr->function.c_function;
        } else {
            uop = (MPI_User_function *) op_ptr->function.f77_function;
        }
    }
    int context_id = (comm_ptr->comm_kind == MPID_INTRACOMM) ?
                MPID_CONTEXT_INTRA_COLL : MPID_CONTEXT_INTER_COLL;

    MPIR_Type_get_true_extent_impl(datatype, &true_lb, &true_extent);
    MPID_Datatype_get_extent_macro(datatype, extent);

    MPI_Aint type_size = MPIR_MAX(extent, true_extent);


    if (count % comm_size != 0 || sendbuf == MPI_IN_PLACE ||
            count < comm_size || !is_commutative) {
        mpi_errno = MPIR_Allreduce_pt2pt_rs_MV2(sendbuf, recvbuf, count, datatype,
                op, comm_ptr, errflag);
        return mpi_errno;
    }
    MPIR_TIMER_START(coll,allreduce,pt2pt_ring_inplace);
    MPIR_T_PVAR_COUNTER_INC(MV2, mv2_coll_allreduce_pt2pt_ring_inplace, 1);
    //memset(recvbuf, 0, type_size*count);

    MPIU_Assert((count * type_size) % comm_size == 0);
    MPI_Aint chunk_size  = (count * type_size) / comm_size;
    int chunk_count = count / comm_size;
    

    /* TODO: try to do this malloc only once for entire job run */
    void  * temp_buf = MPIU_Malloc(chunk_size);

    {
        /* do a reduce-scatter followed by allgather */
        MPIU_Assert(sendbuf == MPI_IN_PLACE);

        int i = 0;
        int left    = (rank - 1 + comm_size) % comm_size;
        int right   = (rank + 1) % comm_size;
        void * recv_chunk = NULL, * send_chunk = NULL, * comp_chunk = NULL;
        MPID_Request *recv_req_ptr = NULL, *send_req_ptr = NULL;

        //fprintf(stderr, "starting the  main loop\n");
        for (i = 1; i < comm_size; i++) {

            MPID_Request *recv_req_ptr = NULL, *send_req_ptr = NULL;

            /* iteration 1: read chunk murank - 1 from myrank - 1 and do compute locally, at the
             * same time, send chunk myrank + 1 to myrank + 1 */

            send_chunk  = recvbuf + ((rank - (i-1) + comm_size) % comm_size)*chunk_size;

            //recv_chunk  = recvbuf + ((rank - i + comm_size) % comm_size)*chunk_size;
            recv_chunk = temp_buf;

            //comp_chunk  = sendbuf + ((rank - i + comm_size) % comm_size)*chunk_size;
            comp_chunk  = recvbuf + ((rank - i + comm_size) % comm_size)*chunk_size;

            /*
            PRINT_DEBUG(1, "i = %d, sending chunk=%d to rank=%d and receiving chunk=%d from rank=%d and doing compute on chunk=%d \n",
                   i, ((rank - (i-1) + comm_size) % comm_size), right, ((rank -
                            i + comm_size) % comm_size), left, ((rank - i +
                                comm_size) % comm_size));
            */
            MPIR_PVAR_INC(allreduce, pt2pt_ring_inplace, recv, chunk_count, datatype);
            mpi_errno = MPID_Irecv(recv_chunk, chunk_count, datatype, left, 11, comm_ptr,
                    context_id, &recv_req_ptr);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            MPIR_PVAR_INC(allreduce, pt2pt_ring_inplace, send, chunk_count, datatype);
            mpi_errno = MPID_Isend(send_chunk, chunk_count, datatype, right, 11,
                                   comm_ptr, context_id, &send_req_ptr);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            mpi_errno = MPIC_Wait(recv_req_ptr, errflag);
            if (mpi_errno) MPIR_ERR_POPFATAL(mpi_errno);

            /* now do the computation with some MPI_Test (TODO) in between */
#if defined(HAVE_CXX_BINDING)
            if (is_cxx_uop) {
                (*MPIR_Process.cxx_call_op_fn) (recv_chunk, comp_chunk, 
                                                chunk_count, datatype, uop);
            } else {
#endif                          /* defined(HAVE_CXX_BINDING) */
                (*uop) (recv_chunk, comp_chunk, &chunk_count, &datatype);
#if defined(HAVE_CXX_BINDING)
            }
#endif

            mpi_errno = MPIC_Wait(send_req_ptr, errflag);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            MPID_Request_release(send_req_ptr);
            MPID_Request_release(recv_req_ptr);

          //  fprintf(stderr, "finished it = %d \n");
        }



        /*
        if (rank == 0) {
        PRINT_DEBUG(1, "Final Results count = %d\n", count);
        for (i = 0; i < count; i++) {
            fprintf(stderr, "%d ", ((float*)recvbuf)[i]);
        }
        PRINT_DEBUG(1, "\n");
        }
        */
           /* Allgather on recvbuf with  one elemented  shifted to right */
          // MPIR_Allgather_MV2(MPI_IN_PLACE, chunk_count, datatype, recvbuf, chunk_count,
            //       datatype, comm_ptr, errflag);
           

        for (i = 1; i < comm_size; i++) {
        
            recv_chunk = ((rank - (i-1) + comm_size)%comm_size)*chunk_size + recvbuf;
            MPIR_PVAR_INC(allreduce, pt2pt_ring_inplace, recv, chunk_count, datatype);
            mpi_errno = MPID_Irecv(recv_chunk, chunk_count, datatype, left, 11, comm_ptr,
                    context_id, &recv_req_ptr);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            send_chunk = ((rank - (i-2) + comm_size)%comm_size)*chunk_size + recvbuf;
            MPIR_PVAR_INC(allreduce, pt2pt_ring_inplace, send, chunk_count, datatype);
            mpi_errno = MPID_Isend(send_chunk, chunk_count, datatype, right, 11,
                                   comm_ptr, context_id, &send_req_ptr);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            mpi_errno = MPIC_Wait(recv_req_ptr, errflag);
            if (mpi_errno) MPIR_ERR_POPFATAL(mpi_errno);
            mpi_errno = MPIC_Wait(send_req_ptr, errflag);
            if (mpi_errno) MPIR_ERR_POP(mpi_errno);

            MPID_Request_release(send_req_ptr);
            MPID_Request_release(recv_req_ptr);
        }
    }




    /* check if multiple threads are calling this collective function */
    MPIDU_ERR_CHECK_MULTIPLE_THREADS_EXIT(comm_ptr);

    if (MPIU_THREADPRIV_FIELD(op_errno)) {
        mpi_errno = MPIU_THREADPRIV_FIELD(op_errno);
    }

  fn_exit:
    MPIU_CHKLMEM_FREEALL();
    MPIU_Free(temp_buf);
    MPIR_TIMER_END(coll,allreduce,pt2pt_ring);
    return (mpi_errno);

  fn_fail:
    goto fn_exit;
}
