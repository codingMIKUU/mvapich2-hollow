
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
#ifndef _IBV_SEND_INLINE_H_
#define _IBV_SEND_INLINE_H_

#include "mpichconf.h"
#include "mpiimpl.h"
#include <mpimem.h>
#include "rdma_impl.h"
#include "ibv_impl.h"
#include "vbuf.h"
#include "upmi.h"
#include "mpiutil.h"
#include "dreg.h"
#include "debug_utils.h"
#if defined(_MCST_SUPPORT_)
#include "ibv_mcast.h"
#endif 

#define SET_CREDIT(header, vc, rail, transport)                             \
{                                                                           \
    if (transport  == IB_TRANSPORT_RC)  {                                   \
        vc->mrail.rfp.ptail_RDMA_send += header->rdma_credit;               \
        if (vc->mrail.rfp.ptail_RDMA_send >= num_rdma_buffer)               \
            vc->mrail.rfp.ptail_RDMA_send -= num_rdma_buffer;               \
        vc->mrail.srp.credits[rail].remote_cc = header->remote_credit;      \
        vc->mrail.srp.credits[rail].remote_credit += header->vbuf_credit;   \
    } else {                                                                \
    }                                                                       \
}

MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_vbuf_allocated);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_vbuf_freed);
MPIR_T_PVAR_ULONG_LEVEL_DECL_EXTERN(MV2, mv2_vbuf_available);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ud_vbuf_allocated);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ud_vbuf_freed);
MPIR_T_PVAR_ULONG_LEVEL_DECL_EXTERN(MV2, mv2_ud_vbuf_available);

#define CHECK_CQ_OVERFLOW(_cq_overflow, _vc, _rail)                     \
    if (likely(mv2_use_ib_channel == 1)) {                              \
        (_cq_overflow) = check_cq_overflow_for_ib((_vc), (_rail));      \
    } else {                                                            \
        (_cq_overflow) = check_cq_overflow_for_iwarp((_vc), (_rail));   \
    }

#define INCR_EXT_SENDQ_SIZE(_c,_rail)                                   \
    ++rdma_global_ext_sendq_size;                                       \
    ++(_c)->mrail.rails[(_rail)].ext_sendq_size;

#define DECR_EXT_SENDQ_SIZE(_c,_rail)                                   \
    --rdma_global_ext_sendq_size;                                       \
    --(_c)->mrail.rails[(_rail)].ext_sendq_size; 

#define FLUSH_SQUEUE(_vc) {                                             \
    if(NULL != (_vc)->mrail.coalesce_vbuf) {                            \
        MRAILI_Ext_sendq_send(_vc, (_vc)->mrail.coalesce_vbuf->rail);   \
    }                                                                   \
}

#define FLUSH_RAIL(_vc,_rail) {                                         \
    if(unlikely(NULL != (_vc)->mrail.coalesce_vbuf &&                   \
                (_vc)->mrail.coalesce_vbuf->rail == _rail)) {           \
        MRAILI_Ext_sendq_send(_vc, (_vc)->mrail.coalesce_vbuf->rail);   \
        (_vc)->mrail.coalesce_vbuf = NULL;                              \
    }                                                                   \
}

static inline int check_cq_overflow_for_ib(MPIDI_VC_t *c, int rail)
{
    return 0;
}

static inline int check_cq_overflow_for_iwarp(MPIDI_VC_t *c, int rail)
{
    char cq_overflow = 0;

    if(rdma_iwarp_use_multiple_cq) {
      if ((NULL != c->mrail.rails[rail].send_cq_hndl) &&
          (mv2_MPIDI_CH3I_RDMA_Process.global_used_send_cq >=
           rdma_default_max_cq_size)) {
          /* We are monitoring CQ's and there is CQ overflow */
          cq_overflow = 1;
      }
    } else {
      if ((NULL != c->mrail.rails[rail].send_cq_hndl) &&
          ((mv2_MPIDI_CH3I_RDMA_Process.global_used_send_cq +
            mv2_MPIDI_CH3I_RDMA_Process.global_used_recv_cq) >=
            rdma_default_max_cq_size)) {
          /* We are monitoring CQ's and there is CQ overflow */
          cq_overflow = 1;
      }
    }

    return cq_overflow;
}

static inline int MRAILI_Coalesce_ok(MPIDI_VC_t * vc, int rail)
{
    if (unlikely(rdma_use_coalesce && 
                (vc->mrail.outstanding_eager_vbufs >= rdma_coalesce_threshold || 
                 vc->mrail.rails[rail].send_wqes_avail == 0) &&
                (mv2_MPIDI_CH3I_RDMA_Process.has_srq || 
                 (vc->mrail.srp.credits[rail].remote_credit > 0 && 
                  NULL == &(vc->mrail.srp.credits[rail].backlog))))) {
        return 1;
    }

    return 0;
}

/* to handle Send Q overflow, we maintain an extended send queue
 * above the HCA.  This permits use to have a virtually unlimited send Q depth
 * (limited by number of vbufs available for send)
 */
#undef FUNCNAME
#define FUNCNAME MRAILI_Ext_sendq_enqueue
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline void MRAILI_Ext_sendq_enqueue(MPIDI_VC_t *c,
        int rail, 
        vbuf * v)          
{
    MPIDI_STATE_DECL(MPID_STATE_MRAILI_EXT_SENDQ_ENQUEUE);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_EXT_SENDQ_ENQUEUE);

    v->desc.next = NULL;

    if (c->mrail.rails[rail].ext_sendq_head == NULL) {
        c->mrail.rails[rail].ext_sendq_head = v;
    } else {                                     
        c->mrail.rails[rail].ext_sendq_tail->desc.next = v;
    }
    c->mrail.rails[rail].ext_sendq_tail = v;  
    PRINT_DEBUG(DEBUG_SEND_verbose>1, "[ibv_send] enqueue, head %p, tail %p\n", 
            c->mrail.rails[rail].ext_sendq_head, 
            c->mrail.rails[rail].ext_sendq_tail); 

    INCR_EXT_SENDQ_SIZE(c, rail)

        if (c->mrail.rails[rail].ext_sendq_size > rdma_rndv_ext_sendq_size) {
#ifdef _ENABLE_CUDA_
            if (!rdma_enable_cuda)
#endif
            {
                c->force_rndv = 1;
            }
        }

    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_EXT_SENDQ_ENQUEUE);
}

/* dequeue and send as many as we can from the extended send queue
 * this is called in each function which may post send prior to it attempting
 * its send, hence ordering of sends is maintained
 */
#undef FUNCNAME
#define FUNCNAME MRAILI_Ext_sendq_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline void MRAILI_Ext_sendq_send(MPIDI_VC_t *c, int rail)    
{
    vbuf *v = NULL;
    char cq_overflow = 0;

    MPIDI_STATE_DECL(MPID_STATE_MRAILI_EXT_SENDQ_SEND);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_EXT_SENDQ_SEND);

#ifdef _ENABLE_HOLLOW_RC_
    /* A transient shared-SQ failure is retried once per progress pass.  Clear
     * the marker before the attempt; IBV_POST_SR sets it again if the shared
     * reservation window is still full. */
    if (c->mrail.rails[rail].hollow_sq_blocked) {
        c->mrail.rails[rail].hollow_sq_blocked = 0;
        __sync_fetch_and_sub(
            &mv2_MPIDI_CH3I_RDMA_Process.hollow_blocked_rails, 1);
    }
#endif

#ifdef _ENABLE_XRC_
    MPIU_Assert (!USE_XRC || VC_XST_ISUNSET (c, XF_INDIRECT_CONN));
