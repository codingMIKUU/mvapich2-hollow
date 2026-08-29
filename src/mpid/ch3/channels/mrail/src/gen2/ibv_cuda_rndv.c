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

#include "mpichconf.h"
#include <mpimem.h>
#include "rdma_impl.h"
#include "ibv_impl.h"
#include "vbuf.h"
#include "dreg.h"
#include "ibv_send_inline.h"

#ifdef _ENABLE_CUDA_
#include "ibv_cuda_util.h"

MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_vbuf_allocated);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_vbuf_freed);
MPIR_T_PVAR_ULONG_LEVEL_DECL_EXTERN(MV2, mv2_vbuf_available);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ud_vbuf_allocated);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ud_vbuf_freed);
MPIR_T_PVAR_ULONG_LEVEL_DECL_EXTERN(MV2, mv2_ud_vbuf_available);

int MPIDI_CH3I_MRAIL_Prepare_rndv_device(MPIDI_VC_t * vc,
            MPID_Request * sreq, MPID_Request * rreq)
{
    MPID_Request *req = NULL;
    int i = 0;

    if (NULL == sreq) {
        req = rreq;
    } else {
        req = sreq;
    }

    DEBUG_PRINT
        ("[prepare cts] rput protocol, recv size %d, segsize %d, io count %d\n",
         req->dev.recv_data_sz, req->dev.segment_size, req->dev.iov_count);

    if (MV2_RNDV_PROTOCOL_RPUT == rdma_rndv_protocol) {
        req->mrail.protocol = MV2_RNDV_PROTOCOL_RPUT;
    } else {
        PRINT_ERROR("RGET and R3 are not supported for GPU to GPU transfer \n");
        exit(EXIT_FAILURE);
    }

    /* Step 1: ready for user space (user buffer or pack) */
    if (1 == req->dev.iov_count && (req->dev.OnDataAvail == NULL 
                    || req->dev.OnDataAvail == req->dev.OnFinal
                    || req->dev.OnDataAvail == MPIDI_CH3_ReqHandler_UnpackSRBufComplete
                    || req->dev.OnDataAvail == MPIDI_CH3_ReqHandler_unpack_device))
    {

        req->mrail.rndv_buf = req->dev.iov[0].MPL_IOV_BUF;
        req->mrail.rndv_buf_sz = req->dev.iov[0].MPL_IOV_LEN;
        req->mrail.rndv_buf_alloc = 0;
    } else {

        req->mrail.rndv_buf_sz = req->dev.segment_size;
        req->mrail.rndv_buf = MPIU_Malloc(req->mrail.rndv_buf_sz);

        if (req->mrail.rndv_buf == NULL) {
            PRINT_ERROR("RGET and R3 are not supported for GPU to GPU transfer \n");
            exit(EXIT_FAILURE);
        } else {
            req->mrail.rndv_buf_alloc = 1;
        }

    }
    req->mrail.rndv_buf_off = 0;

    req->mrail.num_device_blocks =
            ROUNDUP(req->mrail.rndv_buf_sz, mv2_device_stage_block_size);
    for (i = 0; i < MIN(req->mrail.num_device_blocks, mv2_device_num_rndv_blocks);
         i++) {
        GET_VBUF_BY_OFFSET_WITHOUT_LOCK(req->mrail.device_vbuf[i], MV2_CUDA_VBUF_POOL_OFFSET);
    }
    req->mrail.device_block_offset = 0;
    req->mrail.num_remote_device_pending =
        MIN(req->mrail.num_device_blocks, mv2_device_num_rndv_blocks);
    req->mrail.num_remote_device_done = 0;
    req->mrail.num_remote_device_inflight = 0;
    req->mrail.local_complete = 0;

    return 1;
}

