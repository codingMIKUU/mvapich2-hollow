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

#if defined(_MCST_SUPPORT_)
#include "ibv_mcast.h"
#endif 

#include "ibv_send_inline.h"
#ifdef CHANNEL_MRAIL_GEN2
#include "coll_shmem.h"
#endif

/* rdma_impl.h is included by posting paths that do not include the large
 * ibv_send_inline.h header.  Keep the queue implementation inline for the
 * normal fast path, and expose these two small wrappers for shared-SQ retry
 * handling across translation units. */
void MRAILI_Hollow_enqueue_retry(MPIDI_VC_t *vc, int rail, vbuf *v)
{
    MRAILI_Ext_sendq_enqueue(vc, rail, v);
}

void MRAILI_Hollow_retry_blocked(void)
{
    MRAILI_Hollow_retry_blocked_inline();
}

#if defined(MPIDI_MRAILI_COALESCE_ENABLED)
/* TODO: Ticket #1433 */
void FLUSH_SQUEUE_NOINLINE(MPIDI_VC_t *vc)
{
    FLUSH_SQUEUE(vc);
}
#endif /*defined(MPIDI_MRAILI_COALESCE_ENABLED)*/

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_RDMA_put_datav
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3I_RDMA_put_datav(MPIDI_VC_t * vc, MPL_IOV * iov, int n,
                              int *num_bytes_ptr)
{
    int mpi_errno = MPI_SUCCESS;
    /* all variable must be declared before the state declarations */
    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_PUT_DATAV);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_PUT_DATAV);

   /* Insert implementation here */
    PRINT_ERROR("MPIDI_CH3I_RDMA_put_datav is not implemented\n" );
    exit(EXIT_FAILURE);

   MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_PUT_DATAV);
    return mpi_errno;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_RDMA_read_datav
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3I_RDMA_read_datav(MPIDI_VC_t * recv_vc_ptr, MPL_IOV * iov,
                               int iovlen, int
                               *num_bytes_ptr)
{
    int mpi_errno = MPI_SUCCESS;
    /* all variable must be declared before the state declarations */
    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_RDMA_READ_DATAV);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_RDMA_READ_DATAV);

    /* Insert implementation here */
    PRINT_ERROR("MPIDI_CH3I_RDMA_read_datav Function not implemented\n");
    exit(EXIT_FAILURE);

    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_RDMA_READ_DATAV);
    return mpi_errno;
}

#undef FUNCNAME
#define FUNCNAME mv2_post_srq_buffers
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int mv2_post_srq_buffers(int num_bufs, int hca_num)
{
    int i = 0;
    vbuf* v = NULL;
    struct ibv_recv_wr* bad_wr = NULL;
#ifdef _ENABLE_HOLLOW_RC_
    static int hollow_post_target_reported;
    uint32_t actual_srqn = 0;

    if (!hollow_post_target_reported && getenv("MLX5_SRM_WQE_DEBUG") &&
        ibv_get_srq_num(
            mv2_MPIDI_CH3I_RDMA_Process.srq_hndl[hca_num],
            &actual_srqn) == 0) {
        hollow_post_target_reported = 1;
        fprintf(stderr,
                "HOLLOW_SRQ_POST_TARGET pid=%ld hca=%d actual_srqn=%u published_srqn=%u handle=%p\n",
                (long)getpid(), hca_num, actual_srqn,
                mv2_MPIDI_CH3I_RDMA_Process.hollow_srqn[hca_num],
                (void *)mv2_MPIDI_CH3I_RDMA_Process.srq_hndl[hca_num]);
    }
#endif
    MPIDI_STATE_DECL(MPID_STATE_POST_SRQ_BUFFERS);
    MPIDI_FUNC_ENTER(MPID_STATE_POST_SRQ_BUFFERS);

    if (num_bufs > mv2_srq_fill_size)
    {
        ibv_va_error_abort(
            GEN_ASSERT_ERR,
            "Try to post %d to SRQ, max %d\n",
            num_bufs,
            mv2_srq_fill_size);
    }

    for (; i < num_bufs; ++i)
    {
        if ((v = get_vbuf_by_offset(MV2_RECV_VBUF_POOL_OFFSET)) == NULL)
        {
            break;
        }
        VBUF_INIT_RECV(
            v,
            VBUF_BUFFER_SIZE,
            hca_num * rdma_num_ports * rdma_num_qp_per_port);
            v->transport = IB_TRANSPORT_RC;

        if (ibv_post_srq_recv(mv2_MPIDI_CH3I_RDMA_Process.srq_hndl[hca_num], &v->desc.u.rr, &bad_wr))
        {
            release_vbuf(v);
            break;
        }
    }
    PRINT_DEBUG(DEBUG_SEND_verbose>1, "Posted %d buffers to SRQ\n",num_bufs);

    MPIDI_FUNC_EXIT(MPID_STATE_POST_SRQ_BUFFERS);
    return i;
}

