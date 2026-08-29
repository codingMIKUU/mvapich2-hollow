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

#include "mpidi_ch3_impl.h"
#include "mpiutil.h"
#include "rdma_impl.h"
#include "ibv_cuda_util.h"
#include "dreg.h"

MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_vbuf_allocated);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_vbuf_freed);
MPIR_T_PVAR_ULONG_LEVEL_DECL_EXTERN(MV2, mv2_vbuf_available);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ud_vbuf_allocated);
MPIR_T_PVAR_ULONG_COUNTER_DECL_EXTERN(MV2, mv2_ud_vbuf_freed);
MPIR_T_PVAR_ULONG_LEVEL_DECL_EXTERN(MV2, mv2_ud_vbuf_available);

#ifdef _ENABLE_CUDA_
void *cuda_event_region = NULL;
mv2_device_event_t *free_cudaipc_event_list_head = NULL;
mv2_device_event_t *free_cuda_event_list_head = NULL;
mv2_device_event_t *busy_cuda_event_list_head = NULL;
mv2_device_event_t *busy_cuda_event_list_tail = NULL;

cudaStream_t  stream_d2h = 0, stream_h2d = 0, stream_kernel = 0;
cudaEvent_t cuda_nbstream_sync_event = 0;

void allocate_cuda_rndv_streams()
{
    if (mv2_device_nonblocking_streams) {
        CUDA_CHECK(cudaStreamCreateWithFlags(&stream_d2h, cudaStreamNonBlocking));
        CUDA_CHECK(cudaStreamCreateWithFlags(&stream_h2d, cudaStreamNonBlocking));
        CUDA_CHECK(cudaStreamCreateWithFlags(&stream_kernel, cudaStreamNonBlocking));
        CUDA_CHECK(cudaEventCreateWithFlags(&cuda_nbstream_sync_event,
                  cudaEventDisableTiming));
    } else {
        CUDA_CHECK(cudaStreamCreate(&stream_d2h));
        CUDA_CHECK(cudaStreamCreate(&stream_h2d));
        CUDA_CHECK(cudaStreamCreate(&stream_kernel));
    }
}

void deallocate_cuda_rndv_streams()
{
    if (stream_d2h) {
        cudaStreamDestroy(stream_d2h);
    }
    if (stream_h2d) {
        cudaStreamDestroy(stream_h2d);
    }
    if (stream_kernel) {
        cudaStreamDestroy(stream_kernel);
    }
    if (cuda_nbstream_sync_event) {
        cudaEventDestroy(cuda_nbstream_sync_event);
    }
}

void allocate_cuda_events()
{
    int i;
    cudaError_t result;
    mv2_device_event_t *curr;
    cuda_event_region = (void *) MPIU_Malloc(sizeof(mv2_device_event_t)
                                              * mv2_device_event_count);
    if (stream_d2h == 0 && stream_h2d == 0) {
        allocate_cuda_rndv_streams();
    }
    MPIU_Assert(free_cuda_event_list_head == NULL);
    free_cuda_event_list_head = cuda_event_region;
    curr = (mv2_device_event_t *) cuda_event_region;
    for (i = 1; i <= mv2_device_event_count; i++) {
        curr->next = (mv2_device_event_t *) ((size_t) cuda_event_region +
                                        i * sizeof(mv2_device_event_t));
        result = cudaEventCreateWithFlags(&(curr->event), cudaEventDisableTiming
#if defined(HAVE_CUDA_IPC)
            | cudaEventInterprocess
#endif
            );
        if (result != cudaSuccess) {
            ibv_error_abort(GEN_EXIT_ERR, "Cuda Event Creation failed \n");
        }
        curr->op_type = -1;
        if (i == mv2_device_event_count) {
            curr->next = NULL;
        }
        curr->prev = NULL;
        curr->flags =  CUDA_EVENT_FREE_POOL;
        curr = curr->next;
    }
}

