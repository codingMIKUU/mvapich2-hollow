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

#include "rdma_impl.h"
#include "upmi.h"
#include "mpiutil.h"
#include "cm.h"
#ifdef _ENABLE_UD_
#include "mv2_ud.h"
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

#undef DEBUG_PRINT
#ifdef DEBUG
#define DEBUG_PRINT(args...) \
do {                                                          \
    int rank;                                                 \
    UPMI_GET_RANK(&rank);                                      \
    fprintf(stderr, "[%d][%s:%d] ", rank, __FILE__, __LINE__);\
    fprintf(stderr, args);  fflush(stderr);                   \
} while (0)
#else
#define DEBUG_PRINT(args...)
#endif

int MPIDI_CH3I_MRAILI_Recv_addr(MPIDI_VC_t * vc, void *vstart)
{
    MPIDI_CH3_Pkt_address_t *pkt = vstart;
    int i;
    int ret;
#ifdef _ENABLE_XRC_
    if (USE_XRC && (0 == xrc_rdmafp_init || 
            VC_XST_ISSET (vc, XF_CONN_CLOSING)))
        return MPI_ERR_INTERN;
#endif

    DEBUG_PRINT("set rdma address, dma address %p\n",
            (void *)pkt->rdma_address);

    /* check if it has accepted max allowing connections */
    if (rdma_fp_sendconn_accepted >= rdma_polling_set_limit)
    {
        vbuf_address_reply_send(vc, RDMA_FP_MAX_SEND_CONN_REACHED);
        goto fn_exit;
    }

    if (pkt->rdma_address != 0) {
	    /* Allocating the send vbufs for the eager RDMA flow */
        ret = vbuf_fast_rdma_alloc(vc, 0);
        if (ret == MPI_SUCCESS) {
	        for (i = 0; i < rdma_num_hcas; i ++) {
	            vc->mrail.rfp.RDMA_remote_buf_rkey[i] = pkt->rdma_hndl[i];
	        }
	        vc->mrail.rfp.remote_RDMA_buf = (void *)pkt->rdma_address;
            vbuf_address_reply_send(vc, RDMA_FP_SUCCESS);
            rdma_fp_sendconn_accepted++;
        } else {
            vbuf_address_reply_send(vc, RDMA_FP_SENDBUFF_ALLOC_FAILED);
            return -1;
        } 
    }
fn_exit:
    return MPI_SUCCESS;
}

int MPIDI_CH3I_MRAILI_Recv_addr_reply(MPIDI_VC_t * vc, void *vstart)
{
    int hca_index;
    int ret;
    MPIDI_CH3_Pkt_address_reply_t *pkt = vstart;
    DEBUG_PRINT("Received addr reply packet. reply data :%d\n", pkt->reply_data);
    
    if (pkt->reply_data == RDMA_FP_SENDBUFF_ALLOC_FAILED 
        || pkt->reply_data == RDMA_FP_MAX_SEND_CONN_REACHED) {

        DEBUG_PRINT("RDMA FP setup failed. clean up recv buffers\n ");
    
        if (!mv2_rdma_fast_path_preallocate_buffers) {
            /* de-regster the recv buffers */
            for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
                if (vc->mrail.rfp.RDMA_recv_buf_mr[hca_index]) {
                    ret = deregister_memory(vc->mrail.rfp.RDMA_recv_buf_mr[hca_index]);
                    if (ret) {
                        MPL_error_printf("Failed to deregister mr (%d)\n", ret);
                    } else {
                        vc->mrail.rfp.RDMA_recv_buf_mr[hca_index] = NULL;
                    }
                }
            }
            /* deallocate recv RDMA buffers */
            if (vc->mrail.rfp.RDMA_recv_buf_DMA) {
                MPIU_Memalign_Free(vc->mrail.rfp.RDMA_recv_buf_DMA);
                vc->mrail.rfp.RDMA_recv_buf_DMA = NULL;
            }

            /* deallocate vbuf struct buffers */
            if (vc->mrail.rfp.RDMA_recv_buf) {
                MPIU_Memalign_Free(vc->mrail.rfp.RDMA_recv_buf);
                vc->mrail.rfp.RDMA_recv_buf = NULL;
            }
        } else {
            for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
                vc->mrail.rfp.RDMA_recv_buf_mr[hca_index] = NULL;
            }
            vc->mrail.rfp.RDMA_recv_buf_DMA = NULL;
            vc->mrail.rfp.RDMA_recv_buf = NULL;
        }
        
        /* set flag to mark that FP setup is failed/rejected. 
        we shouldn't try further on this vc */
        vc->mrail.rfp.rdma_failed = 1;

    } else if (pkt->reply_data == RDMA_FP_SUCCESS) {
            
        /* set pointers */
        vc->mrail.rfp.p_RDMA_recv = 0;
        vc->mrail.rfp.p_RDMA_recv_tail = num_rdma_buffer - 1;

        /* Add the connection to the RDMA polling list */
        MPIU_Assert(mv2_MPIDI_CH3I_RDMA_Process.polling_group_size < rdma_polling_set_limit);

        mv2_MPIDI_CH3I_RDMA_Process.polling_set
            [mv2_MPIDI_CH3I_RDMA_Process.polling_group_size] = vc;
        mv2_MPIDI_CH3I_RDMA_Process.polling_group_size++;

        vc->mrail.cmanager.num_channels      += 1;
        vc->mrail.cmanager.num_local_pollings = 1;
        vc->mrail.rfp.in_polling_set          = 1;
        vc->mrail.state &= ~(MRAILI_RFP_CONNECTING);
        vc->mrail.state |= MRAILI_RFP_CONNECTED;
        if (mv2_use_eager_fast_send &&
            !(SMP_INIT && (vc->smp.local_nodes >= 0))) {
		vc->use_eager_fast_rfp_fn = 1;
        }
    } else {
        ibv_va_error_abort(GEN_EXIT_ERR,
                "Invalid reply data received. reply_data: pkt->reply_data%d\n",
                                                              pkt->reply_data);
    }
    
    rdma_pending_conn_request--;
    
    return MPI_SUCCESS;
}