#if defined(HAVE_CUDA_IPC)
void MPIDI_CH3_DEVICE_IPC_Rendezvous_push(MPIDI_VC_t * vc, MPID_Request * sreq)
{
    int local_index, shared_index;
    int i, nbytes, cudaipc_memcpy_sync = 0;
    char *src;
    cudaStream_t strm = 0;

    if (sreq->mrail.rndv_buf_sz < cudaipc_sync_limit) {
        cudaipc_memcpy_sync = 1;
    }
    if (cudaipc_memcpy_sync == 0) {
        strm = stream_d2h;
    }
        
    local_index = CUDAIPC_BUF_LOCAL_IDX(vc->smp.local_rank);
    shared_index = CUDAIPC_BUF_SHARED_IDX(deviceipc_my_local_id, vc->smp.local_rank);
    i = sreq->mrail.device_ipc_stage_index;

    while(1) {
        if (cudaipc_shared_data[shared_index + i].sync_flag != CUDAIPC_BUF_EMPTY) {
            break;
        }
        src = sreq->mrail.rndv_buf + sreq->mrail.rndv_buf_off;
        nbytes = MIN(cudaipc_stage_buffer_size, 
                    sreq->mrail.rndv_buf_sz - sreq->mrail.rndv_buf_off);
        CUDA_CHECK(cudaStreamWaitEvent(strm, 
                    cudaipc_local_data[local_index + i].ipcEvent, 0));
        if (cudaipc_memcpy_sync) {
            MPIU_Memcpy_Device(cudaipc_local_data[local_index + i].buffer,
                        src, nbytes, cudaMemcpyDefault);
        } else {
            MPIU_Memcpy_Device_Async(cudaipc_local_data[local_index + i].buffer,
                        src, nbytes, cudaMemcpyDefault, strm);
        }
        CUDA_CHECK(cudaEventRecord(
                    cudaipc_local_data[local_index + i].ipcEvent, strm));

        PRINT_DEBUG(CUDAIPC_DEBUG, "cudaipc data-in: src:%p bytes:%d," 
                    "stage index:%d\n", src, nbytes, shared_index + i);

        /* signal the data in */
        cudaipc_shared_data[shared_index + i].sync_flag = CUDAIPC_BUF_FULL;

        sreq->mrail.rndv_buf_off += nbytes;
        if (sreq->mrail.rndv_buf_off == sreq->mrail.rndv_buf_sz) {
            if (cudaipc_memcpy_sync) {
                int complete;
                if (sreq->mrail.rndv_buf_alloc == 1 && 
                        sreq->mrail.rndv_buf != NULL) { 
                    /* a temporary host rndv buffer would have been allocated only when the 
                       sender buffers is noncontiguous and is in the host memory */
                    MPIU_Assert(sreq->mrail.device_transfer_mode == HOST_TO_DEVICE);
                    MPIU_Free_Device_Pinned_Host(sreq->mrail.rndv_buf);
                    sreq->mrail.rndv_buf_alloc = 0;
                    sreq->mrail.rndv_buf = NULL;
                }
                MPIDI_CH3U_Handle_send_req(vc, sreq, &complete);
                MPIU_Assert(complete == TRUE);
            } else {
                sreq->mrail.device_event = get_device_event();
                if (sreq->mrail.device_event == NULL) {
                    allocate_cuda_event(&sreq->mrail.device_event);
                    sreq->mrail.device_event->is_query_done = 0;
                }
                sreq->mrail.device_event->op_type = CUDAIPC_SEND;
                sreq->mrail.device_event->vc = vc;
                sreq->mrail.device_event->req = sreq;
                CUDA_CHECK(cudaEventRecord(sreq->mrail.device_event->event, strm));
                if (sreq->mrail.device_event->flags == CUDA_EVENT_DEDICATED) {
                    /* add to the busy list */
                    CUDA_LIST_ADD(sreq->mrail.device_event,
                            busy_cuda_event_list_head, busy_cuda_event_list_tail);          
                }
            }
            sreq->mrail.nearly_complete = 1;
            break;
        }
        i++;
        if (i == cudaipc_num_stage_buffers) {
            i = 0;
        }
    }
    sreq->mrail.device_ipc_stage_index = i;
}

void MPIDI_CH3_DEVICE_IPC_Rendezvous_recv(MPIDI_VC_t * vc, MPID_Request * rreq)
{
    int local_index, shared_index;
    int i, nbytes, cudaipc_memcpy_sync = 0;
    char *dst;
    cudaStream_t strm = 0;

    if (rreq->mrail.rndv_buf_sz < cudaipc_sync_limit) {
        cudaipc_memcpy_sync = 1;
    }
    if (cudaipc_memcpy_sync == 0) {
        strm = stream_h2d;
    }

    local_index = CUDAIPC_BUF_LOCAL_IDX(vc->smp.local_rank);
    shared_index = CUDAIPC_BUF_SHARED_IDX(vc->smp.local_rank, deviceipc_my_local_id);
    i = rreq->mrail.device_ipc_stage_index;

    while(1) {
        if (cudaipc_shared_data[shared_index + i].sync_flag != CUDAIPC_BUF_FULL) {
            break;
        }
        dst = rreq->mrail.rndv_buf + rreq->mrail.rndv_buf_off;
        nbytes = MIN(cudaipc_stage_buffer_size, 
                        rreq->mrail.rndv_buf_sz - rreq->mrail.rndv_buf_off);

        CUDA_CHECK(cudaStreamWaitEvent(strm, 
                    cudaipc_remote_data[local_index + i].ipcEvent, 0));
        if (cudaipc_memcpy_sync) {
            MPIU_Memcpy_Device(dst, cudaipc_remote_data[local_index + i].buffer,
                        nbytes, cudaMemcpyDefault);
        } else {
            MPIU_Memcpy_Device_Async(dst, cudaipc_remote_data[local_index + i].buffer,
                        nbytes, cudaMemcpyDefault, strm);
        }
        CUDA_CHECK(cudaEventRecord(cudaipc_remote_data[local_index + i].ipcEvent, strm));

        PRINT_DEBUG(CUDAIPC_DEBUG, "cudaipc data-out: dst:%p bytes:%d, stage index:%d\n", 
                            dst, nbytes, shared_index + i);
        /* signal data-out done */
        cudaipc_shared_data[shared_index + i].sync_flag = CUDAIPC_BUF_EMPTY;
        rreq->mrail.rndv_buf_off += nbytes;
        if (rreq->mrail.rndv_buf_off == rreq->mrail.rndv_buf_sz) {
            if (cudaipc_memcpy_sync) {
                int complete;
                int mpi_errno = MPI_SUCCESS;
                mpi_errno = MPIDI_CH3U_Handle_recv_req(vc, rreq, &complete);
                if (mpi_errno != MPI_SUCCESS) {
                    ibv_error_abort(IBV_RETURN_ERR,
                            "MPIDI_CH3U_Handle_recv_req returned error");
                }
                MPIU_Assert(complete == TRUE);

            } else {
                rreq->mrail.device_event = get_device_event();
                if (rreq->mrail.device_event == NULL) {
                    allocate_cuda_event(&rreq->mrail.device_event);
                    rreq->mrail.device_event->is_query_done = 0;
                }
                rreq->mrail.device_event->op_type = CUDAIPC_RECV;
                rreq->mrail.device_event->vc = vc;
                rreq->mrail.device_event->req = rreq;
                CUDA_CHECK(cudaEventRecord(rreq->mrail.device_event->event, strm));
                if (rreq->mrail.device_event->flags == CUDA_EVENT_DEDICATED) {
                    /* add to the busy list */
                    CUDA_LIST_ADD(rreq->mrail.device_event,
                            busy_cuda_event_list_head, busy_cuda_event_list_tail);          
                }
            }
            vc->ch.recv_active = NULL;
            rreq->mrail.nearly_complete = 1;
            break;
        }
        i++;
        if (i == cudaipc_num_stage_buffers) {
            i = 0;
        }
    }
    rreq->mrail.device_ipc_stage_index = i;
}