#ifdef _ENABLE_UD_
#undef FUNCNAME
#define FUNCNAME mv2_post_ud_recv_buffers
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int mv2_post_ud_recv_buffers(int num_bufs, mv2_ud_ctx_t *ud_ctx)
{
    int i = 0,ret = 0;
    vbuf* v = NULL;
    struct ibv_recv_wr* bad_wr = NULL;
    int max_ud_bufs = (rdma_use_ud_srq)?mv2_ud_srq_fill_size:rdma_default_max_ud_recv_wqe;

    MPIDI_STATE_DECL(MPID_STATE_POST_RECV_BUFFERS);
    MPIDI_FUNC_ENTER(MPID_STATE_POST_RECV_BUFFERS);

    if (num_bufs > max_ud_bufs) {
        ibv_va_error_abort(
                GEN_ASSERT_ERR,
                "Try to post %d to UD recv buffers, max %d\n",
                num_bufs, max_ud_bufs);
    }
    for (i = 0; i < num_bufs; ++i) {
        v = get_ud_vbuf_by_offset(MV2_RECV_UD_VBUF_POOL_OFFSET);
        if (v == NULL) {
            break;
        }
        vbuf_init_ud_recv(v, rdma_default_ud_mtu, ud_ctx->hca_num);
        v->transport = IB_TRANSPORT_UD;
        if (ud_ctx->qp->srq) {
            ret = ibv_post_srq_recv(ud_ctx->qp->srq, &v->desc.u.rr, &bad_wr);
        } else {
           ret = ibv_post_recv(ud_ctx->qp, &v->desc.u.rr, &bad_wr);
        }
        if (ret)
        {
            release_vbuf(v);
            break;
        }
    }
    PRINT_DEBUG(DEBUG_UD_verbose>0 ,"Posted %d buffers of size:%d to UD QP on HCA %d\n",
                num_bufs, rdma_default_ud_mtu, ud_ctx->hca_num);

    MPIDI_FUNC_EXIT(MPID_STATE_POST_RECV_BUFFERS);
    return i;
}
#endif /*_ENABLE_UD_*/

#undef FUNCNAME
#define FUNCNAME MRAILI_Fill_start_buffer
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MRAILI_Fill_start_buffer(vbuf * v,
                             MPL_IOV * iov,
                             int n_iov)
{
    int i = 0;
    int avail = 0;
#ifdef _ENABLE_CUDA_
    if (mv2_enable_device) {
        avail = ((vbuf_pool_t*)v->pool_index)->buf_size - v->content_size;
    } else 
#endif
    {
        avail = VBUF_BUFFER_SIZE - v->content_size;
    }
    void *ptr = (v->buffer + v->content_size);
    int len = 0;
#ifdef _ENABLE_UD_
    if( rdma_enable_hybrid && v->transport == IB_TRANSPORT_UD) {
        avail = MRAIL_MAX_UD_SIZE - v->content_size;
    }
#endif

    MPIDI_STATE_DECL(MPID_STATE_MRAILI_FILL_START_BUFFER);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_FILL_START_BUFFER);

    DEBUG_PRINT("buffer: %p, content size: %d\n", v->buffer, v->content_size);

#ifdef _ENABLE_CUDA_
    if (mv2_enable_device && n_iov > 1 && is_device_buffer(iov[1].MPL_IOV_BUF)) {
        /* in the case of GPU buffers, there is only one data iov, if data is non-contiguous
         * it should have been packed before this */
        MPIU_Assert(n_iov == 2);

        MPIU_Memcpy(ptr, iov[0].MPL_IOV_BUF,
                (iov[0].MPL_IOV_LEN));
        len += (iov[0].MPL_IOV_LEN);
        avail -= (iov[0].MPL_IOV_LEN);
        ptr = (void *) ((unsigned long) ptr + iov[0].MPL_IOV_LEN);

        if (avail >= iov[1].MPL_IOV_LEN) {
            MPIU_Memcpy_Device(ptr,
                    iov[1].MPL_IOV_BUF,
                    iov[1].MPL_IOV_LEN,
                    deviceMemcpyDeviceToHost);
            len += iov[1].MPL_IOV_LEN;
        } else {
            MPIU_Memcpy_Device(ptr,
                    iov[1].MPL_IOV_BUF,
                    avail,
                    deviceMemcpyDeviceToHost);
            len += avail;
            avail = 0;
        }
    } else 
#endif
    {
        for (i = 0; i < n_iov; i++) {
            DEBUG_PRINT("[fill buf]avail %d, len %d\n", avail,
                    iov[i].MPL_IOV_LEN);
            if (avail >= iov[i].MPL_IOV_LEN) {
                DEBUG_PRINT("[fill buf] cpy ptr %p\n", ptr);
                MPIU_Memcpy(ptr, iov[i].MPL_IOV_BUF,
                        (iov[i].MPL_IOV_LEN));
                len += (iov[i].MPL_IOV_LEN);
                avail -= (iov[i].MPL_IOV_LEN);
                ptr = (void *) ((unsigned long) ptr + iov[i].MPL_IOV_LEN);
            } else {
                MPIU_Memcpy(ptr, iov[i].MPL_IOV_BUF, avail);
                len += avail;
                avail = 0;
                break;
            }
        }
    }
    v->content_size += len;

    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_FILL_START_BUFFER);
    return len;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAILI_rget_finish
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3I_MRAILI_rget_finish(MPIDI_VC_t * vc,
                                 MPL_IOV * iov,
                                 int n_iov,
                                 int *num_bytes_ptr, vbuf ** buf_handle, 
                                 int rail)
{
    vbuf *v;
    int mpi_errno;
    size_t nbytes = MAX(DEFAULT_MEDIUM_VBUF_SIZE, *num_bytes_ptr);

    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_MRAILI_RGET_FINISH);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_MRAILI_RGET_FINISH);

    if (likely(nbytes <= DEFAULT_MEDIUM_VBUF_SIZE)) {
        GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_MEDIUM_DATA_VBUF_POOL_OFFSET);
    } else {
        GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_LARGE_DATA_VBUF_POOL_OFFSET);
    }
    *buf_handle = v;
    *num_bytes_ptr = MRAILI_Fill_start_buffer(v, iov, n_iov);

    vbuf_init_send(v, *num_bytes_ptr, rail);

    mpi_errno = post_send(vc, v, rail);
    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_RGET_FINISH); 
    return mpi_errno;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_CH3I_MRAILI_rput_complete
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIDI_CH3I_MRAILI_rput_complete(MPIDI_VC_t * vc,
                                 MPL_IOV * iov,
                                 int n_iov,
                                 int *num_bytes_ptr, vbuf ** buf_handle, 
                                 int rail)
{
    vbuf * v;
    int mpi_errno;

    MPIDI_STATE_DECL(MPID_STATE_MPIDI_CH3I_MRAILI_RPUT_COMPLETE);
    MPIDI_FUNC_ENTER(MPID_STATE_MPIDI_CH3I_MRAILI_RPUT_COMPLETE);

    MRAILI_Get_buffer(vc, v, iov->MPL_IOV_LEN);
    *buf_handle = v;
    DEBUG_PRINT("[eager send]vbuf addr %p\n", v);
    *num_bytes_ptr = MRAILI_Fill_start_buffer(v, iov, n_iov);

    DEBUG_PRINT("[eager send] len %d, selected rail hca %d, rail %d\n",
                *num_bytes_ptr, vc->mrail.rails[rail].hca_index, rail);

    vbuf_init_send(v, *num_bytes_ptr, rail);

    mpi_errno = post_send(vc, v, rail);
    MPIDI_FUNC_EXIT(MPID_STATE_MPIDI_CH3I_MRAILI_RPUT_COMPLETE);
    return mpi_errno;
}