#endif

    CHECK_CQ_OVERFLOW(cq_overflow, c, rail);

    while (c->mrail.rails[rail].send_wqes_avail
            && !cq_overflow
#ifdef _ENABLE_HOLLOW_RC_
            && !c->mrail.rails[rail].hollow_sq_blocked
#endif
            && c->mrail.rails[rail].ext_sendq_head) {
        v = c->mrail.rails[rail].ext_sendq_head;
        c->mrail.rails[rail].ext_sendq_head = v->desc.next;
        if (v == c->mrail.rails[rail].ext_sendq_tail) {
            c->mrail.rails[rail].ext_sendq_tail = NULL;
        }
        v->desc.next = NULL;
        --c->mrail.rails[rail].send_wqes_avail;                

        DECR_EXT_SENDQ_SIZE(c, rail)

        if (unlikely(1 == v->coalesce)) {
            PRINT_DEBUG(DEBUG_SEND_verbose>1, "Sending coalesce vbuf %p\n", v);
            MPIDI_CH3I_MRAILI_Pkt_comm_header *p = v->pheader;
            vbuf_init_send(v, v->content_size, v->rail);

            p->seqnum = v->seqnum;

            if(c->mrail.coalesce_vbuf == v) {
                c->mrail.coalesce_vbuf = NULL;
            }
        } 

        IBV_POST_SR(v, c, rail, "Mrail_post_sr (MRAILI_Ext_sendq_send)");
    }

    PRINT_DEBUG(DEBUG_SEND_verbose>1,  "[ibv_send] dequeue, head %p, tail %p\n",
            c->mrail.rails[rail].ext_sendq_head,
            c->mrail.rails[rail].ext_sendq_tail);

    if (c->mrail.rails[rail].ext_sendq_size <= rdma_rndv_ext_sendq_size) {
        c->force_rndv = 0;
    }

    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_EXT_SENDQ_SEND);
}

#ifdef _ENABLE_HOLLOW_RC_
static inline void MRAILI_Hollow_retry_blocked_inline(void)
{
    int i;

    if (__atomic_load_n(
            &mv2_MPIDI_CH3I_RDMA_Process.hollow_blocked_rails,
            __ATOMIC_ACQUIRE) <= 0)
        return;

    for (i = 0; i < mv2_MPIDI_CH3I_RDMA_Process.hollow_retry_capacity;
         i++) {
        MPIDI_VC_t *vc = __atomic_load_n(
            &mv2_MPIDI_CH3I_RDMA_Process.hollow_retry_set[i],
            __ATOMIC_ACQUIRE);

        if (!vc || !vc->mrail.rails ||
            !vc->mrail.rails[0].hollow_sq_blocked)
            continue;
        MRAILI_Ext_sendq_send(vc, 0);
    }
}
#else
static inline void MRAILI_Hollow_retry_blocked_inline(void) { }
#endif

#undef FUNCNAME
#define FUNCNAME MRAILI_Get_Vbuf
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline vbuf * MRAILI_Get_Vbuf(MPIDI_VC_t * vc, size_t pkt_len)
{
    int rail = 0;
    vbuf* temp_v = NULL;

    MPIDI_STATE_DECL(MPID_STATE_MRAILI_GET_VBUF);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_GET_VBUF);

    if (unlikely(NULL != vc->mrail.coalesce_vbuf)) {
        int coalesc_buf_size = 0;
#if defined(_ENABLE_UD_) || defined(_MCST_SUPPORT_)
        if (!vc->mrail.coalesce_vbuf->pool_index) {
            coalesc_buf_size = MRAIL_MAX_UD_SIZE;
        } else
#endif
        {
            coalesc_buf_size = ((vbuf_pool_t*)vc->mrail.coalesce_vbuf->pool_index)->buf_size;
        }

        if((coalesc_buf_size - vc->mrail.coalesce_vbuf->content_size) 
                >= pkt_len) {
            PRINT_DEBUG(DEBUG_SEND_verbose>1, "returning back a coalesce buffer\n");
            return vc->mrail.coalesce_vbuf;
        } else {
            FLUSH_SQUEUE(vc);
            vc->mrail.coalesce_vbuf = NULL;
            PRINT_DEBUG(DEBUG_SEND_verbose>1, "Send out the coalesce vbuf\n");
        }
    }

    rail = MRAILI_Send_select_rail(vc, pkt_len);
    /* if there already wasn't a vbuf that could
     * hold our packet we need to allocate a 
     * new one
     */
    if (likely(NULL == temp_v)) {
        /* are we trying to coalesce? If so, place
         * it as the new coalesce vbuf and add it
         * to the extended sendq
         */

        if(unlikely(MRAILI_Coalesce_ok(vc, rail)) &&
                (pkt_len*2 <= DEFAULT_MEDIUM_VBUF_SIZE)) {
            MRAILI_Get_buffer(vc, temp_v, DEFAULT_MEDIUM_VBUF_SIZE);
            vc->mrail.coalesce_vbuf = temp_v;

            temp_v->seqnum = vc->mrail.seqnum_next_tosend;
            vc->mrail.seqnum_next_tosend++;

            temp_v->coalesce = 1;
            temp_v->rail = rail;
            MRAILI_Ext_sendq_enqueue(vc, temp_v->rail, temp_v); 
            PRINT_DEBUG(DEBUG_SEND_verbose>1, "coalesce is ok\n");

            if(!mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
                --vc->mrail.srp.credits[temp_v->rail].remote_credit;
            }

        } else {
            MRAILI_Get_buffer(vc, temp_v, pkt_len);
            PRINT_DEBUG(DEBUG_SEND_verbose>1, "coalesce not ok\n");
        }

        PRINT_DEBUG(DEBUG_SEND_verbose>1, "buffer is %p\n", temp_v->buffer);
        PRINT_DEBUG(DEBUG_SEND_verbose>1, "pheader buffer is %p\n", temp_v->pheader);

        temp_v->rail = rail;
        temp_v->eager = 1;
        temp_v->content_size = 0;

        PRINT_DEBUG(DEBUG_SEND_verbose>1, "incrementing the outstanding eager vbufs: eager %d\n",
                vc->mrail.outstanding_eager_vbufs);

        if (temp_v->transport == IB_TRANSPORT_RC)
            ++vc->mrail.outstanding_eager_vbufs;
    }

    MPIU_Assert(temp_v != NULL);

    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_GET_VBUF);
    return temp_v;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAILI_Fast_rdma_fill_start_buf
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MRAILI_Fast_rdma_fill_start_buf(MPIDI_VC_t * vc,
        MPL_IOV * iov, int n_iov,
        int *num_bytes_ptr)
{
    /* FIXME: Here we assume that iov holds a packet header */
#ifndef MV2_DISABLE_HEADER_CACHING 
    MPIDI_CH3_Pkt_send_t *cached =  vc->mrail.rfp.cached_outgoing;
#endif
    MPIDI_CH3_Pkt_send_t *header;
    vbuf *v = &(vc->mrail.rfp.RDMA_send_buf[vc->mrail.rfp.phead_RDMA_send]);
    void *vstart;
    void *data_buf;

    int len = *num_bytes_ptr, avail = 0; 
    uint16_t seq_num = 0;
    int i;

    header = iov[0].MPL_IOV_BUF;

    seq_num =  header->seqnum = vc->mrail.seqnum_next_tosend;
    vc->mrail.seqnum_next_tosend++;

    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_FILL_START_BUF);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_FILL_START_BUF);

    /* Calculate_IOV_len(iov, n_iov, len); */

    avail   = len;
    PACKET_SET_RDMA_CREDIT(header, vc);
    *num_bytes_ptr = 0;

    PRINT_DEBUG(DEBUG_SEND_verbose>1, "Header info, tag %d, rank %d, context_id %d\n", 
            header->match.parts.tag, header->match.parts.rank, header->match.parts.context_id);