int MPIDI_CH3I_MRAIL_Prepare_rndv_device_ipc_buffered (MPIDI_VC_t * vc,
                                            MPID_Request * sreq) 
{
    if (vc->smp.local_rank == -1
        || vc->smp.can_access_peer != MV2_DEVICE_IPC_ENABLED
        || sreq->mrail.device_transfer_mode == NONE
        || sreq->dev.iov_count > 1 
        || sreq->dev.iov[0].MPL_IOV_LEN < mv2_device_ipc_threshold) {
        return 0;
    }

    sreq->mrail.protocol = MV2_RNDV_PROTOCOL_CUDAIPC;
    sreq->mrail.d_entry = NULL;
    sreq->mrail.device_ipc_stage_index = 0;

    sreq->mrail.rndv_buf = (void *) sreq->dev.iov[0].MPL_IOV_BUF;
    sreq->mrail.rndv_buf_sz = sreq->dev.iov[0].MPL_IOV_LEN;

    return 1;
}

int MPIDI_CH3I_MRAIL_Revert_rndv_device_ipc_buffered (MPIDI_VC_t * vc,
                                            MPID_Request * sreq)
{
    int i, mpi_errno = MPI_SUCCESS;
    uintptr_t buf;

    /*free any preallocated buffer*/
    if (sreq->mrail.d_entry != NULL) {
        dreg_unregister(sreq->mrail.d_entry);
        sreq->mrail.d_entry = NULL;
    }
    if (sreq->mrail.rndv_buf_alloc == 1 && 
            sreq->mrail.rndv_buf != NULL) { 
        MPIU_Free(sreq->mrail.rndv_buf);
        sreq->mrail.rndv_buf = NULL;
    }
    sreq->mrail.protocol = MV2_RNDV_PROTOCOL_CUDAIPC;
    sreq->mrail.device_ipc_stage_index = 0;

    if (1 == sreq->dev.iov_count && (sreq->dev.OnDataAvail == NULL
                || sreq->dev.OnDataAvail == sreq->dev.OnFinal)) { 

        sreq->mrail.rndv_buf = sreq->dev.iov[0].MPL_IOV_BUF;
        sreq->mrail.rndv_buf_sz = sreq->dev.iov[0].MPL_IOV_LEN;
        sreq->mrail.rndv_buf_alloc = 0;

    } else {

        sreq->mrail.rndv_buf_sz = sreq->dev.segment_size;
        MPIU_Malloc_Device_Pinned_Host(sreq->mrail.rndv_buf, sreq->mrail.rndv_buf_sz);
        if (sreq->mrail.rndv_buf == NULL) { 
            ibv_error_abort(IBV_STATUS_ERR, "rndv buf allocation failed"); 
        }
        sreq->mrail.rndv_buf_alloc = 1;

        buf = (uintptr_t) sreq->mrail.rndv_buf;
        for (i = 0; i < sreq->dev.iov_count; i++) {
            MPIU_Memcpy((void *) buf, sreq->dev.iov[i].MPL_IOV_BUF,
                    sreq->dev.iov[i].MPL_IOV_LEN);
            buf += sreq->dev.iov[i].MPL_IOV_LEN;
        }

        while (sreq->dev.OnDataAvail ==
                MPIDI_CH3_ReqHandler_SendReloadIOV) {
            sreq->dev.iov_count = MPL_IOV_LIMIT;
            mpi_errno =
                MPIDI_CH3U_Request_load_send_iov(sreq,
                        sreq->dev.iov,
                        &sreq->dev.iov_count);
            /* --BEGIN ERROR HANDLING-- */
            if (mpi_errno != MPI_SUCCESS) {
                ibv_error_abort(IBV_STATUS_ERR, "Reload iov error");
            }

            for (i = 0; i < sreq->dev.iov_count; i++) {
               MPIU_Memcpy((void *) buf, sreq->dev.iov[i].MPL_IOV_BUF,
                        sreq->dev.iov[i].MPL_IOV_LEN);
                buf += sreq->dev.iov[i].MPL_IOV_LEN;
            }

        }
    }

    return 1;
}
 
