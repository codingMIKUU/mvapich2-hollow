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

#include "upmi.h"
#include "mpiimpl.h"
#include "rdma_impl.h"
#include "mpiutil.h"
#include <debug_utils.h>
#if defined(_MCST_SUPPORT_)
#include "ibv_mcast.h"
#endif

#undef DEBUG_PRINT
#ifdef DEBUG
#define DEBUG_PRINT(args...)                                          \
    do {                                                              \
        int rank;                                                     \
        UPMI_GET_RANK(&rank);                                          \
        fprintf(stderr, "[%d][%s:%d] ", rank, __FILE__, __LINE__);    \
        fprintf(stderr, args);                                        \
        fflush(stderr);                                               \
    } while (0)
#else
#define DEBUG_PRINT(args...)
#endif

static inline int perform_blocking_progress_for_ib(int hca_num, int num_cqs);
static inline int perform_blocking_progress_for_iwarp(int hca_num, int num_cqs);

#define PERFORM_BLOCKING_PROGRESS(_hca_num, _num_cqs)                   \
    if (likely(mv2_use_ib_channel)) {                                   \
        perform_blocking_progress_for_ib((_hca_num), (_num_cqs));       \
    } else {                                                            \
        perform_blocking_progress_for_iwarp((_hca_num), (_num_cqs));    \
    }

int cq_poll_completion = 0;
static pthread_spinlock_t g_apm_lock;
static int num_cqes[MAX_NUM_HCAS] = { 0 };
static int curr_cqe[MAX_NUM_HCAS] = { 0 };
static struct ibv_wc wc[MAX_NUM_HCAS][RDMA_MAX_CQE_ENTRIES_PER_POLL] = { 0 };
static unsigned long nspin = 0;

MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_vbuf_allocated);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_vbuf_freed);
MPIR_T_PVAR_ULONG_LEVEL_DECL_EXTERN(MV2, mv2_vbuf_available);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ud_vbuf_allocated);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ud_vbuf_freed);
MPIR_T_PVAR_ULONG_LEVEL_DECL_EXTERN(MV2, mv2_ud_vbuf_available);

MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_rdmafp_ctrl_packet_count);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2,
        mv2_rdmafp_out_of_order_packet_count);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_rdmafp_exact_recv_count);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ibv_channel_ctrl_packet_count);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2,
        mv2_ibv_channel_out_of_order_packet_count);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ibv_channel_exact_recv_count);

/* Utility function to print detailed messages for failed work completions.
 * Thanks to IBM and RDMAmojo.com for the detailed descriptions.
 */
void mv2_print_wc_status_error(enum ibv_wc_status status)
{
    switch(status) {
        case IBV_WC_LOC_LEN_ERR:
            PRINT_ERROR("IBV_WC_LOC_LEN_ERR: This event is generated when a)"
                        " the receive buffer is smaller than the incoming send,"
                        " or b) a work request that was posted in a local send"
                        " queue contains a message that is greater than the"
                        " maximum message size that is supported by the RDMA"
                        " device port that should send the message.\n");
            break;
        case IBV_WC_LOC_QP_OP_ERR:
            PRINT_ERROR("IBV_WC_LOC_QP_OP_ERR: This event is generated when a"
                        " QP error occurs. For example, it may be generated if"
                        " a) user neglects to specify responder_resources and"
                        " initiator_depth values in struct rdma_conn_param"
                        " before calling rdma_connect() on the client side and"
                        " rdma_accept() on the server side, b) a Work Request"
                        " that was posted in a local Send Queue of a UD QP"
                        " contains an Address Handle that is associated with a"
                        " Protection Domain to a QP which is associated with a"
                        " different Protection Domain, or c) an opcode which"
                        " is not supported by the transport type of the QP is"
                        " not supported (for example: RDMA Write over a UD QP).\n");
            break;
        case IBV_WC_LOC_EEC_OP_ERR:
            PRINT_ERROR("IBV_WC_LOC_EEC_OP_ERR: This event is generated when"
                        " there is an error related to the local EECs receive"
                        " logic while executing the request packet. The"
                        " responder is unable to complete the request. This"
                        " error is not caused by the sender.\n");
            break;
        case IBV_WC_LOC_PROT_ERR:
            PRINT_ERROR("IBV_WC_LOC_PROT_ERR: This event is generated when a"
                        " user attempts to access an address outside of the"
                        " registered memory region. For example, this may"
                        " happen if the Lkey does not match the address in"
                        " the WR.\n");
            break;
        case IBV_WC_WR_FLUSH_ERR:
            PRINT_ERROR("IBV_WC_WR_FLUSH_ERR: This event is generated when an"
                        " invalid remote error is thrown when the responder"
                        " detects an invalid request. It may be that the"
                        " operation is not supported by the request queue or"
                        " there is insufficient buffer space to receive the"
                        " request.\n");
            break;
        case IBV_WC_MW_BIND_ERR:
            PRINT_ERROR("IBV_WC_MW_BIND_ERR: This event is generated when a"
                        " memory management operation error occurs. The error"
                        " may be due to the fact that the memory window and the"
                        " QP belong to different protection domains. It may"
                        " also be that the memory window is not allowed to be"
                        " bound to the specified MR or the access permissions"
                        " may be wrong.\n");
            break;
        case IBV_WC_BAD_RESP_ERR:
            PRINT_ERROR("IBV_WC_BAD_RESP_ERR: This event is generated when an"
                        " unexpected transport layer opcode is returned by the"
                        " responder. Relevant to: RC or DC QPs.\n");
            break;
        case IBV_WC_LOC_ACCESS_ERR:
            PRINT_ERROR("IBV_WC_LOC_ACCESS_ERR: This event is generated when a"
                        " local protection error occurs on a local data buffer"
                        " during the process of an RDMA Write with Immediate"
                        " Data operation sent from the remote node."
                        " Relevant to: RC or DC QPs.\n");
            break;
        case IBV_WC_REM_INV_REQ_ERR:
            PRINT_ERROR("IBV_WC_REM_INV_REQ_ERR: This event is generated when"
                        " the responder detects an invalid message on the"
                        " channel. Possible causes include a) the receive"
                        " buffer is smaller than the incoming send, b) "
                        " operation is not supported by this receive queue"
                        " (qp_access_flags on the remote QP was not"
                        " configured to support this operation), or c) the"
                        " length specified in a RDMA request is greater than"
                        " 2^31 bytes. It is generated on the sender side of"
                        " the connection. Relevant to: RC or DC QPs.\n");
            break;
        case IBV_WC_REM_ACCESS_ERR:
            PRINT_ERROR("IBV_WC_REM_ACCESS_ERR: This event is generated when"
                        " a protection error occurs on a remote data buffer"
                        " to be read by an RDMA read, written by an RDMA Write"
                        " or accessed by an atomic operation. The error is"
                        " reported only on RDMA operations or atomic"
                        " operations. Relevant to: RC or DC QPs.\n");
            break;
        case IBV_WC_REM_OP_ERR:
            PRINT_ERROR("IBV_WC_REM_OP_ERR: This event is generated when an"
                        " operation cannot be completed successfully by the"
                        " responder. The failure to complete the operation"
                        " may be due to QP related errors which prevent the"
                        " responder from completing the request or a malformed"
                        " WQE on the Receive Queue. Relevant to: RC or DC QPs.\n");
            break;
        case IBV_WC_RETRY_EXC_ERR:
            PRINT_ERROR("IBV_WC_RETRY_EXC_ERR: This event is generated when"
                        " a sender is unable to receive feedback from the"
                        " receiver. This means that either the receiver just"
                        " never ACKs sender messages in a specified time"
                        " period, or it has been disconnected or it is in a"
                        " bad state which prevents it from responding."
                        " If this happens when sending the first message,"
                        " usually it means that the QP connection attributes"
                        " are wrong or the remote side is not in a state"
                        " that it can respond to messages. If this happens"
                        " after sending the first message, usually it means"
                        " that the remote QP is not available anymore or that"
                        " there is congestion in the network preventing the"
                        " packets from reaching on time."
                        " Relevant to: RC or DC QPs.\n");
            break;
        case IBV_WC_RNR_RETRY_EXC_ERR:
            PRINT_ERROR("IBV_WC_RNR_RETRY_EXC_ERR: This event is generated"
                        " when the RNR NAK retry count is exceeded. This"
                        " may be caused by lack of receive buffers on the"
                        " responder side. Relevant to: RC or DC QPs.\n");
            break;
        case IBV_WC_LOC_RDD_VIOL_ERR:
            PRINT_ERROR("IBV_WC_LOC_RDD_VIOL_ERR: This event is generated when"
                        " the RDD associated with the QP does not match the"
                        " RDD associated with the EEC.\n");
            break;
        case IBV_WC_REM_INV_RD_REQ_ERR:
            PRINT_ERROR("IBV_WC_REM_INV_RD_REQ_ERR: This event is generated"
                        " when the responder detects an invalid incoming RD"
                        " message. The message may be invalid because it has"
                        " in invalid Q_Key or there may be a Reliable"
                        " Datagram Domain (RDD) violation.\n");
            break;
        case IBV_WC_REM_ABORT_ERR:
            PRINT_ERROR("IBV_WC_REM_ABORT_ERR: This event is generated when an"
                        " error occurs on the responder side which causes it"
                        " to abort the operation. Relevant to: UD or UC QPs"
                        " associated with a SRQ.\n");
            break;
        case IBV_WC_INV_EECN_ERR:
            PRINT_ERROR("IBV_WC_INV_EECN_ERR: This event is generated when an"
                        " invalid End to End Context Number (EECN) is detected.\n");
            break;
        case IBV_WC_INV_EEC_STATE_ERR:
            PRINT_ERROR("IBV_WC_INV_EEC_STATE_ERR: This event is generated when"
                        " an illegal operation is detected in a request for the"
                        " specified EEC state.\n");
            break;
        case IBV_WC_FATAL_ERR:
            PRINT_ERROR("IBV_WC_FATAL_ERR: This event is generated when a fatal"
                        " transport error occurs. The user may have to restart"
                        " the RDMA device driver or reboot the server to"
                        " recover from the error.\n");
            break;
        case IBV_WC_RESP_TIMEOUT_ERR:
            PRINT_ERROR("IBV_WC_RESP_TIMEOUT_ERR: This event is generated when"
                        " the responder is unable to respond to a request"
                        " within the timeout period. It generally indicates that"
                        " the receiver is not ready to process requests.\n");
            break;
        case IBV_WC_GENERAL_ERR:
            PRINT_ERROR("IBV_WC_GENERAL_ERR: This event is generated when there"
                        " is a transport error which cannot be described by the"
                        " other specific events discussed here.\n");
            break;
        default:
            PRINT_ERROR("ERROR: Unknown work completion status %d\n", status);
            break;
    }
}