#ifndef MV2_DISABLE_HEADER_CACHING 

    if ((header->type == MPIDI_CH3_PKT_EAGER_SEND) &&
            (len - sizeof(MPIDI_CH3_Pkt_eager_send_t) <= MAX_SIZE_WITH_HEADER_CACHING) &&
            (header->match.parts.tag == cached->match.parts.tag) &&
            (header->match.parts.rank == cached->match.parts.rank) &&
            (header->match.parts.context_id == cached->match.parts.context_id) &&
            (header->vbuf_credit == cached->vbuf_credit) &&
            (header->remote_credit == cached->remote_credit) &&
            (header->rdma_credit == cached->rdma_credit)) {
        /* change the header contents */
        ++vc->mrail.rfp.cached_hit;

        if (header->sender_req_id == cached->sender_req_id) {
            MPIDI_CH3I_MRAILI_Pkt_fast_eager *fast_header;
            vstart = v->buffer;

            /*
               DEBUG_PRINT 
               ("[send: fill buf], head cached, head_flag %p, vstart %p, length %d",
               &v->head_flag, vstart,
               len - sizeof(MPIDI_CH3_Pkt_eager_send_t) + 
               sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager));
               */

            fast_header = vstart;
            fast_header->type = MPIDI_CH3_PKT_FAST_EAGER_SEND;
            fast_header->bytes_in_pkt = len - sizeof(MPIDI_CH3_Pkt_eager_send_t);
            fast_header->seqnum = seq_num;
            v->pheader = fast_header;
            data_buf = (void *) ((unsigned long) vstart +
                    sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager));

            if (iov[0].MPL_IOV_LEN - sizeof(MPIDI_CH3_Pkt_eager_send_t)) 
                MPIU_Memcpy(data_buf, (void *)((uintptr_t)iov[0].MPL_IOV_BUF +
                            sizeof(MPIDI_CH3_Pkt_eager_send_t)), 
                        iov[0].MPL_IOV_LEN - sizeof(MPIDI_CH3_Pkt_eager_send_t));

            data_buf = (void *)((uintptr_t)data_buf + iov[0].MPL_IOV_LEN -
                    sizeof(MPIDI_CH3_Pkt_eager_send_t));

            *num_bytes_ptr += sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager);
            avail -= sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager);
        } else {
            MPIDI_CH3I_MRAILI_Pkt_fast_eager_with_req *fast_header;
            vstart = v->buffer;

            DEBUG_PRINT
                ("[send: fill buf], head cached, head_flag %p, vstart %p, length %d\n",
                 &v->head_flag, vstart,
                 len - sizeof(MPIDI_CH3_Pkt_eager_send_t) + 
                 sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager_with_req));

            fast_header = vstart;
            fast_header->type = MPIDI_CH3_PKT_FAST_EAGER_SEND_WITH_REQ;
            fast_header->bytes_in_pkt = len - sizeof(MPIDI_CH3_Pkt_eager_send_t);
            fast_header->seqnum = seq_num;
            fast_header->sender_req_id = header->sender_req_id;
            cached->sender_req_id = header->sender_req_id;
            v->pheader = fast_header;
            data_buf =
                (void *) ((unsigned long) vstart +
                        sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager_with_req));
            if (iov[0].MPL_IOV_LEN - sizeof(MPIDI_CH3_Pkt_eager_send_t)) 
                MPIU_Memcpy(data_buf, (void *)((uintptr_t)iov[0].MPL_IOV_BUF +
                            sizeof(MPIDI_CH3_Pkt_eager_send_t)), 
                        iov[0].MPL_IOV_LEN - sizeof(MPIDI_CH3_Pkt_eager_send_t));

            data_buf = (void *)((uintptr_t)data_buf + iov[0].MPL_IOV_LEN -
                    sizeof(MPIDI_CH3_Pkt_eager_send_t));

            *num_bytes_ptr += sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager_with_req);
            avail -= sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager_with_req);
        }
    } else
#endif
    {
        vstart = v->buffer;
        DEBUG_PRINT
            ("[send: fill buf], head not cached, v %p, vstart %p, length %d, header size %d\n",
             v, vstart, len, iov[0].MPL_IOV_LEN);
        MPIU_Memcpy(vstart, header, iov[0].MPL_IOV_LEN);
#ifndef MV2_DISABLE_HEADER_CACHING 
        if (header->type == MPIDI_CH3_PKT_EAGER_SEND &&
                ((len - sizeof(MPIDI_CH3_Pkt_eager_send_t)) <= MAX_SIZE_WITH_HEADER_CACHING)) {
            MPIU_Memcpy(cached, header, sizeof(MPIDI_CH3_Pkt_eager_send_t));
            ++vc->mrail.rfp.cached_miss;
        }
#endif
        data_buf = (void *) ((unsigned long) vstart + iov[0].MPL_IOV_LEN);
        *num_bytes_ptr += iov[0].MPL_IOV_LEN;
        avail -= iov[0].MPL_IOV_LEN;
        v->pheader = vstart;
    }


    /* We have filled the header, it is time to fit in the actual data */
#ifdef _ENABLE_CUDA_
    if (rdma_enable_cuda && n_iov > 1 && is_device_buffer(iov[1].MPL_IOV_BUF)) {
        /* in the case of GPU buffers, there is only one data iov, if data is non-contiguous
         * it should have been packed before this */
        MPIU_Assert(n_iov == 2);

        MPIU_Memcpy_Device(data_buf,
                iov[1].MPL_IOV_BUF,
                iov[1].MPL_IOV_LEN,
                cudaMemcpyDeviceToHost);
        *num_bytes_ptr += iov[1].MPL_IOV_LEN;
        avail -= iov[1].MPL_IOV_LEN;

        MPIU_Assert(avail >= 0);
    } else
#endif
    {
        for (i = 1; i < n_iov; i++) {
            if (avail >= iov[i].MPL_IOV_LEN) {
                MPIU_Memcpy(data_buf, iov[i].MPL_IOV_BUF, iov[i].MPL_IOV_LEN);
                data_buf = (void *) ((unsigned long) data_buf + iov[i].MPL_IOV_LEN);
                *num_bytes_ptr += iov[i].MPL_IOV_LEN;
                avail -= iov[i].MPL_IOV_LEN;
            } else if (avail > 0) {
                MPIU_Memcpy(data_buf, iov[i].MPL_IOV_BUF, avail);
                data_buf = (void *) ((unsigned long) data_buf + avail);
                *num_bytes_ptr += avail;
                avail = 0;
                break;
            } else break;
        }
    }

    PRINT_DEBUG(DEBUG_SEND_verbose>1, "[send: fill buf], num bytes copied %d\n", *num_bytes_ptr);
    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_FILL_START_BUF);
    return MPI_SUCCESS;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAILI_Fast_rdma_send_complete
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
/* INOUT: num_bytes_ptr holds the pkt_len as input parameter */
static inline int MPIDI_CH3I_MRAILI_Fast_rdma_send_complete(MPIDI_VC_t * vc,
        MPL_IOV * iov,
        int n_iov,
        int *num_bytes_ptr,
        vbuf ** vbuf_handle)
{
    int rail;
    int  post_len;
    char cq_overflow = 0;
    VBUF_FLAG_TYPE flag;
    vbuf *v =
        &(vc->mrail.rfp.RDMA_send_buf[vc->mrail.rfp.phead_RDMA_send]);
    char *rstart;

    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_SEND_COMPLETE);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_SEND_COMPLETE);

    rail = MRAILI_Send_select_rail(vc, *num_bytes_ptr);
    MRAILI_Fast_rdma_fill_start_buf(vc, iov, n_iov, num_bytes_ptr);

    post_len = *num_bytes_ptr;
    rstart = vc->mrail.rfp.remote_RDMA_buf +
        (vc->mrail.rfp.phead_RDMA_send * rdma_fp_buffer_size);
    PRINT_DEBUG(DEBUG_SEND_verbose>1, "[send: rdma_send] local vbuf %p, remote start %p, align size %d\n",
            v, rstart, post_len);

    if (++(vc->mrail.rfp.phead_RDMA_send) >= num_rdma_buffer)
        vc->mrail.rfp.phead_RDMA_send = 0;

    v->rail = rail;
    v->padding = BUSY_FLAG;

    /* requirements for coalescing */
    ++vc->mrail.outstanding_eager_vbufs;
    v->eager = 1;
    v->vc = (void *) vc;

    /* set tail flag with the size of the content */
    if ((int) *(VBUF_FLAG_TYPE *) (v->buffer + post_len) == post_len) {
        flag = (VBUF_FLAG_TYPE) (post_len + FAST_RDMA_ALT_TAG);
    } else {
        flag = (VBUF_FLAG_TYPE) post_len;
    }
    /* set head flag */
    *v->head_flag = (VBUF_FLAG_TYPE) flag;
    /* set tail flag */    
    *((VBUF_FLAG_TYPE *)(v->buffer + post_len)) = flag;

    PRINT_DEBUG(DEBUG_SEND_verbose>1, "incrementing the outstanding eager vbufs: RFP %d\n", vc->mrail.outstanding_eager_vbufs);

    /* generate a completion, following statements should have been executed during
     * initialization */
    post_len += VBUF_FAST_RDMA_EXTRA_BYTES;

    PRINT_DEBUG(DEBUG_SEND_verbose>1, "[send: rdma_send] lkey %x, rkey %p, len %d, flag %lu\n",
            vc->mrail.rfp.RDMA_send_buf_mr[vc->mrail.rails[rail].hca_index]->lkey,
            vc->mrail.rfp.RDMA_remote_buf_rkey, post_len, *v->head_flag);

    VBUF_SET_RDMA_ADDR_KEY(v, post_len, v->head_flag,
            vc->mrail.rfp.RDMA_send_buf_mr[vc->mrail.rails[rail].hca_index]->lkey, rstart,
            vc->mrail.rfp.RDMA_remote_buf_rkey[vc->mrail.rails[rail].hca_index]);

    XRC_FILL_SRQN_FIX_CONN (v, vc, rail);
    FLUSH_RAIL(vc, rail);