#undef FUNCNAME
#define FUNCNAME MRAILI_Backlog_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MRAILI_Backlog_send(MPIDI_VC_t * vc, int rail)
{
    char cq_overflow = 0;
    ibv_backlog_queue_t *q;

    MPIDI_STATE_DECL(MPID_STATE_MRAILI_BACKLOG_SEND);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_BACKLOG_SEND);

    q = &vc->mrail.srp.credits[rail].backlog;

#ifdef CKPT
    if (mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
        PRINT_ERROR("[%s, %d] CKPT has_srq error\n", __FILE__, __LINE__  );
        exit(EXIT_FAILURE);
    }
#endif

    while ((q->len > 0)
           && (vc->mrail.srp.credits[rail].remote_credit > 0)) {
        vbuf *v = NULL;
        MPIDI_CH3I_MRAILI_Pkt_comm_header *p;
        MPIU_Assert(q->vbuf_head != NULL);
        BACKLOG_DEQUEUE(q, v);

        /* Assumes packet header is at beginning of packet structure */
        p = (MPIDI_CH3I_MRAILI_Pkt_comm_header *) v->pheader;

        PACKET_SET_CREDIT(p, vc, rail);
#ifdef CRC_CHECK
	p->mrail.crc = update_crc(1, (void *)((uintptr_t)p+sizeof *p),
                                  v->desc.sg_entry.length - sizeof *p);
#endif
        --vc->mrail.srp.credits[rail].remote_credit;

        if (mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
#ifdef _ENABLE_UD_
		if(rdma_enable_hybrid) {
                p->src.rank    = MPIDI_Process.my_pg_rank;
		} else
#endif
        {
                p->src.vc_addr = vc->mrail.remote_vc_addr;
		}
            p->rail        = rail;
        }

     	v->vc = vc;
        v->rail = rail;

        XRC_FILL_SRQN_FIX_CONN (v, vc, rail);
        FLUSH_RAIL(vc, rail);
 
	CHECK_CQ_OVERFLOW(cq_overflow, vc, rail);

        if (likely(vc->mrail.rails[rail].send_wqes_avail > 0 && !cq_overflow)) {
            --vc->mrail.rails[rail].send_wqes_avail;

            IBV_POST_SR(v, vc, rail,
                        "ibv_post_sr (MRAILI_Backlog_send)");
        } else {
            MRAILI_Ext_sendq_enqueue(vc, rail, v);
            continue;
        }
    }

    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_BACKLOG_SEND);
    return 0;
}


#undef FUNCNAME
#define FUNCNAME MRAILI_Flush_wqe
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MRAILI_Flush_wqe(MPIDI_VC_t *vc, vbuf *v , int rail)
{
    MPIDI_STATE_DECL(MPID_STATE_MRAILI_FLUSH_WQE);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_FLUSH_WQE);
    FLUSH_RAIL(vc, rail);
    if (!vc->mrail.rails[rail].send_wqes_avail)
    {
        MRAILI_Ext_sendq_enqueue(vc, rail, v);
        MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_FLUSH_WQE);
        return MPI_MRAIL_MSG_QUEUED;
    }

    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_FLUSH_WQE);
    return 0;
}
#undef FUNCNAME
#define FUNCNAME MRAILI_Process_send
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MRAILI_Process_send(void *vbuf_addr)
{
    int mpi_errno = MPI_SUCCESS;

    vbuf            *v = vbuf_addr;
    MPIDI_CH3I_MRAILI_Pkt_comm_header *p;
    MPIDI_VC_t      *vc;
    MPIDI_VC_t      *orig_vc;
    MPID_Request    *req;
    int             complete;
#ifdef _ENABLE_HSAM_
    double time_taken;
#endif

    MPIDI_STATE_DECL(MPID_STATE_MRAILI_PROCESS_SEND);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_PROCESS_SEND);

    vc  = v->vc;
    p = v->pheader;
#ifdef _ENABLE_XRC_
    if (USE_XRC && VC_XST_ISSET (vc, XF_INDIRECT_CONN)) {
        orig_vc = vc->ch.orig_vc;
    }
    else 