volatile int mv2_in_blocking_progress = 0;

/*
 * TODO add error handling
 */
void init_apm_lock()
{
    pthread_spin_init(&g_apm_lock, 0);
}

#undef FUNCNAME
#define FUNCNAME lock_apm
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static void lock_apm()
{
    pthread_spin_lock(&g_apm_lock);
    return;
}

#undef FUNCNAME
#define FUNCNAME unlock_apm 
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static void unlock_apm()
{
    pthread_spin_unlock(&g_apm_lock);
    return;
}

const char *MPIDI_CH3_VC_GetStateString(MPIDI_VC_t *vc)
{
    return NULL;
}

MRAILI_Channel_manager *arriving_head = NULL;
MRAILI_Channel_manager *arriving_tail = NULL;

#define INDEX_GLOBAL(_cmanager,_global_index) (_global_index) 

#define INDEX_LOCAL(_cmanager,_local_index) \
    (((_cmanager)->num_channels - (_cmanager)->num_local_pollings) + (_local_index))

static inline void CHANNEL_ENQUEUE(MRAILI_Channel_manager* cmanager)
{
    if (arriving_tail == NULL)
    {
        arriving_head = arriving_tail = cmanager;
        cmanager->next_arriving = NULL;
    }
    else
    {
        arriving_tail->next_arriving = cmanager;
        cmanager->next_arriving = NULL;
        arriving_tail = cmanager;
    }

    cmanager->inqueue = 1;
}

static inline void VQUEUE_ENQUEUE(MRAILI_Channel_manager* cmanager, int index, vbuf* v)
{                      
    v->desc.next = NULL;

    if (cmanager->msg_channels[index].v_queue_tail == NULL)
    { 
        cmanager->msg_channels[index].v_queue_head = v;      
    }
    else
    {
        cmanager->msg_channels[index].v_queue_tail->desc.next = v;
    }

    cmanager->msg_channels[index].v_queue_tail = v;
    ++cmanager->msg_channels[index].len;

    if (!cmanager->inqueue)
    {
        CHANNEL_ENQUEUE(cmanager);
    }
}

static inline vbuf* VQUEUE_DEQUEUE(MRAILI_Channel_manager* cmanager, int index)
{
    vbuf* v = cmanager->msg_channels[index].v_queue_head;
    cmanager->msg_channels[index].v_queue_head = v->desc.next;

    if (v == cmanager->msg_channels[index].v_queue_tail)
    {
        cmanager->msg_channels[index].v_queue_tail = NULL;
    }

    --cmanager->msg_channels[index].len;
    v->desc.next = NULL;
    return v;
}

static inline int PKT_IS_NOOP(void* v)
{        
    return ((MPIDI_CH3I_MRAILI_Pkt_comm_header*) ((vbuf*) v)->pheader)->type == MPIDI_CH3_PKT_NOOP;
}

#undef FUNCNAME
#define FUNCNAME GetSeqNumVbuf
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
/*FIXME: Ideally this functionality should be provided by higher levels*/
static inline int GetSeqNumVbuf(vbuf * buf)
{
    MPIDI_CH3I_MRAILI_Pkt_comm_header *p;
    if (NULL == buf) {
        return PKT_IS_NULL;
    }

    p = buf->pheader;

    switch (p->type) {
        case MPIDI_CH3_PKT_NOOP:
        {
            return PKT_NO_SEQ_NUM;
        }
#ifndef MV2_DISABLE_HEADER_CACHING 
        case MPIDI_CH3_PKT_FAST_EAGER_SEND:
        case MPIDI_CH3_PKT_FAST_EAGER_SEND_WITH_REQ:
        {
            return ((MPIDI_CH3I_MRAILI_Pkt_fast_eager *)(p))->seqnum;
        }
#endif
        default:
            return (p->seqnum);
    }
}

static inline vbuf * MPIDI_CH3I_RDMA_poll(MPIDI_VC_t * vc)
{
    vbuf *v = NULL;
    volatile VBUF_FLAG_TYPE *head;
    volatile VBUF_FLAG_TYPE *tail;
    int size;

    if (num_rdma_buffer == 0)
        return NULL;

    v = &(vc->mrail.rfp.RDMA_recv_buf[vc->mrail.rfp.p_RDMA_recv]);
    head = v->head_flag;

    if (*head && vc->mrail.rfp.p_RDMA_recv != vc->mrail.rfp.p_RDMA_recv_tail) {
        size = (*head & FAST_RDMA_SIZE_MASK);
        tail = (VBUF_FLAG_TYPE *) (v->buffer + size);
        /* rdma write is in progress, the tail has not received yet.*/
        if(*head != *tail) {
            return NULL;
        }
        /* advance receive pointer */
        if (++(vc->mrail.rfp.p_RDMA_recv) >= num_rdma_buffer) {
            vc->mrail.rfp.p_RDMA_recv = 0;
        }
        v->pheader = v->buffer;
            PRINT_DEBUG(DEBUG_CHM_verbose, "recv %d, tail %d, size %ld\n",
                    vc->mrail.rfp.p_RDMA_recv, vc->mrail.rfp.p_RDMA_recv_tail, *head);
        v->content_size = (*head & FAST_RDMA_SIZE_MASK);
    } else {
        v = NULL;
    }
    return v;
}

static int MPIDI_CH3I_MRAILI_Test_pkt(vbuf **vbuf_handle)
{
    int type = T_CHANNEL_NO_ARRIVE;
    while (arriving_head) {
        type = MPIDI_CH3I_MRAILI_Waiting_msg(arriving_head->vc, vbuf_handle, 0);
        if (type == T_CHANNEL_NO_ARRIVE) {
            arriving_head->inqueue = 0;
            arriving_head = arriving_head->next_arriving;
            if (!arriving_head)
                arriving_tail = NULL;
        } else {
            break;
        }
    }

    return type;
}

/* Yet this functionality has not been implemented */
int MPIDI_CH3I_MRAILI_Register_channels(MPIDI_VC_t *vc, int num, vbuf *(*func[])(void *))
{
    return MPI_SUCCESS;    
}

static inline void handle_multiple_cqs_for_iwarp(int num_cqs, int cq_choice, int is_send_completion)
{
    if (2 == num_cqs) {
        if (0 == cq_choice) {
            if (mv2_MPIDI_CH3I_RDMA_Process.global_used_send_cq) {
                 mv2_MPIDI_CH3I_RDMA_Process.global_used_send_cq--;
            } else {
                PRINT_DEBUG(DEBUG_CHM_verbose,
                    "Possibly received a duplicate send completion event\n");
            }
        } 
    } else {
        if (is_send_completion) {
            if (mv2_MPIDI_CH3I_RDMA_Process.global_used_send_cq > 0) {
                mv2_MPIDI_CH3I_RDMA_Process.global_used_send_cq--;
            } else {
                PRINT_DEBUG(DEBUG_CHM_verbose,
                        "Possibly received a duplicate send completion event\n");
            }
        }     
    }

    return;
}