#ifdef CRC_CHECK
    p->crc = update_crc(1, (void *)((uintptr_t)p+sizeof *p),
            *v->head_flag - sizeof *p);
#endif

    CHECK_CQ_OVERFLOW(cq_overflow, vc, rail);

    if (likely(vc->mrail.rails[rail].send_wqes_avail > 0 && !cq_overflow)) {
        --vc->mrail.rails[rail].send_wqes_avail;
        *vbuf_handle = v;

        IBV_POST_SR(v, vc, rail, "ibv_post_sr (post_fast_rdma)");
        PRINT_DEBUG(DEBUG_SEND_verbose>1, "[send:post rdma] desc posted\n");
    } else {
        PRINT_DEBUG(DEBUG_SEND_verbose>1, "[send: rdma_send] Warning! no send wqe or send cq available\n");
        MRAILI_Ext_sendq_enqueue(vc, rail, v);
        *vbuf_handle = v;
        return MPI_MRAIL_MSG_QUEUED;
    }

    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_SEND_COMPLETE);
    return MPI_SUCCESS;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAILI_Fast_rdma_ok
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_CH3I_MRAILI_Fast_rdma_ok(MPIDI_VC_t * vc, MPIDI_msg_sz_t len)
{
    int i = 0;

    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_OK);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_OK);

    if(unlikely(vc->tmp_dpmvc)) {
        return 0;
    }

#ifdef _ENABLE_UD_
    if(rdma_enable_hybrid)
    {
        if(unlikely(!(vc->mrail.state & MRAILI_RC_CONNECTED))) {
            return 0;
        }
    }
#endif /* _ENABLE_UD_ */

    if (unlikely(len > MRAIL_MAX_RDMA_FP_SIZE)) {
        return 0;
    }

    if (unlikely(num_rdma_buffer < 2
                || vc->mrail.rfp.phead_RDMA_send == vc->mrail.rfp.ptail_RDMA_send
                || vc->mrail.rfp.RDMA_send_buf[vc->mrail.rfp.phead_RDMA_send].padding == BUSY_FLAG
                || MRAILI_Coalesce_ok(vc, 0))) /* We can only coalesce with send/recv. */
    {
        MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_OK);
        return 0;
    }

    if (unlikely(!mv2_MPIDI_CH3I_RDMA_Process.has_srq)) {
        for (i = 0; i < rdma_num_rails; i++)
        {
            if (vc->mrail.srp.credits[i].backlog.len != 0)
            {
                MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_OK);
                return 0;
            }
        }
    }

    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_FAST_RDMA_OK);
    return 1;
} 

#undef FUNCNAME
#define FUNCNAME post_srq_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int post_srq_send(MPIDI_VC_t* vc, vbuf* v, int rail)
{
    char cq_overflow = 0;
    MPIDI_CH3I_MRAILI_Pkt_comm_header *p = v->pheader;
    PACKET_SET_CREDIT(p, vc, rail);

    MPIDI_STATE_DECL(MPID_STATE_POST_SRQ_SEND);
    MPIDI_FUNC_ENTER(MPID_STATE_POST_SRQ_SEND);

    v->vc = (void *) vc;
    p->rail        = rail;
#ifdef _ENABLE_UD_
    if(rdma_enable_hybrid) {
        p->src.rank    = MPIDI_Process.my_pg_rank;
    } else
#endif
    {
        p->src.vc_addr = vc->mrail.remote_vc_addr;
    }
    MPIU_Assert(v->transport == IB_TRANSPORT_RC);

    if (p->type == MPIDI_CH3_PKT_NOOP) {
        v->seqnum = p->seqnum = -1;
    } else {
        v->seqnum = p->seqnum = vc->mrail.seqnum_next_tosend;
        vc->mrail.seqnum_next_tosend++;
    }

    p->acknum = vc->mrail.seqnum_next_toack;
    MARK_ACK_COMPLETED(vc);

    XRC_FILL_SRQN_FIX_CONN (v, vc, rail);

    FLUSH_RAIL(vc, rail);

    CHECK_CQ_OVERFLOW(cq_overflow, vc, rail);

    if (likely(vc->mrail.rails[rail].send_wqes_avail > 0 && !cq_overflow)) {
        --vc->mrail.rails[rail].send_wqes_avail;

        IBV_POST_SR(v, vc, rail, "ibv_post_sr (post_send_desc)");
    } else {
        MRAILI_Ext_sendq_enqueue(vc, rail, v);
        MPIDI_FUNC_EXIT(MPID_STATE_POST_SRQ_SEND);
        return MPI_MRAIL_MSG_QUEUED;
    }

    MPIDI_FUNC_EXIT(MPID_STATE_POST_SRQ_SEND);
    return 0;
}

#undef FUNCNAME
#define FUNCNAME post_nosrq_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int post_nosrq_send(MPIDI_VC_t * vc, vbuf * v, int rail)
{
    char cq_overflow = 0;
    MPIDI_CH3I_MRAILI_Pkt_comm_header *p = v->pheader;

    MPIDI_STATE_DECL(MPID_STATE_POST_SEND);
    MPIDI_FUNC_ENTER(MPID_STATE_POST_SEND);
    PRINT_DEBUG(DEBUG_SEND_verbose>1, 
            "[post send] credit %d,type noop %d, "
            "backlog %d, wqe %d, nb will be %d\n",
            vc->mrail.srp.credits[rail].remote_credit,
            p->type == MPIDI_CH3_PKT_NOOP, 
            vc->mrail.srp.credits[0].backlog.len,
            vc->mrail.rails[rail].send_wqes_avail,
            v->desc.sg_entry.length);

    v->vc = (void *) vc;
    p->rail        = rail;
#ifdef _ENABLE_UD_
    if(rdma_enable_hybrid) {
        p->src.rank = MPIDI_Process.my_pg_rank;
    } else
#endif
    {
        p->src.vc_addr = vc->mrail.remote_vc_addr;
    }

    MPIU_Assert(v->transport == IB_TRANSPORT_RC);

    if (p->type == MPIDI_CH3_PKT_NOOP) {
        v->seqnum = p->seqnum = -1;
    } else {
        v->seqnum = p->seqnum = vc->mrail.seqnum_next_tosend;
        vc->mrail.seqnum_next_tosend++;
    }
    p->acknum = vc->mrail.seqnum_next_toack;
    MARK_ACK_COMPLETED(vc);

    PRINT_DEBUG(DEBUG_UD_verbose>1, "sending seqnum:%d acknum:%d\n",p->seqnum,p->acknum);

    if (vc->mrail.srp.credits[rail].remote_credit > 0
            || p->type == MPIDI_CH3_PKT_NOOP) {

        PACKET_SET_CREDIT(p, vc, rail);
#ifdef CRC_CHECK
        p->crc = update_crc(1, (void *)((uintptr_t)p+sizeof *p),
                v->desc.sg_entry.length - sizeof *p );
#endif
        if (p->type != MPIDI_CH3_PKT_NOOP)
        {
            --vc->mrail.srp.credits[rail].remote_credit;
        }

        v->vc = (void *) vc;

        XRC_FILL_SRQN_FIX_CONN (v, vc, rail);
        FLUSH_RAIL(vc, rail);

        CHECK_CQ_OVERFLOW(cq_overflow, vc, rail);

        if (likely(vc->mrail.rails[rail].send_wqes_avail > 0 && !cq_overflow)) {
            --vc->mrail.rails[rail].send_wqes_avail;
            IBV_POST_SR(v, vc, rail, "ibv_post_sr (post_send_desc)");
        } else {
            MRAILI_Ext_sendq_enqueue(vc, rail, v);
            MPIDI_FUNC_EXIT(MPID_STATE_POST_SEND);
            return MPI_MRAIL_MSG_QUEUED;
        }
    }
    else
    {
        ibv_backlog_queue_t *q = &(vc->mrail.srp.credits[rail].backlog);
        BACKLOG_ENQUEUE(q, v);
        MPIDI_FUNC_EXIT(MPID_STATE_POST_SEND);
        return MPI_MRAIL_MSG_QUEUED;
    }

    MPIDI_FUNC_EXIT(MPID_STATE_POST_SEND);
    return 0;
}