void deallocate_cuda_events()
{
    mv2_device_event_t *curr_event = free_cuda_event_list_head;
    MPIU_Assert(busy_cuda_event_list_head == NULL);
    while(curr_event) {
        cudaEventDestroy(curr_event->event);
        curr_event = curr_event->next;
    }

    if (cuda_event_region != NULL) {
        MPIU_Free(cuda_event_region);
        cuda_event_region = NULL;
    }
    /* Free IPC event pool */
    while(free_cudaipc_event_list_head) {
        curr_event = free_cudaipc_event_list_head;
        cudaEventDestroy(curr_event->event);
        free_cudaipc_event_list_head = free_cudaipc_event_list_head->next;
        MPIU_Free(curr_event);
    }
}

int allocate_cuda_event(mv2_device_event_t **cuda_event)
{
    cudaError_t result;
    *cuda_event = (mv2_device_event_t *) MPIU_Malloc(sizeof(mv2_device_event_t));
    result = cudaEventCreateWithFlags(&((*cuda_event)->event), cudaEventDisableTiming
#if defined(HAVE_CUDA_IPC)
            | cudaEventInterprocess
#endif
            );
    /* if OOM for allocating CUDA event, return NULL and we will try it later */
    if (cudaErrorMemoryAllocation == result) {
        MPIU_Free(*cuda_event);
        *cuda_event = NULL;
        return 0;
    }
    if (result != cudaSuccess) {
        ibv_error_abort(GEN_EXIT_ERR, "Cuda Event Creation failed \n");
    }
    (*cuda_event)->next = (*cuda_event)->prev = NULL;
    (*cuda_event)->device_vbuf_head = (*cuda_event)->device_vbuf_tail = NULL;
    (*cuda_event)->flags = CUDA_EVENT_DEDICATED;
    return 0;
}

void deallocate_cuda_event(mv2_device_event_t **cuda_event)
{
    CUDA_CHECK(cudaEventDestroy((*cuda_event)->event));
    MPIU_Free(*cuda_event);
    *cuda_event = NULL;
}

#if defined(HAVE_CUDA_IPC)
mv2_device_event_t *get_free_cudaipc_event()
{
    mv2_device_event_t *curr_event;

    if (NULL == free_cudaipc_event_list_head) {
        allocate_cuda_event(&curr_event);
    } else {
        curr_event = free_cudaipc_event_list_head;
        free_cudaipc_event_list_head = free_cudaipc_event_list_head->next;
    }

    curr_event->next = curr_event->prev = NULL;
    curr_event->is_query_done = 0;

    return curr_event;
}

void release_cudaipc_event(mv2_device_event_t *event)
{
    /* add event to the free list */
    event->next = free_cudaipc_event_list_head;
    free_cudaipc_event_list_head = event; 
}
#endif

