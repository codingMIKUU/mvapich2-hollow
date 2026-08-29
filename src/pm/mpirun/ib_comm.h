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

#ifndef __IB_COMM__
#define __IB_COMM__

#include <mpichconf.h>
#include "thread_pool.h"
#include "bitmap.h"

#include "atomic.h"
#include "list.h"

#include "ckpt_file.h"
#include "openhash.h"
#ifdef _ENABLE_IBV_DLOPEN_
#include <dlfcn.h>
#endif /*_ENABLE_IBV_DLOPEN_*/

#define USE_MPI                 // undef it if we dont need multiple client nodes
#undef USE_MPI

#define    PAGE_SIZE    (4096)

#define        MAX_QP_PER_CONNECTION    (16)

#define     BUF_SLOT_COUNT    (256)

#define MPIRUN_STRIGIFY(v_) #v_

#ifdef _ENABLE_IBV_DLOPEN_
#define MPIRUN_DLSYM(_struct_, _handle_, _prefix_, _function_)              \
do {                                                                        \
    _struct_._function_ = dlsym((_handle_),                                 \
                                MPIRUN_STRIGIFY(_prefix_##_##_function_));  \
    if (_struct_._function_ == NULL) {                                      \
        fprintf(stderr, "Error opening %s: %s. Falling back.\n",            \
                MPIRUN_STRIGIFY(_prefix_##_##_function_), dlerror());       \
    }                                                                       \
} while (0)
#else /* _ENABLE_IBV_DLOPEN_ */
#define MPIRUN_DLSYM(_struct_, _handle_, _prefix_, _function_)              \
do {                                                                        \
    _struct_._function_ = _prefix_##_##_function_;                          \
} while (0)
#endif /*_ENABLE_IBV_DLOPEN_*/

typedef enum dl_err_t {
    SUCCESS_DLOPEN = 0,
    ERROR_DLOPEN = -1,
    ERROR_DLSYM = -2
} dl_err_t;

/* Structure to abstract calls to verbs library */
typedef struct _mpirun_ibv_ops_t {
    /* Functions to manipulate list of devices */
    struct ibv_device** (*get_device_list)(int *num_devices);
    const char* (*get_device_name)(struct ibv_device *device);
    void (*free_device_list)(struct ibv_device **list);
    /* Functions to manipulate device context */
    struct ibv_context* (*open_device)(struct ibv_device *device);
    int (*close_device)(struct ibv_context *context);
    /* Functions to manipulate completion channel */
    struct ibv_comp_channel* (*create_comp_channel)(struct ibv_context *context);
    int (*destroy_comp_channel)(struct ibv_comp_channel *channel);
    /* Functions to manipulate PD */
    struct ibv_pd* (*alloc_pd)(struct ibv_context *context);
    int (*dealloc_pd)(struct ibv_pd *pd);
    /* Functions to manipulate CQ */
    struct ibv_cq* (*create_cq)(struct ibv_context *context, int cqe,
                                void *cq_context, struct ibv_comp_channel *channel,
                                int comp_vector);
    int (*resize_cq)(struct ibv_cq *cq, int cqe);
    int (*get_cq_event)(struct ibv_comp_channel *channel, struct ibv_cq **cq,
                        void **cq_context);
    void (*ack_cq_events)(struct ibv_cq *cq, unsigned int nevents);
    int (*destroy_cq)(struct ibv_cq *cq);
    /* Functions to manipulate QP */
    struct ibv_qp* (*create_qp)(struct ibv_pd *pd,
                                struct ibv_qp_init_attr *qp_init_attr);
    int (*query_qp)(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                    int attr_mask, struct ibv_qp_init_attr *init_attr);
    int (*modify_qp)(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                    int attr_mask);
    int (*destroy_qp)(struct ibv_qp *qp);
    /* Functions to manipulate SRQ */
    struct ibv_srq* (*create_srq)(struct ibv_pd *pd, struct
                               ibv_srq_init_attr *srq_init_attr);
    int (*query_srq)(struct ibv_srq *srq, struct ibv_srq_attr *srq_attr);
    int (*modify_srq)(struct ibv_srq *srq,
                    struct ibv_srq_attr *srq_attr,
                    int srq_attr_mask);
    int (*destroy_srq)(struct ibv_srq *srq);
    /* Functions to manipulate AH */
    struct ibv_ah* (*create_ah)(struct ibv_pd *pd, struct ibv_ah_attr *attr);
    int (*destroy_ah)(struct ibv_ah *ah);
    /* Functions to query hardware */
    int (*query_device)(struct ibv_context *context,
                     struct ibv_device_attr *device_attr);
    int (*query_port)(struct ibv_context *context, uint8_t port_num,
                      struct ibv_port_attr *port_attr);
    int (*query_gid)(struct ibv_context *context, uint8_t port_num,
                    int index, union ibv_gid *gid);
    int (*query_pkey)(struct ibv_context *context, uint8_t port_num,
                    int index, uint16_t *pkey);
    /* Functions to manipulate MR */
    struct ibv_mr* (*reg_mr)(struct ibv_pd *pd, void *addr,
                            size_t length, int access);
    int (*dereg_mr)(struct ibv_mr *mr);
    int (*fork_init)(void);
    /* Functions related to debugging events */
    const char* (*event_type_str)(enum ibv_event_type event_type);
    const char* (*node_type_str)(enum ibv_node_type node_type);
    const char* (*port_state_str)(enum ibv_port_state port_state);
    const char* (*wc_status_str)(enum ibv_wc_status status);
} mpirun_ibv_ops_t;

extern mpirun_ibv_ops_t mpirun_ibv_ops;
extern void *mpirun_ibv_dl_handle;

// endpoint of a QP
typedef struct qp_endpoint {
    int lid;                    // port's lid num
    int qpn;                    // qp num
    int psn;                    // psn of SQ in this qp
} qp_endpoint_t;

// a memory-region used by IB
typedef struct ib_buffer {
    pthread_mutex_t mutex;      //lock to protect the buf

    int num_slot;               // number of valid slots in the buf
    int free_slots;             // num of free slots

//  struct bitmap   bitmap; // bitmap of all slots in this buf

    //unsigned char state[BUF_SLOT_COUNT]; // state of each slot
    struct bitmap bitmap;       // bitmap for slots in this buf
    pthread_mutex_t lock[BUF_SLOT_COUNT];

    //this lock is to wake a thread waiting on send/recv at this slot

    sem_t buf_sem;              // if no free slots in this buf, wait on this sem

    struct ibv_mr *mr;          // used if this buf is ib-mr

    void *addr;                 //
    unsigned long size;         // total size of this buffer
    int slot_size;              // Each slot is of this size. i.e., each send()/recv() must <= this size

    char name[16];

} ib_buffer_t;

typedef struct qp_stat {
    int send;                   // num of ibv_send
    unsigned long send_size;    // total size of send

    int recv;                   // num of ibv_recv
    unsigned long recv_size;    // total size of recv

    int rdma_read;
    int rdma_write;

} qp_stat_t;

typedef struct ib_HCA {
    pthread_mutex_t mutex;
    int init;                   // 0: not inited, 1: has been initialized

    atomic_t ref;               // 
    // then iothreads fetch RR rqst from this queue

    struct ibv_context *context;
    struct ibv_comp_channel *comp_channel;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_srq *srq;        // for future use

    int max_rq_wqe;             // = num of recv-buf slots
    int rq_wqe;                 // num of remaining WQE on RQ(SRQ)
    int next_rbuf_slot;         // post srq WQE starting from this slot. 
    // next_rbuf_slot is mono-increasing, wrap-around by max_rq_wqe

    int comp_event_cnt;         // how many comp_channel event in comp_channel
    int total_cqe;              // how many CQE at CQ

    struct ib_buffer *send_buf;
    struct ib_buffer *recv_buf;
    struct ib_buffer *rdma_buf;

    int rx_depth;               // depth of CQ
    enum ibv_mtu mtu;
    int ib_port;

} ib_HCA_t;

/*a IB connection between 2 nodes.  Each side of a IB connection has this struct
(1 port, 1 CQ, multiple QP) in each side
*/
typedef struct ib_connection {
    ///////////////
/*    struct ibv_context  *context;
    struct ibv_comp_channel *comp_channel;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_srq      *srq;  // for future use
    
    struct ib_buffer    *send_buf;
    struct ib_buffer    *recv_buf;    
    struct ib_buffer    *rdma_buf;    */
    /////////////////

    struct ib_HCA *hca;         // a hca encaps all above elems
    unsigned int status;        // uninit -> active -> terminated

    struct ibv_qp *qp[MAX_QP_PER_CONNECTION];
    struct qp_stat qpstat[MAX_QP_PER_CONNECTION];   // statistics of each QP

    int num_qp;
    int next_use_qp;            // round-robin the qp[]. Next initiating msg goes to this qp

    /// some statistics about all QPs in this connection
    int send_cqe;
    int recv_cqe;
    int rr_cqe;
    /////////////////

    struct qp_endpoint my_ep[MAX_QP_PER_CONNECTION];
    struct qp_endpoint remote_ep[MAX_QP_PER_CONNECTION];

} ib_connection_t;

/////////////////////////////////////////

enum {
    SLOT_FREE = 0,
    SLOT_INUSE = 1,

    /// status of a connection
    connection_uninit = 0,
    connection_active = 1,
    connection_terminated = 2,

};

enum {
    qpidx_shift = 12,           // in send-wr: wr_id = qpidx | sbuf-id

};

/*
A request-pkt, will be sent by ibv_post_send, and recv's RQ will generate a CQE on this
So, it corresponds to a ibv_post_recv()
*/
typedef struct generic_pkt {
    unsigned char dummy[64];

} __attribute__ ((packed)) generic_pkt_t;

#define  arg_invalid  0xffffffffffffffff

/// define constants for ib_packet.command
enum {

    /// a generic request
    rqst_gen = 0x00000000,
    reply_gen = (1 << 31) | rqst_gen,

    /// client initiates RR by sending request
    rqst_RR = 0x00000001,
    reply_RR = (1 << 31) | rqst_RR,

    /// RDMA-write??

    /// terminate 
    rqst_terminate = 0x7fffffff,
    reply_terminate = 0xffffffff,
};

/* format of a pkt:

    u32 command
    {
        (request or reply) | cmd:
        cmd:  RDMA_READ        
    }

    RDMA_READ_request
    {
        u32    remote_proc_pid
        u64 remote_addr;
        u32 rkey;
        u32 size;
        u32 offset; // the data's offset in original     
    }

*/

///////////////////////////////////

extern int g_srv_tcpport;       // server listens on this tcp port for incoming connection requests
extern int g_num_qp;            // QP num in one connection
extern int g_rx_depth;          // RQ depth

// server listens on this tcp port for incoming connection requests
extern int numRR;               // num_qp * 4
extern int g_num_srv;

extern int sendbuf_size;        //4096; //8192;
extern int recvbuf_size;        //4096; //8192;
extern int rdmabuf_size;
extern long cli_rdmabuf_size;
extern long srv_rdmabuf_size;

extern int sendslot_size;
extern int recvslot_size;
extern int rdmaslot_size;

extern int g_iopool_size;

extern int g_exit;              // cli/srv shall exit??

//extern int g_ibtpool_size;
////////////////////////////////////

int ib_HCA_init(struct ib_HCA *hca, int is_srv);
void ib_HCA_destroy(struct ib_HCA *hca);
int get_HCA(struct ib_HCA *hca);
int put_HCA(struct ib_HCA *hca);

//int   ib_connection_init_1(struct ib_connection* conn, int num_qp, int rx_depth);
int ib_connection_init_1(struct ib_connection *conn, int num_qp, struct ib_HCA *hca);
int ib_connection_exchange_server(struct ib_connection *conn, int sock);
int ib_connection_exchange_client(struct ib_connection *conn, int sock);
int ib_connection_init_2(struct ib_connection *conn);

void ib_connection_release(struct ib_connection *conn);

//int   ib_connection_buf_init(struct ib_connection* conn, int sendsize, int recvsize, int rdmasize);
int ib_connection_buf_init(struct ib_HCA *hca, int sendsize, int recvsize, int rdmasize);

int ib_connection_post_send(struct ib_connection *conn, int qp_index, int sbuf_slot, int size);
int ib_connection_post_recv(struct ib_connection *conn, int qp_index, int rbuf_slot, int size);
int ib_connection_fillup_srq(struct ib_HCA *hca);
int ib_connection_send_RR_rqst(struct ib_connection *conn, int qpidx, int rbufid, int rprocid, int rckptid, int size, int offset, int is_last_chunk);
void ib_connection_send_terminate_rqst(struct ib_connection *connarray);

//int server_loop(struct ib_connection* conn );
int ib_server_loop(struct ib_connection *conn, struct thread_pool *tp);

int ib_client_loop(struct ib_connection *conn);

void pass_hash_table(hash_table_t * ht);

int ib_connection_post_RR(struct ib_connection *conn, int qpidx, ib_packet_t * rrpkt);
int ib_connection_send_chunk_RR_rqst(struct ib_connection *conn, ckpt_file_t * cfile, ckpt_chunk_t * chunk, void *arg);

/* Calls to create abstractions */
static inline int mv2_mpirun_ibv_dlopen_init()
{
#ifdef _ENABLE_IBV_DLOPEN_
    mpirun_ibv_dl_handle = dlopen("libibverbs.so", RTLD_NOW);
    if (!mpirun_ibv_dl_handle) {
        return ERROR_DLOPEN;
    }
#endif /*_ENABLE_IBV_DLOPEN_*/

    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, get_device_list);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, get_device_name);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, free_device_list);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, open_device);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, close_device);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, create_comp_channel);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, destroy_comp_channel);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, alloc_pd);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, dealloc_pd);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, create_cq);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, resize_cq);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, get_cq_event);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, ack_cq_events);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, destroy_cq);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, create_qp);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, query_qp);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, modify_qp);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, destroy_qp);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, create_srq);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, query_srq);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, modify_srq);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, destroy_srq);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, create_ah);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, destroy_ah);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, query_device);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, query_port);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, query_gid);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, query_pkey);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, reg_mr);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, dereg_mr);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, event_type_str);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, node_type_str);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, port_state_str);
    MPIRUN_DLSYM(mpirun_ibv_ops, mpirun_ibv_dl_handle, ibv, wc_status_str);

    return SUCCESS_DLOPEN;
}

/* Calls to finalize abstractions */
static inline void mv2_mpirun_ibv_dlopen_finalize()
{
#ifdef _ENABLE_IBV_DLOPEN_
    if (mpirun_ibv_dl_handle) {
        dlclose(mpirun_ibv_dl_handle);
    }
#endif /*_ENABLE_IBV_DLOPEN_*/
}

#endif                          // __IB_COMM__