int MPIDI_CH3I_MRAIL_Prepare_rndv_recv_cuda_ipc_buffered(MPIDI_VC_t * vc, 
                                            MPID_Request * rreq) 
{

    rreq->mrail.protocol = MV2_RNDV_PROTOCOL_R3;
    rreq->mrail.d_entry = NULL;
    rreq->mrail.device_ipc_stage_index = 0;

    rreq->mrail.rndv_buf = (void *) rreq->dev.iov[0].MPL_IOV_BUF;
    rreq->mrail.rndv_buf_sz = rreq->dev.iov[0].MPL_IOV_LEN;

    DEVICE_IPC_RECV_IN_PROGRESS(vc, rreq);
    PUSH_FLOWLIST(vc);
    return 1;
}

int MPIDI_CH3I_MRAIL_Prepare_rndv_device_ipc(MPIDI_VC_t * vc,
                                            MPID_Request * sreq) 
{
    int status = 1;
    size_t size;
    CUresult cuerr = CUDA_SUCCESS;
    cudaError_t cudaerr = cudaSuccess;
    CUdeviceptr baseptr;

    if (vc->smp.local_rank == -1
        || vc->smp.can_access_peer != MV2_DEVICE_IPC_ENABLED
        || sreq->mrail.device_transfer_mode == NONE
        || sreq->dev.iov_count > 1 
        || sreq->dev.iov[0].MPL_IOV_LEN < mv2_device_ipc_threshold) {
        status = 0;
        goto fn_exit;
    }

    sreq->mrail.protocol = MV2_RNDV_PROTOCOL_RGET;
    sreq->mrail.d_entry = NULL;

    cuerr = cuMemGetAddressRange(&baseptr, &size, (CUdeviceptr) sreq->dev.iov[0].MPL_IOV_BUF);
    if (cuerr != CUDA_SUCCESS) {
        DEBUG_PRINT("cuMemGetAddressRange failed, reverting to R3 \n");
        status = 0;
        goto fn_exit;
    }


    cudaerr = cudaIpcGetMemHandle (&sreq->mrail.ipc_memhandle, (void *) baseptr);
    if (cudaerr != cudaSuccess) {
        DEBUG_PRINT("cudaIpcGetMemHandle failed, reverting to R3");
        status = 0;
        goto fn_exit;
    }

    sreq->mrail.ipc_displ = (uint64_t) sreq->dev.iov[0].MPL_IOV_BUF - (uint64_t) baseptr;

    sreq->mrail.ipc_baseptr = (void *)baseptr;
    sreq->mrail.ipc_size = size;

    sreq->mrail.ipc_device_event =  get_free_cudaipc_event();

    cudaerr = cudaIpcGetEventHandle (&sreq->mrail.ipc_eventhandle, 
                    sreq->mrail.ipc_device_event->event);
    if (cudaerr != cudaSuccess) {
        DEBUG_PRINT("cudaIpcGetEventHandle failed, reverting to R3 \n");
        status = 0;
        goto fn_exit;
    }

    cudaerr = cudaEventRecord(sreq->mrail.ipc_device_event->event, 0);
    if (cudaerr != cudaSuccess) {
        DEBUG_PRINT("cudaEventRecord failed, reverting to R3 \n");
        status = 0;
        goto fn_exit;
    }

fn_exit:
    return status; 
} 