void process_cuda_event_op(mv2_device_event_t * event)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_Request *req = event->req;
    MPIDI_VC_t *vc = (MPIDI_VC_t *) event->vc;
    vbuf *cuda_vbuf = event->device_vbuf_head;
    int displacement = event->displacement;
    int is_finish = event->is_finish;
    int is_pipeline = (!displacement && is_finish) ? 0 : 1;
    int size = event->size;
    int rail, stripe_avg, stripe_size, offset;
    vbuf *v, *orig_vbuf = NULL;

    if (event->op_type == SEND) {

        req->mrail.pipeline_nm++;
        is_finish = (req->mrail.pipeline_nm == req->mrail.num_device_blocks)? 1 : 0;
 
        if (req->mrail.device_transfer_mode == DEVICE_TO_DEVICE) {
            if (size <= rdma_large_msg_rail_sharing_threshold) {
                rail = MRAILI_Send_select_rail(vc, size);

                v = cuda_vbuf;
                v->sreq = req;        

                MRAILI_RDMA_Put(vc, v,
                    (char *) (v->buffer),
                    v->region->mem_handle[vc->mrail.rails[rail].hca_index]->lkey,
                    (char *) (req->mrail.device_remote_addr[req->mrail.num_remote_device_done]),
                    req->mrail.device_remote_rkey[req->mrail.num_remote_device_done]
                            [vc->mrail.rails[rail].hca_index], size, rail);
            } else {
                stripe_avg = size / rdma_num_rails;
                orig_vbuf = cuda_vbuf;

                for (rail = 0; rail < rdma_num_rails; rail++) {
                    offset = rail*stripe_avg; 
                    stripe_size = (rail < rdma_num_rails-1) ? stripe_avg : (size - offset);

                    GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_SMALL_DATA_VBUF_POOL_OFFSET);
                    v->sreq = req;
                    v->orig_vbuf = orig_vbuf;

                    MRAILI_RDMA_Put(vc, v,
                        (char *) (orig_vbuf->buffer) + offset,
                        orig_vbuf->region->mem_handle[vc->mrail.rails[rail].hca_index]->lkey,
                        (char *) (req->mrail.device_remote_addr[req->mrail.num_remote_device_done])
                        + offset,
                        req->mrail.device_remote_rkey[req->mrail.num_remote_device_done]
                                [vc->mrail.rails[rail].hca_index], stripe_size, rail);
                }
            }

            for(rail = 0; rail < rdma_num_rails; rail++) {
                MRAILI_RDMA_Put_finish_device(vc, req, rail, is_pipeline, is_finish,
                                        mv2_device_stage_block_size * displacement);
            }
            req->mrail.num_remote_device_done++;
        } else if (req->mrail.device_transfer_mode == DEVICE_TO_HOST) {
            if (size <= rdma_large_msg_rail_sharing_threshold) {
                rail = MRAILI_Send_select_rail(vc, size);

                v = cuda_vbuf;
                v->sreq = req;

                MRAILI_RDMA_Put(vc, v,
                    (char *) (v->buffer),
                    v->region->mem_handle[vc->mrail.rails[rail].hca_index]->lkey,
                    (char *) (req->mrail.remote_addr) + displacement * 
                    mv2_device_stage_block_size,
                    req->mrail.rkey[vc->mrail.rails[rail].hca_index],
                    size, rail);
            } else {   
                stripe_avg = size / rdma_num_rails;
                orig_vbuf = cuda_vbuf;

                for (rail = 0; rail < rdma_num_rails; rail++) {
                    offset = rail*stripe_avg;
                    stripe_size = (rail < rdma_num_rails-1) ? stripe_avg : (size - offset);

                    GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_SMALL_DATA_VBUF_POOL_OFFSET);
                    v->sreq = req;
                    v->orig_vbuf = orig_vbuf;

                    MRAILI_RDMA_Put(vc, v,
                        (char *) (orig_vbuf->buffer) + offset,
                        orig_vbuf->region->mem_handle[vc->mrail.rails[rail].hca_index]->lkey,
                        (char *) (req->mrail.remote_addr) + displacement *
                        mv2_device_stage_block_size + offset,
                        req->mrail.rkey[vc->mrail.rails[rail].hca_index],
                        stripe_size, rail);
                } 
            }
            if (is_finish) {
               for(rail = 0; rail < rdma_num_rails; rail++) {
                    MRAILI_RDMA_Put_finish_device(vc, req, rail, is_pipeline,
                                        is_finish,
                                        mv2_device_stage_block_size *
                                        displacement);
               }
            }
        }

        PRINT_DEBUG(DEBUG_CUDA_verbose > 1, "RDMA write block: "
                    "send offset:%d  rem idx: %d is_fin:%d"
                    "addr offset:%d strm:%p\n", displacement,
                    req->mrail.num_remote_device_done, is_finish,
                    mv2_device_stage_block_size * displacement, event);

        req->mrail.num_remote_device_pending--;
        if (req->mrail.num_send_device_copy != req->mrail.num_device_blocks) {
            PUSH_FLOWLIST(vc);
        }
        if (event->flags == CUDA_EVENT_DEDICATED) {
            deallocate_cuda_event(&event);
        }
    } else if (event->op_type == RECV) {

        vbuf *temp_buf;
        /* release all the vbufs in the recv event */
        while(cuda_vbuf)
        {
            PRINT_DEBUG(DEBUG_CUDA_verbose > 2, "CUDA copy block: "
                "buf:%p\n", cuda_vbuf->buffer);
            temp_buf = cuda_vbuf->next;
            cuda_vbuf->next = NULL;
            release_vbuf(cuda_vbuf);
            cuda_vbuf = temp_buf;
        }
        event->device_vbuf_head = event->device_vbuf_tail = NULL;
        
        if (event->is_finish) {
            int complete = 0;
            MPIDI_CH3I_MRAILI_RREQ_RNDV_FINISH(req);
            /* deallocate the stream if request is finished */
            if (req->mrail.device_event) {
                deallocate_cuda_event(&req->mrail.device_event);
            }
            
            mpi_errno = MPIDI_CH3U_Handle_recv_req(vc, req, &complete);
            if (mpi_errno != MPI_SUCCESS) {
                ibv_error_abort(IBV_RETURN_ERR,
                        "MPIDI_CH3U_Handle_recv_req returned error");
            }
            MPIU_Assert(complete == TRUE);
            vc->ch.recv_active = NULL;
        }
#if defined(HAVE_CUDA_IPC)
    } else if (event->op_type == RGET) {
        cudaError_t cudaerr = cudaSuccess;

        cudaerr = cudaEventRecord(req->mrail.ipc_event, stream_d2h);
        if (cudaerr != cudaSuccess) {
           ibv_error_abort(IBV_STATUS_ERR,
                    "cudaEventRecord failed");
        }

        cudaerr = cudaStreamWaitEvent(0, req->mrail.ipc_event, 0);
        if (cudaerr != cudaSuccess) {
            ibv_error_abort(IBV_RETURN_ERR,"cudaStreamWaitEvent failed\n");
        }


        cudaerr = cudaEventDestroy(req->mrail.ipc_event);
        if (cudaerr != cudaSuccess) {
            ibv_error_abort(IBV_RETURN_ERR,"cudaEventDestroy failed\n");
        }

        /* deallocate the stream if it is allocated */
        if (req->mrail.device_event) {
            deallocate_cuda_event(&req->mrail.device_event);
        }

        if (req->mrail.device_reg) {
            cudaipc_deregister(req->mrail.device_reg);
            req->mrail.device_reg = NULL;
        }
        MRAILI_RDMA_Get_finish(vc, req, 0);

    } else if (event->op_type == CUDAIPC_SEND) {
        int complete;
        if (req->mrail.rndv_buf_alloc == 1 && 
                req->mrail.rndv_buf != NULL) { 
            /* a temporary host rndv buffer would have been allocated only when the 
               sender buffers is noncontiguous and is in the host memory */
            MPIU_Assert(req->mrail.device_transfer_mode == HOST_TO_DEVICE);
            MPIU_Free_Device_Pinned_Host(req->mrail.rndv_buf);
            req->mrail.rndv_buf_alloc = 0;
            req->mrail.rndv_buf = NULL;
        }
        MPIDI_CH3U_Handle_send_req(vc, req, &complete);
        MPIU_Assert(complete == TRUE);
        if (event->flags == CUDA_EVENT_DEDICATED) {
            deallocate_cuda_event(&req->mrail.device_event);
        }
    } else if (event->op_type == CUDAIPC_RECV) {
        int complete;
        int mpi_errno = MPI_SUCCESS;
        mpi_errno = MPIDI_CH3U_Handle_recv_req(vc, req, &complete);
        if (mpi_errno != MPI_SUCCESS) {
            ibv_error_abort(IBV_RETURN_ERR,
                    "MPIDI_CH3U_Handle_recv_req returned error");
        }
        MPIU_Assert(complete == TRUE);
        if (event->flags == CUDA_EVENT_DEDICATED) {
            deallocate_cuda_event(&req->mrail.device_event);
        }
#endif
    } else if (event->op_type == SMP_SEND) {
        smp_device_send_copy_complete(event->vc, event->req, event->smp_ptr);
        if (event->flags == CUDA_EVENT_DEDICATED) {
            deallocate_cuda_event(&event);
        }         
    } else if (event->op_type == SMP_RECV) {
        smp_device_recv_copy_complete(event->vc, event->req, event->smp_ptr);
        if (event->flags == CUDA_EVENT_DEDICATED) {
            deallocate_cuda_event(&event);
        }
    } else { 
        ibv_error_abort(IBV_RETURN_ERR, "Invalid op type in event");
    }
}