static inline void handle_multiple_cqs_for_ib(int num_cqs, int cq_choice, int is_send_completion)
{
    return;
}

static inline int handle_cqe(vbuf **vbuf_handle, MPIDI_VC_t * vc_req,
        int receiving, struct ibv_wc wc, int num_cqs, int cq_choice,
        int hca_num)
{
    vbuf *v = NULL;
    int type = T_CHANNEL_NO_ARRIVE;
    int needed = 0;
    MPIDI_VC_t *vc = NULL;
    int is_send_completion = 0;
    MPIDI_CH3I_MRAILI_Pkt_comm_header *p = NULL;

    v = (vbuf *) ((uintptr_t) wc.wr_id);
    vc = (MPIDI_VC_t *) (v->vc);
    cq_poll_completion = 1;

    if (unlikely(wc.status != IBV_WC_SUCCESS)) {
        if (wc.opcode == IBV_WC_SEND ||
            wc.opcode == IBV_WC_RDMA_WRITE ||
            wc.opcode == IBV_WC_FETCH_ADD ) {
            PRINT_ERROR("Send desc error in msg to %d, wc_opcode=%d\n", vc->pg_rank, wc.opcode );
        } else {
    		PRINT_ERROR("Recv desc error in msg from %d, wc_opcode=%d\n",vc->pg_rank, wc.opcode);
		}
        PRINT_ERROR("Msg from %d: wc.status=%d (%s), wc.wr_id=%p, wc.opcode=%d, vbuf->phead->type=%d = %s\n",
                    vc->pg_rank, wc.status, ibv_ops.wc_status_str(wc.status), v, wc.opcode,
                    ((MPIDI_CH3I_MRAILI_Pkt_comm_header*)v->pheader)->type,
                    MPIDI_CH3_Pkt_type_to_string[((MPIDI_CH3I_MRAILI_Pkt_comm_header*)v->pheader)->type]);
        mv2_print_wc_status_error(wc.status);

        ibv_va_error_abort(IBV_STATUS_ERR,
                "[] Got completion with error %d, vendor code=0x%x, dest rank=%d\n",
                wc.status, wc.vendor_err, ((MPIDI_VC_t *)v->vc)->pg_rank);
    }

    is_send_completion = (wc.opcode == IBV_WC_SEND ||
                          wc.opcode == IBV_WC_RDMA_WRITE ||
                          wc.opcode == IBV_WC_RDMA_READ ||
                          wc.opcode == IBV_WC_FETCH_ADD ||
#ifdef _ENABLE_WC_DRV1_
                          wc.opcode == IBV_WC_DRIVER1 ||
#endif /* _ENABLE_WC_DRV1_ */
#ifdef _ENABLE_WC_DRV2_
                          wc.opcode == IBV_WC_DRIVER2 ||
#endif /* _ENABLE_WC_DRV2_ */
                          wc.opcode == IBV_WC_COMP_SWAP);

    /* iWARP has the need for multiple CQ's, IB does not */
    if (unlikely(mv2_use_ib_channel == 0)) {
	handle_multiple_cqs_for_iwarp(num_cqs, cq_choice, is_send_completion);
    }

	/* get the VC and increase its wqe */
	if (is_send_completion) {
#ifdef _ENABLE_UD_
        p = v->pheader;
        if (rdma_enable_hybrid
#ifdef _MCST_SUPPORT_
            && !IS_MCAST_MSG(p)
#endif
           )
        {
            if(v->transport == IB_TRANSPORT_RC  || 
                (v->pheader && IS_CNTL_MSG(p) && v->transport != IB_TRANSPORT_UD)) {
                MRAILI_Process_send(v);
            }
            if (v->transport == IB_TRANSPORT_UD) {
                mv2_ud_update_send_credits(v);
            }
            if(v->transport == IB_TRANSPORT_UD &&
               v->flags & UD_VBUF_SEND_INPROGRESS) {
                v->flags &= ~(UD_VBUF_SEND_INPROGRESS);
                if (v->flags & UD_VBUF_FREE_PENIDING) {
                    v->flags &= ~(UD_VBUF_FREE_PENIDING);
                    MRAILI_Release_vbuf(v);
                }
            }
        }
        else
#endif
        {
	        MRAILI_Process_send(v);
        }
            type = T_CHANNEL_NO_ARRIVE;
            *vbuf_handle = NULL;
    } else {
        /* This is a receive completion */
        if (mv2_MPIDI_CH3I_RDMA_Process.has_srq
#ifdef _ENABLE_UD_
            || v->transport == IB_TRANSPORT_UD
#endif
           ) {
            SET_PKT_LEN_HEADER(v, wc);
            SET_PKT_HEADER_OFFSET(v);
            p = v->pheader;
#ifdef _MCST_SUPPORT_
            if (IS_MCAST_MSG(p)) {
                mv2_process_mcast_msg(v);
                return T_CHANNEL_NO_ARRIVE;
            }
            else
#endif
#ifdef _ENABLE_UD_
	        if (rdma_enable_hybrid) {
                MPIDI_PG_Get_vc(MPIDI_Process.my_pg, p->src.rank, &vc);
	        } else
#endif
            {
                vc = (MPIDI_VC_t *)p->src.vc_addr;
            }

            v->vc = vc;
            v->rail = p->rail;
        }
        if (likely(NULL == vc_req || vc_req == vc)) {
            /* In this case, we should return the vbuf
             * any way if it is next expected*/
            int seqnum = GetSeqNumVbuf(v);
            *vbuf_handle = v;
            SET_PKT_LEN_HEADER(v, wc);
            SET_PKT_HEADER_OFFSET(v);
            v->seqnum =  seqnum;
            p = v->pheader;
            PRINT_DEBUG(DEBUG_CHM_verbose>1,"Received from rank:%d seqnum :%d "
                    "ack:%d size:%zu type:%d transport:%d \n",vc->pg_rank,
                    v->seqnum, p->acknum, v->content_size, p->type, v->transport);
#ifdef _ENABLE_UD_
            if (v->transport == IB_TRANSPORT_UD)
            {
                mv2_ud_ctx_t *ud_ctx =
                    mv2_MPIDI_CH3I_RDMA_Process.ud_rails[hca_num];
                if (rdma_use_ud_srq) {
                    /* Check if we need to release the SRQ limit thread */
                    if (unlikely(mv2_MPIDI_CH3I_RDMA_Process.
                                srq_zero_post_counter[hca_num] >= 1)) {
                        pthread_mutex_lock(
                                &mv2_MPIDI_CH3I_RDMA_Process.
                                srq_post_mutex_lock[hca_num]);
                        mv2_MPIDI_CH3I_RDMA_Process.srq_zero_post_counter[hca_num] = 0;
                        pthread_cond_signal(&mv2_MPIDI_CH3I_RDMA_Process.
                                srq_post_cond[hca_num]);
                        pthread_mutex_unlock(
                                &mv2_MPIDI_CH3I_RDMA_Process.
                                srq_post_mutex_lock[hca_num]);
                    }
                } else {
                    --ud_ctx->num_recvs_posted;
                    if(ud_ctx->num_recvs_posted < ud_ctx->credit_preserve) {
                        if(ud_ctx->num_recvs_posted < ud_ctx->credit_preserve) {
                            int max_ud_bufs = (rdma_use_ud_srq)?mv2_ud_srq_fill_size:rdma_default_max_ud_recv_wqe;
                            ud_ctx->num_recvs_posted += mv2_post_ud_recv_buffers(
                                                            (max_ud_bufs-ud_ctx->num_recvs_posted), ud_ctx);
                        }
                    }
                }
            }
            else
#endif 
                if (likely(mv2_MPIDI_CH3I_RDMA_Process.has_srq)) {
                    /* Check if we need to release the SRQ limit thread */
                    if (unlikely(mv2_MPIDI_CH3I_RDMA_Process.
                                srq_zero_post_counter[hca_num] >= 1)) {
                        pthread_mutex_lock(
                                &mv2_MPIDI_CH3I_RDMA_Process.
                                srq_post_mutex_lock[hca_num]);
                        mv2_MPIDI_CH3I_RDMA_Process.srq_zero_post_counter[hca_num] = 0;
                        pthread_cond_signal(&mv2_MPIDI_CH3I_RDMA_Process.
                                srq_post_cond[hca_num]);
                        pthread_mutex_unlock(
                                &mv2_MPIDI_CH3I_RDMA_Process.
                                srq_post_mutex_lock[hca_num]);
                    }
                }
                else
                {
                    --vc->mrail.srp.credits[v->rail].preposts;

                    needed = rdma_prepost_depth + rdma_prepost_noop_extra
                        + MIN(rdma_prepost_rendezvous_extra,
                                vc->mrail.srp.credits[v->rail].
                                rendezvous_packets_expected);
                }
#ifdef _ENABLE_UD_
            if (rdma_enable_hybrid){
                if (IS_CNTL_MSG(p)){
                    type = T_CHANNEL_CONTROL_MSG_ARRIVE;
                    MPIR_T_PVAR_COUNTER_INC(MV2,
                            mv2_ibv_channel_ctrl_packet_count, 1);
                } else {
                    type = T_CHANNEL_HYBRID_MSG_ARRIVE;
                }
            }
            else
#endif
            {
                if (seqnum == PKT_NO_SEQ_NUM){
                    type = T_CHANNEL_CONTROL_MSG_ARRIVE;
                    MPIR_T_PVAR_COUNTER_INC(MV2,
                            mv2_ibv_channel_ctrl_packet_count, 1);
                } else if (seqnum == vc->mrail.seqnum_next_torecv) {
                    vc->mrail.seqnum_next_toack = vc->mrail.seqnum_next_torecv;
                    ++vc->mrail.seqnum_next_torecv;
                    type = T_CHANNEL_EXACT_ARRIVE;
                    PRINT_DEBUG(DEBUG_CHM_verbose, "[channel manager] get one with exact seqnum\n");
                } else {
                    type = T_CHANNEL_OUT_OF_ORDER_ARRIVE;
                    VQUEUE_ENQUEUE(&vc->mrail.cmanager,
                            INDEX_GLOBAL(&vc->mrail.cmanager, v->rail),
                            v);
                    PRINT_DEBUG(DEBUG_CHM_verbose, "get recv %d (%d)\n", seqnum,
                            vc->mrail.seqnum_next_torecv);
                }
            }
            if (unlikely(!mv2_MPIDI_CH3I_RDMA_Process.has_srq && v->transport != IB_TRANSPORT_UD)) {

                if (PKT_IS_NOOP(v)) {
                    PREPOST_VBUF_RECV(vc, v->rail);
                    /* noops don't count for credits */
                    --vc->mrail.srp.credits[v->rail].local_credit;
                }
                else if ((vc->mrail.srp.credits[v->rail].preposts
                            < rdma_rq_size) &&
                        (vc->mrail.srp.credits[v->rail].preposts +
                         rdma_prepost_threshold < needed))
                {
                    do {
                        PREPOST_VBUF_RECV(vc, v->rail);
                    } while (vc->mrail.srp.credits[v->rail].preposts
                            < rdma_rq_size &&
                            vc->mrail.srp.credits[v->rail].preposts
                            < needed);
                }

                MRAILI_Send_noop_if_needed(vc, v->rail);
            }
            if (type == T_CHANNEL_CONTROL_MSG_ARRIVE ||
                type == T_CHANNEL_EXACT_ARRIVE ||
                type == T_CHANNEL_HYBRID_MSG_ARRIVE ||
                type == T_CHANNEL_OUT_OF_ORDER_ARRIVE) {
                goto fn_exit;
            }
        } else {
            /* Now since this is not the packet we want, enqueue it */
            type = T_CHANNEL_OUT_OF_ORDER_ARRIVE;
            *vbuf_handle = NULL;
            v->content_size = wc.byte_len;
            VQUEUE_ENQUEUE(&vc->mrail.cmanager,
                    INDEX_GLOBAL(&vc->mrail.cmanager, v->rail),
                    v);
            if (v->transport != IB_TRANSPORT_UD) {
                if (unlikely(!mv2_MPIDI_CH3I_RDMA_Process.has_srq)) {
                    --vc->mrail.srp.credits[v->rail].preposts;

                    needed = rdma_prepost_depth + rdma_prepost_noop_extra
                        + MIN(rdma_prepost_rendezvous_extra,
                                vc->mrail.srp.credits[v->rail].
                                rendezvous_packets_expected);

                    if (PKT_IS_NOOP(v)) {
                        PREPOST_VBUF_RECV(vc, v->rail);
                        --vc->mrail.srp.credits[v->rail].local_credit;
                    }
                    else if ((vc->mrail.srp.credits[v->rail].preposts
                                < rdma_rq_size) &&
                            (vc->mrail.srp.credits[v->rail].preposts +
                             rdma_prepost_threshold < needed)) {
                        do {
                            PREPOST_VBUF_RECV(vc, v->rail);
                        } while (vc->mrail.srp.credits[v->rail].preposts
                                < rdma_rq_size &&
                                vc->mrail.srp.credits[v->rail].preposts
                                < needed);
                    }
                    MRAILI_Send_noop_if_needed(vc, v->rail);
                }
            }
        }
    }

fn_exit:
    return type;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_RDMA_cq_poll_iwarp
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3I_MRAILI_Cq_poll_iwarp(vbuf **vbuf_handle, 
        MPIDI_VC_t * vc_req, int receiving, int is_blocking)
{
    int ne = 0;
    int i = 0;
    int cq_choice = 0;
    int num_cqs = 0;
    int type = T_CHANNEL_NO_ARRIVE;
    struct ibv_cq *chosen_cq = NULL;

    MPIDI_STATE_DECL(MPID_GEN2_MRAILI_CQ_POLL_IWARP);
    MPIDI_FUNC_ENTER(MPID_GEN2_MRAILI_CQ_POLL_IWARP);

    *vbuf_handle = NULL;
    MRAILI_Hollow_retry_blocked();

    if (!vc_req) {
        type = MPIDI_CH3I_MRAILI_Test_pkt(vbuf_handle);
        if (type == T_CHANNEL_EXACT_ARRIVE 
                || type == T_CHANNEL_CONTROL_MSG_ARRIVE)
            goto fn_exit;
    }

    if (rdma_iwarp_use_multiple_cq &&
        MV2_IS_CHELSIO_IWARP_CARD(mv2_MPIDI_CH3I_RDMA_Process.hca_type) &&
        (mv2_MPIDI_CH3I_RDMA_Process.cluster_size != VERY_SMALL_CLUSTER)) {
        num_cqs = 2;
    } else {
        num_cqs = 1;
    }

    for (i = 0; i < rdma_num_hcas; ++i) {
        for (cq_choice = 0; cq_choice < num_cqs; ++cq_choice) {
            if (1 == num_cqs) {
	            chosen_cq = mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[i];
	        } else {
	            if (0 == cq_choice) {
	                chosen_cq = mv2_MPIDI_CH3I_RDMA_Process.send_cq_hndl[i];
                } else {
	                chosen_cq = mv2_MPIDI_CH3I_RDMA_Process.recv_cq_hndl[i];
                }
	        }

            if (curr_cqe[i] < num_cqes[i]) {
                /* We already have CQEs polled out from last call to
                 * ibv_poll_cq. Drain them before polling again */
                do {
                    type = handle_cqe(vbuf_handle, vc_req, receiving, wc[i][curr_cqe[i]],
                                    num_cqs, cq_choice, i);
                    curr_cqe[i]++;
                    /* Drain till we get in-order recv or run out of polled CQEs */
                } while ((*vbuf_handle == NULL) && (curr_cqe[i] < num_cqes[i]));

                if (*vbuf_handle != NULL) {
                    /* We got in-order data, deliver it to higher level */
                    goto fn_exit;
                }
            } else {
                memset(wc[i], 0, sizeof(struct ibv_wc) * num_cqes[i]);
	            ne = ibv_poll_cq(chosen_cq, rdma_num_cqes_per_poll, wc[i]);

    	        if (unlikely(ne < 0)) {
    	            ibv_error_abort(IBV_RETURN_ERR, "Fail to poll cq\n");
    	        } else if (ne) {         
                    curr_cqe[i] = 0;
                    num_cqes[i] = ne;
    
                    do {
                        type = handle_cqe(vbuf_handle, vc_req, receiving, wc[i][curr_cqe[i]],
                                            num_cqs, cq_choice, i);
                        curr_cqe[i]++;
                        /* Drain till we get in-order recv or run out of polled CQEs */
                    } while ((*vbuf_handle == NULL) && (curr_cqe[i] < num_cqes[i]));

                    if (*vbuf_handle != NULL) {
                        /* We got in-order data, deliver it to higher level */
                        goto fn_exit;
                    }
    	        } else {
    	            *vbuf_handle = NULL;
    	            type = T_CHANNEL_NO_ARRIVE;
    	            ++nspin;
    	
    	            /* Blocking mode progress */
    	            if(rdma_use_blocking && is_blocking && nspin >= rdma_blocking_spin_count_threshold) {
                        mv2_in_blocking_progress = 1;
                        PERFORM_BLOCKING_PROGRESS(i, num_cqs);
                        mv2_in_blocking_progress = 0;
    	                nspin = 0;
    	            }
    	        }
            }
        }
    }
fn_exit:
    MPIDI_FUNC_EXIT(MPID_GEN2_MRAILI_CQ_POLL_IWARP);
    return type;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAILI_Get_next_vbuf
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3I_MRAILI_Get_next_vbuf(MPIDI_VC_t** vc_ptr, vbuf** vbuf_ptr) 
{
    *vc_ptr = NULL;
    *vbuf_ptr = NULL;
    VBUF_FLAG_TYPE size;
    int type;
    MPIDI_STATE_DECL(MPID_GEN2_MPIDI_CH3I_MRAILI_GET_NEXT_VBUF);
    MPIDI_FUNC_ENTER(MPID_GEN2_MPIDI_CH3I_MRAILI_GET_NEXT_VBUF);

    int i = 0;
    MPIDI_VC_t* vc = NULL;
    int seq;
    vbuf* v = NULL;
    volatile VBUF_FLAG_TYPE* tail = NULL;
    volatile VBUF_FLAG_TYPE* head = NULL;

    /* no msg is queued, poll rdma polling set */
    for (i = 0; i < mv2_MPIDI_CH3I_RDMA_Process.polling_group_size; ++i)
    {
        vc = mv2_MPIDI_CH3I_RDMA_Process.polling_set[i];
        v = &(vc->mrail.rfp.RDMA_recv_buf[vc->mrail.rfp.p_RDMA_recv]);
        head = v->head_flag;

        if (*head && vc->mrail.rfp.p_RDMA_recv != vc->mrail.rfp.p_RDMA_recv_tail)
        {
            size = (*head & FAST_RDMA_SIZE_MASK);
            tail = (VBUF_FLAG_TYPE *) (v->buffer + size);
            /* If the tail has not received yet, than go ahead and
            ** poll next connection */
            READBAR();
            if (*head != *tail) {
                continue;
            }
            
            PRINT_DEBUG(DEBUG_CHM_verbose, "Get one!\n");

            if (++vc->mrail.rfp.p_RDMA_recv >= num_rdma_buffer)
            {
                vc->mrail.rfp.p_RDMA_recv = 0;
            }

            v->pheader = v->buffer;
            v->content_size = size;
            *head = 0;

            seq = GetSeqNumVbuf(v);

#ifdef _ENABLE_UD_
            if (rdma_enable_hybrid){
                v->seqnum = seq;
                type = T_CHANNEL_HYBRID_MSG_ARRIVE;
                *vbuf_ptr = v;
                *vc_ptr = v->vc;
                PRINT_DEBUG(DEBUG_UD_verbose>1,"received seqnum:%d expected:%d vc_ptr:%p\n",seq, vc->mrail.seqnum_next_torecv, v->vc);
                goto fn_exit;
            } 
            else
#endif
            {
                if (seq == vc->mrail.seqnum_next_torecv)
                {
                    MPIR_T_PVAR_COUNTER_INC(MV2,
                            mv2_rdmafp_exact_recv_count, 1);
                    PRINT_DEBUG(DEBUG_CHM_verbose, "Get one exact seq: %d\n", seq);
                    type = T_CHANNEL_EXACT_ARRIVE;
                    ++vc->mrail.seqnum_next_torecv;
                    *vbuf_ptr = v;
                    *vc_ptr = v->vc;
                    goto fn_exit;
                }
                else if (seq == PKT_NO_SEQ_NUM)
                {
                    MPIR_T_PVAR_COUNTER_INC(MV2,
                            mv2_rdmafp_ctrl_packet_count, 1);
                    type = T_CHANNEL_CONTROL_MSG_ARRIVE;
                    PRINT_DEBUG(DEBUG_CHM_verbose, "[vbuf_local]: get control msg\n");
                    *vbuf_ptr = v;
                    *vc_ptr = v->vc;
                    goto fn_exit;
                }
                else
                {
                    MPIR_T_PVAR_COUNTER_INC(MV2,
                            mv2_rdmafp_out_of_order_packet_count, 1);
                    PRINT_DEBUG(DEBUG_CHM_verbose, "Get one out of order seq: %d, expecting %d\n",
                            seq, vc->mrail.seqnum_next_torecv);
                    VQUEUE_ENQUEUE(&vc->mrail.cmanager, INDEX_LOCAL(&vc->mrail.cmanager, 0), v);
                    goto out_of_order_handling;
                }
            }
        }
out_of_order_handling:
        /* No packets in regular flow. Look for out of order packets */
        seq  = GetSeqNumVbuf(vc->mrail.cmanager.msg_channels[INDEX_LOCAL(&vc->mrail.cmanager,0)].v_queue_head);
        if (seq == vc->mrail.seqnum_next_torecv) {
            *vbuf_ptr = VQUEUE_DEQUEUE(&vc->mrail.cmanager, INDEX_LOCAL(&vc->mrail.cmanager, 0));
            *vc_ptr = (*vbuf_ptr)->vc;
            ++vc->mrail.seqnum_next_torecv;
            type = T_CHANNEL_EXACT_ARRIVE;
            MPIR_T_PVAR_COUNTER_INC(MV2, mv2_ibv_channel_exact_recv_count, 1);
            goto fn_exit;
        } else if (seq == PKT_NO_SEQ_NUM) {
            *vbuf_ptr = VQUEUE_DEQUEUE(&vc->mrail.cmanager, INDEX_LOCAL(&vc->mrail.cmanager, 0));
            *vc_ptr = (*vbuf_ptr)->vc;
            type = T_CHANNEL_CONTROL_MSG_ARRIVE;
            goto fn_exit;
        }

    }
    /* Check for out of order packets */
    type = MPIDI_CH3I_MRAILI_Test_pkt(vbuf_ptr);
    switch(type)
    {
    case T_CHANNEL_CONTROL_MSG_ARRIVE:
    case T_CHANNEL_EXACT_ARRIVE:
            *vc_ptr = (*vbuf_ptr)->vc;
        goto fn_exit;
    case T_CHANNEL_OUT_OF_ORDER_ARRIVE:
            type = T_CHANNEL_NO_ARRIVE;
            *vbuf_ptr = NULL;
        break;
    }

fn_exit:
    MPIDI_FUNC_EXIT(MPID_GEN2_MPIDI_CH3I_MRAILI_GET_NEXT_VBUF);
    return type;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAILI_Waiting_msg
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3I_MRAILI_Waiting_msg(MPIDI_VC_t * vc, vbuf ** vbuf_handle, int blocking) 
{
    MRAILI_Channel_manager * cmanager = &vc->mrail.cmanager;
    int i = 0;
    int receiving = 0;
    int seq;
    int seq_expected = vc->mrail.seqnum_next_torecv;
    int type = T_CHANNEL_NO_ARRIVE;
    MPIDI_STATE_DECL(MPID_GEN2_MPIDI_CH3I_MRAILIWAITING_MSG);
    MPIDI_FUNC_ENTER(MPID_GEN2_MPIDI_CH3I_MRAILIWAITING_MSG);

    *vbuf_handle = NULL;

    PRINT_DEBUG((blocking&&DEBUG_CHM_verbose),
            "{entering} solve_out_of_order next expected %d, channel %d, head %p (%d)\n",
            vc->mrail.seqnum_next_torecv, cmanager->num_channels,
            cmanager->msg_channels[0].v_queue_head,
            GetSeqNumVbuf(cmanager->msg_channels[0].v_queue_head));

    for (; i < cmanager->num_channels; ++i)
    {
        seq = GetSeqNumVbuf(cmanager->msg_channels[i].v_queue_head);
        if (seq == seq_expected) {
            *vbuf_handle = VQUEUE_DEQUEUE(cmanager, i);
            type = T_CHANNEL_EXACT_ARRIVE;
            ++vc->mrail.seqnum_next_torecv;
            goto fn_exit;
        } else if (PKT_NO_SEQ_NUM == seq) {
            *vbuf_handle = VQUEUE_DEQUEUE(cmanager, i);
            type = T_CHANNEL_CONTROL_MSG_ARRIVE;
            MPIR_T_PVAR_COUNTER_INC(MV2, mv2_ibv_channel_ctrl_packet_count, 1);
            goto fn_exit;
        } else if (PKT_IS_NULL == seq) {
            /* Do nothing */
        } else {
            *vbuf_handle = cmanager->msg_channels[i].v_queue_head;
            type = T_CHANNEL_OUT_OF_ORDER_ARRIVE;
        }
    }

    /* Obviously the packet with correct sequence hasn't arrived */
    while (blocking) {
        /* poll local subrails*/
        for (i = 0; i < cmanager->num_local_pollings; ++i) {
            seq = GetSeqNumVbuf(cmanager->msg_channels[INDEX_LOCAL(cmanager,i)].v_queue_head);
            if (seq == seq_expected) {
                *vbuf_handle = VQUEUE_DEQUEUE(cmanager, INDEX_LOCAL(cmanager,i));
                ++vc->mrail.seqnum_next_torecv;
                type = T_CHANNEL_EXACT_ARRIVE;
                goto fn_exit;
            } else if (seq == PKT_NO_SEQ_NUM) {
                *vbuf_handle = VQUEUE_DEQUEUE(cmanager, INDEX_LOCAL(cmanager,i));
                type = T_CHANNEL_CONTROL_MSG_ARRIVE;
                MPIR_T_PVAR_COUNTER_INC(MV2, mv2_ibv_channel_ctrl_packet_count,
                        1);
                goto fn_exit;
            }
            else if (vc->mrail.rfp.in_polling_set) {
                *vbuf_handle = MPIDI_CH3I_RDMA_poll(vc);
                seq = GetSeqNumVbuf(*vbuf_handle);
                if (seq == seq_expected) {
                    type = T_CHANNEL_EXACT_ARRIVE;
                    ++vc->mrail.seqnum_next_torecv;
                    goto fn_exit;
                }
                else if( seq == PKT_NO_SEQ_NUM) {
                    type = T_CHANNEL_CONTROL_MSG_ARRIVE;
                    MPIR_T_PVAR_COUNTER_INC(MV2,
                            mv2_ibv_channel_ctrl_packet_count, 1);
                    goto fn_exit;
                } else if (*vbuf_handle != NULL){
                    VQUEUE_ENQUEUE(cmanager, INDEX_LOCAL(cmanager,i), *vbuf_handle);
                    *vbuf_handle = NULL;
                }
            }
        }

        if (likely(mv2_use_ib_channel == 1)) {
            type = MPIDI_CH3I_MRAILI_Cq_poll_ib(vbuf_handle, vc, receiving, blocking);
        } else {
            type = MPIDI_CH3I_MRAILI_Cq_poll_iwarp(vbuf_handle, vc, receiving, blocking);
        }
        if (type != T_CHANNEL_NO_ARRIVE) {
            switch(type) {
                case (T_CHANNEL_EXACT_ARRIVE):
                case (T_CHANNEL_CONTROL_MSG_ARRIVE):
                    goto fn_exit;
                case (T_CHANNEL_OUT_OF_ORDER_ARRIVE):
                    continue;
                default:
                    ibv_error_abort(GEN_ASSERT_ERR, "Unexpected return type\n");
                    break;
            }
        }
    }
fn_exit:
    PRINT_DEBUG((blocking&&DEBUG_CHM_verbose), "{return} solve_out_of_order, type %d, next expected %d\n", 
                type, vc->mrail.seqnum_next_torecv);
    MPIDI_FUNC_EXIT(MPID_GEN2_MPIDI_CH3I_MRAILIWAITING_MSG);
    return type;
}

static inline int perform_blocking_progress_for_ib(int hca_num, int num_cqs)
{
    int ret = 0, err = 0;
    void *ev_ctx = NULL;
    struct ibv_cq *ev_cq = NULL;

    /* Okay ... spun long enough, now time to go to sleep! */

#if (MPICH_THREAD_LEVEL == MPI_THREAD_MULTIPLE)
    MPIU_THREAD_CHECK_BEGIN
    MPID_Thread_mutex_unlock(&MPIR_ThreadInfo.global_mutex, &err);
    MPIU_THREAD_CHECK_END
#endif
    do {    
        if (unlikely(ibv_req_notify_cq(
                        mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_num], 0))) {
            ibv_error_abort(IBV_RETURN_ERR,
                "Couldn't request for CQ notification\n");
        }
        ret = ibv_ops.get_cq_event(
                mv2_MPIDI_CH3I_RDMA_Process.comp_channel[hca_num], 
                &ev_cq, &ev_ctx);
        if (unlikely(ret && errno != EINTR)) {
            ibv_va_error_abort(IBV_RETURN_ERR,
                    "Failed to get cq event: %d\n", ret);
        }       
    } while (ret && errno == EINTR); 
#if (MPICH_THREAD_LEVEL == MPI_THREAD_MULTIPLE)
    MPIU_THREAD_CHECK_BEGIN
    MPID_Thread_mutex_lock(&MPIR_ThreadInfo.global_mutex, &err);
    MPIU_THREAD_CHECK_END
#endif

	if (unlikely(ev_cq != mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_num])) {
	    ibv_error_abort(IBV_STATUS_ERR,
                         "Event in unknown CQ\n");
	}
	
    ibv_ops.ack_cq_events(mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_num], 1);
	
    return 0;
}

