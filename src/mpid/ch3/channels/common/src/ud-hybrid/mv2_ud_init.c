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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "mpichconf.h"
#include "mpimem.h"
#include "mv2_ud.h"
#include "debug_utils.h"
#include "rdma_impl.h"

extern mv2_MPIDI_CH3I_RDMA_Process_t mv2_MPIDI_CH3I_RDMA_Process;

/* create UD context */
struct ibv_qp * mv2_ud_create_qp(mv2_ud_qp_info_t *qp_info, int hca_index)
{
    struct ibv_qp *qp;
    struct ibv_qp_init_attr init_attr;
 
    memset(&init_attr, 0, sizeof(struct ibv_qp_init_attr));
    init_attr.send_cq = qp_info->send_cq;
    init_attr.recv_cq = qp_info->recv_cq;
    init_attr.cap.max_send_wr = qp_info->cap.max_send_wr;
    
    if (qp_info->srq) {
        init_attr.srq = qp_info->srq;
        init_attr.cap.max_recv_wr = 0;
    } else {    
        init_attr.cap.max_recv_wr = qp_info->cap.max_recv_wr;
    }

    init_attr.cap.max_send_sge = qp_info->cap.max_send_sge;
    init_attr.cap.max_recv_sge = qp_info->cap.max_recv_sge;
    init_attr.cap.max_inline_data = qp_info->cap.max_inline_data;
    init_attr.qp_type = IBV_QPT_UD;

    qp = ibv_ops.create_qp(qp_info->pd, &init_attr);
    if(!qp)
    {
        fprintf(stderr,"error in creating UD qp\n");
        return NULL;
    }
    rdma_max_inline_size = init_attr.cap.max_inline_data;
    
    if (mv2_ud_qp_transition(qp, hca_index)) {
        return NULL;
    }

    PRINT_DEBUG(DEBUG_UD_verbose>0," UD QP:%p qpn:%d \n",qp, qp->qp_num);

    return qp;
}

int mv2_ud_qp_transition(struct ibv_qp *qp, int hca_index)
{
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(struct ibv_qp_attr));

    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = mv2_MPIDI_CH3I_RDMA_Process.ports[hca_index][0];
    attr.qkey = rdma_default_qkey;

    if (ibv_ops.modify_qp(qp, &attr,
                IBV_QP_STATE |
                IBV_QP_PKEY_INDEX |
                IBV_QP_PORT | IBV_QP_QKEY)) {
            fprintf(stderr,"Failed to modify QP to INIT\n");
            return 1;
    }    
        
    memset(&attr, 0, sizeof(struct ibv_qp_attr));

    attr.qp_state = IBV_QPS_RTR;
    if (ibv_ops.modify_qp(qp, &attr, IBV_QP_STATE)) {
            fprintf(stderr, "Failed to modify QP to RTR\n");
            return 1;
    }   

    memset(&attr, 0, sizeof(struct ibv_qp_attr));

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = rdma_default_psn;
    if (ibv_ops.modify_qp(qp, &attr,
                IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    return 0;

}

mv2_ud_ctx_t* mv2_ud_create_ctx (mv2_ud_qp_info_t *qp_info, int hca_index)
{
    mv2_ud_ctx_t *ctx;

    ctx = MPIU_Malloc( sizeof(mv2_ud_ctx_t) );
    if (!ctx){
        fprintf( stderr, "%s:no memory!\n", __func__ );
        return NULL;
    }
    memset( ctx, 0, sizeof(mv2_ud_ctx_t) );

    ctx->qp = mv2_ud_create_qp(qp_info, hca_index);
    if(!ctx->qp) {
        fprintf(stderr, "Error in creating UD QP\n");
        return NULL;
    }

    return ctx;
}

/* destroy ud context */
void mv2_ud_destroy_ctx (mv2_ud_ctx_t *ctx)
{
    if (ctx->qp) {
        ibv_ops.destroy_qp(ctx->qp);
    }
    MPIU_Free(ctx);
}