void progress_cuda_events()
{
    cudaError_t result = cudaSuccess;
    mv2_device_event_t *curr_event = busy_cuda_event_list_head;
    mv2_device_event_t *next_event;
    uint8_t event_pool_flag;

    while (NULL != curr_event) {
        if (!curr_event->is_query_done) {
            result = cudaEventQuery(curr_event->event);
        }
        if (cudaSuccess == result || curr_event->is_query_done) {
            curr_event->is_query_done = 1;
            if ((SEND == curr_event->op_type) &&
                ((1 != ((MPID_Request *) curr_event->req)->mrail.cts_received)
                || !((MPID_Request *) curr_event->req)->mrail.
                num_remote_device_pending)) {
                curr_event = curr_event->next;
            } else {
                next_event = curr_event->next;

                /* detach finished event from the busy list*/    
                if (curr_event->prev) {
                    curr_event->prev->next = curr_event->next;
                } else {
                    busy_cuda_event_list_head = curr_event->next;
                }
                if (curr_event->next) {
                    curr_event->next->prev = curr_event->prev;
                } else {
                    busy_cuda_event_list_tail = curr_event->prev;
                }

                curr_event->is_query_done = 0;
                curr_event->next = curr_event->prev = NULL;
                event_pool_flag = curr_event->flags;

                /* process finished event */
                process_cuda_event_op(curr_event);

                /* Dedicated event will be deallocated after 
                ** the request is finished*/
                if (event_pool_flag == CUDA_EVENT_FREE_POOL) {
                    curr_event->next = free_cuda_event_list_head;
                    free_cuda_event_list_head = curr_event;
                }
                
                curr_event = next_event;
            }
        } else {
            curr_event = curr_event->next;
        }
    }
}