static inline int perform_blocking_progress_for_iwarp(int hca_num, int num_cqs)
{
    int ret = 0, err = 0;
    void *ev_ctx = NULL;
    struct ibv_cq *ev_cq = NULL;

    /* Okay ... spun long enough, now time to go to sleep! */

#if (MPICH_THREAD_LEVEL == MPI_THREAD_MULTIPLE)
    MPIU_THREAD_CHECK_BEGIN
    MPID_Thread_mutex_unlock(&MPIR_ThreadInfo.global_mutex, &err);
    MPIU_THREAD_CHECK_END
#endif
    do {    
        if (num_cqs == 1) {
    	    if (ibv_req_notify_cq(
                        mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_num], 0)) {
    	        ibv_error_abort(IBV_RETURN_ERR,
    	                "Couldn't request for CQ notification\n");
    	    }
        } else {
    	    if (ibv_req_notify_cq(
                  mv2_MPIDI_CH3I_RDMA_Process.send_cq_hndl[hca_num], 0)) {
    	        ibv_error_abort(IBV_RETURN_ERR,
    	           "Couldn't request for CQ notification\n");
    	    }
    	    if (ibv_req_notify_cq(
                  mv2_MPIDI_CH3I_RDMA_Process.recv_cq_hndl[hca_num], 0)) {
    	        ibv_error_abort(IBV_RETURN_ERR,
    	           "Couldn't request for CQ notification\n");
    	    }
        }
        ret = ibv_ops.get_cq_event(
                mv2_MPIDI_CH3I_RDMA_Process.comp_channel[hca_num], 
                &ev_cq, &ev_ctx);
        if (ret && errno != EINTR) {
            ibv_va_error_abort(IBV_RETURN_ERR,
                    "Failed to get cq event: %d\n", ret);
        }       
    } while (ret && errno == EINTR); 