#ifdef _ENABLE_UD_
#undef FUNCNAME
#define FUNCNAME post_hybrid_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int post_hybrid_send(MPIDI_VC_t* vc, vbuf* v, int rail)
{
    mv2_MPIDI_CH3I_RDMA_Process_t *proc = &mv2_MPIDI_CH3I_RDMA_Process;

    MPIDI_STATE_DECL(MPID_STATE_POST_HYBRID_SEND);
    MPIDI_FUNC_ENTER(MPID_STATE_POST_HYBRID_SEND);

    switch (v->transport) {
        case IB_TRANSPORT_UD:
            /* Enable RC connection if total no of msgs on UD channel reachd a
             * threshold and total rc connections less than threshold  
             */
            if (!(vc->mrail.state & (MRAILI_RC_CONNECTED | MRAILI_RC_CONNECTING)) 
                    && (rdma_ud_num_msg_limit)
                    && (vc->mrail.rely.total_messages > rdma_ud_num_msg_limit)
                    && ((mv2_MPIDI_CH3I_RDMA_Process.rc_connections + rdma_hybrid_pending_rc_conn)
                        < rdma_hybrid_max_rc_conn)
                    && vc->mrail.rely.ext_window.head == NULL
                    && !(vc->state == MPIDI_VC_STATE_LOCAL_CLOSE || vc->state == MPIDI_VC_STATE_CLOSE_ACKED)) {
                /* This is hack to create RC channel using CM protocol.
                 ** Need to handle this by sending REQ/REP on UD channel itself
                 */
                vc->ch.state = MPIDI_CH3I_VC_STATE_UNCONNECTED;
#ifdef _ENABLE_XRC_
                if(USE_XRC) {
                    VC_XST_CLR (vc, XF_SEND_IDLE);
                }
#endif
                PRINT_DEBUG(DEBUG_UD_verbose>1, "Connection initiated to :%d\n", vc->pg_rank);
                MV2_HYBRID_SET_RC_CONN_INITIATED(vc);
            } 
            post_ud_send(vc, v, rail, NULL);
            break;
        case IB_TRANSPORT_RC:
            MPIU_Assert(vc->mrail.state & MRAILI_RC_CONNECTED);
            if(proc->has_srq) {
                post_srq_send(vc, v, rail);
            } else {
                post_nosrq_send(vc, v, rail);
            }
            break;
        default:
            PRINT_DEBUG(DEBUG_UD_verbose>1,"Invalid IB transport protocol\n");
            return -1;
    }

    MPIDI_FUNC_EXIT(MPID_STATE_POST_HYBRID_SEND);
    return 0;
}
#endif /* _ENABLE_UD_ */

#undef FUNCNAME
#define FUNCNAME post_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int post_send(MPIDI_VC_t * vc, vbuf * v, int rail)
{
#ifdef _ENABLE_UD_
    if (rdma_enable_only_ud) {
        return post_ud_send(vc, v, rail, NULL);
    } else if (rdma_enable_hybrid) {
        return post_hybrid_send(vc, v, rail);
    } else
#endif
    {
        if (likely(mv2_use_post_srq_send)) {
            return post_srq_send(vc, v, rail);
        } else {
            return post_nosrq_send(vc, v, rail);
        }
    }
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAILI_Eager_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_CH3I_MRAILI_Eager_send(MPIDI_VC_t * vc,
        MPL_IOV * iov,
        int n_iov,
        size_t pkt_len,
        int *num_bytes_ptr,
        vbuf **buf_handle)
{
    vbuf * v;

    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_MRAILI_EAGER_SEND);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_MRAILI_EAGER_SEND);

    /* first we check if we can take the RDMA FP */
    if(likely(MPIDI_CH3I_MRAILI_Fast_rdma_ok(vc, pkt_len))) {

        *num_bytes_ptr = pkt_len;
        MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_EAGER_SEND);
        return MPIDI_CH3I_MRAILI_Fast_rdma_send_complete(vc, iov,
                n_iov, num_bytes_ptr, buf_handle);
    } 

    /* otherwise we can always take the send/recv path */
    v = MRAILI_Get_Vbuf(vc, pkt_len);

    PRINT_DEBUG(DEBUG_SEND_verbose>1, "[eager send]vbuf addr %p, buffer: %p\n", v, v->buffer);
    *num_bytes_ptr = MRAILI_Fill_start_buffer(v, iov, n_iov);

#ifdef CKPT
    /* this won't work properly at the moment... 
     *
     * My guess is that if vc->ch.state != MPIDI_CH3I_VC_STATE_IDLE
     * just have Coalesce_ok return 0 -- then you'll always get a new vbuf
     * (actually there are a few other things to change as well...)
     */

    if (vc->ch.state != MPIDI_CH3I_VC_STATE_IDLE) {
        /*MPIDI_CH3I_MRAILI_Pkt_comm_header * p = (MPIDI_CH3I_MRAILI_Pkt_comm_header *) v->pheader;*/
        MPIDI_CH3I_CR_msg_log_queue_entry_t *entry;
        if (rdma_use_coalesce) {
            entry = MSG_LOG_QUEUE_TAIL(vc);
            if (entry->buf == v) /*since the vbuf is already filled, no need to queue it again*/
            {
                PRINT_DEBUG(DEBUG_FT_verbose, "coalesced buffer\n");
                return MPI_MRAIL_MSG_QUEUED;
            }
        }
        entry = (MPIDI_CH3I_CR_msg_log_queue_entry_t *) MPIU_Malloc(sizeof(MPIDI_CH3I_CR_msg_log_queue_entry_t));
        entry->buf = v;
        entry->len = *num_bytes_ptr;
        MSG_LOG_ENQUEUE(vc, entry);
        MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_EAGER_SEND);
        return MPI_MRAIL_MSG_QUEUED;
    }
#endif

    /* send the buffer if we aren't trying to coalesce it */
    if(likely(vc->mrail.coalesce_vbuf != v))  {
        PRINT_DEBUG(DEBUG_SEND_verbose>1, "[eager send] len %d, selected rail hca %d, rail %d\n",
                *num_bytes_ptr, vc->mrail.rails[v->rail].hca_index, v->rail);
        vbuf_init_send(v, *num_bytes_ptr, v->rail);
        post_send(vc, v, v->rail);
    } else {
        MPIDI_CH3I_MRAILI_Pkt_comm_header *p = (MPIDI_CH3I_MRAILI_Pkt_comm_header *)
            (v->buffer + v->content_size - *num_bytes_ptr);

        PACKET_SET_CREDIT(p, vc, v->rail);
#ifdef CRC_CHECK
        p->crc = update_crc(1, (void *)((uintptr_t)p+sizeof *p),
                v->desc.sg_entry.length - sizeof *p);
#endif
        v->vc                = (void *) vc;
        p->rail        = v->rail;
#ifdef _ENABLE_UD_
        if(rdma_enable_hybrid) {
            p->src.rank    = MPIDI_Process.my_pg_rank;
        } else
#endif
        {
            p->src.vc_addr = vc->mrail.remote_vc_addr;
        }
    }

    *buf_handle = v;

    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_EAGER_SEND);
    return 0;
}

