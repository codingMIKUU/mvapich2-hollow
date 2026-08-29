
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

#ifndef _IBV_CUDA_UTIL_H_
#define _IBV_CUDA_UTIL_H_

#if defined(_ENABLE_CUDA_)
#include "cuda.h"
#include "cuda_runtime.h"
#include "vbuf.h"

/* abstract device-specific type and map to CUDA definitions */
enum deviceMemcpyKind
{
    deviceMemcpyHostToHost = cudaMemcpyHostToHost,
    deviceMemcpyHostToDevice = cudaMemcpyHostToDevice,
    deviceMemcpyDeviceToHost = cudaMemcpyDeviceToHost,
    deviceMemcpyDeviceToDevice = cudaMemcpyDeviceToDevice,
    deviceMemcpyDefault = cudaMemcpyDefault
};
typedef CUcontext deviceContext;
typedef cudaEvent_t deviceEvent_t;
typedef cudaIpcMemHandle_t deviceIpcMemHandle_t;
typedef cudaIpcEventHandle_t deviceIpcEventHandle_t;
typedef cudaStream_t deviceStream_t;

/* generate flags used in device calls; use prefix 'cuda' */
#define MV2_DEVICE_FLAG(_flag) cuda##_flag

extern int cudaipc_init;

typedef enum cuda_async_op {
    SEND = 0,
    RECV,
    RGET,
    CUDAIPC_SEND,
    CUDAIPC_RECV,
    SMP_SEND,
    SMP_RECV,
} cuda_async_op_t;

/* cuda stream pool flags */
#define CUDA_STREAM_FREE_POOL 0x01
#define CUDA_STREAM_DEDICATED 0x02

typedef struct mv2_device_event {
    cudaEvent_t event;
    cuda_async_op_t op_type;
    uint8_t flags;
    uint8_t is_finish;
    uint8_t is_query_done;
    uint32_t size;
    uint32_t displacement;
    void *vc;
    void *req;
    struct vbuf *device_vbuf_head, *device_vbuf_tail;
    void *smp_ptr;
    struct mv2_device_event *next, *prev;
} mv2_device_event_t;
/* cuda event pool flags */
#define CUDA_EVENT_FREE_POOL 0x01
#define CUDA_EVENT_DEDICATED 0x02

void allocate_cuda_events();   /* allocate event pool */
void deallocate_cuda_events(); /* deallocate event pool */
int allocate_cuda_event(mv2_device_event_t **); /* allocate single event */
void deallocate_cuda_event(mv2_device_event_t **); /* deallocate single event */
void progress_cuda_events();
mv2_device_event_t *get_device_event();
mv2_device_event_t *get_free_cudaipc_event();
void release_cudaipc_event(mv2_device_event_t *event);
void release_cuda_event(mv2_device_event_t *);
extern mv2_device_event_t *free_cuda_event_list_head;
extern mv2_device_event_t *busy_cuda_event_list_head;
extern mv2_device_event_t *busy_cuda_event_list_tail;

typedef struct MPIDI_PG MPIDI_PG_t;
void cudaipc_allocate_shared_region (MPIDI_PG_t *pg, int num_processes, int my_rank);
void cudaipc_allocate_ipc_region    (MPIDI_PG_t *pg, int num_processes, int my_rank);
void cudaipc_share_device_info();
void allocate_cuda_rndv_streams();
void deallocate_cuda_rndv_streams();

extern cudaStream_t stream_d2h, stream_h2d, stream_kernel;
extern cudaEvent_t cuda_nbstream_sync_event;

#define CUDA_LIST_ADD(item, head, tail)         \
do {                                            \
    if (NULL == head) {                         \
        head = item;                            \
        tail = item;                            \
    } else {                                    \
        tail->next = item;                      \
        item->prev = tail;                      \
        tail = item;                            \
    }                                           \
} while(0)

#define MV2_DEVICE_PROGRESS()                     \
do {                                            \
    if (rdma_enable_cuda) {                     \
        progress_cuda_events();                 \
    }                                           \
} while(0)

#define MPIU_Malloc_Device(_buf, _size)           \
do {                                            \
    cudaError_t cuerr = cudaSuccess;            \
    cuerr = cudaMalloc((void **) &_buf,_size);  \
    if (cuerr != cudaSuccess) {                 \
        PRINT_INFO(1, "cudaMalloc failed with error:%d at %s:%d\n", \
            cuerr, __FILE__, __LINE__);         \
        exit(EXIT_FAILURE);                               \
    }                                           \
}while(0)

#define MPIU_Free_Device(_buf)                    \
do {                                            \
    cudaError_t cuerr = cudaSuccess;            \
    cuerr = cudaFree(_buf);                     \
    if (cuerr != cudaSuccess) {                 \
        PRINT_INFO(1, "cudaFree failed with error:%d at %s:%d\n",  \
                    cuerr, __FILE__, __LINE__); \
        exit(EXIT_FAILURE);                               \
    }                                           \
}while(0)