#endif
    {
        orig_vc = vc;
    }
    if (v->transport == IB_TRANSPORT_RC) {
        if (v->padding == RDMA_ONE_SIDED) {
            ++(orig_vc->mrail.rails[v->rail].send_wqes_avail);
            if (orig_vc->mrail.rails[v->rail].ext_sendq_head) {
                MRAILI_Ext_sendq_send(orig_vc, v->rail);
            }

            if ((mpi_errno = MRAILI_Handle_one_sided_completions(v)) != MPI_SUCCESS)
            {
                MPIR_ERR_POP(mpi_errno);
            }

            MRAILI_Release_vbuf(v);
            goto fn_exit;
        }

    
        ++orig_vc->mrail.rails[v->rail].send_wqes_avail;


        if(vc->free_vc) {
            if(vc->mrail.rails[v->rail].send_wqes_avail == rdma_default_max_send_wqe)   {
                if (v->padding == NORMAL_VBUF_FLAG) {
                    DEBUG_PRINT("[process send] normal flag, free vbuf\n");
                    MRAILI_Release_vbuf(v);
                } else {
                    v->padding = FREE_FLAG;
                }

                MPIU_Memset(vc, 0, sizeof(MPIDI_VC_t));
                MPIU_Free(vc); 
                mpi_errno = MPI_SUCCESS;
                goto fn_exit;
            }
        }

        if(v->eager) {
            --vc->mrail.outstanding_eager_vbufs;
            DEBUG_PRINT("Eager, decrementing to: %d\n", vc->mrail.outstanding_eager_vbufs);

            if(vc->mrail.outstanding_eager_vbufs < 
                    rdma_coalesce_threshold) {
                DEBUG_PRINT("Flushing coalesced\n", v);
                FLUSH_SQUEUE(vc);
            }
            v->eager = 0;
        }
 
        if (orig_vc->mrail.rails[v->rail].ext_sendq_head) {
            MRAILI_Ext_sendq_send(orig_vc, v->rail);
        }

        if(v->padding == COLL_VBUF_FLAG) { 
#ifdef CHANNEL_MRAIL_GEN2
            shmem_info_t *shmem = (shmem_info_t *)v->zcpy_coll_shmem_info;
            /* Free slotted shmem regions/de-register memory here only if
             * 1) Process has posted sends in a zcopy bcast/reduce.
             * 2) The number of pending send_ops is zero.
             * 3) The shmem_info struct's free is supposed to be deferred.
             * This is to ensure that the buffers are freed only after all the
             * send operations are complete to avoid local protection errors in
             * the send operation */
            if (shmem != NULL && shmem->zcpy_coll_pending_send_ops > 0) {
                shmem->zcpy_coll_pending_send_ops--;
                if (shmem->defer_free == 1 && shmem->zcpy_coll_pending_send_ops == 0) {
                    mv2_shm_coll_cleanup(shmem);
                    MPIU_Free(shmem);
                    v->zcpy_coll_shmem_info = NULL;
                }
            }
#endif
            MRAILI_Release_vbuf(v);
            goto fn_exit;
        } 

        if (v->padding == RPUT_VBUF_FLAG) {

            req = (MPID_Request *)v->sreq;

            PRINT_DEBUG(DEBUG_RNDV_verbose, "Processing RPUT completion "
                    "req: %p, protocol: %d, local: %d, remote: %d\n",
                    req, req->mrail.protocol, req->mrail.local_complete, req->mrail.remote_complete);

#ifdef _ENABLE_HSAM_
            /* HSAM is Activated */
            if (mv2_MPIDI_CH3I_RDMA_Process.has_hsam) {
                req = (MPID_Request *)v->sreq;
                MPIU_Assert(req != NULL);
                get_wall_time(&time_taken);
                req->mrail.stripe_finish_time[v->rail] = 
                    time_taken;
            }
#endif /*_ENABLE_HSAM_*/
#ifdef _ENABLE_CUDA_
            if (mv2_enable_device
                && v->orig_vbuf != NULL) {
                vbuf *orig_vbuf = (vbuf *) (v->orig_vbuf);
                orig_vbuf->finish_count++;
                if (orig_vbuf->finish_count == rdma_num_rails) {
                    MRAILI_Release_vbuf(orig_vbuf);
                }
            }
#endif
            MRAILI_Release_vbuf(v);
            goto fn_exit;
        }
        if (v->padding == RGET_VBUF_FLAG) {

            req = (MPID_Request *)v->sreq;

#ifdef _ENABLE_HSAM_
            /* HSAM is Activated */
            if (mv2_MPIDI_CH3I_RDMA_Process.has_hsam) {
                MPIU_Assert(req != NULL);
                get_wall_time(&time_taken);
                /* Record the time only the first time a data transfer
                 * is scheduled on this rail
                 * this may occur for very large size messages */

                /* SS: The value in measuring time is a double.
                 * As long as it is below some epsilon value, it
                 * can be considered same as zero */
                if(req->mrail.stripe_finish_time[v->rail] < ERROR_EPSILON) {
                    req->mrail.stripe_finish_time[v->rail] = 
                        time_taken;
                }
            }
#endif /*_ENABLE_HSAM_*/
            ++req->mrail.local_complete;
            PRINT_DEBUG(DEBUG_RNDV_verbose, "Processing RGET completion "
                    "req: %p, protocol: %d, local: %d, remote: %d\n",
                    req, req->mrail.protocol, req->mrail.local_complete, req->mrail.remote_complete);

            /* If the message size if less than the striping threshold, send a
             * finish message immediately
             *
             * If HSAM is defined, wait for rdma_num_rails / stripe_factor
             * number of completions before sending the finish message.
             * After sending the finish message, adjust the weights of different
             * paths
             *
             * If HSAM is not defined, wait for rdma_num_rails completions
             * before sending the finish message
             */

            if(req->mrail.rndv_buf_sz > rdma_large_msg_rail_sharing_threshold) {
#ifdef _ENABLE_HSAM_
                if(mv2_MPIDI_CH3I_RDMA_Process.has_hsam && 
                        (req->mrail.local_complete == 
                         req->mrail.num_rdma_read_completions )) { 

                    MRAILI_RDMA_Get_finish(vc, 
                            (MPID_Request *) v->sreq, v->rail);

                    adjust_weights(v->vc, req->mrail.stripe_start_time,
                            req->mrail.stripe_finish_time, 
                            req->mrail.initial_weight);                       

                } else
#endif /*_ENABLE_HSAM_*/
                if (!mv2_MPIDI_CH3I_RDMA_Process.has_hsam && 
                        (req->mrail.local_complete == 
                         req->mrail.num_rdma_read_completions)) {

                    MRAILI_RDMA_Get_finish(vc,
                            (MPID_Request *) v->sreq, v->rail);
                }
            } else {
                MRAILI_RDMA_Get_finish(vc,
                        (MPID_Request *) v->sreq, v->rail);
            }

            MRAILI_Release_vbuf(v);
            goto fn_exit;
        }
        if (v->padding == CREDIT_VBUF_FLAG) {
            PRINT_DEBUG(DEBUG_XRC_verbose>0, "CREDIT Vbuf\n");
            --orig_vc->mrail.rails[v->rail].send_wqes_avail;
            goto fn_exit;
        }
    }
    
    switch (p->type) {
#ifdef CKPT
    case MPIDI_CH3_PKT_CM_SUSPEND:
    case MPIDI_CH3_PKT_CM_REACTIVATION_DONE:
        MPIDI_CH3I_CM_Handle_send_completion(vc, p->type,v);
        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        }
        break;
    case MPIDI_CH3_PKT_CR_REMOTE_UPDATE:
        MPIDI_CH3I_CR_Handle_send_completion(vc, p->type,v);
        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        }
        break;