#undef FUNCNAME
#define FUNCNAME mv2_eager_fast_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int mv2_eager_fast_send(MPIDI_VC_t* vc, const void *buf,
        MPIDI_msg_sz_t data_sz, int rank, int tag,
        MPID_Comm *comm, int context_offset, MPID_Request **sreq_p)
{
    int rail = 0;
    int retval = 0;
    vbuf* v = NULL;
    int len = 0;
    void *ptr = NULL;
    MPID_Seqnum_t seqnum;
    MPIDI_CH3_Pkt_t *upkt = NULL;
    MPIDI_CH3_Pkt_eager_send_t *eager_pkt = NULL;

    rail = MRAILI_Send_select_rail(vc, data_sz + sizeof(MPIDI_CH3_Pkt_eager_send_t));

    /* Get VBUF */
    MRAILI_Get_buffer(vc, v, data_sz+sizeof(MPIDI_CH3_Pkt_eager_send_t));

    /* Point header to start of buffer */
    upkt = (MPIDI_CH3_Pkt_t *) v->buffer;
    eager_pkt = &((*upkt).eager_send);

    /* Create packet header */
    MPIDI_Pkt_init(eager_pkt, MPIDI_CH3_PKT_EAGER_SEND);
    eager_pkt->data_sz                 = data_sz;
    eager_pkt->match.parts.tag         = tag;
    eager_pkt->match.parts.rank        = comm->rank;
    eager_pkt->match.parts.context_id  = comm->context_id + context_offset;

    /* Set sequence number */
    MPIDI_VC_FAI_send_seqnum(vc, seqnum);
    MPIDI_Pkt_set_seqnum(eager_pkt, seqnum);

    /* Copy data */
    ptr = (void*) v->buffer + sizeof(MPIDI_CH3_Pkt_eager_send_t);

    memcpy(ptr, buf, data_sz);
    /* Compute size of pkt */
    len = sizeof(MPIDI_CH3_Pkt_eager_send_t) + data_sz;

    /* Initialize other vbuf parameters */
    vbuf_init_send(v, len, rail);

    /* Send the packet */
    retval = post_send(vc, v, rail);

    return retval;
}

#undef FUNCNAME
#define FUNCNAME mv2_eager_fast_coalesce_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int mv2_eager_fast_coalesce_send(MPIDI_VC_t* vc, const void *buf,
        MPIDI_msg_sz_t data_sz, int rank, int tag,
        MPID_Comm *comm, int context_offset, MPID_Request **sreq_p)
{
    int retval = 0;
    vbuf* v = NULL;
    int len = 0;
    void *ptr = NULL;
    MPID_Seqnum_t seqnum;
    MPIDI_CH3_Pkt_t *upkt = NULL;
    MPIDI_CH3_Pkt_eager_send_t *eager_pkt = NULL;

    /* Get VBUF */
    v = MRAILI_Get_Vbuf(vc, data_sz+sizeof(MPIDI_CH3_Pkt_eager_send_t));

    /* Point header to start of buffer */
    upkt = (MPIDI_CH3_Pkt_t *) (v->buffer + v->content_size);
    eager_pkt = &((*upkt).eager_send);

    /* Create packet header */
    MPIDI_Pkt_init(eager_pkt, MPIDI_CH3_PKT_EAGER_SEND);
    eager_pkt->data_sz                 = data_sz;
    eager_pkt->match.parts.tag         = tag;
    eager_pkt->match.parts.rank        = comm->rank;
    eager_pkt->match.parts.context_id  = comm->context_id + context_offset;

    /* Set sequence number */
    MPIDI_VC_FAI_send_seqnum(vc, seqnum);
    MPIDI_Pkt_set_seqnum(eager_pkt, seqnum);

    /* Copy data */
    ptr = (void*) v->buffer + v->content_size + sizeof(MPIDI_CH3_Pkt_eager_send_t);

    memcpy(ptr, buf, data_sz);
    /* Compute size of pkt */
    len = sizeof(MPIDI_CH3_Pkt_eager_send_t) + data_sz;

    /* Update length */
    v->content_size += len;

    /* send the buffer if we aren't trying to coalesce it */
    if(likely(vc->mrail.coalesce_vbuf != v))  {
        /* Initialize other vbuf parameters */
        vbuf_init_send(v, len, v->rail);
        /* Send the packet */
        retval = post_send(vc, v, v->rail);
    } else {
        MPIDI_CH3I_MRAILI_Pkt_comm_header *p = (MPIDI_CH3I_MRAILI_Pkt_comm_header *)
            (v->buffer + v->content_size - len);

        PACKET_SET_CREDIT(p, vc, v->rail);
#ifdef CRC_CHECK
        p->crc = update_crc(1, (void *)((uintptr_t)p+sizeof *p),
                v->desc.sg_entry.length - sizeof *p);
#endif
        v->vc                = (void *) vc;
        p->rail        = v->rail;
#ifdef _ENABLE_UD_
        if(rdma_enable_hybrid) {
            p->src.rank    = MPIDI_Process.my_pg_rank;
        } else
#endif
        {
            p->src.vc_addr = vc->mrail.remote_vc_addr;
        }
    }

    return retval;
}

#undef FUNCNAME
#define FUNCNAME mv2_eager_fast_rfp_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int mv2_eager_fast_rfp_send(MPIDI_VC_t* vc, const void *buf,
        MPIDI_msg_sz_t data_sz, int rank, int tag,
        MPID_Comm *comm, int context_offset, MPID_Request **sreq_p)
{
    /* For short send n_iov is always 2 */
    int n_iov = 2;
    MPID_Seqnum_t seqnum;
    vbuf *buf_handle = NULL;
    int num_bytes_ptr = 0;
    MPL_IOV iov[2];
    MPIDI_CH3_Pkt_t upkt;
    MPIDI_CH3_Pkt_eager_send_t * const eager_pkt = &upkt.eager_send;

    if (unlikely(!MPIDI_CH3I_MRAILI_Fast_rdma_ok(vc, data_sz+sizeof(*eager_pkt)))) {
        if (likely(rdma_use_coalesce)) {
            return mv2_eager_fast_coalesce_send(vc, buf, data_sz, rank,
                    tag, comm, context_offset, sreq_p);
        } else {
            return mv2_eager_fast_send(vc, buf, data_sz, rank,
                    tag, comm, context_offset, sreq_p);
        }
    }

    /* Create packet header */
    MPIDI_Pkt_init(eager_pkt, MPIDI_CH3_PKT_EAGER_SEND);
    eager_pkt->data_sz                 = data_sz;
    eager_pkt->match.parts.tag         = tag;
    eager_pkt->match.parts.rank        = comm->rank;
    eager_pkt->match.parts.context_id  = comm->context_id + context_offset;

    /* Create IOV (header) */
    iov[0].MPL_IOV_BUF = (MPL_IOV_BUF_CAST)eager_pkt;
    iov[0].MPL_IOV_LEN = sizeof(*eager_pkt);
    /* Create IOV (data) */
    iov[1].MPL_IOV_BUF = (MPL_IOV_BUF_CAST) buf;
    iov[1].MPL_IOV_LEN = data_sz;

    /* Compute size of pkt */
    num_bytes_ptr = iov[0].MPL_IOV_LEN + iov[1].MPL_IOV_LEN;

    /* Set sequence number */
    MPIDI_VC_FAI_send_seqnum(vc, seqnum);
    MPIDI_Pkt_set_seqnum(eager_pkt, seqnum);

    return MPIDI_CH3I_MRAILI_Fast_rdma_send_complete(vc, iov,
            n_iov, &num_bytes_ptr, &buf_handle);
}

#undef FUNCNAME
#define FUNCNAME mv2_eager_fast_wrapper
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int mv2_eager_fast_wrapper(MPIDI_VC_t* vc, const void *buf,
        MPIDI_msg_sz_t data_sz, int rank, int tag,
        MPID_Comm *comm, int context_offset, MPID_Request **sreq_p)
{
    if (likely(vc->smp.local_nodes < 0)) {
        /* Inter-node scenario */
        if (vc->use_eager_fast_rfp_fn) {
            return mv2_eager_fast_rfp_send(vc, buf, data_sz, rank,
                    tag, comm, context_offset, sreq_p);
        } else if (likely(rdma_use_coalesce)) {
            return mv2_eager_fast_coalesce_send(vc, buf, data_sz, rank,
                    tag, comm, context_offset, sreq_p);
        } else {
            return mv2_eager_fast_send(vc, buf, data_sz, rank,
                    tag, comm, context_offset, sreq_p);
        }
    } else {
        /* Intra-node scenario */
        return mv2_smp_fast_write_contig(vc, buf, data_sz, rank,
                tag, comm, context_offset, sreq_p);
    }
}