int MPIDI_CH3I_MRAIL_Rndv_transfer_device_ipc (MPIDI_VC_t * vc,
                                MPID_Request * rreq, 
                                MPIDI_CH3_Pkt_rndv_req_to_send_t *rts_pkt)
{
    int status = 1;
    void *remote_buf;
    cudaError_t cudaerr = cudaSuccess;
    void *base_ptr;
    size_t size;
    device_regcache_entry_t *reg;

    MPIU_Memcpy (&rreq->mrail.ipc_eventhandle, 
            &rts_pkt->rndv.ipc_eventhandle, sizeof(cudaIpcEventHandle_t));
    MPIU_Memcpy (&rreq->mrail.ipc_memhandle, 
            &rts_pkt->rndv.ipc_memhandle, sizeof(cudaIpcMemHandle_t));
    base_ptr = rts_pkt->rndv.ipc_baseptr;
    size = rts_pkt->rndv.ipc_size;

    cudaipc_register(base_ptr, size, vc->smp.local_rank, rreq->mrail.ipc_memhandle, &reg);
    rreq->mrail.device_reg = reg;

    rreq->mrail.ipc_baseptr = reg->remote_base; 
    remote_buf = (void *)((uint64_t) reg->remote_base + rts_pkt->rndv.ipc_displ);

    cudaerr = cudaIpcOpenEventHandle(&rreq->mrail.ipc_event, 
                    rts_pkt->rndv.ipc_eventhandle);
    if (cudaerr != cudaSuccess) {
        ibv_error_abort(IBV_STATUS_ERR, "cudaIpcOpenMemHandle failed");
    }

    cudaerr = cudaEventRecord(rreq->mrail.ipc_event, 0); 
    if (cudaerr != cudaSuccess) {
        ibv_error_abort(IBV_STATUS_ERR, "cudaEventRecord failed");
    }   

    cudaerr = cudaStreamWaitEvent(0, rreq->mrail.ipc_event, 0);
    if (cudaerr != cudaSuccess) {
        ibv_error_abort(IBV_RETURN_ERR,"cudaStreamWaitEvent failed\n");
    }

    mv2_device_event_t *cuda_event;
    cuda_event = get_device_event();
    if (cuda_event == NULL) {
        allocate_cuda_event(&rreq->mrail.device_event);
        cuda_event = rreq->mrail.device_event;
        /* add to the busy list */
        cuda_event->is_query_done = 0;
        CUDA_LIST_ADD(cuda_event, 
            busy_cuda_event_list_head, busy_cuda_event_list_tail);
    }

    MPIU_Memcpy_Device_Async(rreq->mrail.rndv_buf,
           remote_buf, rreq->mrail.rndv_buf_sz, cudaMemcpyDefault, stream_d2h);
    
    cuda_event->op_type = RGET;
    cuda_event->vc = vc;
    cuda_event->req = rreq;

    cudaerr = cudaEventRecord(cuda_event->event, stream_d2h);
    if (cudaerr != cudaSuccess) {
       ibv_error_abort(IBV_STATUS_ERR,
                "cudaEventRecord failed");
    }

    rreq->mrail.nearly_complete = 1;

   return status;
}
#endif

void MPIDI_CH3I_MRAIL_Send_device_cts_conti(MPIDI_VC_t * vc, MPID_Request * req)
{

    MPIDI_CH3_Pkt_device_cts_cont_t *cts_pkt;
    vbuf *v;
    int i;

    MRAILI_Get_buffer(vc, v, rdma_vbuf_total_size);
    cts_pkt = (MPIDI_CH3_Pkt_device_cts_cont_t *) v->buffer;
    MPIDI_Pkt_init(cts_pkt, MPIDI_CH3_PKT_CUDA_CTS_CONTI);
    cts_pkt->sender_req_id = req->dev.sender_req_id;
    cts_pkt->receiver_req_id = req->handle;

    for (i = 0;
         i < MIN((req->mrail.num_device_blocks - req->mrail.device_block_offset),
                 mv2_device_num_rndv_blocks); i++) {
        GET_VBUF_BY_OFFSET_WITHOUT_LOCK(req->mrail.device_vbuf[i], MV2_CUDA_VBUF_POOL_OFFSET);
    }
    req->mrail.num_remote_device_pending =
        MIN((req->mrail.num_device_blocks - req->mrail.device_block_offset),
            mv2_device_num_rndv_blocks);
    MPIDI_CH3I_MRAIL_SET_PKT_RNDV_DEVICE(cts_pkt, req);

    req->mrail.num_remote_device_done = 0;
    req->mrail.num_remote_device_inflight = 0;
    vbuf_init_send(v, sizeof(MPIDI_CH3_Pkt_device_cts_cont_t), 0);
    post_send(vc, v, 0);

    PRINT_DEBUG(DEBUG_CUDA_verbose, "Send CTS conti : offset: %d"
                " no.of buffers:%d req:%p\n", req->mrail.device_block_offset,
                req->mrail.num_remote_device_pending, req);

}

void MPIDI_CH3_Rendezvous_device_cts_conti(MPIDI_VC_t * vc,
                     MPIDI_CH3_Pkt_device_cts_cont_t * cts_pkt)
{
    int i, hca_index;
    MPID_Request *sreq;
    MPID_Request_get_ptr(cts_pkt->sender_req_id, sreq);

    for (i = 0; i < cts_pkt->rndv.num_device_blocks; i++) {
        sreq->mrail.device_remote_addr[i] = cts_pkt->rndv.buffer_addr[i];
        for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
            sreq->mrail.device_remote_rkey[i][hca_index] =
                cts_pkt->rndv.buffer_rkey[i][hca_index];
        }
        sreq->mrail.num_remote_device_pending = cts_pkt->rndv.num_device_blocks;
        sreq->mrail.device_block_offset = cts_pkt->rndv.device_block_offset;
        sreq->mrail.num_remote_device_done = 0;
    }
    PRINT_DEBUG(DEBUG_CUDA_verbose, "received cuda CTS conti: sreq:%p\n", sreq);

    MV2_DEVICE_PROGRESS();
    PUSH_FLOWLIST(vc);

}