#if (MPICH_THREAD_LEVEL == MPI_THREAD_MULTIPLE)
    MPIU_THREAD_CHECK_BEGIN
    MPID_Thread_mutex_lock(&MPIR_ThreadInfo.global_mutex, &err);
    MPIU_THREAD_CHECK_END
#endif

    if (num_cqs == 1) {
	    if (ev_cq != mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_num]) {
	        ibv_error_abort(IBV_STATUS_ERR,
                             "Event in unknown CQ\n");
	    }
       ibv_ops.ack_cq_events(mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_num], 1);
    } else {
	    if (ev_cq == mv2_MPIDI_CH3I_RDMA_Process.send_cq_hndl[hca_num]) {
            ibv_ops.ack_cq_events(
                    mv2_MPIDI_CH3I_RDMA_Process.send_cq_hndl[hca_num], 1);
        } else if (ev_cq == 
                    mv2_MPIDI_CH3I_RDMA_Process.recv_cq_hndl[hca_num]) {
            ibv_ops.ack_cq_events(
                    mv2_MPIDI_CH3I_RDMA_Process.recv_cq_hndl[hca_num], 1);
	    } else {
	       ibv_error_abort(IBV_STATUS_ERR, "Event in unknown CQ\n");
        }
    }

    return 0;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_RDMA_cq_poll_ib
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3I_MRAILI_Cq_poll_ib(vbuf **vbuf_handle, 
        MPIDI_VC_t * vc_req, int receiving, int is_blocking)
{
    int ne = 0;
    int i = 0;
    int cq_choice = 0;
    int num_cqs = 1;
    int type = T_CHANNEL_NO_ARRIVE;
    struct ibv_cq *chosen_cq = NULL;

    MPIDI_STATE_DECL(MPID_GEN2_MRAILI_CQ_POLL_IB);
    MPIDI_FUNC_ENTER(MPID_GEN2_MRAILI_CQ_POLL_IB);

    *vbuf_handle = NULL;
    MRAILI_Hollow_retry_blocked();

    for (i = 0; i < rdma_num_hcas; ++i) {
        chosen_cq = mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[i];

        if (curr_cqe[i] < num_cqes[i]) {
            /* We already have CQEs polled out from last call to
             * ibv_poll_cq. Drain them before polling again */
            do {
                type = handle_cqe(vbuf_handle, vc_req, receiving,
                                  wc[i][curr_cqe[i]], num_cqs, cq_choice, i);
                curr_cqe[i]++;
                /* Drain till we get in-order recv or run out of polled CQEs */
            } while ((*vbuf_handle == NULL) && (curr_cqe[i] < num_cqes[i]));

            if (*vbuf_handle != NULL) {
                /* We got in-order data, deliver it to higher level */
                goto fn_exit;
            }
        } else {
get_blocking_message:
            memset(wc[i], 0, sizeof(struct ibv_wc) * num_cqes[i]);
	        ne = ibv_poll_cq(chosen_cq, rdma_num_cqes_per_poll, wc[i]);

    	    if (unlikely(ne < 0)) {
    	        ibv_error_abort(IBV_RETURN_ERR, "Fail to poll cq\n");
    	    } else if (ne) {         
                curr_cqe[i] = 0;
                num_cqes[i] = ne;

                do {
                    type = handle_cqe(vbuf_handle, vc_req, receiving, wc[i][curr_cqe[i]],
                                        num_cqs, cq_choice, i);
                    curr_cqe[i]++;
                    /* Drain till we get in-order recv or run out of polled CQEs */
                } while ((*vbuf_handle == NULL) && (curr_cqe[i] < num_cqes[i]));

                if (*vbuf_handle != NULL) {
                    /* We got in-order data, deliver it to higher level */
                    goto fn_exit;
                }
    	    } else {
    	        *vbuf_handle = NULL;
    	        type = T_CHANNEL_NO_ARRIVE;
    	        ++nspin;

    	        /* Blocking mode progress */
    	        if (unlikely(rdma_use_blocking && is_blocking &&
                            nspin >= rdma_blocking_spin_count_threshold)) {
                    mv2_in_blocking_progress = 1;
                    PERFORM_BLOCKING_PROGRESS(i, num_cqs);
                    mv2_in_blocking_progress = 0;
    	            nspin = 0;
                    goto get_blocking_message;
    	        }

                if (unlikely(mv2_srq_repost_pool.num_free &&
                             mv2_srq_repost_pool.num_free >= rdma_credit_preserve &&
                                 nspin >= rdma_blocking_spin_count_threshold)) {
                    MV2_REPOST_VBUF_FROM_POOL_TO_SRQ(&mv2_srq_repost_pool);
                    nspin = 0;
                }
    	    }
        }
    }
    if (!vc_req) {
        type = MPIDI_CH3I_MRAILI_Test_pkt(vbuf_handle);
        if (type == T_CHANNEL_EXACT_ARRIVE ||
            type == T_CHANNEL_CONTROL_MSG_ARRIVE) {
            goto fn_exit;
        }
    }
fn_exit:

    MPIDI_FUNC_EXIT(MPID_GEN2_MRAILI_CQ_POLL_IB);
    return type;
}