#endif        
#ifndef MV2_DISABLE_HEADER_CACHING 
    case MPIDI_CH3_PKT_FAST_EAGER_SEND:
    case MPIDI_CH3_PKT_FAST_EAGER_SEND_WITH_REQ:
#endif
#if defined(USE_EAGER_SHORT)
    case MPIDI_CH3_PKT_EAGERSHORT_SEND:
#endif /* defined(USE_EAGER_SHORT) */
    case MPIDI_CH3_PKT_EAGER_SEND:
    case MPIDI_CH3_PKT_EAGER_SYNC_SEND: 
    case MPIDI_CH3_PKT_PACKETIZED_SEND_DATA:
    case MPIDI_CH3_PKT_RNDV_R3_DATA:
    case MPIDI_CH3_PKT_READY_SEND:
    case MPIDI_CH3_PKT_PUT:
    case MPIDI_CH3_PKT_PUT_IMMED:
    case MPIDI_CH3_PKT_ACCUMULATE:
    case MPIDI_CH3_PKT_ACCUMULATE_IMMED:
    case MPIDI_CH3_PKT_PUT_RNDV:
    case MPIDI_CH3_PKT_GET_RNDV:
    case MPIDI_CH3_PKT_GET_ACCUM:
    case MPIDI_CH3_PKT_GET_ACCUM_RESP_IMMED:
        req = v->sreq;
        v->sreq = NULL;
        DEBUG_PRINT("[process send] complete for eager msg, req %p\n",
                    req);
        if (req != NULL) {
            MPIDI_CH3U_Handle_send_req(vc, req, &complete);

            DEBUG_PRINT("[process send] req not null\n");
            if (complete != TRUE) {
                ibv_error_abort(IBV_STATUS_ERR, "Get incomplete eager send request\n");
            }
        }
        if (v->padding == NORMAL_VBUF_FLAG) {
            DEBUG_PRINT("[process send] normal flag, free vbuf\n");
            MRAILI_Release_vbuf(v);
        } else {
            v->padding = FREE_FLAG;
        }
        break;
    case MPIDI_CH3_PKT_RPUT_FINISH:
        req = (MPID_Request *) (v->sreq);
        if (req == NULL) {
            ibv_va_error_abort(GEN_EXIT_ERR,
                    "s == NULL, s is the send, v is %p "
                    "handler of the rput finish", v);
        }

#ifdef _ENABLE_CUDA_
        int process_rput_finish = 0;
        MPIDI_CH3_Pkt_rput_finish_t *rput_pkt =
                        (MPIDI_CH3_Pkt_rput_finish_t *) v->buffer;
        if (mv2_enable_device) {
            if (req->mrail.device_transfer_mode == NONE
                        || rput_pkt->device_pipeline_finish) {
                process_rput_finish = 1;
            }
        }
        if (!mv2_enable_device || process_rput_finish)
#endif
        {

        ++req->mrail.local_complete;
        if (req->mrail.local_complete == rdma_num_rails) {
            req->mrail.local_complete = UINT32_MAX;
        }
        PRINT_DEBUG(DEBUG_RNDV_verbose, "Processing RPUT FIN completion "
                "req: %p, protocol: %d, local: %d, remote: %d\n",
                req, req->mrail.protocol, req->mrail.local_complete, req->mrail.remote_complete);

        if(MPIDI_CH3I_MRAIL_Finish_request(req)) {

            if (req->mrail.d_entry != NULL) {
                dreg_unregister(req->mrail.d_entry);
                req->mrail.d_entry = NULL;
            }

#ifdef _ENABLE_HSAM_
            if(mv2_MPIDI_CH3I_RDMA_Process.has_hsam && 
               ((req->mrail.rndv_buf_sz > rdma_large_msg_rail_sharing_threshold))) {

                /* Adjust the weights of different paths according to the
                 * timings obtained for the stripes */

                adjust_weights(v->vc, req->mrail.stripe_start_time,
                        req->mrail.stripe_finish_time, 
                        req->mrail.initial_weight);
            }
#endif /*_ENABLE_HSAM_*/
            
            MPIDI_CH3I_MRAIL_FREE_RNDV_BUFFER(req);        
            req->mrail.d_entry = NULL;
            MPIDI_CH3U_Handle_send_req(vc, req, &complete);

            if (complete != TRUE) {
                ibv_error_abort(IBV_STATUS_ERR, 
                        "Get incomplete eager send request\n");
            }
        }
        }

        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        } else {
            v->padding = FREE_FLAG;
        }
        break;
    case MPIDI_CH3_PKT_GET_RESP:
    case MPIDI_CH3_PKT_GET_RESP_IMMED:
        DEBUG_PRINT("[process send] get get respond finish\n");
        req = (MPID_Request *) (v->sreq);
        v->sreq = NULL;
        if (NULL != req) {
            if (MV2_RNDV_PROTOCOL_RPUT == req->mrail.protocol) {
                if (req->mrail.d_entry != NULL) {
                    dreg_unregister(req->mrail.d_entry);
                    req->mrail.d_entry = NULL;
                }
                MPIDI_CH3I_MRAIL_FREE_RNDV_BUFFER(req);
                req->mrail.d_entry = NULL;
            }

            MPIDI_CH3U_Handle_send_req(vc, req, &complete);
            if (complete != TRUE) {
                ibv_error_abort(IBV_STATUS_ERR, "Get incomplete eager send request\n");
            }
        }

        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        } else {
            v->padding = FREE_FLAG;
        }
        break;

    case MPIDI_CH3_PKT_RGET_FINISH:

        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        } else {
            v->padding = FREE_FLAG;
        }

        break;