/* Ensure definition of FUNCNAME & FCNAME do not leak into other files */
#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAIL_Parse_header
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_CH3I_MRAIL_Parse_header(MPIDI_VC_t * vc,
                                  vbuf * v, void **pkt, int *header_size)
{
    void *vstart;
    MPIDI_CH3I_MRAILI_Pkt_comm_header *header;
#ifdef CRC_CHECK
    unsigned long crc;
#endif
    int mpi_errno = MPI_SUCCESS;
    int ret;
    MPIDI_STATE_DECL(MPIDI_STATE_CH3I_MRAIL_PARSE_HEADER);
    MPIDI_FUNC_ENTER(MPIDI_STATE_CH3I_MRAIL_PARSE_HEADER);

    DEBUG_PRINT("[parse header] vbuf address %p\n", v);
    vstart = v->pheader;
    header = vstart;
    DEBUG_PRINT("[parse header] header type %d\n", header->type);

    /* set it to the header size by default */
    *header_size = MPIDI_CH3_Pkt_size_index[header->type];
#ifdef CRC_CHECK
    crc = update_crc(1, (void *)((uintptr_t)header+sizeof *header),
                     v->content_size - sizeof *header);
    if (crc != header->crc) {
        int rank; UPMI_GET_RANK(&rank);
        MPL_error_printf(stderr, "CRC mismatch, get %lx, should be %lx "
                "type %d, ocntent size %d\n",
                crc, header->crc, header->type, v->content_size);
        exit(EXIT_FAILURE);
    }
#endif
    switch (header->type) {
#ifndef MV2_DISABLE_HEADER_CACHING 
    case (MPIDI_CH3_PKT_FAST_EAGER_SEND):
    case (MPIDI_CH3_PKT_FAST_EAGER_SEND_WITH_REQ):
        {
            MPIDI_CH3I_MRAILI_Pkt_fast_eager *fast_header = vstart;
            MPIDI_CH3_Pkt_eager_send_t *eager_header =
                (MPIDI_CH3_Pkt_eager_send_t *) vc->mrail.rfp.
                cached_incoming;

            if (MPIDI_CH3_PKT_FAST_EAGER_SEND == header->type) {
                *header_size = sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager);
            } else {
                *header_size =
                    sizeof(MPIDI_CH3I_MRAILI_Pkt_fast_eager_with_req);
                eager_header->sender_req_id =
                    ((MPIDI_CH3I_MRAILI_Pkt_fast_eager_with_req *)
                     vstart)->sender_req_id;
            }

            DEBUG_PRINT("[receiver side] cached credit %d\n",
                        eager_header->rdma_credit);

            eager_header->data_sz = fast_header->bytes_in_pkt;
            eager_header->seqnum = fast_header->seqnum;

            *pkt = (void *) eager_header;
            DEBUG_PRINT
                ("[recv: parse header] faster headersize returned %d\n",
                 *header_size);
        }
        break;
#endif
#ifdef USE_EAGER_SHORT
    case (MPIDI_CH3_PKT_EAGERSHORT_SEND):
        {
            *pkt = vstart;
            *header_size = sizeof(MPIDI_CH3_Pkt_eagershort_send_t);
        }
        break;
#endif /*USE_EAGER_SHORT*/
    case (MPIDI_CH3_PKT_EAGER_SEND):
        {
            DEBUG_PRINT("[recv: parse header] pkt eager send\n");
#ifndef MV2_DISABLE_HEADER_CACHING 
            if (v->padding != NORMAL_VBUF_FLAG &&
                ((v->content_size - sizeof(MPIDI_CH3_Pkt_eager_send_t)) <= MAX_SIZE_WITH_HEADER_CACHING )) {
                /* Only cache header if the packet is from RdMA path 
                 * XXXX: what is R3_FLAG? 
                 */
                MPIU_Memcpy((vc->mrail.rfp.cached_incoming), vstart,
                       sizeof(MPIDI_CH3_Pkt_eager_send_t));
            }
#endif
            *pkt = (MPIDI_CH3_Pkt_t *) vstart;
#if 0
            if (v->padding == NORMAL_VBUF_FLAG)
#endif
            *header_size = sizeof(MPIDI_CH3_Pkt_eager_send_t);
            DEBUG_PRINT("[recv: parse header] headersize returned %d\n",
                        *header_size);
        }
        break;
    case (MPIDI_CH3_PKT_RNDV_REQ_TO_SEND):
    case (MPIDI_CH3_PKT_RNDV_READY_REQ_TO_SEND):
    case (MPIDI_CH3_PKT_RNDV_CLR_TO_SEND):
    case (MPIDI_CH3_PKT_RMA_RNDV_CLR_TO_SEND):
    case (MPIDI_CH3_PKT_CUDA_CTS_CONTI):
    case (MPIDI_CH3_PKT_RPUT_FINISH):
    case (MPIDI_CH3_PKT_ZCOPY_FINISH):
    case (MPIDI_CH3_PKT_ZCOPY_ACK):
    case (MPIDI_CH3_PKT_MCST_NACK):
    case (MPIDI_CH3_PKT_MCST_INIT_ACK):
    case (MPIDI_CH3_PKT_NOOP):
    case MPIDI_CH3_PKT_EAGER_SYNC_ACK:
    case MPIDI_CH3_PKT_CANCEL_SEND_REQ:
    case MPIDI_CH3_PKT_CANCEL_SEND_RESP:
#ifdef CKPT
    case MPIDI_CH3_PKT_CM_SUSPEND:
    case MPIDI_CH3_PKT_CM_REACTIVATION_DONE:
    case MPIDI_CH3_PKT_CR_REMOTE_UPDATE:
#endif
        {
            *pkt = vstart;
        }
        break;
    case MPIDI_CH3_PKT_RNDV_R3_ACK:
        {
            *pkt = vstart;
        }
        goto fn_exit;
    case MPIDI_CH3_PKT_ADDRESS:
        {
            *pkt = vstart;
            MPIDI_CH3I_MRAILI_Recv_addr(vc, vstart);
            break;
        }
    case MPIDI_CH3_PKT_ADDRESS_REPLY:
    {
        *pkt = vstart;
        MPIDI_CH3I_MRAILI_Recv_addr_reply(vc, vstart);
        break;
    }
    case MPIDI_CH3_PKT_CM_ESTABLISH:
        {
            *pkt = vstart;
            *header_size = sizeof(MPIDI_CH3_Pkt_cm_establish_t);
            break;
        }
    case MPIDI_CH3_PKT_PACKETIZED_SEND_START:
        {
            *pkt = vstart;
            *header_size = sizeof(MPIDI_CH3_Pkt_packetized_send_start_t);
            break;
        }
    case MPIDI_CH3_PKT_PACKETIZED_SEND_DATA:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_packetized_send_data_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_RNDV_R3_DATA:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_rndv_r3_data_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_EAGER_SYNC_SEND:
    case MPIDI_CH3_PKT_READY_SEND:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_send_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_PUT_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_put_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_PUT:
        {
            /*Put uses MPIDI_CH3_Pkt_t type*/
            *header_size = sizeof(MPIDI_CH3_Pkt_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_PUT_RNDV:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_put_rndv_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_GET_RNDV:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_get_rndv_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_GET:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_get_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_GET_RESP:
    case MPIDI_CH3_PKT_GET_RESP_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_get_resp_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_ACCUMULATE_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_accum_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_ACCUMULATE:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_ACCUMULATE_RNDV:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_accum_rndv_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_GET_ACCUM_RNDV:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_get_accum_rndv_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_LOCK:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_lock_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_LOCK_ACK:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_lock_ack_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_LOCK_OP_ACK:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_lock_op_ack_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_UNLOCK:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_unlock_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_FLUSH:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_flush_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_ACK:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_ack_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_DECR_AT_COUNTER:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_decr_at_counter_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_FOP_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_fop_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_FOP:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_FOP_RESP:
    case MPIDI_CH3_PKT_FOP_RESP_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_fop_resp_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_CAS_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_cas_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_CAS_RESP_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_cas_resp_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_GET_ACCUM_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_get_accum_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_GET_ACCUM:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_GET_ACCUM_RESP:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_get_accum_resp_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_GET_ACCUM_RESP_IMMED:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_get_accum_resp_t);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_FLOW_CNTL_UPDATE:
        {
            *header_size = sizeof(MPIDI_CH3I_MRAILI_Pkt_flow_cntl);
            *pkt = vstart;
            break;
        }
    case MPIDI_CH3_PKT_CLOSE:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_close_t);
            *pkt = vstart;
        }
        break;
    case MPIDI_CH3_PKT_RGET_FINISH:
        {
            *header_size = sizeof(MPIDI_CH3_Pkt_rget_finish_t);
            *pkt = vstart;
            break;
        }
    default:
        {
            /* Header is corrupted if control has reached here in prototype */
            /* */
            MPIR_ERR_SETFATALANDJUMP2(mpi_errno,
                    MPI_ERR_OTHER,
                    "**fail",
                    "**fail %s %d",
                    "Control shouldn't reach here "
                    "in prototype, header %d\n",
                    header->type);
        }
    }

    PRINT_DEBUG(DEBUG_CHM_verbose>1, "Before set credit, vc: %p, v->rail: %d, "
                "pkt: %p, pheader: %p\n", vc, v->rail, pkt, v->pheader);

    SET_CREDIT((&(((MPIDI_CH3_Pkt_t *)
                        (*pkt))->eager_send)), vc, (v->rail),v->transport);


    if (vc->mrail.srp.credits[v->rail].remote_credit > 0 &&
        vc->mrail.srp.credits[v->rail].backlog.len > 0) {
        MRAILI_Backlog_send(vc, v->rail);
    }

    /* if any credits remain, schedule rendezvous progress */
    if (((vc->mrail.srp.credits[v->rail].remote_credit > 0
            || (vc->mrail.rfp.ptail_RDMA_send !=
                vc->mrail.rfp.phead_RDMA_send))
        )
        && (vc->mrail.sreq_head != NULL)) {
        PUSH_FLOWLIST(vc);
    }

    if (vc->mrail.state & MRAILI_RC_CONNECTED
            && v->transport == IB_TRANSPORT_RC
            && vc->mrail.rfp.RDMA_recv_buf == NULL
            && num_rdma_buffer && !vc->mrail.rfp.rdma_failed) {
        if ((mv2_MPIDI_CH3I_RDMA_Process.polling_group_size + rdma_pending_conn_request) <
                rdma_polling_set_limit) {
            vc->mrail.rfp.eager_start_cnt++;
            if (rdma_polling_set_threshold <
                    vc->mrail.rfp.eager_start_cnt) {
                MPICM_lock();
#ifdef _ENABLE_XRC_
                if (xrc_rdmafp_init &&
                        USE_XRC && VC_XST_ISUNSET (vc, XF_SEND_IDLE)) {
                    if (VC_XSTS_ISUNSET (vc, XF_START_RDMAFP |
                                XF_CONN_CLOSING | XF_DPM_INI)) {
                        PRINT_DEBUG(DEBUG_XRC_verbose>0, "Trying to FP to %d st: %d xr: 0x%08x",
                                vc->pg_rank, vc->ch.state, vc->ch.xrc_flags);
                        VC_XST_SET (vc, XF_START_RDMAFP);
                        MPICM_unlock();
                        MPIDI_CH3I_CM_Connect (vc);
                        goto fn_exit;
                    }
                }
                else if (!USE_XRC ||
                        (xrc_rdmafp_init &&
                        VC_XSTS_ISUNSET(vc,
                            XF_DPM_INI | XF_CONN_CLOSING | XF_START_RDMAFP)
                        && VC_XSTS_ISSET (vc, XF_SEND_IDLE | XF_RECV_IDLE)
                        && header->type != MPIDI_CH3_PKT_ADDRESS))
#endif
                {
                    DEBUG_PRINT("FP to %d (IDLE)\n", vc->pg_rank);
                    MPICM_unlock();
                    ret = vbuf_fast_rdma_alloc(vc, 1);
                    if (ret == MPI_SUCCESS) {
                        vbuf_address_send(vc);
                        rdma_pending_conn_request++;
                        vc->mrail.state |=  MRAILI_RFP_CONNECTING;
                    } else {
                        vc->mrail.rfp.rdma_failed = 1;
                    }
                    goto fn_exit;
                }
                MPICM_unlock();
            }
        }
    }