void async_thread(void *context)
{
    struct ibv_srq *srq_hndl = NULL;
    struct ibv_async_event event;
    struct ibv_srq_attr srq_attr;
    int post_new = 0, i = 0, hca_num = -1;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while (1) {
        if (ibv_ops.get_async_event((struct ibv_context *) context, &event)) {
            fprintf(stderr, "Error getting event!\n"); 
        }

        for(i = 0; i < rdma_num_hcas; i++) {
            if(mv2_MPIDI_CH3I_RDMA_Process.nic_context[i] == context) {
                hca_num = i;
            }
        }

        pthread_mutex_lock(&mv2_MPIDI_CH3I_RDMA_Process.async_mutex_lock[hca_num]);
        switch (event.event_type) {
            /* Fatal */
            case IBV_EVENT_CQ_ERR:
                ibv_va_error_abort(GEN_EXIT_ERR, "Got FATAL event %s on CQ %p\n",
                                    ibv_ops.event_type_str(event.event_type),
                                    event.element.cq);
                break;
            case IBV_EVENT_COMM_EST:
            case IBV_EVENT_SQ_DRAINED:
                PRINT_DEBUG(DEBUG_CHM_verbose, "Async event %s on QP 0x%x\n",
                            ibv_ops.event_type_str(event.event_type),
                            event.element.qp->qp_num);
                break;
            case IBV_EVENT_QP_FATAL:
            case IBV_EVENT_QP_REQ_ERR:
            case IBV_EVENT_QP_ACCESS_ERR:
                ibv_va_error_abort(GEN_EXIT_ERR, "Got FATAL event %s on QP 0x%x\n",
                                    ibv_ops.event_type_str(event.event_type),
                                    event.element.qp->qp_num);
                break;
            case IBV_EVENT_PATH_MIG_ERR:
#ifdef DEBUG
                if(mv2_MPIDI_CH3I_RDMA_Process.has_apm) {
                    PRINT_DEBUG(DEBUG_CHM_verbose, "Path Migration Failed\n");
                }
#endif /* ifdef DEBUG */
                ibv_va_error_abort(GEN_EXIT_ERR, "Got FATAL event %s on QP 0x%x\n",
                                    ibv_ops.event_type_str(event.event_type),
                                    event.element.qp->qp_num);
                break;
            case IBV_EVENT_PATH_MIG:
                if(mv2_MPIDI_CH3I_RDMA_Process.has_apm && !apm_tester){
                    PRINT_DEBUG(DEBUG_CHM_verbose, "Path Migration Successful\n");
                    reload_alternate_path((&event)->element.qp);
                }

                if(!mv2_MPIDI_CH3I_RDMA_Process.has_apm) {
                    ibv_va_error_abort(GEN_EXIT_ERR, "Got FATAL event %s on QP 0x%x\n",
                                        ibv_ops.event_type_str(event.event_type),
                                        event.element.qp->qp_num);
                }
                
                break;
            case IBV_EVENT_DEVICE_FATAL:
            case IBV_EVENT_PORT_ERR:
                ibv_va_error_abort(GEN_EXIT_ERR, "Got FATAL event %s on port %d\n",
                                    ibv_ops.event_type_str(event.event_type),
                                    event.element.port_num);
                break;
            case IBV_EVENT_SRQ_ERR:
                ibv_va_error_abort(GEN_EXIT_ERR, "Got FATAL event %s on SRQ %p\n",
                                    ibv_ops.event_type_str(event.event_type),
                                    event.element.srq);
                break;
            case IBV_EVENT_QP_LAST_WQE_REACHED:
                PRINT_DEBUG(DEBUG_CHM_verbose, "Async event %s on SRQ %p\n",
                            ibv_ops.event_type_str(event.event_type),
                            event.element.srq);
                break;
            case IBV_EVENT_GID_CHANGE:
            case IBV_EVENT_CLIENT_REREGISTER:
                PRINT_DEBUG(DEBUG_CHM_verbose, "Async event %s\n",
                            ibv_ops.event_type_str(event.event_type));
                break;
            case IBV_EVENT_PORT_ACTIVE:
            case IBV_EVENT_LID_CHANGE:
            case IBV_EVENT_PKEY_CHANGE:
            case IBV_EVENT_SM_CHANGE:
                PRINT_DEBUG(DEBUG_CHM_verbose, "Async event %s on port %d",
                            ibv_ops.event_type_str(event.event_type),
                            event.element.port_num);
                break;

            case IBV_EVENT_SRQ_LIMIT_REACHED:

                pthread_spin_lock(&mv2_MPIDI_CH3I_RDMA_Process.srq_post_spin_lock);

                if(-1 == hca_num) {
                    /* Was not able to find the context,
                     * error condition */
                    ibv_error_abort(GEN_EXIT_ERR,
                            "Couldn't find out SRQ context\n");
                }

#ifdef _ENABLE_UD_
                PRINT_DEBUG(DEBUG_CHM_verbose, "Async event %s on %s SRQ\n",
                            ibv_ops.event_type_str(event.event_type),
                            (event.element.srq == mv2_MPIDI_CH3I_RDMA_Process.ud_srq_hndl[hca_num])?"UD":"RC");

                if (event.element.srq == mv2_MPIDI_CH3I_RDMA_Process.ud_srq_hndl[hca_num]) {
                    /* Dynamically re-size the SRQ to be larger */
                    mv2_ud_srq_fill_size *= 2;
                    if (mv2_ud_srq_fill_size > mv2_ud_srq_alloc_size) {
                        mv2_ud_srq_fill_size = mv2_ud_srq_alloc_size;
                    }

                    /* Need to post more to the SRQ */
                    srq_hndl = mv2_MPIDI_CH3I_RDMA_Process.ud_srq_hndl[hca_num];

                    post_new = mv2_post_ud_recv_buffers(
                                            mv2_ud_srq_fill_size - mv2_ud_srq_limit,
                                            mv2_MPIDI_CH3I_RDMA_Process.ud_rails[hca_num]);
                    mv2_MPIDI_CH3I_RDMA_Process.ud_rails[hca_num]->num_recvs_posted += post_new;

                    /* Set new UD SRQ parameters */
                    srq_attr.srq_limit = mv2_ud_srq_limit;
                } else
#endif /*_ENABLE_UD_*/
                {
                    /* Dynamically re-size the SRQ to be larger */
                    mv2_srq_fill_size *= 2;
                    if (mv2_srq_fill_size > mv2_srq_alloc_size) {
                        mv2_srq_fill_size = mv2_srq_alloc_size;
                    }

                    /* Need to post more to the SRQ */
                    post_new = mv2_srq_limit;

                    srq_hndl = mv2_MPIDI_CH3I_RDMA_Process.srq_hndl[hca_num];
                    post_new += mv2_post_srq_buffers(mv2_srq_fill_size -
                                                    mv2_srq_limit, hca_num);
                    post_new = mv2_srq_limit - post_new;

                    /* Set new SRQ parameters */
                    srq_attr.srq_limit = mv2_srq_limit;
                }

                pthread_spin_unlock(&mv2_MPIDI_CH3I_RDMA_Process.
                        srq_post_spin_lock);

                if(!post_new) {
                    pthread_mutex_lock(
                            &mv2_MPIDI_CH3I_RDMA_Process.
                            srq_post_mutex_lock[hca_num]);

                    ++mv2_MPIDI_CH3I_RDMA_Process.srq_zero_post_counter[hca_num];

                    while(mv2_MPIDI_CH3I_RDMA_Process.srq_zero_post_counter[hca_num] >= 1
                            && !mv2_MPIDI_CH3I_RDMA_Process.is_finalizing) {
                        /* Cannot post to SRQ, since all WQEs
                         * might be waiting in CQ to be pulled out */
                        pthread_cond_wait(
                                &mv2_MPIDI_CH3I_RDMA_Process.
                                srq_post_cond[hca_num],
                                &mv2_MPIDI_CH3I_RDMA_Process.
                                srq_post_mutex_lock[hca_num]);
                    }
                    pthread_mutex_unlock(&mv2_MPIDI_CH3I_RDMA_Process.
                            srq_post_mutex_lock[hca_num]);
                } else {
                    /* Was able to post some, so erase old counter */
                    if(mv2_MPIDI_CH3I_RDMA_Process.
                            srq_zero_post_counter[hca_num]) {
                        mv2_MPIDI_CH3I_RDMA_Process.
                            srq_zero_post_counter[hca_num] = 0;
                    }
                }

                pthread_spin_lock(&mv2_MPIDI_CH3I_RDMA_Process.srq_post_spin_lock);

                srq_attr.max_sge = 1;

                if (ibv_ops.modify_srq(srq_hndl, &srq_attr, IBV_SRQ_LIMIT)) {
                    ibv_va_error_abort(GEN_EXIT_ERR,
                            "Couldn't modify SRQ limit (%u) after posting %d\n",
                            srq_attr.srq_limit, post_new);
                }

                pthread_spin_unlock(&mv2_MPIDI_CH3I_RDMA_Process.srq_post_spin_lock);

                break;
            default:
                PRINT_ERROR("Got unknown event %d ... continuing ...\n", event.event_type);
        }
        ibv_ops.ack_async_event(&event);
        pthread_mutex_unlock(&mv2_MPIDI_CH3I_RDMA_Process.async_mutex_lock[hca_num]);
    }
}