void MRAILI_RDMA_Put_finish_device(MPIDI_VC_t * vc,
                                 MPID_Request * sreq, int rail,
                                 int is_device_pipeline, int device_pipeline_finish,
                                 int offset)
{
    MPIDI_CH3_Pkt_rput_finish_t rput_pkt;
    MPL_IOV iov;
    int n_iov = 1;
    int nb;
    vbuf *buf;
    MPID_Seqnum_t seqnum;

    MPIDI_Pkt_init(&rput_pkt, MPIDI_CH3_PKT_RPUT_FINISH);
    rput_pkt.receiver_req_id = sreq->mrail.partner_id;
    MPIDI_VC_FAI_send_seqnum(vc, seqnum);
    MPIDI_Pkt_set_seqnum(&rput_pkt, seqnum);

    rput_pkt.is_device = 1;
    rput_pkt.is_device_pipeline = is_device_pipeline;
    rput_pkt.device_pipeline_finish = device_pipeline_finish;
    rput_pkt.device_pipeline_offset = offset;

    iov.MPL_IOV_BUF = &rput_pkt;
    iov.MPL_IOV_LEN = sizeof(MPIDI_CH3_Pkt_rput_finish_t);

    DEBUG_PRINT("Sending RPUT FINISH\n");

    int rc = MPIDI_CH3I_MRAILI_rput_complete(vc, &iov, n_iov, &nb, &buf, rail);

    if (rc != 0 && rc != MPI_MRAIL_MSG_QUEUED) {
        ibv_error_abort(IBV_STATUS_ERR,
                        "Cannot send rput through send/recv path");
    }

    buf->sreq = (void *) sreq;

    /* mark MPI send complete when VIA send completes */

    DEBUG_PRINT("VBUF ASSOCIATED: %p, %08x\n", buf, buf->desc.u.sr.wr_id);
}

void MPIDI_CH3I_MRAILI_Rendezvous_rput_push_device(MPIDI_VC_t * vc,
        MPID_Request * sreq)
{
    int i = 0,rail;
    int nbytes, rail_index;
    cudaError_t cuda_error = cudaSuccess;
    vbuf *v;

    if (sreq->mrail.device_transfer_mode == DEVICE_TO_DEVICE
        || sreq->mrail.device_transfer_mode == DEVICE_TO_HOST){

        sreq->mrail.num_device_blocks =
            ROUNDUP(sreq->mrail.rndv_buf_sz, mv2_device_stage_block_size);

        /* get device_vbuf and enqueue asynchronous copy*/
        i = sreq->mrail.num_send_device_copy;
        mv2_device_event_t *cuda_event;
        for (; i < sreq->mrail.num_device_blocks; i++) {
            /* get cuda event */
            cuda_event = get_device_event();
            if (cuda_event == NULL) {
                break;
            }
            nbytes = mv2_device_stage_block_size;
            cuda_event->is_finish = 0;
            if (i == (sreq->mrail.num_device_blocks - 1)) {
                if (sreq->mrail.rndv_buf_sz % mv2_device_stage_block_size) {
                    nbytes = sreq->mrail.rndv_buf_sz % mv2_device_stage_block_size;
                }
                cuda_event->is_finish = 1;
            }
            cuda_event->op_type = SEND;
            cuda_event->vc = vc;
            cuda_event->req = sreq;
            cuda_event->displacement = i;
            cuda_event->size = nbytes; 
            GET_VBUF_BY_OFFSET_WITHOUT_LOCK(cuda_event->device_vbuf_head, MV2_CUDA_VBUF_POOL_OFFSET);
            sreq->mrail.num_send_device_copy++;
            MPIU_Memcpy_Device_Async(cuda_event->device_vbuf_head->buffer,
                    (const void *)(sreq->dev.iov[0].MPL_IOV_BUF + 
                        mv2_device_stage_block_size * i), nbytes,
                    cudaMemcpyDeviceToHost, stream_d2h);
            PRINT_DEBUG(DEBUG_CUDA_verbose>1, 
                    "Issue cudaMemcpyAsync :%d req:%p strm:%p\n",
                    sreq->mrail.num_send_device_copy, sreq, cuda_event);

            /* recoed the event */
            cuda_error = cudaEventRecord(cuda_event->event, stream_d2h);
            if (cuda_error != cudaSuccess) {
                ibv_error_abort(IBV_RETURN_ERR,
                        "cudaEventRecord failed\n");
            } 
        }
    } else if (rdma_enable_cuda 
            && sreq->mrail.device_transfer_mode == HOST_TO_DEVICE) {

        int i = 0, is_finish = 0, is_pipeline = 0;
        int stripe_avg, stripe_size, offset;

        sreq->mrail.num_device_blocks =
            ROUNDUP(sreq->mrail.rndv_buf_sz, mv2_device_stage_block_size);

        if (sreq->mrail.num_device_blocks > 1) {
            is_pipeline = 1;
        }

        i = sreq->mrail.num_send_device_copy;
        for (; i < sreq->mrail.num_device_blocks; i++) {
            if(sreq->mrail.num_remote_device_pending == 0) {
                PRINT_DEBUG(DEBUG_CUDA_verbose>1, "done:%d cuda_copy:%d "
                "num_blk:%d\n", sreq->mrail.num_remote_device_done,
                sreq->mrail.num_send_device_copy, sreq->mrail.num_device_blocks);

                break;
            }
            sreq->mrail.pipeline_nm++;

            nbytes = mv2_device_stage_block_size;
            if (i == (sreq->mrail.num_device_blocks - 1)) {
                if (sreq->mrail.rndv_buf_sz % mv2_device_stage_block_size) {
                    nbytes = sreq->mrail.rndv_buf_sz % mv2_device_stage_block_size;
                }
                is_finish = 1;
            }

            if (nbytes <= rdma_large_msg_rail_sharing_threshold) {
                rail = MRAILI_Send_select_rail(vc, nbytes);
                rail_index = vc->mrail.rails[rail].hca_index;

                GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_SMALL_DATA_VBUF_POOL_OFFSET);
                v->sreq = sreq;

                MRAILI_RDMA_Put(vc, v,
                        (char *) (sreq->mrail.rndv_buf) +
                        sreq->mrail.rndv_buf_off + mv2_device_stage_block_size * i,
                        ((dreg_entry *)sreq->mrail.d_entry)->memhandle[rail_index]->lkey,
                        (char *) (sreq->mrail.
                            device_remote_addr[sreq->mrail.num_remote_device_done]),
                        sreq->mrail.
                        device_remote_rkey[sreq->mrail.num_remote_device_done][rail_index],
                        nbytes, rail);
            } else { 
                stripe_avg = nbytes / rdma_num_rails;

                for (rail = 0; rail < rdma_num_rails; rail++) {
                    offset = rail*stripe_avg;
                    stripe_size = (rail < rdma_num_rails-1) ? stripe_avg : (nbytes - offset);

                    rail_index = vc->mrail.rails[rail].hca_index;

                    GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_SMALL_DATA_VBUF_POOL_OFFSET);
                    v->sreq = sreq;

                    MRAILI_RDMA_Put(vc, v,
                        (char *) (sreq->mrail.rndv_buf) +
                        sreq->mrail.rndv_buf_off + mv2_device_stage_block_size * i + offset,
                        ((dreg_entry *)sreq->mrail.d_entry)->memhandle[rail_index]->lkey,
                        (char *) (sreq->mrail.
                            device_remote_addr[sreq->mrail.num_remote_device_done]) + offset,
                        sreq->mrail.
                        device_remote_rkey[sreq->mrail.num_remote_device_done][rail_index],
                        stripe_size, rail);
                }
            }

            for(rail = 0; rail < rdma_num_rails; rail++) {
                MRAILI_RDMA_Put_finish_device(vc, sreq, rail, is_pipeline, is_finish,
                        mv2_device_stage_block_size*i);
            }

            sreq->mrail.num_remote_device_pending--;
            sreq->mrail.num_remote_device_done++;
            sreq->mrail.num_send_device_copy++;
        }
    }

    if (sreq->mrail.num_device_blocks == sreq->mrail.num_send_device_copy) {
        sreq->mrail.nearly_complete = 1;
    } else {
        sreq->mrail.nearly_complete = 0;
    }
}