mv2_device_event_t *get_device_event()
{

    mv2_device_event_t *curr_event;
   
    if (NULL == free_cuda_event_list_head) {
        if (NULL != busy_cuda_event_list_head) {
            /* if there are no more free events in the list, allocate a temporary one */
            mv2_device_event_t *dedicate_cuda_event = NULL;
            allocate_cuda_event(&dedicate_cuda_event);
            if (NULL != dedicate_cuda_event) {
                /* add to the busy list */
                dedicate_cuda_event->is_query_done = 0;
                CUDA_LIST_ADD(dedicate_cuda_event,
                    busy_cuda_event_list_head, busy_cuda_event_list_tail);
            }
            return dedicate_cuda_event;
        } else {
            allocate_cuda_events();
        }
    } 

    curr_event = free_cuda_event_list_head;
    free_cuda_event_list_head = free_cuda_event_list_head->next;
    curr_event->next = curr_event->prev = NULL;

    /* Enqueue event to the busy list*/
    if (NULL == busy_cuda_event_list_head) {
        busy_cuda_event_list_head = curr_event;
        busy_cuda_event_list_tail = curr_event;
    } else {
        busy_cuda_event_list_tail->next = curr_event;
        curr_event->prev = busy_cuda_event_list_tail;
        busy_cuda_event_list_tail = curr_event;
    }
    curr_event->is_query_done = 0;
    return curr_event;
}
#endif