#if defined(_MCST_SUPPORT_)
    case MPIDI_CH3_PKT_MCST:
    case MPIDI_CH3_PKT_MCST_INIT:
        PRINT_DEBUG(DEBUG_MCST_verbose > 4, 
                "mcast send completion\n");
        mcast_ctx->ud_ctx->send_wqes_avail++;
        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        } else {
            v->padding = FREE_FLAG;
        }
        break;
    case MPIDI_CH3_PKT_MCST_NACK:
        if (mcast_use_mcast_nack) {
            mcast_ctx->ud_ctx->send_wqes_avail++;
        }
    case MPIDI_CH3_PKT_MCST_INIT_ACK:
        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        } else {
            v->padding = FREE_FLAG;
        }
        break;
    
#endif
    case MPIDI_CH3_PKT_NOOP:
    case MPIDI_CH3_PKT_ADDRESS:
    case MPIDI_CH3_PKT_ADDRESS_REPLY:
    case MPIDI_CH3_PKT_CM_ESTABLISH:
    case MPIDI_CH3_PKT_PACKETIZED_SEND_START:
    case MPIDI_CH3_PKT_RNDV_REQ_TO_SEND:
    case MPIDI_CH3_PKT_RNDV_READY_REQ_TO_SEND:
    case MPIDI_CH3_PKT_RNDV_CLR_TO_SEND:
    case MPIDI_CH3_PKT_EAGER_SYNC_ACK:
    case MPIDI_CH3_PKT_CANCEL_SEND_REQ:
    case MPIDI_CH3_PKT_CANCEL_SEND_RESP:
    case MPIDI_CH3_PKT_RMA_RNDV_CLR_TO_SEND:
    case MPIDI_CH3_PKT_CUDA_CTS_CONTI:
    case MPIDI_CH3_PKT_GET:
    case MPIDI_CH3_PKT_ACCUMULATE_RNDV:
    case MPIDI_CH3_PKT_LOCK:
    case MPIDI_CH3_PKT_LOCK_ACK:
    case MPIDI_CH3_PKT_LOCK_OP_ACK:
    case MPIDI_CH3_PKT_UNLOCK:
    case MPIDI_CH3_PKT_FLUSH:
    case MPIDI_CH3_PKT_ACK:
    case MPIDI_CH3_PKT_DECR_AT_COUNTER:
    case MPIDI_CH3_PKT_FOP:
    case MPIDI_CH3_PKT_FOP_RESP:
    case MPIDI_CH3_PKT_FOP_RESP_IMMED:
    case MPIDI_CH3_PKT_FOP_IMMED:
    case MPIDI_CH3_PKT_CAS_IMMED:
    case MPIDI_CH3_PKT_CAS_RESP_IMMED:
    case MPIDI_CH3_PKT_GET_ACCUM_RNDV:
    case MPIDI_CH3_PKT_GET_ACCUM_IMMED:
    case MPIDI_CH3_PKT_FLOW_CNTL_UPDATE:
    case MPIDI_CH3_PKT_RNDV_R3_ACK:
    case MPIDI_CH3_PKT_ZCOPY_FINISH:
    case MPIDI_CH3_PKT_ZCOPY_ACK:
        PRINT_DEBUG(DEBUG_SEND_verbose>1, "[process send] get %d\n", p->type);
        /* In MPIDI_CH3I_MRAILI_Eager_send, we save the request object in the
         * vbuf in case send operation could not be completed due to lack of
         * WQEs. In this scenario, we should call MPIDI_CH3U_Handle_send_req
         * upon sucessful completion of the send operation. This assert is to
         * make sure that we do not release a vbuf which needs additional
         * handling without doing it. */
        if (v->sreq != NULL) {
            PRINT_ERROR("Releasing vbuf for packet %s without handling sreq\n",
                        MPIDI_CH3_Pkt_type_to_string[p->type]);
        }
        MPIU_Assert(v->sreq == NULL);
        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        } else {
            v->padding = FREE_FLAG;
        }
        break;
   case MPIDI_CH3_PKT_GET_ACCUM_RESP:
        req = v->sreq;
        v->sreq = NULL;
        if (NULL != req) {
            MPIDI_CH3I_MRAILI_RREQ_RNDV_FINISH(req);

            MPIDI_CH3U_Handle_send_req(vc, req, &complete);
            if (complete != TRUE) {
                ibv_error_abort(IBV_STATUS_ERR, "Get incomplete eager send request\n");
            }
        }

        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        } else {
            v->padding = FREE_FLAG;
        }
        break;
   case MPIDI_CH3_PKT_CLOSE:  /*24*/
        DEBUG_PRINT("[process send] get %d\n", p->type);
        vc->pending_close_ops -= 1;
        if (vc->disconnect == 1 && vc->pending_close_ops == 0)
        {
            mpi_errno = MPIDI_CH3_Connection_terminate(vc);
            if(mpi_errno)
            {
              MPIR_ERR_POP(mpi_errno);
            }
        }

        if (v->padding == NORMAL_VBUF_FLAG) {
            MRAILI_Release_vbuf(v);
        }
        else {
            v->padding = FREE_FLAG;
        }
        break;
    default:
        dump_vbuf("unknown packet (send finished)", v);
        ibv_va_error_abort(IBV_STATUS_ERR,
                         "Unknown packet type %d in "
                         "MRAILI_Process_send MPIDI_CH3_PKT_FOP: %d", p->type, MPIDI_CH3_PKT_FOP);
    }
    DEBUG_PRINT("return from process send\n");

fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_PROCESS_SEND);
    return mpi_errno;

fn_fail:
    goto fn_exit;
}
#undef FUNCNAME
#define FUNCNAME MRAILI_Send_noop
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
void MRAILI_Send_noop(MPIDI_VC_t * c, int rail)
{
    /* always send a noop when it is needed even if there is a backlog.
     * noops do not consume credits.
     * this is necessary to avoid credit deadlock.
     * RNR NAK will protect us if receiver is low on buffers.
     * by doing this we can force a noop ahead of any other queued packets.
     */

    vbuf* v = get_vbuf_by_offset(MV2_RECV_VBUF_POOL_OFFSET);

    MPIDI_CH3I_MRAILI_Pkt_noop* p = (MPIDI_CH3I_MRAILI_Pkt_noop *) v->pheader;

    MPIDI_STATE_DECL(MPID_STATE_MRAILI_SEND_NOOP);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_SEND_NOOP);

    p->type = MPIDI_CH3_PKT_NOOP;
    vbuf_init_send(v, sizeof(MPIDI_CH3I_MRAILI_Pkt_noop), rail);
    post_send(c, v, rail);
    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_SEND_NOOP);
}

#undef FUNCNAME
#define FUNCNAME MRAILI_Send_noop_if_needed
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MRAILI_Send_noop_if_needed(MPIDI_VC_t * vc, int rail)
{
    MPIDI_STATE_DECL(MPID_STATE_MRAILI_SEND_NOOP_IF_NEEDED);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_SEND_NOOP_IF_NEEDED);

    if (mv2_MPIDI_CH3I_RDMA_Process.has_srq
     || vc->ch.state != MPIDI_CH3I_VC_STATE_IDLE)
	return MPI_SUCCESS;

    DEBUG_PRINT( "[ibv_send]local credit %d, rdma redit %d\n",
        vc->mrail.srp.credits[rail].local_credit,
        vc->mrail.rfp.rdma_credit);

    if (vc->mrail.srp.credits[rail].local_credit >=
        rdma_dynamic_credit_threshold
        || vc->mrail.rfp.rdma_credit > num_rdma_buffer / 2
        || (vc->mrail.srp.credits[rail].remote_cc <=
            rdma_credit_preserve
            && vc->mrail.srp.credits[rail].local_credit >=
            rdma_credit_notify_threshold)
        ) {
        MRAILI_Send_noop(vc, rail);
    } 
    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_SEND_NOOP_IF_NEEDED);
    return MPI_SUCCESS;
}

#undef FUNCNAME
#define FUNCNAME MRAILI_RDMA_Get
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
void MRAILI_RDMA_Get(   MPIDI_VC_t * vc, vbuf *v,
                        char * local_addr, uint32_t lkey,
                        char * remote_addr, uint32_t rkey,
                        int nbytes, int rail
                    )
{
    char cq_overflow = 0;

    MPIDI_STATE_DECL(MPID_STATE_MRAILI_RDMA_GET);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_RDMA_GET);

    DEBUG_PRINT("MRAILI_RDMA_Get: RDMA Read, "
            "remote addr %p, rkey %p, nbytes %d, hca %d\n",
            remote_addr, rkey, nbytes, vc->mrail.rails[rail].hca_index);

    vbuf_init_rget(v, (void *)local_addr, lkey,
                   remote_addr, rkey, nbytes, rail);
    
    v->vc = (void *)vc;

    XRC_FILL_SRQN_FIX_CONN (v, vc, rail);
    
    CHECK_CQ_OVERFLOW(cq_overflow, vc, rail);

    if (likely(vc->mrail.rails[rail].send_wqes_avail > 0 && !cq_overflow)) {
        --vc->mrail.rails[rail].send_wqes_avail;
        IBV_POST_SR(v, vc, rail, "MRAILI_RDMA_Get");
    } else {
        MRAILI_Ext_sendq_enqueue(vc,rail, v);
    }

    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_RDMA_GET);
    return;
}

#undef FUNCNAME
#define FUNCNAME MRAILI_RDMA_Put
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
void MRAILI_RDMA_Put(   MPIDI_VC_t * vc, vbuf *v,
                        char * local_addr, uint32_t lkey,
                        char * remote_addr, uint32_t rkey,
                        int nbytes, int rail
                    )
{
    char cq_overflow = 0;

    MPIDI_STATE_DECL(MPID_STATE_MRAILI_RDMA_PUT);
    MPIDI_FUNC_ENTER(MPID_STATE_MRAILI_RDMA_PUT);

    DEBUG_PRINT("MRAILI_RDMA_Put: RDMA write, "
            "remote addr %p, rkey %p, nbytes %d, hca %d\n",
            remote_addr, rkey, nbytes, vc->mrail.rails[rail].hca_index);

    vbuf_init_rput(v, (void *)local_addr, lkey,
                   remote_addr, rkey, nbytes, rail);
    
    v->vc = (void *)vc;
    XRC_FILL_SRQN_FIX_CONN (v, vc, rail);
 
    CHECK_CQ_OVERFLOW(cq_overflow, vc, rail);

    if (likely(vc->mrail.rails[rail].send_wqes_avail > 0 && !cq_overflow)) {
        --vc->mrail.rails[rail].send_wqes_avail;
        IBV_POST_SR(v, vc, rail, "MRAILI_RDMA_Put");
    } else {
        MRAILI_Ext_sendq_enqueue(vc,rail, v);
    }

    MPIDI_FUNC_EXIT(MPID_STATE_MRAILI_RDMA_PUT);
    return;
}