/* This function is used for implementing  "Alternate Path Specification"
 * and "Path Loading Request Module", (SMTPS 2007 Paper) */

/* Description:
 * sl: service level, which can be changed once the QoS is enabled 
 * 
 * alt_timeout: alternate timeout, which can be increased to maximum, which
 * may make the failover little slow
 *
 * path_mig_state: Path migration state, since we are combining the modules
 * this should be the path migration state of the QP
 *
 * pkey_index: Index of the partition
 *
 * static_rate: typically "0" is the safest, which corresponds to the
 * maximum value of the static rate
 *
 * Alternate Path can be specified in multiple ways:
 * Using Same Port, but different LMC (common case)
 * rdma_num_qp_per_port refers to the number of QPs per port. The QPs
 * will use the first "rdma_num_qp_per_port" set of paths, and their
 * alternate paths are specified by a displacement of rdma_num_qp_per_port.
 * As an example, with rdma_num_qp_per_port = 4, first QP will use path0 and
 * path4, second QP will use path1 and path5 and so on, as the primary and
 * alternate path respectively.
 *
 * Finally, Since this function also implements Path Loading Request Module,
 * it should modify the QP with the specification of the alternate path */

int reload_alternate_path(struct ibv_qp *qp)
{

    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    enum ibv_qp_attr_mask attr_mask;

    lock_apm();

    /* For Sanity */
    MPIU_Memset(&attr, 0, sizeof attr);
    MPIU_Memset(&init_attr, 0, sizeof init_attr);

    attr_mask = 0;

    if (ibv_ops.query_qp(qp, &attr,
                attr_mask, &init_attr)) {
        ibv_error_abort(GEN_EXIT_ERR, "Failed to query QP\n");
    }

    /* This value should change with enabling of QoS */
    attr.alt_ah_attr.sl =  attr.ah_attr.sl;
    attr.alt_ah_attr.static_rate = attr.ah_attr.static_rate;
    attr.alt_ah_attr.port_num =  attr.ah_attr.port_num;
    attr.alt_ah_attr.is_global =  attr.ah_attr.is_global;
    attr.alt_timeout = attr.timeout;
    attr.alt_port_num = attr.port_num;
    attr.alt_ah_attr.src_path_bits =
        (attr.ah_attr.src_path_bits + rdma_num_qp_per_port) %
        power_two(mv2_MPIDI_CH3I_RDMA_Process.lmc);
    attr.alt_ah_attr.dlid =
        attr.ah_attr.dlid - attr.ah_attr.src_path_bits
        + attr.alt_ah_attr.src_path_bits;
    attr.path_mig_state = IBV_MIG_REARM;
    attr_mask = 0;
    attr_mask |= IBV_QP_ALT_PATH;
    attr_mask |= IBV_QP_PATH_MIG_STATE;

    if (ibv_ops.modify_qp(qp, &attr, attr_mask))
    {
        ibv_error_abort(GEN_EXIT_ERR, "Failed to modify QP\n");
    }

    unlock_apm();
    
    return 0;
}