#define MPIU_Malloc_Device_Pinned_Host(_buf, _size)      \
do {                                            \
    cudaError_t cuerr = cudaSuccess;            \
    cuerr = cudaMallocHost((void **)&_buf,_size);\
    if (cuerr != cudaSuccess) {                 \
        PRINT_INFO(1, "cudaMallocHost failed with error:%d at %s:%d\n",  \
                cuerr, __FILE__, __LINE__);     \
        exit(EXIT_FAILURE);                               \
    }                                           \
}while(0)

#define MPIU_Free_Device_Pinned_Host(_buf)               \
do {                                            \
    cudaError_t cuerr = cudaSuccess;            \
    cuerr = cudaFreeHost(_buf);                 \
    if (cuerr != cudaSuccess) {                 \
        PRINT_INFO(1, "cudaFreeHost failed with error:%d at %s:%d\n",    \
                cuerr, __FILE__, __LINE__);     \
        exit(EXIT_FAILURE);                               \
    }                                           \
}while(0)

#define MPIU_Memcpy_Device_Async(_dst, _src, _size, _type, _stream)  \
do {                                                               \
    cudaError_t cuerr = cudaSuccess;                               \
    cuerr = cudaMemcpyAsync(_dst, _src, _size, _type, _stream);    \
    if (cuerr != cudaSuccess) {                                    \
        PRINT_INFO(1, "cudaMemcpyAsync failed with %d at %s:%d\n", \
                    cuerr, __FILE__,__LINE__);  \
        exit(EXIT_FAILURE);                                                  \
    }                                                              \
}while(0)

#define MPIU_Memcpy_Device(_dst, _src, _size, _type)      \
do {                                                    \
    cudaError_t cuerr = cudaSuccess;                    \
    cuerr = cudaMemcpy(_dst, _src, _size, _type);       \
    if (cuerr != cudaSuccess) {                         \
        PRINT_INFO(1, "cudaMemcpy failed with %d at %d\n", cuerr, __LINE__);  \
        exit(EXIT_FAILURE);                             \
    }                                                   \
}while(0)

#define CUDA_CHECK(stmt)                                \
do {                                                    \
    cudaError_t result = (stmt);                        \
    if (cudaSuccess != result) {                        \
        PRINT_ERROR("[%s:%d] cuda failed with %d \n",   \
         __FILE__, __LINE__,result);                    \
        exit(EXIT_FAILURE);                             \
    }                                                   \
    MPIU_Assert(cudaSuccess == result);                 \
} while (0)

#define CU_CHECK(stmt)                                  \
do {                                                    \
    CUresult result = (stmt);                           \
    if (CUDA_SUCCESS != result) {                       \
        PRINT_ERROR("[%s:%d] cuda failed with %d \n",   \
         __FILE__, __LINE__,result);                    \
        exit(EXIT_FAILURE);                             \
    }                                                   \
    MPIU_Assert(CUDA_SUCCESS == result);                \
} while (0)

#define MPIU_Device_CtxGetCurrent(_ctx)     \
do {                                        \
    CU_CHECK(cuCtxGetCurrent(_ctx));        \
} while (0)

#define MPIU_Device_EventCreate(_event)     \
do {                                        \
    CUDA_CHECK(cudaEventCreate(_event));    \
} while (0)

#define MPIU_Device_EventCreateWithFlags(_event, _flags)    \
do {                                                        \
    CUDA_CHECK(cudaEventCreateWithFlags(_event, _flags));  \
} while (0)

#define MPIU_Device_EventRecord(_event, _stream)    \
do {                                                \
    CUDA_CHECK(cudaEventRecord(_event, _stream));   \
} while (0)

#define MPIU_Device_EventSynchronize(_event)    \
do {                                            \
    CUDA_CHECK(cudaEventSynchronize(_event));   \
} while (0)

#define MPIU_Device_StreamWaitEvent(_stream, _event, _flag)     \
do {                                                            \
    CUDA_CHECK(cudaStreamWaitEvent(_stream, _event, _flag));    \
} while (0)

#define MPIU_Device_EventDestroy(_event)    \
do {                                        \
    CUDA_CHECK(cudaEventDestroy(_event));   \
} while (0)

void ibv_device_register(void * ptr, size_t size);
void ibv_device_unregister(void *ptr);
void DEVICE_COLL_Finalize ();
#if defined(HAVE_CUDA_IPC)
#define CUDAIPC_DEBUG 0

#define MPIU_Device_IpcGetMemHandle(_memhandle_out, _base)      \
do {                                                            \
    CUDA_CHECK(cudaIpcGetMemHandle(_memhandle_out, _base));     \
} while (0)

#define MPIU_Device_IpcOpenMemHandle(_base_out, _memhandle)             \
do {                                                                    \
    CUDA_CHECK(cudaIpcOpenMemHandle(_base_out, _memhandle,              \
                                    cudaIpcMemLazyEnablePeerAccess));   \
} while (0)