void vbuf_address_send(MPIDI_VC_t *vc)
{
    int rail, i;

    vbuf* v = NULL;
    GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_SMALL_DATA_VBUF_POOL_OFFSET);
    MPIDI_CH3_Pkt_address_t* p = (MPIDI_CH3_Pkt_address_t *) v->pheader;

    rail = MRAILI_Send_select_rail(vc, rdma_iba_eager_threshold-1);
    p->type = MPIDI_CH3_PKT_ADDRESS;
    p->rdma_address = (unsigned long)vc->mrail.rfp.RDMA_recv_buf_DMA;

    for (i = 0; i < rdma_num_hcas; i++) {    
	DEBUG_PRINT("mr %p\n", vc->mrail.rfp.RDMA_recv_buf_mr[i]);
	p->rdma_hndl[i]   = vc->mrail.rfp.RDMA_recv_buf_mr[i]->rkey;
    }
    vbuf_init_send(v, sizeof(MPIDI_CH3_Pkt_address_t), rail);
    post_send(vc, v, rail);
}

void vbuf_address_reply_send(MPIDI_VC_t *vc, uint8_t data)
{
    int rail;

    vbuf *v = NULL;
    GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_SMALL_DATA_VBUF_POOL_OFFSET);
    MPIDI_CH3_Pkt_address_reply_t *p = (MPIDI_CH3_Pkt_address_reply_t *) v->pheader;

    rail = MRAILI_Send_select_rail(vc, rdma_iba_eager_threshold-1);
    p->type = MPIDI_CH3_PKT_ADDRESS_REPLY;
    p->reply_data = data;
    
    vbuf_init_send(v, sizeof(MPIDI_CH3_Pkt_address_reply_t), rail);
    post_send(vc, v, rail);
}


int mv2_shm_coll_post_send(vbuf *v, int rail, MPIDI_VC_t * vc)
{ 
    char cq_overflow = 0;
    int mpi_errno = MPI_SUCCESS;

    v->rail = rail; 

    CHECK_CQ_OVERFLOW(cq_overflow, vc, rail);

    if (likely(vc->mrail.rails[rail].send_wqes_avail > 0 && !cq_overflow)) {
        --vc->mrail.rails[rail].send_wqes_avail;

        IBV_POST_SR(v, vc, rail, "ibv_post_sr (post_fast_rdma)");
        DEBUG_PRINT("[send:post rdma] desc posted\n");
    } else {
        DEBUG_PRINT("[send: rdma_send] Warning! no send wqe or send cq available\n");
        MRAILI_Ext_sendq_enqueue(vc, rail, v);
        mpi_errno = MPI_MRAIL_MSG_QUEUED;
    }

    return mpi_errno; 
}

void mv2_shm_coll_prepare_post_send(void *zcpy_coll_shmem_info, uint64_t local_rdma_addr, uint64_t remote_rdma_addr, 
                      uint32_t local_rdma_key, uint32_t remote_rdma_key, 
                      int len, int rail, MPIDI_VC_t * vc)
{
    vbuf *v=NULL;
    GET_VBUF_BY_OFFSET_WITHOUT_LOCK(v, MV2_SMALL_DATA_VBUF_POOL_OFFSET);
    v->desc.u.sr.next = NULL;
    v->desc.u.sr.opcode = IBV_WR_RDMA_WRITE;
    if (likely(len <= rdma_max_inline_size)) {
        v->desc.u.sr.send_flags = IBV_SEND_INLINE | IBV_SEND_SIGNALED;
    } else {
        v->desc.u.sr.send_flags = IBV_SEND_SIGNALED;
    }
    (v)->desc.u.sr.wr.rdma.remote_addr = (uintptr_t) (remote_rdma_addr);
    (v)->desc.u.sr.wr.rdma.rkey = (remote_rdma_key);
    v->desc.u.sr.wr_id = (uintptr_t) v;
    v->desc.u.sr.num_sge = 1;
    v->desc.u.sr.sg_list = &(v->desc.sg_entry);
    (v)->desc.sg_entry.length = len;

    (v)->desc.sg_entry.lkey = (local_rdma_key);
    (v)->desc.sg_entry.addr =  (uintptr_t) (local_rdma_addr);
    (v)->padding = COLL_VBUF_FLAG;
    (v)->vc   = vc;
    (v)->zcpy_coll_shmem_info = zcpy_coll_shmem_info;
    XRC_FILL_SRQN_FIX_CONN (v, vc, rail);
    mv2_shm_coll_post_send(v, rail, vc);

    return;
}

int mv2_shm_coll_reg_buffer(void *buffer, int size, struct ibv_mr *mem_handle[], 
                           int *buffer_registered)
{
   int i=0;
   int mpi_errno = MPI_SUCCESS;

    for ( i = 0 ; i < rdma_num_hcas; i ++ ) {
        mem_handle[i]  = (struct ibv_mr *) register_memory(buffer, size, i);

        if (!mem_handle[i]) {
            /* de-register already registered with other hcas*/
            for (i = i-1; i >=0 ; --i)
            {
                if (mem_handle[i] != NULL) {
                    deregister_memory(mem_handle[i]);
                }
            }
            *buffer_registered = 0;
        }
    }
    *buffer_registered = 1;

    return mpi_errno;
}

int mv2_shm_coll_dereg_buffer(struct ibv_mr *mem_handle[])
{ 
   int i=0, mpi_errno = MPI_SUCCESS;
   for ( i = 0 ; i < rdma_num_hcas; i ++ ) {
       if (mem_handle[i] != NULL) {
           if (deregister_memory(mem_handle[i])) { 
               ibv_error_abort(IBV_RETURN_ERR,
                                        "deregistration failed\n");
           }
           mem_handle[i] = NULL;
       }
   }
   return mpi_errno; 
}