/* This function is used for implementing "Path Migration Module"
 * (SMTPS 2007) */

/* Description:
 * A lock is used to make sure that either the main thread or the
 * asynchronous thread is able to query the QP and set the parameters
 *
 * APM is not performed on every iteration, it is extremely slow, APM_COUNT
 * is used to make sure that it occurs only once during APM_COUNT
 */

int perform_manual_apm(struct ibv_qp* qp)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    enum ibv_qp_attr_mask attr_mask;
    static int count_to_apm = -1;

    ++count_to_apm;

    if (count_to_apm)
    {
        return 0;
    }
    
    /* For Sanity */    
    MPIU_Memset(&attr, 0, sizeof attr);
    MPIU_Memset(&init_attr, 0, sizeof init_attr);
    attr_mask = 0;
    lock_apm();
    ibv_ops.query_qp(qp, &attr,
                attr_mask, &init_attr);

    if (IBV_MIG_ARMED == attr.path_mig_state)
    {
        attr.path_mig_state = IBV_MIG_MIGRATED;
        MPIU_Assert(attr.qp_state == IBV_QPS_RTS);
        ibv_ops.modify_qp(qp, &attr,
                    IBV_QP_PATH_MIG_STATE);
    }
    
    unlock_apm(); 
    return 0;
}

void MPIDI_CH3I_Cleanup_cqes(void)
{
    int i;

    nspin = 0;
    arriving_head = arriving_tail = NULL;
    for (i=0; i< MAX_NUM_HCAS; i++) {
        num_cqes[i] = 0;
        curr_cqe[i] = 0;
    }
}

void
MPIDI_CH3I_Cleanup_after_connection(MPIDI_VC_t *vc)
{
    int i;
#define pset        mv2_MPIDI_CH3I_RDMA_Process.polling_set
#define pgrsz       mv2_MPIDI_CH3I_RDMA_Process.polling_group_size

    if(0 == pgrsz)
        return;

    for(i = 0; i < pgrsz; i++) {
        if(vc == pset[i]) {
            pset[i] = pset[pgrsz - 1];
            pgrsz -= 1;
            PRINT_DEBUG(DEBUG_CHM_verbose, "VC removed from polling set\n");
            return;
        }
    }
#undef pgrsz
#undef pset
}

int
MPIDI_CH3I_Check_pending_send(MPIDI_VC_t *vc)
{
    int i;
    for(i = 0; i < rdma_num_rails; i++) {
        if(vc->mrail.rails[i].send_wqes_avail != rdma_default_max_send_wqe) {
            vc->free_vc = 1;
            return 1;
        }
    }

    return 0;
}