/* return true if request if complete */
int MPIDI_CH3I_MRAILI_Process_device_finish(MPIDI_VC_t * vc, MPID_Request * rreq,
                                    MPIDI_CH3_Pkt_rput_finish_t * rf_pkt)
{
    int nbytes;
    int rreq_complete = 0;
    cudaError_t cuda_error = cudaSuccess;
    vbuf *device_vbuf = NULL;
    int i, displacement; 

    MV2_DEVICE_PROGRESS();
    
    MPIU_Assert (rf_pkt->is_device == 1);

    displacement = rf_pkt->device_pipeline_offset / mv2_device_stage_block_size;
    i=rreq->mrail.num_remote_device_done;
    for (; i<rreq->mrail.num_remote_device_inflight; i++) {
        if (rreq->mrail.device_vbuf[i]->displacement == displacement) {
            break;
        } 
    }
    rreq->mrail.device_vbuf[i]->finish_count++;

    if (i == rreq->mrail.num_remote_device_inflight) {
        /*writes to a new cuda vbuf are inflight on other rails*/
        rreq->mrail.device_vbuf[i]->displacement = displacement;
        rreq->mrail.num_remote_device_inflight++;
    }

    if (rreq->mrail.device_vbuf[i]->finish_count == rdma_num_rails) {
        device_vbuf = rreq->mrail.device_vbuf[i];
    } else {
       goto fn_exit; 
    }

    rreq->mrail.pipeline_nm++;
    nbytes = (rreq->mrail.rndv_buf_sz - rf_pkt->device_pipeline_offset < mv2_device_stage_block_size) ?
             (rreq->mrail.rndv_buf_sz - rf_pkt->device_pipeline_offset) : mv2_device_stage_block_size;
    
    mv2_device_event_t *cuda_event = NULL;
    if (rreq->mrail.device_event == NULL) {
        /* allocate cuda event for handle recv requests*/
        allocate_cuda_event(&rreq->mrail.device_event);
    }
    cuda_event = rreq->mrail.device_event;

    cuda_event->op_type = RECV;
    cuda_event->is_finish = (rreq->mrail.pipeline_nm == 
            rreq->mrail.num_device_blocks) ? 1 : 0;
    cuda_event->displacement = displacement;
    cuda_event->vc = vc;
    cuda_event->req = rreq;

    /* add vbuf to the list of buffer on this event */
    if (cuda_event->device_vbuf_head == NULL) {
        cuda_event->device_vbuf_head  = device_vbuf;
    } else {
        cuda_event->device_vbuf_tail->next = device_vbuf;
    }
    cuda_event->device_vbuf_tail = device_vbuf;
    cuda_event->device_vbuf_tail->next = NULL;

    rreq->mrail.num_remote_device_done++;
    rreq->mrail.num_remote_device_pending--;
    if (rreq->mrail.num_remote_device_pending == 0 &&
            rreq->mrail.pipeline_nm != rreq->mrail.num_device_blocks) {
        rreq->mrail.device_block_offset += mv2_device_num_rndv_blocks;
        MPIDI_CH3I_MRAIL_Send_device_cts_conti(vc, rreq);
    }

    MPIU_Memcpy_Device_Async(rreq->mrail.rndv_buf + rf_pkt->device_pipeline_offset,
            (const void *)(device_vbuf->buffer),
            nbytes, cudaMemcpyHostToDevice, stream_h2d);

    /* record the event */
    cuda_error = cudaEventRecord(cuda_event->event, stream_h2d);
    if (cuda_error != cudaSuccess) {
        ibv_error_abort(IBV_RETURN_ERR,
                "cudaEventRecord failed\n");
    } 

    /* Add event to the polling(busy) list if it is not already in it*/
    if (cuda_event->next == NULL && cuda_event->prev == NULL &&
            cuda_event != busy_cuda_event_list_head) {
        cuda_event->is_query_done = 0;
        CUDA_LIST_ADD(cuda_event, 
            busy_cuda_event_list_head, busy_cuda_event_list_tail);
    }

    rreq->mrail.local_complete = 0;

    PRINT_DEBUG(DEBUG_CUDA_verbose>1, "RECV cudaMemcpyAsync :%d "
            "req:%p strm:%p\n", rreq->mrail.num_remote_device_done,
            rreq, cuda_event);

fn_exit:
    return rreq_complete;
}