fn_exit:
    MPIDI_FUNC_EXIT(MPIDI_STATE_CH3I_MRAIL_PARSE_HEADER);
    return mpi_errno;

fn_fail:
    goto fn_exit;

}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAIL_Fill_Request
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_CH3I_MRAIL_Fill_Request(MPID_Request * req, vbuf * v,
                                  int header_size, int *nb)
{   
    MPL_IOV    *iov;
    int         n_iov;
    size_t      len_avail;
    void        *data_buf;
    int         i;
    MPIDI_STATE_DECL(MPIDI_STATE_CH3I_MRAIL_FILL_REQUEST);
    MPIDI_FUNC_ENTER(MPIDI_STATE_CH3I_MRAIL_FILL_REQUEST);
    
    len_avail   = v->content_size - header_size;
    iov         = (req == NULL) ? NULL : req->dev.iov;
    n_iov       = (req == NULL) ? 0 : req->dev.iov_count;
    data_buf    = (void *) ((uintptr_t) v->pheader + header_size);
    
    DEBUG_PRINT
        ("[recv:fill request] total len %d, head len %d, n iov %d\n",
         v->content_size, header_size, n_iov);
    
    if (req->mrail.is_eager_vbuf_queued == 1) {
        if (req->mrail.eager_vbuf_head == NULL) {
            req->mrail.eager_vbuf_head = v;
            req->mrail.eager_vbuf_tail = v;
        } else {
            req->mrail.eager_vbuf_tail->next = v;
            req->mrail.eager_vbuf_tail = v;
        }
        v->next = NULL;
        v->content_consumed = v->content_size;
        v->in_eager_sgl_queue = 1;   
        v->unexp_data_buf = data_buf;
        req->mrail.eager_unexp_size += len_avail;
        v->data_size = len_avail;
        *nb = len_avail;
        return MPI_SUCCESS;
    }

#ifdef _ENABLE_CUDA_
    if ( rdma_enable_cuda && is_device_buffer(iov[0].MPL_IOV_BUF)) {
        
        *nb = 0;
        MPIU_Assert(req->dev.iov_offset == 0 && n_iov == 1);
        MPIU_Memcpy_Device(iov[0].MPL_IOV_BUF,
                data_buf, 
                iov[0].MPL_IOV_LEN,
                cudaMemcpyHostToDevice);
        *nb += iov[0].MPL_IOV_LEN;
        len_avail -= iov[0].MPL_IOV_LEN;
    } else {
#endif

    *nb = 0; 
    for (i = req->dev.iov_offset; i < n_iov; i++) {
        if (len_avail >= (MPIDI_msg_sz_t) iov[i].MPL_IOV_LEN
            && iov[i].MPL_IOV_LEN != 0) {
            MPIU_Memcpy(iov[i].MPL_IOV_BUF, data_buf, iov[i].MPL_IOV_LEN);
            data_buf = (void *) ((uintptr_t) data_buf + iov[i].MPL_IOV_LEN);
            len_avail -= iov[i].MPL_IOV_LEN;
            *nb += iov[i].MPL_IOV_LEN;
        } else if (len_avail > 0) {
            MPIU_Memcpy(iov[i].MPL_IOV_BUF, data_buf, len_avail);
            *nb += len_avail;
            break;
        }
    }
#ifdef _ENABLE_CUDA_
    }
#endif
    v->content_consumed = header_size + *nb;

    DEBUG_PRINT
        ("[recv:fill request] about to return form request, nb %d\n", *nb);
    MPIDI_FUNC_EXIT(MPIDI_STATE_CH3I_MRAIL_FILL_REQUEST);
    return MPI_SUCCESS;
}

static inline void MPIDI_CH3I_MRAIL_Release_vbuf(vbuf * v)
{
    if (v->in_eager_sgl_queue == 1) {
        return;
    }

    v->eager = 0;
    v->coalesce = 0;
    v->content_size = 0; 
          
    if (v->padding == NORMAL_VBUF_FLAG || v->padding == RPUT_VBUF_FLAG) {
        MRAILI_Release_vbuf(v);
    } else {
        MRAILI_Release_recv_rdma(v);
        MRAILI_Send_noop_if_needed((MPIDI_VC_t *) v->vc, v->rail);
    }        
}

#undef FCNAME
#endif /*_IBV_SEND_INLINE_H_*/