#define MPIU_Device_IpcOpenEventHandle(_event, _handle)     \
do {                                                        \
    CUDA_CHECK(cudaIpcOpenEventHandle(_event, _handle));    \
} while (0)

#define MPIU_Device_IpcGetEventHandle(_handle, _event)  \
do {                                                    \
    CUDA_CHECK(cudaIpcGetEventHandle(_handle, _event)); \
} while (0)

#define MPIU_Device_IpcCloseMemHandle(_base)    \
do {                                            \
    CUDA_CHECK(cudaIpcCloseMemHandle(_base));   \
} while (0)

#define DEVICE_IPC_RECV_IN_PROGRESS(c, s) {                     \
    MPIR_Request_add_ref(s);                                    \
    if (NULL == (c)->mrail.device_ipc_sreq_tail) {              \
        (c)->mrail.device_ipc_sreq_head = (void *)(s);          \
    } else {                                                    \
        ((MPID_Request *)                                       \
         (c)->mrail.device_ipc_sreq_tail)->mrail.next_inflow =  \
            (void *)(s);                                        \
    }                                                           \
    (c)->mrail.device_ipc_sreq_tail = (void *)(s);              \
    ((MPID_Request *)(s))->mrail.next_inflow = NULL;            \
}

#define DEVICE_IPC_RECV_DONE(c) {                               \
    MPID_Request *req = (c)->mrail.device_ipc_sreq_head;        \
    (c)->mrail.device_ipc_sreq_head =                           \
    ((MPID_Request *)                                           \
     (c)->mrail.device_ipc_sreq_head)->mrail.next_inflow;       \
        if (NULL == (c)->mrail.device_ipc_sreq_head) {          \
            (c)->mrail.device_ipc_sreq_tail = NULL;             \
        }                                                       \
    MPID_Request_release(req);                                  \
}

#define CUDAIPC_BUF_LOCAL_IDX(rank)  (cudaipc_num_stage_buffers * rank)
#define CUDAIPC_BUF_SHARED_IDX(i, j)      \
    ((i * cudaipc_num_stage_buffers * deviceipc_num_local_procs)  \
        + (cudaipc_num_stage_buffers * j))

typedef struct cuda_regcache_entry {
    uint8_t flags;
    void *remote_base;
    void *addr;
    size_t size;
    uint64_t cuda_memHandle[8];
    int refcount;
    int rank;
    struct cuda_regcache_entry *next;
    struct cuda_regcache_entry *prev;
} device_regcache_entry_t;

typedef struct cudaipc_local_info
{
    cudaEvent_t ipcEvent;
    void *buffer;
} cudaipc_local_info_t;

typedef cudaipc_local_info_t cudaipc_remote_info_t;

typedef struct cudaipc_shared_info
{
    volatile int sync_flag;
    cudaIpcEventHandle_t ipcEventHandle;
    cudaIpcMemHandle_t ipcMemHanlde;
} cudaipc_shared_info_t;

typedef volatile int cudaipc_device_id_t;

/* sync flag */
#define CUDAIPC_BUF_EMPTY 0
#define CUDAIPC_BUF_FULL 1

extern device_regcache_entry_t **cudaipc_cache_list;
extern int *num_cudaipc_cache_entries;
extern int deviceipc_num_local_procs;
void cuda_get_user_parameters();
extern int deviceipc_my_local_id;
void cudaipc_register(void *base_ptr, size_t size, int rank,  
        cudaIpcMemHandle_t memhandle, device_regcache_entry_t **cuda_reg);
void cudaipc_deregister(device_regcache_entry_t *reg);
void cudaipc_flush_regcache(int rank, int count);
void device_ipc_initialize_cache();
void cudaipc_shmem_cleanup();
void cudaipc_finalize();
extern cudaipc_shared_info_t *cudaipc_shared_data;
extern cudaipc_local_info_t *cudaipc_local_data;
extern cudaipc_remote_info_t *cudaipc_remote_data;
extern int cudaipc_num_stage_buffers;
extern int cudaipc_stage_buffer_size;
extern int mv2_device_use_ipc_stage_buffer;
extern size_t mv2_device_ipc_stage_buffer_limit;
extern int cudaipc_sync_limit;
#endif
#endif
#if defined(USE_GPU_KERNEL)
void pack_subarray( void *dst, void *src, int dim, int nx, int ny, int nz, int sub_nx, int sub_ny, int sub_nz, int h_x, int h_y, int h_z, int sub_order, int el_size, cudaStream_t stream);
void unpack_subarray( void *dst, void *src, int dim, int nx, int ny, int nz, int sub_nx, int sub_ny, int sub_nz, int h_x, int h_y, int h_z, int sub_order, int el_size, cudaStream_t stream);
void pack_unpack_vector_kernel( void *dst, int dpitch, void *src, int spitch, int width, int height, cudaStream_t stream);
#endif
#endif /* _IBV_CUDA_UTIL_H_ */