#undef FUNCNAME
#define FUNCNAME MPIDI_CH3_Prepare_rndv_cts_device
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3_Prepare_rndv_cts_device(MPIDI_VC_t * vc,
        MPIDI_CH3_Pkt_rndv_clr_to_send_t * cts_pkt,
        MPID_Request * rreq)
{
    int mpi_errno = MPI_SUCCESS;

    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_PREPARE_RNDV_CTS_CUDA);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_PREPARE_RNDV_CTS_CUDA);

    cts_pkt->rndv.device_transfer_mode = DEVICE_TO_DEVICE;

#ifdef CKPT
    MPIDI_CH3I_CR_lock();
#endif

#if defined(HAVE_CUDA_IPC)
    if (mv2_device_use_ipc && mv2_device_use_ipc_stage_buffer && vc->smp.can_access_peer == MV2_DEVICE_IPC_ENABLED) {
        if ((rreq->mrail.protocol == MV2_RNDV_PROTOCOL_CUDAIPC && 
                rreq->mrail.device_transfer_mode == NONE) ||
            (rreq->mrail.protocol != MV2_RNDV_PROTOCOL_CUDAIPC &&
                rreq->mrail.device_transfer_mode != NONE)) {
            if (vc->smp.local_nodes >= 0) {
                rreq->mrail.protocol = MV2_RNDV_PROTOCOL_R3;
            } else {
                rreq->mrail.protocol = MV2_RNDV_PROTOCOL_CUDAIPC;
            }
        }
    }
#endif

    switch (rreq->mrail.protocol) {
        case MV2_RNDV_PROTOCOL_RPUT:
            {
                MPIDI_CH3I_MRAIL_Prepare_rndv_device(vc, NULL, rreq);
                MPIDI_CH3I_MRAIL_SET_PKT_RNDV_DEVICE(cts_pkt, rreq);
                PRINT_DEBUG(DEBUG_CUDA_verbose, "CTS: offset:%d "
                        " #blocks: %d rreq:%p\n", cts_pkt->rndv.device_block_offset,
                        cts_pkt->rndv.num_device_blocks, rreq);

                MPIDI_CH3I_MRAIL_REVERT_RPUT(rreq);
                break;
            }
        case MV2_RNDV_PROTOCOL_CUDAIPC:
            {
#if defined(HAVE_CUDA_IPC)
                cts_pkt->rndv.protocol = MV2_RNDV_PROTOCOL_CUDAIPC;
                MPIDI_CH3I_MRAIL_Prepare_rndv_recv_cuda_ipc_buffered(vc, rreq);
#else
                PRINT_ERROR("CUDAIPC rndv is requested, but CUDA IPC is not supported\n");
                mpi_errno = -1;
#endif
                break;
            }
        case MV2_RNDV_PROTOCOL_R3:
            {
                cts_pkt->rndv.protocol = MV2_RNDV_PROTOCOL_R3;
                break;
            }
        default:
            {
                PRINT_ERROR("Unknown protocol %d type from rndv req to send\n",
                        rreq->mrail.protocol);
                mpi_errno = -1;
                break;
            }
    }

#ifdef CKPT
    MPIDI_CH3I_CR_unlock();
#endif

    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_PREPARE_RNDV_CTS_CUDA);
    return mpi_errno;
}
#endif /* _ENABLE_CUDA_ */
