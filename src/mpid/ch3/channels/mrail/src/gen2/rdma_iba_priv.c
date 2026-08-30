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
#include "mpichconf.h"
#include "upmi.h"
#include "vbuf.h"
#include "ibv_param.h"
#include "rdma_cm.h"
#include "mpiutil.h"
#include "cm.h"
#include "dreg.h"
#include "debug_utils.h"
#include "ibv_mcast.h"
#include "mv2_utils.h"

#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>   
#include "rdma_impl.h"
#include "ibv_param.h"
#include "hwloc_bind.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#undef DEBUG_PRINT
#ifdef DEBUG
#define DEBUG_PRINT(args...) \
do {                                                          \
    int rank;                                                 \
    UPMI_GET_RANK(&rank);                                      \
    fprintf(stderr, "[%d][%s:%d] ", rank, __FILE__, __LINE__);\
    fprintf(stderr, args);                                    \
} while (0)
#else
#define DEBUG_PRINT(args...)
#endif

extern int mv2_my_numa_id;
extern int g_atomics_support;

extern int MPIDI_Get_num_nodes();
extern int get_numa_bound_info(int *numa_bound, int *num_numas, int *num_cores_numa, int *is_uniform);

int mv2_ib_hca_socket_info[MAX_NUM_HCAS] = {-1};
int mv2_ib_hca_numa_info[MAX_NUM_HCAS] = {-1};
int mv2_selected_ib_hca_socket_info[MAX_NUM_HCAS] = {-1};
int mv2_selected_ib_hca_numa_info[MAX_NUM_HCAS] = {-1};
int mv2_num_ud_ah_created = 0;
int mv2_num_usable_hcas = 0;

#if defined(RDMA_CM)
ip_address_enabled_devices_t * ip_address_enabled_devices = NULL;
int num_ip_enabled_devices = 0;

/*
 * Iterate over available interfaces
 * and determine verbs capable ones and return their ip addresses and names.
 * Input : 1) num_interfaces: number of available devices 
 * Output: 1) Array of ip_address_enabled_devices_t which contains ip_address
 *         and device_name for each device 
 *         2) num_interfaces: `number of devices with ip addresses
 *  
 */
int mv2_get_verbs_ips_dev_names(int *num_interfaces, ip_address_enabled_devices_t * ip_address_enabled_devices)
{
    int mpi_errno = MPI_SUCCESS;
    int i = 0, max_ips = 0, ret = 0;
    char ip[INET_ADDRSTRLEN];
    char *dev_name = NULL;
    struct ifaddrs *ifaddr = NULL, *ifa;
    struct rdma_cm_id *cm_id = NULL;
    struct rdma_event_channel *ch = NULL;
    struct sockaddr_in *sin = NULL;
    struct ibv_port_attr port_attr;
    
    max_ips = *num_interfaces;
    ret = getifaddrs(&ifaddr);
    if (ret) {
        MPIR_ERR_SETANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                "getifaddrs error %d\n", errno);
    }

    for (ifa = ifaddr; ifa != NULL && i < max_ips; ifa = ifa->ifa_next) {
        PRINT_DEBUG(DEBUG_RDMACM_verbose,"Searching ip\n");
        if (ifa->ifa_addr != NULL
            && ifa->ifa_addr->sa_family == AF_INET
            && ifa->ifa_flags & IFF_UP
            && !(ifa->ifa_flags & IFF_LOOPBACK)
            && !(ifa->ifa_flags & IFF_POINTOPOINT)
        ) {
            ch =  rdma_ops.create_event_channel();
            if(!ch) {
                MPIR_ERR_SETANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                        "rdma_create_event_channel error %d\n", errno);
            }

            if (rdma_ops.create_id(ch, &cm_id, NULL, RDMA_PS_TCP)) {
                MPIR_ERR_SETANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                        "rdma_create_id error %d\n", errno);
            }

            PRINT_DEBUG(DEBUG_RDMACM_verbose,"ip: %s\n", ip);
            sin = (struct sockaddr_in *) ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));

            ret = rdma_ops.bind_addr(cm_id, ifa->ifa_addr);
            if (ret == 0 && cm_id->verbs != 0) {
                dev_name = (char *) ibv_ops.get_device_name(cm_id->verbs->device);
                /* Skip interfaces that are not in active state */
                if (dev_name && (!ibv_ops.query_port(cm_id->verbs, cm_id->port_num, &port_attr))
                    && port_attr.state == IBV_PORT_ACTIVE) {
                    strcpy(ip_address_enabled_devices[i].ip_address,ip);
                    strcpy(ip_address_enabled_devices[i].device_name,dev_name);
                    PRINT_DEBUG(DEBUG_RDMACM_verbose,"Active: dev_name: %s, port: %d, ip: %s\n",
                                dev_name, cm_id->port_num, ip);
                    i++;
                } else {
                    PRINT_DEBUG(DEBUG_RDMACM_verbose,"Not Active (%d): dev_name: %s, port: %d, ip: %s\n",
                                port_attr.state, dev_name, cm_id->port_num, ip);
                }
            }

            PRINT_DEBUG(DEBUG_RDMACM_verbose, "i: %d, interface: %s, device: %s, ip: %s, verbs: %d\n",
                                            i, ifa->ifa_name, dev_name, ip, !!cm_id->verbs);

            rdma_ops.destroy_id(cm_id); cm_id = NULL;
            rdma_ops.destroy_event_channel(ch); ch = NULL;
            dev_name = NULL;
        }
    }

    *num_interfaces = i;
fn_fail:
    freeifaddrs(ifaddr); ifaddr = NULL;

    return mpi_errno;
}
#endif /*defined(RDMA_CM)*/

int qp_required(MPIDI_VC_t * vc, int my_rank, int dst_rank)
{
    int qp_reqd = 1;

    if (g_atomics_support) {
        /* If we support atomics, we always need to create QP to self to
         * ensure correctness */
        qp_reqd = 1;
    } else if ((my_rank == dst_rank) || (
        !mv2_MPIDI_CH3I_RDMA_Process.force_ib_atomic 
        && rdma_use_smp && (vc->smp.local_rank != -1))) {
        /* Process is local */
        qp_reqd = 0;
    }

    return qp_reqd;
}

int power_two(int x)
{
    int pow = 1;

    while (x) {
        pow = pow * 2;
        x--;
    }

    return pow;
}

struct process_init_info *alloc_process_init_info(int pg_size, int rails)
{
    struct process_init_info *info;
    int i;

    info = MPIU_Malloc(sizeof *info);
    if (!info) {
        return NULL;
    }

    info->lid = (uint16_t **) MPIU_Malloc(pg_size * sizeof(uint16_t *));
    info->gid = (union ibv_gid **)
        MPIU_Malloc(pg_size * sizeof(union ibv_gid *));
    info->hostid = (int **) MPIU_Malloc(pg_size * sizeof(int *));
    info->qp_num_rdma = (uint32_t **)
        MPIU_Malloc(pg_size * sizeof(uint32_t *));
#ifdef _ENABLE_HOLLOW_RC_
    info->hollow_srqn = (uint32_t **)
        MPIU_Malloc(pg_size * sizeof(uint32_t *));
#endif
    info->arch_hca_type =
        (mv2_arch_hca_type *) MPIU_Malloc(pg_size * sizeof(mv2_arch_hca_type));
    info->vc_addr = (uint64_t *) MPIU_Malloc(pg_size * sizeof(uint64_t));
    if (!info->lid
        || !info->gid
        || !info->hostid
        || !info->qp_num_rdma
#ifdef _ENABLE_HOLLOW_RC_
        || !info->hollow_srqn
#endif
        || !info->arch_hca_type || !info->vc_addr) {
        return NULL;
    }

    for (i = 0; i < pg_size; ++i) {
        info->qp_num_rdma[i] = (uint32_t *)
            MPIU_Malloc(rails * sizeof(uint32_t));
#ifdef _ENABLE_HOLLOW_RC_
        info->hollow_srqn[i] = (uint32_t *)
            MPIU_Malloc(rails * sizeof(uint32_t));
#endif
        info->lid[i] = (uint16_t *) MPIU_Malloc(rails * sizeof(uint16_t));
        info->gid[i] = (union ibv_gid *)
            MPIU_Malloc(rails * sizeof(union ibv_gid));
        info->hostid[i] = (int *) MPIU_Malloc(rails * sizeof(int));
        if (!info->lid[i]
            || !info->gid[i]
            || !info->hostid[i]
            || !info->qp_num_rdma[i]
#ifdef _ENABLE_HOLLOW_RC_
            || !info->hollow_srqn[i]
#endif
            ) {
            return NULL;
        }
    }

    return info;
}

void free_process_init_info(struct process_init_info *info, int pg_size)
{
    int i;

    if (!info)
        return;

    for (i = 0; i < pg_size; ++i) {
        MPIU_Free(info->qp_num_rdma[i]);
#ifdef _ENABLE_HOLLOW_RC_
        MPIU_Free(info->hollow_srqn[i]);
#endif
        MPIU_Free(info->lid[i]);
        MPIU_Free(info->gid[i]);
        MPIU_Free(info->hostid[i]);
    }

    MPIU_Free(info->lid);
    MPIU_Free(info->gid);
    MPIU_Free(info->hostid);
    MPIU_Free(info->qp_num_rdma);
#ifdef _ENABLE_HOLLOW_RC_
    MPIU_Free(info->hollow_srqn);
#endif
    MPIU_Free(info->arch_hca_type);
    MPIU_Free(info->vc_addr);
    MPIU_Free(info);
}

#ifdef _ENABLE_HOLLOW_RC_
static int mv2_hollow_open_xrcd(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc,
                                int hca_num)
{
    struct ibv_xrcd_init_attr xrcd_attr;
    char path[128];

    if (proc->hollow_xrcd[hca_num])
        return 0;

    MPL_snprintf(path, sizeof(path), "/dev/shm/mv2_hollow_rc-%u-%d",
                 (unsigned)getuid(), hca_num);
    proc->hollow_xrcd_fd[hca_num] =
        open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (proc->hollow_xrcd_fd[hca_num] < 0)
        return errno ? errno : EIO;

    MPIU_Memset(&xrcd_attr, 0, sizeof(xrcd_attr));
    xrcd_attr.comp_mask = IBV_XRCD_INIT_ATTR_FD |
                          IBV_XRCD_INIT_ATTR_OFLAGS;
    xrcd_attr.fd = proc->hollow_xrcd_fd[hca_num];
    xrcd_attr.oflags = O_CREAT;
    proc->hollow_xrcd[hca_num] =
        ibv_open_xrcd(proc->nic_context[hca_num], &xrcd_attr);
    if (!proc->hollow_xrcd[hca_num]) {
        int saved_errno = errno ? errno : EIO;
        close(proc->hollow_xrcd_fd[hca_num]);
        proc->hollow_xrcd_fd[hca_num] = -1;
        return saved_errno;
    }

    return 0;
}
#endif

struct ibv_srq *create_srq(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc,
                           int hca_num)
{
    struct ibv_srq_init_attr srq_init_attr;
    struct ibv_srq *srq_ptr = NULL;

    MPIU_Memset(&srq_init_attr, 0, sizeof(srq_init_attr));

    srq_init_attr.srq_context = proc->nic_context[hca_num];
    srq_init_attr.attr.max_wr = mv2_srq_alloc_size;
    srq_init_attr.attr.max_sge = 1;
    /* The limit value should be ignored during SRQ create */
    srq_init_attr.attr.srq_limit = mv2_srq_limit;

#ifdef _ENABLE_HOLLOW_RC_
    {
        struct ibv_srq_init_attr_ex hollow_attr;
        int ret;

        ret = mv2_hollow_open_xrcd(proc, hca_num);
        if (ret) {
            errno = ret;
            ibv_error_abort(IBV_RETURN_ERR,
                            "Error opening Hollow RC XRCD\n");
        }

        MPIU_Memset(&hollow_attr, 0, sizeof(hollow_attr));
        hollow_attr.srq_context = proc->nic_context[hca_num];
        hollow_attr.attr = srq_init_attr.attr;
        hollow_attr.comp_mask = IBV_SRQ_INIT_ATTR_TYPE |
                                IBV_SRQ_INIT_ATTR_PD |
                                IBV_SRQ_INIT_ATTR_XRCD |
                                IBV_SRQ_INIT_ATTR_CQ;
        hollow_attr.srq_type = IBV_SRQT_XRC;
        hollow_attr.pd = proc->ptag[hca_num];
        hollow_attr.xrcd = proc->hollow_xrcd[hca_num];
        hollow_attr.cq = proc->cq_hndl[hca_num];
        srq_ptr = ibv_create_srq_ex(proc->nic_context[hca_num],
                                    &hollow_attr);
        if (srq_ptr && ibv_get_srq_num(srq_ptr,
                                      &proc->hollow_srqn[hca_num])) {
            ibv_ops.destroy_srq(srq_ptr);
            srq_ptr = NULL;
        }
    }
#elif defined(_ENABLE_XRC_)
    if (USE_XRC) {
        srq_ptr = ibv_ops.create_xrc_srq(proc->ptag[hca_num],
                                     proc->xrc_domain[hca_num],
                                     proc->cq_hndl[hca_num], &srq_init_attr);
        PRINT_DEBUG(DEBUG_XRC_verbose > 0, "created xrc srq %d\n",
                    srq_ptr->xrc_srq_num);
    } else
#endif /* transport-specific SRQ */
    {
        srq_ptr = ibv_ops.create_srq(proc->ptag[hca_num], &srq_init_attr);
    }

    if (!srq_ptr) {
        ibv_error_abort(IBV_RETURN_ERR, "Error creating SRQ\n");
    }

    return srq_ptr;
}

#ifdef _ENABLE_HOLLOW_RC_
static struct ibv_qp *mv2_create_hollow_qp(
    struct mv2_MPIDI_CH3I_RDMA_Process_t *proc, int hca_num,
    const struct ibv_qp_init_attr *legacy_attr)
{
    struct ibv_qp_init_attr_ex attr;

    MPIU_Memset(&attr, 0, sizeof(attr));
    attr.qp_context = legacy_attr->qp_context;
    attr.send_cq = legacy_attr->send_cq;
    attr.recv_cq = legacy_attr->recv_cq;
    attr.cap = legacy_attr->cap;
    attr.cap.max_recv_wr = 0;
    attr.cap.max_recv_sge = 0;
    attr.qp_type = IBV_QPT_RC;
    attr.sq_sig_all = legacy_attr->sq_sig_all;
    attr.comp_mask = IBV_QP_INIT_ATTR_PD;
    attr.pd = proc->ptag[hca_num];
    attr.sender_side = 1;
    attr.skip_kern_qp = 0;

    return ibv_create_qp_ex(proc->nic_context[hca_num], &attr);
}
#endif

#ifdef _ENABLE_UD_
struct ibv_srq *create_ud_srq(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc,
                           int hca_num)
{
    struct ibv_srq_init_attr srq_init_attr;
    struct ibv_srq *srq_ptr = NULL;

    MPIU_Memset(&srq_init_attr, 0, sizeof(srq_init_attr));

    srq_init_attr.srq_context = proc->nic_context[hca_num];
    srq_init_attr.attr.max_wr = mv2_ud_srq_alloc_size;
    srq_init_attr.attr.max_sge = 1;
    /* The limit value should be ignored during SRQ create */
    srq_init_attr.attr.srq_limit = mv2_ud_srq_limit;

    srq_ptr = ibv_ops.create_srq(proc->ptag[hca_num], &srq_init_attr);

    if (!srq_ptr) {
        ibv_error_abort(IBV_RETURN_ERR, "Error creating SRQ\n");
    }

    return srq_ptr;
}
#endif

static int check_attrs(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc,
                       struct ibv_port_attr *port_attr,
                       struct ibv_device_attr *dev_attr, int user_set)
{
    int ret = 0;
    char *value = NULL;
#ifdef _ENABLE_XRC_
    if (USE_XRC && !(dev_attr->device_cap_flags & IBV_DEVICE_XRC)) {
        fprintf(stderr, "HCA does not support XRC. Disable MV2_USE_XRC.\n");
        ret = 1;
    }
#endif /* _ENABLE_XRC_ */
    if ((port_attr->active_mtu < rdma_default_mtu)
#ifdef _ENABLE_UD_
        && !rdma_enable_only_ud
#endif
       ) {
        if ((value = getenv("MV2_DEFAULT_MTU")) == NULL) {
            /* Set default value for MTU */
            rdma_default_mtu = port_attr->active_mtu;
        } else {
            MPL_usage_printf(
                    "MV2_DEFAULT_MTU is set to %s, but maximum the HCA supports is %s.\n"
                    "Please reset MV2_DEFAULT_MTU to a value <= %s\n",
                    mv2_ibv_mtu_enum_to_string(rdma_default_mtu),
                    mv2_ibv_mtu_enum_to_string(port_attr->active_mtu),
                    mv2_ibv_mtu_enum_to_string(port_attr->active_mtu));
            ret = 1;
        }
    } else {
        if ((value = getenv("MV2_DEFAULT_MTU")) == NULL) {
            /* Set default value for MTU */
            rdma_default_mtu = port_attr->active_mtu;
        }
    }
#if defined(_ENABLE_UD_) || defined(_MCST_SUPPORT_)
    /* Set UD MTU */
    int max_mtu = mv2_ibv_mtu_enum_to_value(port_attr->active_mtu);
    if ((value = getenv("MV2_UD_MTU")) == NULL) {
        rdma_default_ud_mtu = max_mtu;
    } else {
        if (max_mtu < rdma_default_ud_mtu
#if defined(_ENABLE_UD_)
                && rdma_enable_hybrid
#endif /* #if defined(_ENABLE_UD_) */
           ) {
            MPL_usage_printf(
                    "MV2_UD_MTU is set to %d, but maximum the HCA supports is %d.\n"
                    "Please reset MV2_UD_MTU to a value <= %d\n", rdma_default_ud_mtu,
                    mv2_ibv_mtu_enum_to_value(port_attr->active_mtu),
                    mv2_ibv_mtu_enum_to_value(port_attr->active_mtu));
            ret = 1;
        }
    }
#endif /* #if defined(_ENABLE_UD_) || defined(_MCST_SUPPORT_) */

    if (dev_attr->max_qp_rd_atom < rdma_default_max_rdma_dst_ops) {
        if ((value = getenv("MV2_DEFAULT_MAX_RDMA_DST_OPS")) == NULL) {
            rdma_default_max_rdma_dst_ops = dev_attr->max_qp_rd_atom;
        } else {
            fprintf(stderr,
                    "MV2_DEFAULT_MAX_RDMA_DST_OPS is set to %d, but maximum the HCA supports is %d.\n"
                    "Please reset MV2_DEFAULT_MAX_RDMA_DST_OPS to a value <= %d\n",
                    rdma_default_max_rdma_dst_ops, dev_attr->max_qp_rd_atom, dev_attr->max_qp_rd_atom);
            ret = 1;
        }
    } else {
        if ((value = getenv("MV2_DEFAULT_MAX_RDMA_DST_OPS")) == NULL) {
#ifdef _ENABLE_XRC_
            /* XRC does not seem to support max_qp_rd_atom as reported by the
             * HCA. So, if we are using XRC, then fall back to using the
             * default value of 4.
             */
            if (!USE_XRC)
#endif
            {
                rdma_default_max_rdma_dst_ops = dev_attr->max_qp_rd_atom;
            }
        }
    }

    if (dev_attr->max_qp_rd_atom < rdma_default_qp_ous_rd_atom) {
        if ((value = getenv("MV2_DEFAULT_QP_OUS_RD_ATOM")) == NULL) {
            rdma_default_qp_ous_rd_atom = dev_attr->max_qp_rd_atom;
        } else {
            fprintf(stderr,
                    "MV2_DEFAULT_QP_OUS_RD_ATOM is set to %d, but maximum the HCA supports is %d.\n"
                    "Please reset MV2_DEFAULT_QP_OUS_RD_ATOM to a value <= %d\n",
                    rdma_default_qp_ous_rd_atom, dev_attr->max_qp_rd_atom, dev_attr->max_qp_rd_atom);
            ret = 1;
        }
    } else {
        if ((value = getenv("MV2_DEFAULT_QP_OUS_RD_ATOM")) == NULL) {
#ifdef _ENABLE_XRC_
            /* XRC does not seem to support max_qp_rd_atom as reported by the
             * HCA. So, if we are using XRC, then fall back to using the
             * default value of 4.
             */
            if (!USE_XRC)
#endif
            {
                rdma_default_qp_ous_rd_atom = dev_attr->max_qp_rd_atom;
            }
        }
    }

    if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity && 
            rdma_default_max_rdma_dst_ops < rdma_default_qp_ous_rd_atom) {
        /* on some heterogeneous configurations (different arch), we observed
         * that lack of this check leads to IBV_WC_REM_INV_REQ_ERR for bw and
         * bibw tests */

        PRINT_DEBUG(DEBUG_INIT_verbose, "Warning: in heterogeneous systems, "
                "max_rd_atomic should be less than max_dest_rd_atomic. "
                "qp's max_dest_rd_atomic is now set to max_rd_atomic \n");

       rdma_default_max_rdma_dst_ops = rdma_default_qp_ous_rd_atom; 
    }

    if (mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
        if (dev_attr->max_srq_sge < rdma_default_max_sg_list) {
            if ((value = getenv("MV2_DEFAULT_MAX_SG_LIST")) == NULL) {
                rdma_default_max_sg_list = dev_attr->max_srq_sge;
            } else {
                fprintf(stderr,
                        "MV2_DEFAULT_MAX_SG_LIST is set to %d, but maximum the HCA supports is %d.\n"
                        "Please reset MV2_DEFAULT_MAX_SG_LIST to a value <= %d\n",
                        rdma_default_max_sg_list, dev_attr->max_srq_sge, dev_attr->max_srq_sge);
                ret = 1;
            }
        }

        if (dev_attr->max_srq_wr < mv2_srq_alloc_size) {
            if ((value = getenv("MV2_SRQ_MAX_SIZE")) == NULL) {
                mv2_srq_alloc_size = dev_attr->max_srq_wr;
            } else {
                fprintf(stderr,
                        "MV2_SRQ_SIZE is set to %d, but maximum the HCA supports is %d.\n"
                        "Please reset MV2_SRQ_SIZE to a value <= %d\n",
                        (int) mv2_srq_alloc_size, dev_attr->max_srq_wr, dev_attr->max_srq_wr);
                ret = 1;
            }
        }
    } else {
        if (dev_attr->max_sge < rdma_default_max_sg_list) {
            if ((value = getenv("MV2_DEFAULT_MAX_SG_LIST")) == NULL) {
                rdma_default_max_sg_list = dev_attr->max_sge;
            } else {
                fprintf(stderr,
                        "Max MV2_DEFAULT_MAX_SG_LIST is %d, set to %d\n",
                        dev_attr->max_sge, rdma_default_max_sg_list);
                ret = 1;
            }
        }

        if (dev_attr->max_qp_wr < rdma_default_max_send_wqe) {
            if ((value = getenv("MV2_DEFAULT_MAX_SEND_WQE")) == NULL) {
                rdma_default_max_send_wqe = dev_attr->max_qp_wr;
            } else {
                fprintf(stderr,
                        "MV2_DEFAULT_MAX_SEND_WQE is set to %d, but maximum the HCA supports is %d.\n"
                        "Please reset MV2_DEFAULT_MAX_SEND_WQE to a value <= %d\n",
                        (int) rdma_default_max_send_wqe, dev_attr->max_qp_wr, dev_attr->max_qp_wr);
                ret = 1;
            }
        }
    }
#if defined(_ENABLE_UD_)
    if (rdma_use_ud_srq) {
        if (dev_attr->max_srq_wr < mv2_ud_srq_alloc_size) {
            if ((value = getenv("MV2_UD_SRQ_MAX_SIZE")) == NULL) {
                mv2_ud_srq_alloc_size = dev_attr->max_srq_wr;
            } else {
                fprintf(stderr,
                        "MV2_UD_SRQ_MAX_SIZE is set to %d, but maximum the HCA supports is %d.\n"
                        "Please reset MV2_UD_SRQ_MAX_SIZE to a value <= %d\n",
                        (int) mv2_ud_srq_alloc_size, dev_attr->max_srq_wr, dev_attr->max_srq_wr);
                ret = 1;
            }
        }
    }
#endif /* #if defined(_ENABLE_UD_) */
    if (dev_attr->max_cqe < rdma_default_max_cq_size) {
        if ((value = getenv("MV2_DEFAULT_MAX_CQ_SIZE")) == NULL) {
            rdma_default_max_cq_size = dev_attr->max_cqe;
        } else {
            fprintf(stderr,
                    "MV2_DEFAULT_MAX_CQ_SIZE is set to %d, but maximum the HCA supports is %d.\n"
                    "Please reset MV2_DEFAULT_MAX_CQ_SIZE to a value <= %d\n",
                    (int) rdma_default_max_cq_size, dev_attr->max_cqe, dev_attr->max_cqe);
            ret = 1;
        }
    }

    return ret;
}

/*
 * Function: rdma_find_active_port
 *
 * Description:
 *      Finds if the given device has any active ports.
 *
 * Input:
 *      context -   Pointer to the device context obtained by opening device.
 *      ib_dev  -   Pointer to the device from ibv_get_device_list.
 *
 * Return:
 *      Success:    Port number of the active port.
 *      Failure:    ERROR (-1).
 */
int rdma_find_active_port(struct ibv_context *context,
                          struct ibv_device *ib_dev,
                          int *hca_rate, uint8_t *link_type, uint16_t *sm_lid)
{
    int j = 0;
    struct ibv_port_attr port_attr;

    for (j = 1; j <= RDMA_DEFAULT_MAX_PORTS; ++j) {
        if ((!ibv_ops.query_port(context, j, &port_attr)) && port_attr.state == IBV_PORT_ACTIVE) {
            /* port_attr.lid && !use_iboeth -> This is an IB device as it has
             * LID and user has not specified to use RoCE mode.
             * !port_attr.lid && use_iboeth -> This is a RoCE device as it does
             * not have a LID and uer has specified to use RoCE mode.
             */
            if (link_type) {
                *link_type = port_attr.link_layer;
            }
            if (sm_lid) {
                *sm_lid = port_attr.sm_lid;
            }
            if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
                mv2_system_has_roce = 1;
            } else if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
                mv2_system_has_ib = 1;
            }
            if (hca_rate) {
                *hca_rate = (int)(get_link_width(port_attr.active_width) *
                                  get_link_speed(port_attr.active_speed));
            }
            return j;
        }
    }

    return ERROR;
}

/*
 * Function: rdma_find_network_type
 *
 * Description:
 *      On a multi-rail network, retrieve the network to use
 *
 * Input:
 *      dev_list        -  List of HCAs on the system.
 *      num_devices     -  Number of HCAs on the system.
 *      num_usable_hcas -  Number of usable HCAs on the system.
 *
 * Return:
 *      Success:    MPI_SUCCESS.
 *      Failure:    ERROR (-1).
 */
#undef FUNCNAME
#define FUNCNAME rdma_find_network_type
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int rdma_find_network_type(struct ibv_device **dev_list, int num_devices,
                           struct ibv_device **usable_dev_list,
                           struct ibv_device **usable_devs_on_my_numa,
                           int *num_usable_hcas, int *num_usable_hcas_on_my_numa,
                           uint8_t *all_link_type)
{
    char *value = NULL;
    int i = 0, k = 0, p = 0;
    /* j must be initialized to 1 since we always store index fastest HCA in
     * usable_dev_list[0] */
    int j = 1;
    int hca_type = 0;
    int hca_rate = 0;
    int num_hcas = 0;
    int my_numa = mv2_my_numa_id;
    int user_selected = 0;
    int num_hcas_on_my_numa = 0;
    int fastest_hca_type = MV2_HCA_UNKWN;
    int fastest_hca_rate = 0;
    uint8_t *all_hca_link_type;
    uint16_t sm_lid = 0;
    uint16_t max_hcas_in_sm_lid = 0;
    uint16_t num_unique_sm_lids = 0;
    uint8_t link_type = IBV_LINK_LAYER_UNSPECIFIED;
    uint8_t fastest_hca_link_type = IBV_LINK_LAYER_UNSPECIFIED;
    int fastest_network_type = MV2_NETWORK_CLASS_UNKNOWN;
    int network_type = MV2_NETWORK_CLASS_UNKNOWN;
    int *all_hca_rate = MPIU_Malloc(sizeof(int)*num_devices);
    uint16_t *all_hca_sm_lid = MPIU_Malloc(sizeof(uint16_t)*num_devices);
    uint16_t *unique_hca_sm_lid = MPIU_Malloc(sizeof(uint16_t)*num_devices);
    uint16_t *num_hcas_in_sm = MPIU_Malloc(sizeof(uint16_t)*num_devices);
    struct ibv_context **nic_context = MPIU_Malloc(sizeof(struct ibv_context*)*num_devices);

    if (all_link_type == NULL) {
        all_hca_link_type = MPIU_Malloc(sizeof(uint8_t) * num_devices);
    } else {
        all_hca_link_type = all_link_type;
    }

    if ((value = getenv("MV2_ALLOW_HETEROGENEOUS_HCA_SELECTION")) != NULL) {
        mv2_allow_heterogeneous_hca_selection = !!atoi(value);
    }

    if (my_numa == -1) {
        int is_uniform = 0;
        int num_numas = 0;
        int num_cores_numa = 0;
        get_numa_bound_info(&my_numa, &num_numas, &num_cores_numa, &is_uniform);
        if (my_numa == -1) {
            /* get_numa_bound_info failed to query hwloc */
            my_numa = 0;
            /* Disable process placement aware hca mapping if we are unable to
             * determine our numa */
            if (mv2_process_placement_aware_hca_mapping) {
                PRINT_ERROR("Unable to find the numa process is bound to."
                            " Disabling process placement aware hca mapping.\n");
                mv2_process_placement_aware_hca_mapping = 0;
            }
        }
    }

    for (i = 0; i < num_devices; ++i) {
        nic_context[i] = ibv_ops.open_device(dev_list[i]);
        if (!nic_context[i]) {
            if (dev_list[i]) {
                ibv_ops.free_device_list(dev_list);
            }
            PRINT_ERROR("Failed to open the HCA context\n");
            return 0;
        }
#if defined(_IBV_DEVICE_ATTR_EX_HAS_MAX_DM_SIZE_)
        /* TODO: This is hack to avoid using the SAN HCAs on DGX2 A100 systems. */
        if (num_devices == MV2_DGX2_A100_NUM_HCAS) {
            int ret = 0;
            struct verbs_context *vctx = NULL;
            struct ibv_device_attr_ex dev_attr = {};

            vctx = verbs_get_ctx_op(nic_context[i], query_device_ex);
            if (vctx) {
                ret = vctx->query_device_ex(nic_context[i], NULL, &dev_attr,
                                            sizeof(struct ibv_device_attr_ex));
                if (!ret) {
                    if (dev_attr.max_dm_size == MV2_DGX2_A100_SAN_HCA_MAX_DM_SIZE) {
                        PRINT_DEBUG(DEBUG_INIT_verbose,"Skipping HCA %s for SAN on DGX node at ALCF\n",
                                    dev_list[i]->name);
                        continue;
                    }
                }
            }
        }
#endif /*defined(_IBV_DEVICE_ATTR_EX_HAS_MAX_DM_SIZE_)*/
        if (ERROR == rdma_find_active_port(nic_context[i], NULL, &hca_rate, &link_type, &sm_lid)) {
            /* No active port, skip HCA */
            ibv_ops.close_device(nic_context[i]);
            nic_context[i] = NULL;
            PRINT_DEBUG(DEBUG_INIT_verbose,"Skipping HCA %s since it does not have an active port\n",
                        dev_list[i]->name);
            continue;
        }
        /* Store rate for HCA */
        all_hca_rate[i] = hca_rate;
        /* Store link type for HCA */
        all_hca_link_type[i] = link_type;
        /* Store SM Lid for HCA */
        all_hca_sm_lid[i] = sm_lid;
        /* Get type for HCA */
        hca_type = mv2_get_hca_type(dev_list[i]);
        /* Find the number of unique subnets */
        if (num_unique_sm_lids == 0) {
            unique_hca_sm_lid[num_unique_sm_lids] = sm_lid;
            num_hcas_in_sm[num_unique_sm_lids] = max_hcas_in_sm_lid = 1;
            num_unique_sm_lids++;
            PRINT_DEBUG(DEBUG_INIT_verbose,
                        "Found HCA %s with unique SM LID %u: num_unique_sm_lids = %u, num_hcas_in_sm[%d] = %u\n",
                        dev_list[i]->name, sm_lid, num_unique_sm_lids, num_unique_sm_lids-1,
                        num_hcas_in_sm[num_unique_sm_lids-1]);
        } else {
            for (p = 0; p < num_unique_sm_lids; ++p) {
                /* Find number of HCAs in subnet sm_lid */
                if (unique_hca_sm_lid[p] == sm_lid) {
                    num_hcas_in_sm[p]++;
                    PRINT_DEBUG(DEBUG_INIT_verbose,
                                "Found HCA %s with SM LID %u. Total HCAs in SM = %u\n",
                                dev_list[i]->name, sm_lid, num_hcas_in_sm[p]);
                    /* Update max_hcas_in_sm_lid */
                    if (max_hcas_in_sm_lid < num_hcas_in_sm[p]) {
                        max_hcas_in_sm_lid = num_hcas_in_sm[p];
                        PRINT_DEBUG(DEBUG_INIT_verbose,
                                    "Updating MAX HCAS: sm_lid '%u' has the max num of hcas, max_hcas_in_sm_lid = %u\n",
                                    sm_lid, max_hcas_in_sm_lid);
                    } else if (max_hcas_in_sm_lid == num_hcas_in_sm[p]) {
                        PRINT_DEBUG(DEBUG_INIT_verbose,
                                    "sm_lid '%u' has the max num of hcas, max_hcas_in_sm_lid = %u\n",
                                    sm_lid, max_hcas_in_sm_lid);
                    }
                    break;
                }
            }
            /* We found another unique SM LID */
            if (p == num_unique_sm_lids) {
                num_hcas_in_sm[num_unique_sm_lids] = 1;
                unique_hca_sm_lid[num_unique_sm_lids] = sm_lid;
                num_unique_sm_lids++;
                PRINT_DEBUG(DEBUG_INIT_verbose,
                        "Found HCA %s with unique SM LID %u: num_unique_sm_lids = %u, num_hcas_in_sm[%u] = %u\n",
                        dev_list[i]->name, sm_lid, num_unique_sm_lids, num_unique_sm_lids-1,
                        num_hcas_in_sm[num_unique_sm_lids-1]);
            }
        }
        /* HCA types have been defined in increasing order of speeds. If we have
         * a faster HCA OR if we have the same HCA type, and the current HCAs
         * link type is less than fastest_hca_link_type, use this a fastest HCA.
         * IBV_LINK_LAYER_INFINIBAND = 1, IBV_LINK_LAYER_ETHERNET = 2 (verbs.h)
         */
        if ((fastest_hca_type < hca_type) ||
            ((fastest_hca_type == hca_type) && (link_type < fastest_hca_link_type))) {
            /* Get the HCA type for the fastest HCA */
            fastest_hca_type = hca_type;
            /* Get the network type for the fastest HCA */
            fastest_network_type = MV2_GET_NETWORK_TYPE(fastest_hca_type);
            /* Store the fastest HCA in usable_dev_list[0] */
            usable_dev_list[0] = dev_list[i];
            /* Get the rate for the fastest HCA */
            fastest_hca_rate = hca_rate;
            /* Get the link type for the fastest HCA */
            fastest_hca_link_type = link_type;
        }
        PRINT_DEBUG(DEBUG_INIT_verbose>1, "HCA %s type = %s SM LID = %u.\n",
                    dev_list[i]->name, mv2_get_hca_name(hca_type), sm_lid);
    }
    /* Count fastest HCA type */
    num_hcas++;
    PRINT_DEBUG(DEBUG_INIT_verbose>1, "Fastest Usable HCA %d = %s. Type = %s. Rate = %d\n",
                0, usable_dev_list[0]->name, mv2_get_hca_name(fastest_hca_type), fastest_hca_rate);
    for (i = 0; i < num_devices; ++i) {
        if (nic_context[i] == NULL) {
            continue;
        }
        /* Skip HCA if rate is less than fastest rate and user has not allowed
         * heterogeneous HCAs and user has not specified HCA. */
        if (((all_hca_rate[i] < fastest_hca_rate) ||
            (all_hca_link_type[i] != fastest_hca_link_type)) &&
            !mv2_allow_heterogeneous_hca_selection) {
            /* Check if the user specified to use this HCA */
            user_selected = 0;
            if (strncmp(rdma_iba_hcas[0], RDMA_IBA_NULL_HCA, 32)) {
                for (p = 0; p < rdma_num_req_hcas; p++) {
                    if (!strncmp(rdma_iba_hcas[p], dev_list[i]->name, 32)) {
                        /* User has selected this HCA */
                        user_selected = 1;
                        break;
                    }
                }
            }
            if (!user_selected) {
                if (all_hca_rate[i] < fastest_hca_rate) {
                    PRINT_DEBUG(DEBUG_INIT_verbose>1, "Skipping HCA %s since rate (%d) < fastest_hca_rate (%d)\n",
                                dev_list[i]->name, all_hca_rate[i], fastest_hca_rate);
                } else if (all_hca_link_type[i] != fastest_hca_link_type) {
                    PRINT_DEBUG(DEBUG_INIT_verbose>1, "Skipping HCA %s since HCA link type (%s) != fastest_hca_link_type (%s)\n",
                                dev_list[i]->name,
                                (all_hca_link_type[i] == IBV_LINK_LAYER_ETHERNET)?
                                "IBV_LINK_LAYER_ETHERNET":"IBV_LINK_LAYER_INFINIBAND",
                                (fastest_hca_link_type == IBV_LINK_LAYER_ETHERNET)?
                                "IBV_LINK_LAYER_ETHERNET":"IBV_LINK_LAYER_INFINIBAND");
                }
                continue;
            }
        }
        for (k = 0; k < num_unique_sm_lids; ++k) {
            if (unique_hca_sm_lid[k] == all_hca_sm_lid[i]) {
                break;
            }
        }
        if ((num_hcas_in_sm[k] < max_hcas_in_sm_lid) &&
            !mv2_allow_heterogeneous_hca_selection) {
            /* Check if the user specified to use this HCA */
            user_selected = 0;
            /* User has selected some HCA */
            if (strncmp(rdma_iba_hcas[0], RDMA_IBA_NULL_HCA, 32)) {
                for (p = 0; p < rdma_num_req_hcas; p++) {
                    if (!strncmp(rdma_iba_hcas[p], dev_list[i]->name, 32)) {
                        user_selected = 1;
                        break;
                    }
                }
            }
            if (!user_selected) {
                /* this HCA was neither user selected nor has an SM LID with the max HCAs */
                PRINT_DEBUG(DEBUG_INIT_verbose>1, "Skipping HCA %s with SM LID (%d) since num_hcas_in_sm (%d) < max_hcas_in_sm_lid (%d)\n",
                            dev_list[i]->name, all_hca_sm_lid[i], num_hcas_in_sm[k], max_hcas_in_sm_lid);
                continue;
            }
        }
        if (num_hcas > MAX_NUM_HCAS) {
            PRINT_ERROR("MVAPICH2 only has support for %d HCAs in this build."
                        " %d HCAs detected. Please rebuild after increasing the MAX_NUM_HCAS macro"
                        " in ibv_param.h to support more HCAs.\n", MAX_NUM_HCAS, num_devices);
            break;
        }
        hca_type = mv2_get_hca_type(dev_list[i]);
        /* Get the network type for the HCA */
        network_type = MV2_GET_NETWORK_TYPE(hca_type);
        /* Handle the fastest HCA separately */
        if (dev_list[i] == usable_dev_list[0]) {
            /* Find the socket fastest HCA is connected to */
            mv2_ib_hca_socket_info[0] = get_ib_socket(dev_list[i]);
            /* Find the NUMA fastest HCA is connected to */
            mv2_ib_hca_numa_info[0] = get_ib_numa(dev_list[i]);
            /* Count the number of HCAs on my socket */
            if (mv2_ib_hca_numa_info[0] == my_numa) {
                PRINT_DEBUG(DEBUG_INIT_verbose>1, "1. Adding HCA %s as HCA %d on my numa %d\n",
                            dev_list[i]->name, num_hcas_on_my_numa, my_numa);
                usable_devs_on_my_numa[num_hcas_on_my_numa] = dev_list[i];
                num_hcas_on_my_numa++;
            }
        } else {
            /* Find the socket HCA is connected to */
            mv2_ib_hca_socket_info[j] = get_ib_socket(dev_list[i]);
            /* Find the NUMA HCA is connected to */
            mv2_ib_hca_numa_info[j] = get_ib_numa(dev_list[i]);
        }
        /* If network type is not fastest or if we have already stored the HCA
         * index in usable_dev_list[0] skip it */
        if ((network_type != fastest_network_type) ||
            (dev_list[i] == usable_dev_list[0])) {
            network_type = fastest_network_type;
            ibv_ops.close_device(nic_context[i]);
            nic_context[i] = NULL;
            continue;
        }
        /* Get the HCA type for the HCA */
        hca_type = mv2_get_hca_type(dev_list[i]);
        /* If we have HCAs of different speeds, force SM_SCHEDULING to USE_FIRST
         * so that we always use the fastest HCA for small messages */
        if (fastest_hca_type != hca_type) {
            rdma_rail_sharing_policy = USE_FIRST;
        }
        usable_dev_list[j] = dev_list[i];
        /* Count the number of HCAs on my numa */
        if (mv2_ib_hca_numa_info[j] == my_numa) {
            PRINT_DEBUG(DEBUG_INIT_verbose>1, "2. Adding HCA %s as HCA %d on my numa %d\n",
                        dev_list[i]->name, num_hcas_on_my_numa, my_numa);
            usable_devs_on_my_numa[num_hcas_on_my_numa] = dev_list[i];
            num_hcas_on_my_numa++;
        }
	    PRINT_DEBUG(DEBUG_INIT_verbose>1, "Other usable HCA %d = %s."
                    " Type = %s. Socket = %d.\n",
                    j, dev_list[i]->name, mv2_get_hca_name(hca_type),
                    mv2_ib_hca_numa_info[j]);
        j++;
        num_hcas++;
        if ((MV2_IS_QLE_CARD(hca_type) || MV2_IS_INTEL_CARD(hca_type)) && !mv2_suppress_hca_warnings) {
            PRINT_ERROR("QLogic IB cards or Intel Omni-Path detected in system.\n");
            PRINT_ERROR("Please re-configure the library with the"
                        " '--with-device=ch3:psm' configure option"
                        " for best performance and functionality.\n");
        }
        ibv_ops.close_device(nic_context[i]);
        nic_context[i] = NULL;
    }

    *num_usable_hcas = num_hcas;
    *num_usable_hcas_on_my_numa = num_hcas_on_my_numa;
    PRINT_DEBUG(DEBUG_INIT_verbose, "Fastest HCA (Network) type = %s (%s)."
                " Usable HCAs of type %s = %d, Usable HCAs of type %s on numa %d = %d\n",
                mv2_get_hca_name(fastest_hca_type), mv2_get_network_name(fastest_network_type),
                mv2_get_network_name(fastest_network_type), *num_usable_hcas,
                mv2_get_network_name(fastest_network_type), my_numa,
                *num_usable_hcas_on_my_numa);

    /* Free memory */
    MPIU_Free(all_hca_rate);
    if (all_link_type == NULL) {
        MPIU_Free(all_hca_link_type);
    }
    MPIU_Free(all_hca_sm_lid);
    MPIU_Free(unique_hca_sm_lid);
    MPIU_Free(num_hcas_in_sm);
    MPIU_Free(nic_context);

    return network_type;
}

/*
 * Function: rdma_skip_network_card
 *
 * Description:
 *      On a multi-rail network, skip the HCA that does not match with the
 *      network type
 *
 * Input:
 *      network_type -   Type of underlying network.
 *      ib_dev       -   Pointer to HCA data structure.
 *
 * Return:
 *      Success:    MPI_SUCCESS.
 *      Failure:    ERROR (-1).
 */
#undef FUNCNAME
#define FUNCNAME rdma_skip_network_card
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int rdma_skip_network_card(mv2_iba_network_classes network_type,
                           struct ibv_device *ib_dev)
{
    int skip = 0;

    if (network_type != mv2_get_hca_type(ib_dev)) {
        skip = 1;
    }

    return skip;
}


int rdma_skip_network_card_without_ip(ip_address_enabled_devices_t * ip_address_enabled_devices,
                                        int num_devs,int *ip_index, struct ibv_device *ib_dev)
{
    int skip = 1, index = 0;
    for (index = 0; index < num_devs; index++) {
        PRINT_DEBUG(DEBUG_INIT_verbose>1, "dev name %s, ip %s index %d\n",
                    ip_address_enabled_devices[index].device_name,
                    ip_address_enabled_devices[index].ip_address, index);
        if (!strcmp(ip_address_enabled_devices[index].device_name,
                    ibv_ops.get_device_name(ib_dev))){
             *ip_index = index;
              skip = 0;
              break;
        }
    }

    return skip;
}

void ring_rdma_close_hca(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc)
{
    int err;
    proc->boot_device = NULL;
    err = ibv_ops.dealloc_pd(proc->boot_ptag);
    if (err) {
        MPL_error_printf("Failed to dealloc pd (%s)\n", strerror(errno));
    }
    err = ibv_ops.close_device(proc->boot_context);
    if (err) {
        MPL_error_printf("Failed to close ib device (%s)\n", strerror(errno));
    }
}

#undef FUNCNAME
#define FUNCNAME ring_rdma_open_hca
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int ring_rdma_open_hca(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc)
{
    int i = 0, j = 0;
    int num_devices = 0;
    int err;
    struct ibv_device *ib_dev = NULL;
    struct ibv_device **dev_list = NULL;
    int is_device_opened = 0;

    dev_list = ibv_ops.get_device_list(&num_devices);

    for (i = 0; i < num_devices; i++) {
        ib_dev = dev_list[i];
        if (!ib_dev) {
            goto fn_exit;
        }

        if ((getenv("MV2_IBA_HCA") != NULL) &&
            (strcmp(rdma_iba_hcas[0], RDMA_IBA_NULL_HCA))) {
            int device_match = 0;
            for (j = 0; j < rdma_num_req_hcas; j++) {
                if (!strcmp(ibv_ops.get_device_name(ib_dev), rdma_iba_hcas[j])) {
                    device_match = 1;
                    break;
                }
            }
            if (!device_match) {
                continue;
            }
        }

        proc->boot_device = ib_dev;
        proc->boot_context = ibv_ops.open_device(proc->boot_device);
        if (!proc->boot_context) {
            /* Go to next device */
            continue;
        }

        if (ERROR == rdma_find_active_port(proc->boot_context, ib_dev, NULL, NULL, NULL)) {
            /* No active ports. Go to next device */
            err = ibv_ops.close_device(proc->boot_context);
            if (err) {
                MPL_error_printf("Failed to close ib device (%s)\n",
                                  strerror(errno));
            }
            continue;
        }

        proc->boot_ptag = ibv_ops.alloc_pd(proc->boot_context);
        if (!proc->boot_ptag) {
            err = ibv_ops.close_device(proc->boot_context);
            if (err) {
                MPL_error_printf("Failed to close ib device (%s)\n",
                                  strerror(errno));
            }
            continue;
        }

        is_device_opened = 1;
        break;
    }


  fn_exit:
    /* Clean up before exit */
    if (dev_list) {
        ibv_ops.free_device_list(dev_list);
    }
    return is_device_opened;
}

/*
 * Function: rdma_open_hca
 *
 * Description:
 *      Opens the HCA and allocates protection domain for it.
 *
 * Input:
 *      proc    -   Pointer to the process data structure.
 *
 * Return:
 *      Success:    MPI_SUCCESS.
 *      Failure:    ERROR (-1).
 */
#undef FUNCNAME
#define FUNCNAME rdma_open_hca
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int rdma_open_hca(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc)
{
    int i = 0, j = 0, k = 0, index = 0;
    char *value = NULL;
    int num_devices = 0;
    int num_usable_hcas = 0;
    int num_usable_hcas_on_my_numa = 0;
    int mpi_errno = MPI_SUCCESS;
    struct ibv_device *ib_dev = NULL;
    struct ibv_device **dev_list = NULL;
    struct ibv_device **usable_dev_list = NULL;
    struct ibv_device **usable_devs_on_my_numa = NULL;
    int network_type = MV2_NETWORK_CLASS_UNKNOWN;
    int total_ips = 0, ip_index = 0;
    uint8_t *all_hca_link_type = NULL;
#ifdef CRC_CHECK
    gen_crc_table();
#endif
    int fd = -1;
    char mv2_hca_board_id_path[MV2_MAX_HCA_BOARD_ID_PATH_LEN];
    char read_buf[MV2_MAX_HCA_BOARD_ID_LEN];
    ssize_t read_bytes = -1;

    /* If MV2_MRAIL_SHARING is selected, enable PROCESS PLACEMENT AWARE HCA
     * selection so that small message performance is not affected. Since
     * MRAILI_Send_select_rail will return index as mv2_closest_hca_offset. */
    if (rdma_multirail_usage_policy == MV2_MRAIL_SHARING) {
        mv2_process_placement_aware_hca_mapping = 1;
    }

    if ((value = getenv("MV2_PROCESS_PLACEMENT_AWARE_HCA_MAPPING")) != NULL) {
        mv2_process_placement_aware_hca_mapping = !!atoi(value);
    }

    rdma_num_hcas = 0;

    dev_list = ibv_ops.get_device_list(&num_devices);
    if (num_devices == 0) {
        PRINT_ERROR("No HCAs found on the system.\n");
        MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                                  "**fail %s", "No IB device found");
    }

    /* Allocate memory for devices */
    usable_dev_list = MPIU_Malloc(sizeof(struct ibv_device *)*num_devices);
    all_hca_link_type = MPIU_Malloc(sizeof(uint8_t)*num_devices);
    usable_devs_on_my_numa = MPIU_Malloc(sizeof(struct ibv_device *)*num_devices);

    network_type = rdma_find_network_type(dev_list, num_devices, usable_dev_list,
                                          usable_devs_on_my_numa,
                                          &num_usable_hcas,
                                          &num_usable_hcas_on_my_numa,
                                          all_hca_link_type);

    if (network_type == MV2_NETWORK_CLASS_UNKNOWN) {
        if (num_usable_hcas) {
            PRINT_INFO((mv2_suppress_hca_warnings==0),
			"Unknown HCA type: this build of MVAPICH2 does not"
                        " fully support the HCA found on the system (try with"
                        " other build options)\n");
        } else {
            if ((MPIDI_Get_num_nodes() == 1) && MPIDI_CH3I_Process.has_dpm) {
                PRINT_ERROR("HCA not found on the system. "
                            "DPM functionality requires an active HCA.\n");
            }
            MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                                      "**fail %s", "No IB device found");
        }
    }
#if defined(_MCST_SUPPORT_) && defined(RDMA_CM)
    total_ips = num_devices;
    ip_address_enabled_devices = MPIU_Malloc(num_devices * sizeof(ip_address_enabled_devices_t));
    if (!ip_address_enabled_devices) {
        MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_NO_MEM, goto fn_fail,
                "**fail", "**fail %s",
                "Failed to allocate resources for "
                "multicast");
    }
    MPIU_Memset(ip_address_enabled_devices, 0,
                num_devices * sizeof(ip_address_enabled_devices_t));
    mpi_errno = mv2_get_verbs_ips_dev_names(&total_ips, ip_address_enabled_devices);
    if (mpi_errno) {
        MPIR_ERR_POP(mpi_errno);
    }
    num_ip_enabled_devices = total_ips;
    PRINT_DEBUG(DEBUG_MCST_verbose, "total ips %d\n",total_ips); 
    if (rdma_use_rdma_cm_mcast == 1 && num_ip_enabled_devices == 0) {
        PRINT_ERROR("No IP enabled device found. Disabling rdma_cm multicast\n");
        rdma_use_rdma_cm_mcast = 0;
    }
    if (rdma_use_rdma_cm_mcast == 1) {
       if ((rdma_multirail_usage_policy == MV2_MRAIL_BINDING) &&
            (num_usable_hcas > 1)) {
           PRINT_INFO((MPIDI_Process.my_pg_rank == 0),"[Warning] Setting the"
                 " multirail policy to MV2_MRAIL_SHARING since RDMA_CM based multicast"
                 " is enabled.\n");
       }
       rdma_multirail_usage_policy = MV2_MRAIL_SHARING;
    }
#endif /*defined(_MCST_SUPPORT_) && defined(RDMA_CM)*/
    if (mv2_system_has_ib && mv2_system_has_roce && (num_usable_hcas > 1)
        && mv2_allow_heterogeneous_hca_selection) {
        PRINT_INFO((MPIDI_Process.my_pg_rank == 0),"[Warning] Setting the"
                " multirail policy to MV2_MRAIL_SHARING since IB and RoCE"
                " were detected on the system.\n");
        rdma_multirail_usage_policy = MV2_MRAIL_SHARING;
    } else if (mv2_system_has_ib && mv2_system_has_roce && !use_iboeth) {
        /* If system has IB and RoCE, prevent the code from using the RoCE side
         * of the code. This can lead to hangs in heterogeneous systems where
         * some nodes have RoCE and some others do not. */
        mv2_system_has_roce = 0;
    }

    for (i = 0; i < num_usable_hcas; i++) {
        if (rdma_multirail_usage_policy == MV2_MRAIL_BINDING) {
            /* Bind a process to a HCA */
            if (mrail_use_default_mapping) {
                if (num_usable_hcas_on_my_numa && mv2_process_placement_aware_hca_mapping) {
                    ib_dev = usable_devs_on_my_numa[rdma_local_id % num_usable_hcas_on_my_numa];
                    /* Find the correct index in usable_dev_list to be used
                     * later to find out the correct NUMA and Socket info */
                    for (k = 0; k < num_usable_hcas; ++k) {
                        if (ib_dev == usable_dev_list[k]) {
                            index = k;
                            break;
                        }
                    }
                } else {
                    index = mrail_user_defined_p2r_mapping =
                        rdma_local_id % num_usable_hcas;
                    ib_dev = usable_dev_list[mrail_user_defined_p2r_mapping];
                }
            } else {
                index = mrail_user_defined_p2r_mapping;
                ib_dev = usable_dev_list[mrail_user_defined_p2r_mapping];
                PRINT_DEBUG(DEBUG_INIT_verbose,"Using HCA %s specified by user\n", ib_dev->name);
            }
        } else if (!strncmp(rdma_iba_hcas[i], RDMA_IBA_NULL_HCA, 32)) {
            /* User hasn't specified any HCA name
             * We will use the first available HCA(s) */
            ib_dev = usable_dev_list[i];
            index = i;
        } else {
            /* User specified HCA(s), try to look for it */
            ib_dev = NULL;
            j = 0;
            while (usable_dev_list[j]) {
                if (!strncmp(ibv_ops.get_device_name(usable_dev_list[j]),
                             rdma_iba_hcas[rdma_num_hcas], 32)) {
                    ib_dev = usable_dev_list[j];
                    index = j;
                    break;
                }
                j++;
            }
            if (ib_dev == NULL) {
                PRINT_ERROR("The HCA (%s) the user requested for is not"
                            " available on the system\n", rdma_iba_hcas[rdma_num_hcas]);
                MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                                        "**fail %s", "Requested IB device not found");
            }
            if (all_hca_link_type[index] == IBV_LINK_LAYER_ETHERNET) {
                mv2_system_has_ib = 0;
                mv2_system_has_roce = 1;
            } else if (all_hca_link_type[index] == IBV_LINK_LAYER_INFINIBAND) {
                mv2_system_has_ib = 1;
                mv2_system_has_roce = 0;
            }
        }
        PRINT_DEBUG(DEBUG_INIT_verbose,"Selecting HCA %s on my numa\n", ib_dev->name);

        if (!ib_dev) {
            /* Clean up before exit */
            if (dev_list) {
                ibv_ops.free_device_list(dev_list);
            }
            MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                                      "**fail %s", "No IB device found");
        }

#if defined(_MCST_SUPPORT_) && defined(RDMA_CM)
        /* Only node leader, i.e. local rank 0, will have to worry about using
         * an IB interface with an IP address */
        /* We don't need to skip the HCA as we change the multi rail policy to rail sharing 
         * if RDMA_CM mcast is enabled*/
        if (rdma_use_rdma_cm_mcast == 1 && 
			rdma_skip_network_card_without_ip(ip_address_enabled_devices, total_ips, &ip_index, ib_dev)) {
		/* If the user has specified the use of some HCAs through
		 * MV2_IBA_HCA, then make sure that we don't skip
		 * them automatically. We should honor the request and disable
		 * RDMA_CM based multicast instead after displaying a warning.
		 * TODO: This does not handle the multirail scenario correctly. */
		if (!strncmp(ibv_ops.get_device_name(ib_dev),
					rdma_iba_hcas[rdma_num_hcas], 32)) {
			PRINT_ERROR("User specified HCA %s does not have an IP address."
					" Disabling RDMA_CM based multicast.\n",
					rdma_iba_hcas[j]);
			rdma_use_rdma_cm_mcast = 0;
		}

	}
#endif /*defined(_MCST_SUPPORT_) && defined(RDMA_CM)*/

        proc->ib_dev[rdma_num_hcas] = ib_dev;

        proc->nic_context[rdma_num_hcas] = ibv_ops.open_device(ib_dev);
        if (!proc->nic_context[rdma_num_hcas]) {
            /* Clean up before exit */
            if (dev_list) {
                ibv_ops.free_device_list(dev_list);
            }
            MPIR_ERR_SETFATALANDJUMP2(mpi_errno, MPI_ERR_OTHER, "**fail",
                                      "%s: %s", "Failed to open HCA",
                                      strerror(errno));
        }

        proc->ptag[rdma_num_hcas] =
            ibv_ops.alloc_pd(proc->nic_context[rdma_num_hcas]);
        if (!proc->ptag[rdma_num_hcas]) {
            /* Clean up before exit */
            if (dev_list) {
                ibv_ops.free_device_list(dev_list);
            }
            MPIR_ERR_SETFATALANDJUMP2(mpi_errno, MPI_ERR_OTHER,
                                      "**fail", "%s: %s",
                                      "Failed to alloc pd",
                                      strerror(errno));
        }

        /* Store the socket and NUMA info of selected HCAs */
        mv2_selected_ib_hca_numa_info[rdma_num_hcas] = mv2_ib_hca_numa_info[index];
        mv2_selected_ib_hca_socket_info[rdma_num_hcas] = mv2_ib_hca_socket_info[index];

        PRINT_DEBUG(DEBUG_INIT_verbose, "HCA %d/%d = %s. NUMA = %d, Socket = %d\n",
                    rdma_num_hcas+1, rdma_num_req_hcas,
                    proc->ib_dev[rdma_num_hcas]->name,
                    mv2_selected_ib_hca_numa_info[rdma_num_hcas],
                    mv2_selected_ib_hca_socket_info[rdma_num_hcas]);
        
#if defined(_MCST_SUPPORT_) && defined(RDMA_CM)
        if (rdma_use_rdma_cm_mcast == 1 &&
            !rdma_skip_network_card_without_ip(ip_address_enabled_devices,total_ips, &ip_index, ib_dev)){
            /* We need to know the index of the first ip_address for doing mcast.
             * Has to be changed for multirail mcast. */
            mcast_ctx = MPIU_Malloc(sizeof(mcast_context_t));
            if (!mcast_ctx) {
                MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_NO_MEM, goto fn_fail,
                        "**fail", "**fail %s",
                        "Failed to allocate resources for "
                        "multicast");
            }
            mcast_ctx->ip_index = ip_index;
            mcast_ctx->selected_rail = rdma_num_hcas;
            PRINT_DEBUG(DEBUG_MCST_verbose>2,"Mcast context created. ip indx %d, mcast rail %d\n", ip_index, rdma_num_hcas);
        }
#endif /*defined(_MCST_SUPPORT_) && defined(RDMA_CM)*/
        /* Find the offset in the array of the closest HCA */
        if (mv2_process_placement_aware_hca_mapping) {
            for (j = 0; j < num_usable_hcas_on_my_numa; ++j) {
                if ((num_usable_hcas_on_my_numa > 1) &&
                    (j != (rdma_local_id%num_usable_hcas_on_my_numa))) {
                    /* If we have more than one HCA on our numa, load balance
                     * their use between different processes */
                    PRINT_DEBUG(DEBUG_INIT_verbose,"Skipping closest HCA %s at index %d\n",
                                proc->ib_dev[mv2_closest_hca_offset]->name, mv2_closest_hca_offset);
                    continue;
                }
                if (!strcmp(proc->ib_dev[rdma_num_hcas]->name, usable_devs_on_my_numa[j]->name)) {
                    mv2_closest_hca_offset = rdma_num_hcas;
                    PRINT_DEBUG(DEBUG_INIT_verbose,"Index of closest HCA (%s) = %d\n",
                                proc->ib_dev[mv2_closest_hca_offset]->name, mv2_closest_hca_offset);
                }
            }
        }
        rdma_num_hcas++;
        if ((rdma_multirail_usage_policy == MV2_MRAIL_BINDING) ||
            (rdma_num_req_hcas == rdma_num_hcas)) {
            /* If usage policy is binding, or if we have found enough
             * number of HCAs asked for by the user */
            break;
        }
    }
    
    /* Temporary logic to detect if a Rockport network board is selected. For
     * now, we only consider the case where one HCA is used and use 4 QPs per
     * port for RC connections */
    if (rdma_num_hcas == 1) {
        sprintf(mv2_hca_board_id_path, MV2_HCA_BOARD_ID_PATH, proc->ib_dev[0]->name);
        fd = open(mv2_hca_board_id_path, O_RDONLY);
        if (fd >= 0) {
            read_bytes = read(fd, read_buf, sizeof(read_buf) - 1);
            if (read_bytes < sizeof(read_buf) - 1) {
                read_buf[read_bytes] = '\0';
            }
            if (!strncmp(read_buf, MV2_ROCKPORT_FW_BOARD_ID,
                    strlen(MV2_ROCKPORT_FW_BOARD_ID))) {
                mv2_system_has_rockport = 1;
                rdma_num_qp_per_port = MV2_ROCKPORT_NUM_QP_PER_PORT;
            }
            close(fd);
        }
    }

    if (unlikely(rdma_num_hcas == 0)) {
#if defined(_MCST_SUPPORT_) && defined(RDMA_CM)
        if (rdma_use_rdma_cm_mcast == 1 && rdma_local_id == 0) {
            PRINT_ERROR("No HCA on the node has an IP address."
                        " Disabling RDMA_CM based multicast.\n");
            rdma_use_rdma_cm_mcast = 0;
        }
#endif /*defined(_MCST_SUPPORT_) && defined(RDMA_CM)*/
        MPIR_ERR_SETFATALANDJUMP2(mpi_errno, MPI_ERR_OTHER,
                                  "**fail", "%s %d",
                                  "No active HCAs found on the system!!!",
                                  rdma_num_hcas);
    }

  fn_exit:
    /* Clean up before exit */
    MPIU_Free(usable_dev_list);
    MPIU_Free(all_hca_link_type);
    MPIU_Free(usable_devs_on_my_numa);
    if (dev_list) {
        ibv_ops.free_device_list(dev_list);
    }
    return mpi_errno;

  fn_fail:
    goto fn_exit;
}

static inline int ipv6_addr_v4mapped(const struct in6_addr *a)
{
    return ((a->s6_addr32[0] | a->s6_addr32[1]) |
            (a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL ||
            /* IPv4 encoded multicast addresses */
            (a->s6_addr32[0] == htonl(0xff0e0000) &&
            ((a->s6_addr32[1] |
            (a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL));
}

#undef FUNCNAME
#define FUNCNAME rdma_find_best_gid_index
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int rdma_find_best_gid_index(struct ibv_context *ctx, struct ibv_port_attr *attr, int port)
{
    int gid_index = RDMA_DEFAULT_GID_INDEX, i = 0;
    union ibv_gid temp_gid, temp_gid_rival;
    int is_ipv4, is_ipv4_rival;
    int fd;
    char gid_path[MV2_MAX_GID_PATH_LEN];

    /* If the link layer is ethernet (RoCEv1/RoCEv2), attempt
     * to find the gid index of the first valid interfaces 
     * supporting RoCEv1 and RoCEv2 from the gid table */
    if (attr->link_layer == IBV_LINK_LAYER_ETHERNET) {
        int first_rocev1_index = -1;
        int first_rocev2_index = -1;
        while (i < attr->gid_tbl_len && 
               (first_rocev1_index == -1 || first_rocev2_index == -1)) {
            int ret = ibv_ops.query_gid(ctx, port, i, &temp_gid);
            if (ret == MPI_SUCCESS) {
                sprintf(gid_path, GID_ATTR_PATH, ctx->device->name, port, i);
                is_ipv4 = ipv6_addr_v4mapped((struct in6_addr *)temp_gid.raw);
                if (is_ipv4) {
                    fd = open(gid_path, O_RDONLY);
                    if (fd >= 0) {
                        char buf[16];
                        int buf_size = sizeof(buf);
                        ssize_t read_bytes = -1;
                        read_bytes = read(fd, buf, buf_size - 1);
                        if (read_bytes < buf_size - 1) {
                            buf[read_bytes] = '\0';
                        }
                        if (!strncmp(buf, "IB/RoCE v1", 10)) {
                            first_rocev1_index = i;
                        } else if (!strncmp(buf, "RoCE v2", 7)) {
                            first_rocev2_index = i;
                        }
                        close(fd);
                    }
                }
            }
            i++;
        }
        /* Set gid_index based on the value of mv2_use_roce_mode. If an
         * interface with the RoCE version specified isn't available, we fall
         * back to the latest available version. If neither is available, upper
         * level functions error out and the process/job aborts. */
        if (mv2_use_roce_mode == MV2_ROCE_MODE_V1) {
            gid_index = (first_rocev1_index == -1) ? first_rocev2_index : first_rocev1_index;
        } else if (mv2_use_roce_mode == MV2_ROCE_MODE_V2) {
            gid_index = (first_rocev2_index == -1) ? first_rocev1_index : first_rocev2_index;
        }
    } else {
        for (i = 1; i < attr->gid_tbl_len; i++) {
            if (ibv_ops.query_gid(ctx, port, gid_index, &temp_gid)) {
                return RDMA_DEFAULT_GID_INDEX;
            }

            if (ibv_ops.query_gid(ctx, port, i, &temp_gid_rival)) {
                return RDMA_DEFAULT_GID_INDEX;
            }

            is_ipv4 = ipv6_addr_v4mapped((struct in6_addr *)temp_gid.raw);
            is_ipv4_rival = ipv6_addr_v4mapped((struct in6_addr *)temp_gid_rival.raw);

            if (is_ipv4_rival && !is_ipv4) {
                gid_index = i;
            }
        }
    }

    return gid_index;
}

#undef FUNCNAME
#define FUNCNAME rdma_iba_hca_init_noqp
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int rdma_iba_hca_init_noqp(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc,
                           int pg_rank, int pg_size)
{
    struct ibv_port_attr port_attr;
    struct ibv_device_attr dev_attr;
    union ibv_gid gid;
    int mpi_errno = MPI_SUCCESS;
    int i, j, k, gid_index = 0;

    /* step 1: open hca, create ptags  and create cqs */
    for (i = 0; i < rdma_num_hcas; i++) {

        MPIU_Memset(&gid, 0, sizeof(gid));
        if (ibv_ops.query_device(proc->nic_context[i], &dev_attr)) {
            MPIR_ERR_SETFATALANDJUMP1(mpi_errno,
                                      MPI_ERR_INTERN,
                                      "**fail",
                                      "**fail %s",
                                      "Error getting HCA attributes\n");
        }

        /* Identify the maximum number of atomic operations supported by the HCA */
        rdma_supported_max_rdma_dst_ops   = dev_attr.max_qp_rd_atom;
        rdma_supported_max_qp_ous_rd_atom = dev_attr.max_qp_rd_atom;

        if ((dev_attr.atomic_cap == IBV_ATOMIC_HCA) || (dev_attr.atomic_cap == IBV_ATOMIC_GLOB)) {
            g_atomics_support = 1;
        }
#ifdef ATOMIC_HCA_REPLY_BE
        else if (dev_attr.atomic_cap == ATOMIC_HCA_REPLY_BE) {
                g_atomics_support = 1;
                g_atomics_support_be = 1;
        }
#endif
        else {
                g_atomics_support = 0;
        }

        /* detecting active ports */
        if (rdma_default_port < 0 || rdma_num_ports > 1) {
            k = 0;
            for (j = 1; j <= RDMA_DEFAULT_MAX_PORTS; j++) {
                if ((!ibv_ops.query_port(mv2_MPIDI_CH3I_RDMA_Process.nic_context[i],
                                     j, &port_attr)) &&
                    (port_attr.state == IBV_PORT_ACTIVE)) {
                    /* Store link layer protocol being used */
                    mv2_MPIDI_CH3I_RDMA_Process.link_layer[i][j-1] = port_attr.link_layer;
                    PRINT_DEBUG(DEBUG_INIT_verbose, "Link layer for port %d on HCA %d (%s) = %s\n", j, i,
                                proc->nic_context[i]->device->name,
                                (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)?
                                "IBV_LINK_LAYER_ETHERNET":"IBV_LINK_LAYER_INFINIBAND");
                    if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
                        mv2_system_has_roce = 1;
                    }
                    if (rdma_default_gid_index == RDMA_DEFAULT_GID_INDEX) {
                        gid_index = rdma_find_best_gid_index(proc->nic_context[i], &port_attr, j);
                    } else {
                        gid_index = rdma_default_gid_index;
                    }
                    /* Store GID Index being used */
                    mv2_MPIDI_CH3I_RDMA_Process.gid_index[i][j-1] = gid_index;
                    PRINT_DEBUG(DEBUG_INIT_verbose, "Using GID Index %d on port %d for HCA %d\n",
                                mv2_MPIDI_CH3I_RDMA_Process.gid_index[i][j-1], j, i);
                    if (ibv_ops.query_gid
                            (mv2_MPIDI_CH3I_RDMA_Process.nic_context[i], j, gid_index, &gid)) {
                        MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER,
                                "**fail",
                                "Failed to retrieve gid on rank %d",
                                pg_rank);
                    }
                    mv2_MPIDI_CH3I_RDMA_Process.gids[i][k] = gid;
                    mv2_MPIDI_CH3I_RDMA_Process.lids[i][k] = port_attr.lid;

                    mv2_MPIDI_CH3I_RDMA_Process.ports[i][k++] = j;

                    if (check_attrs(proc, &port_attr, &dev_attr,
                                    (rdma_default_port == RDMA_DEFAULT_PORT)?0:1)) {
                        MPIR_ERR_SETFATALANDJUMP1(mpi_errno,
                                                  MPI_ERR_INTERN,
                                                  "**fail",
                                                  "**fail %s",
                                                  "Attributes failed sanity check");
                    }
                }
            }
            if (k < rdma_num_ports) {
                MPIR_ERR_SETFATALANDJUMP2(mpi_errno,
                                          MPI_ERR_INTERN,
                                          "**fail",
                                          "**fail %s %d",
                                          "Not enough ports are in active state"
                                          "needed active ports %d\n",
                                          rdma_num_ports);
            }
        } else {
            if (ibv_ops.query_port(mv2_MPIDI_CH3I_RDMA_Process.nic_context[i],
                               rdma_default_port, &port_attr)
                || (port_attr.state != IBV_PORT_ACTIVE)) {
                MPIR_ERR_SETFATALANDJUMP2(mpi_errno,
                                          MPI_ERR_INTERN,
                                          "**fail",
                                          "**fail %s %d",
                                          "user specified port %d: fail to"
                                          "query or not ACTIVE\n",
                                          rdma_default_port);
            }
            /* Store link layer protocol being used */
            mv2_MPIDI_CH3I_RDMA_Process.link_layer[i][0] = port_attr.link_layer;
            PRINT_DEBUG(DEBUG_INIT_verbose, "Link layer for port %d on HCA %d (%s) = %s\n", 0, i,
                        proc->nic_context[i]->device->name,
                        (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)?
                        "IBV_LINK_LAYER_ETHERNET":"IBV_LINK_LAYER_INFINIBAND");
            if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
                mv2_system_has_roce = 1;
            }
            if (rdma_default_gid_index == RDMA_DEFAULT_GID_INDEX) {
                gid_index = rdma_find_best_gid_index(proc->nic_context[i], &port_attr, rdma_default_port);
            } else {
                gid_index = rdma_default_gid_index;
            }
            /* Store GID Index being used */
            mv2_MPIDI_CH3I_RDMA_Process.gid_index[i][0] = gid_index;
            PRINT_DEBUG(DEBUG_INIT_verbose, "Using GID Index %d on port %d for HCA %d\n",
                        mv2_MPIDI_CH3I_RDMA_Process.gid_index[i][0], rdma_default_port, i);
            if (ibv_ops.query_gid(mv2_MPIDI_CH3I_RDMA_Process.nic_context[i],
                        rdma_default_port, gid_index, &gid)) {
                MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER,
                        "**fail",
                        "Failed to retrieve gid on rank %d",
                        pg_rank);
            }
            mv2_MPIDI_CH3I_RDMA_Process.gids[i][0] = gid;
            mv2_MPIDI_CH3I_RDMA_Process.lids[i][0] = port_attr.lid;
            mv2_MPIDI_CH3I_RDMA_Process.ports[i][0] = rdma_default_port;

            if (check_attrs(proc, &port_attr, &dev_attr, 1)) {
                MPIR_ERR_SETFATALANDJUMP1(mpi_errno,
                                          MPI_ERR_INTERN,
                                          "**fail",
                                          "**fail %s",
                                          "Attributes failed sanity check");
            }
        }

        if (rdma_use_blocking) {
            proc->comp_channel[i] =
                ibv_ops.create_comp_channel(proc->nic_context[i]);

            if (!proc->comp_channel[i]) {
                fprintf(stderr, "cannot create completion channel\n");
                goto err;
            }

            DEBUG_PRINT("Created comp channel %p\n", proc->comp_channel[i]);

            proc->cq_hndl[i] = ibv_ops.create_cq(proc->nic_context[i],
                                             rdma_default_max_cq_size, NULL,
                                             proc->comp_channel[i], 0);
            proc->send_cq_hndl[i] = NULL;
            proc->recv_cq_hndl[i] = NULL;

            if (!proc->cq_hndl[i]) {
                fprintf(stderr, "cannot create cq\n");
                goto err;
            }
        } else {

            /* Allocate the completion queue handle for the HCA */
            proc->cq_hndl[i] = ibv_ops.create_cq(proc->nic_context[i],
                                             rdma_default_max_cq_size, NULL,
                                             NULL, 0);
            proc->send_cq_hndl[i] = NULL;
            proc->recv_cq_hndl[i] = NULL;

            if (!proc->cq_hndl[i]) {
                fprintf(stderr, "cannot create cq\n");
                goto err;
            }
        }

        if (proc->has_srq) {
            proc->srq_hndl[i] = create_srq(proc, i);
            if ((proc->srq_hndl[i]) == NULL) {
                goto err_cq;
            }
#ifdef _ENABLE_XRC_
            proc->xrc_srqn[i] = proc->srq_hndl[i]->xrc_srq_num;
            PRINT_DEBUG(DEBUG_XRC_verbose > 0, "My SRQN=%d rail:%d\n",
                        proc->xrc_srqn[i], i);
#endif /* _ENABLE_XRC_ */
        }
    }

    /*Port for all mgmt */
    rdma_default_port = mv2_MPIDI_CH3I_RDMA_Process.ports[0][0];
    return 0;
  err_cq:
    for (i = 0; i < rdma_num_hcas; i++) {
        if (proc->cq_hndl[i])
            ibv_ops.destroy_cq(proc->cq_hndl[i]);
    }

  err:
    for (i = 0; i < rdma_num_hcas; i++) {
        if (proc->ptag[i])
            ibv_ops.dealloc_pd(proc->ptag[i]);
    }

    for (i = 0; i < rdma_num_hcas; i++) {
        if (proc->nic_context[i])
            ibv_ops.close_device(proc->nic_context[i]);
    }
  fn_exit:
    return mpi_errno;

  fn_fail:
    goto fn_exit;

}

#undef FUNCNAME
#define FUNCNAME rdma_iba_hca_init
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int rdma_iba_hca_init(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc, int pg_rank,
                      MPIDI_PG_t * pg, struct process_init_info *info)
{
    struct ibv_qp_init_attr attr;
    struct ibv_qp_attr qp_attr;
    struct ibv_port_attr port_attr;
    struct ibv_device_attr dev_attr;

    MPIDI_VC_t *vc;

    int i, j, k, gid_index = 0;
    int hca_index = 0;
    int port_index = 0;
    int rail_index = 0;
    int ports[MAX_NUM_HCAS][MAX_NUM_PORTS];
    int lids[MAX_NUM_HCAS][MAX_NUM_PORTS];
    union ibv_gid gids[MAX_NUM_HCAS][MAX_NUM_PORTS];
    int mpi_errno = MPI_SUCCESS;
    int pg_size = MPIDI_PG_Get_size(pg);

    MPIU_Memset(&gids, 0, sizeof(gids));

    /* step 1: open hca, create ptags  and create cqs */
    for (i = 0; i < rdma_num_hcas; i++) {
        if (ibv_ops.query_device(proc->nic_context[i], &dev_attr)) {
            MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                                      "**fail %s",
                                      "Error getting HCA attributes");
        }
        /* Identify the maximum number of atomic operations supported by the HCA */
        rdma_supported_max_rdma_dst_ops   = dev_attr.max_qp_rd_atom;
        rdma_supported_max_qp_ous_rd_atom = dev_attr.max_qp_rd_atom;

        if ((dev_attr.atomic_cap == IBV_ATOMIC_HCA) || (dev_attr.atomic_cap == IBV_ATOMIC_GLOB)) {
            g_atomics_support = 1;
        }
#ifdef ATOMIC_HCA_REPLY_BE
        else if (dev_attr.atomic_cap == ATOMIC_HCA_REPLY_BE) {
            g_atomics_support = 1;
            g_atomics_support_be = 1;
        }
#endif
        else {
            g_atomics_support = 0;
        }

#ifdef RDMA_CM
        if (!proc->use_rdma_cm) {
#endif
            /* detecting active ports */
            if (rdma_default_port < 0 || rdma_num_ports > 1) {
                k = 0;
                for (j = 1; j <= RDMA_DEFAULT_MAX_PORTS; j++) {
                    if ((!ibv_ops.query_port
                         (mv2_MPIDI_CH3I_RDMA_Process.nic_context[i], j,
                          &port_attr)) && (port_attr.state == IBV_PORT_ACTIVE)) {

                        /* Store link layer protocol being used */
                        mv2_MPIDI_CH3I_RDMA_Process.link_layer[i][j-1] = port_attr.link_layer;
                        PRINT_DEBUG(DEBUG_INIT_verbose, "Link layer for port %d on HCA %d (%s) = %s\n", j, i,
                                    proc->nic_context[i]->device->name,
                                    (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)?
                                    "IBV_LINK_LAYER_ETHERNET":"IBV_LINK_LAYER_INFINIBAND");
                        if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
                            mv2_system_has_roce = 1;
                        }
                        if (rdma_default_gid_index == RDMA_DEFAULT_GID_INDEX) {
                            gid_index = rdma_find_best_gid_index(proc->nic_context[i], &port_attr, j);
                        } else {
                            gid_index = rdma_default_gid_index;
                        }
                        /* Store GID Index being used */
                        mv2_MPIDI_CH3I_RDMA_Process.gid_index[i][j-1] = gid_index;
                        PRINT_DEBUG(DEBUG_INIT_verbose, "Using GID Index %d on port %d for HCA %d\n",
                                    mv2_MPIDI_CH3I_RDMA_Process.gid_index[i][j-1], j, i);
                        if (ibv_ops.query_gid
                                (mv2_MPIDI_CH3I_RDMA_Process.nic_context[i], j,
                                 gid_index, &gids[i][k])) {
                            MPIR_ERR_SETFATALANDJUMP1(mpi_errno,
                                    MPI_ERR_OTHER,
                                    "**fail",
                                    "Failed to retrieve gid on rank %d",
                                    pg_rank);
                        }
                        DEBUG_PRINT("[%d] %s(%d): Getting gid[%d][%d] for"
                                " port %d subnet_prefix = %llx,"
                                " intf_id = %llx\r\n",
                                pg_rank, __FUNCTION__, __LINE__, i, k,
                                k, gids[i][k].global.subnet_prefix,
                                gids[i][k].global.interface_id);
                        lids[i][k] = port_attr.lid;
                        mv2_MPIDI_CH3I_RDMA_Process.ports[i][k] = j;
                        mv2_MPIDI_CH3I_RDMA_Process.gids[i][k] = gids[i][k];
                        ports[i][k++] = j;


                        if (check_attrs(proc, &port_attr, &dev_attr,
                                    (rdma_default_port == RDMA_DEFAULT_PORT)?0:1)) {
                            MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER,
                                                      "**fail", "**fail %s",
                                                      "Attributes failed sanity check");
                        }
                    }
                }
                if (k < rdma_num_ports) {
                    MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER,
                                              "**activeports",
                                              "**activeports %d",
                                              rdma_num_ports);
                }
            } else {
                if (ibv_ops.query_port(mv2_MPIDI_CH3I_RDMA_Process.nic_context[i],
                                   rdma_default_port, &port_attr)
                    || (port_attr.state != IBV_PORT_ACTIVE)) {
                    MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER,
                                              "**portquery", "**portquery %d",
                                              rdma_default_port);
                }

                ports[i][0] = rdma_default_port;
                mv2_MPIDI_CH3I_RDMA_Process.ports[i][0] = rdma_default_port;

                /* Store link layer protocol being used */
                mv2_MPIDI_CH3I_RDMA_Process.link_layer[i][0] = port_attr.link_layer;
                PRINT_DEBUG(DEBUG_INIT_verbose, "Link layer for port %d on HCA %d (%s) = %s\n", 0, i,
                            proc->nic_context[i]->device->name,
                            (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)?
                            "IBV_LINK_LAYER_ETHERNET":"IBV_LINK_LAYER_INFINIBAND");
                if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
                    mv2_system_has_roce = 1;
                }
                if (rdma_default_gid_index == RDMA_DEFAULT_GID_INDEX) {
                    gid_index = rdma_find_best_gid_index(proc->nic_context[i], &port_attr, rdma_default_port);
                } else {
                    gid_index = rdma_default_gid_index;
                }
                /* Store GID Index being used */
                mv2_MPIDI_CH3I_RDMA_Process.gid_index[i][0] = gid_index;
                PRINT_DEBUG(DEBUG_INIT_verbose, "Using GID Index %d on port %d for HCA %d\n",
                            mv2_MPIDI_CH3I_RDMA_Process.gid_index[i][0], rdma_default_port, i);
                if (ibv_ops.query_gid
                        (mv2_MPIDI_CH3I_RDMA_Process.nic_context[i],
                         rdma_default_port, gid_index, &gids[i][0])) {
                    MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER,
                            "**fail",
                            "Failed to retrieve gid on rank %d",
                            pg_rank);
                }

                if (check_attrs(proc, &port_attr, &dev_attr, 1)) {
                    MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER,
                            "**fail", "**fail %s",
                            "Attributes failed sanity check");
                }
                mv2_MPIDI_CH3I_RDMA_Process.gids[i][0] = gids[i][0];
                lids[i][0] = port_attr.lid;
            }

            if (rdma_use_blocking) {
                proc->comp_channel[i] =
                    ibv_ops.create_comp_channel(proc->nic_context[i]);

                if (!proc->comp_channel[i]) {
                    MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_OTHER,
                                              goto err, "**fail", "**fail %s",
                                              "cannot create completion channel");
                }

                proc->cq_hndl[i] = ibv_ops.create_cq(proc->nic_context[i],
                                                 rdma_default_max_cq_size, NULL,
                                                 proc->comp_channel[i], 0);
                proc->send_cq_hndl[i] = NULL;
                proc->recv_cq_hndl[i] = NULL;

                if (!proc->cq_hndl[i]) {
                    MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_OTHER,
                                              goto err, "**fail", "**fail %s",
                                              "cannot create cq");
                }

                if (ibv_req_notify_cq(proc->cq_hndl[i], 0)) {
                    MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_OTHER,
                                              goto err, "**fail", "**fail %s",
                                              "cannot request cq notification");
                }
            } else {
                /* Allocate the completion queue handle for the HCA */
                proc->cq_hndl[i] = ibv_ops.create_cq(proc->nic_context[i],
                                                 rdma_default_max_cq_size, NULL,
                                                 NULL, 0);
                proc->send_cq_hndl[i] = NULL;
                proc->recv_cq_hndl[i] = NULL;

                if (!proc->cq_hndl[i]) {
                    MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_OTHER,
                                              goto err, "**fail", "**fail %s",
                                              "cannot create cq");
                }
            }

            if (proc->has_srq) {
                proc->srq_hndl[i] = create_srq(proc, i);
            }
#ifdef RDMA_CM
        }
#endif /* RDMA_CM */
     }

#ifdef RDMA_CM
    if (proc->hca_type == MV2_HCA_MLX_CX_CONNIB) {
        g_atomics_support = 0;
    }
#endif /* RDMA_CM */

    rdma_default_port = ports[0][0];
    /* step 2: create qps for all vc */
    qp_attr.qp_state = IBV_QPS_INIT;
    if (g_atomics_support) {
        qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
        IBV_ACCESS_REMOTE_ATOMIC;
#ifdef INFINIBAND_VERBS_EXP_H
        if (g_atomics_support_be) {
            qp_attr.qp_access_flags |= IBV_EXP_QP_CREATE_ATOMIC_BE_REPLY;
        }
#endif  /* INFINIBAND_VERBS_EXP_H */
    } else {
        qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
    }


    for (i = 0; i < pg_size; i++) {
        MPIDI_PG_Get_vc(pg, i, &vc);

        vc->mrail.num_rails = rdma_num_rails;
        if (!vc->mrail.rails) {
            vc->mrail.rails = MPIU_Malloc
                (sizeof *vc->mrail.rails * vc->mrail.num_rails);

            if (!vc->mrail.rails) {
                MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_OTHER, goto err_cq,
                        "**fail", "**fail %s",
                        "Failed to allocate resources for "
                        "multirails");
            }

            MPIU_Memset(vc->mrail.rails, 0,
                    (sizeof *vc->mrail.rails * vc->mrail.num_rails));
        }

        if (!vc->mrail.srp.credits) {
            vc->mrail.srp.credits = MPIU_Malloc
                (sizeof *vc->mrail.srp.credits * vc->mrail.num_rails);
            if (!vc->mrail.srp.credits) {
                MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_OTHER, goto err_cq,
                        "**fail", "**fail %s",
                        "Failed to allocate resources for "
                        "credits array");
            }
            MPIU_Memset(vc->mrail.srp.credits, 0,
                    (sizeof *vc->mrail.srp.credits * vc->mrail.num_rails));
        }

        if (!qp_required(vc, pg_rank, i)) {
            continue;
        }
#ifdef RDMA_CM
        if (proc->use_rdma_cm)
            continue;
#endif
        for (rail_index = 0; rail_index < vc->mrail.num_rails; rail_index++) {
            hca_index = rail_index / (vc->mrail.num_rails / rdma_num_hcas);
            port_index = (rail_index / (vc->mrail.num_rails / (rdma_num_hcas *
                                                               rdma_num_ports)))
                % rdma_num_ports;
            MPIU_Memset(&attr, 0, sizeof attr);
            attr.cap.max_send_wr = rdma_default_max_send_wqe;

            if (mv2_MPIDI_CH3I_RDMA_Process.link_layer[hca_index][port_index] == IBV_LINK_LAYER_ETHERNET) {
                vc->mrail.rails[rail_index].is_roce = 1;
                vc->mrail.rails[rail_index].gid_index = proc->gid_index[hca_index][port_index];
                PRINT_DEBUG(DEBUG_INIT_verbose, "Automatically enabling RoCE mode, gid_index = %d\n",
                            vc->mrail.rails[rail_index].gid_index);
            } else {
                vc->mrail.rails[rail_index].is_roce = 0;
            }
            if (proc->has_srq) {
                attr.cap.max_recv_wr = 0;
                attr.srq = proc->srq_hndl[hca_index];
            } else {
                attr.cap.max_recv_wr = rdma_default_max_recv_wqe;
            }

            attr.cap.max_send_sge = rdma_default_max_sg_list;
            attr.cap.max_recv_sge = rdma_default_max_sg_list;
            attr.cap.max_inline_data = rdma_max_inline_size;
            attr.send_cq = proc->cq_hndl[hca_index];
            attr.recv_cq = proc->cq_hndl[hca_index];
            attr.qp_type = IBV_QPT_RC;
            attr.sq_sig_all = 0;

            #ifdef _ENABLE_HOLLOW_RC_
            vc->mrail.rails[rail_index].qp_hndl =
                mv2_create_hollow_qp(proc, hca_index, &attr);
            #else
            vc->mrail.rails[rail_index].qp_hndl =
                ibv_ops.create_qp(proc->ptag[hca_index], &attr);
            #endif

            if (!vc->mrail.rails[rail_index].qp_hndl) {
                MPIR_ERR_SETFATALANDSTMT2(mpi_errno, MPI_ERR_OTHER, goto err_cq,
                                          "**fail", "%s%d",
                                          "Failed to create qp for rank ", i);
            }
            rdma_max_inline_size = attr.cap.max_inline_data;

            vc->mrail.rails[rail_index].nic_context =
                proc->nic_context[hca_index];
            vc->mrail.rails[rail_index].hca_index = hca_index;
            vc->mrail.rails[rail_index].port = ports[hca_index][port_index];
            vc->mrail.rails[rail_index].lid = lids[hca_index][port_index];
            vc->mrail.rails[rail_index].gid = gids[hca_index][port_index];
            vc->mrail.rails[rail_index].cq_hndl = proc->cq_hndl[hca_index];
            vc->mrail.rails[rail_index].send_cq_hndl = NULL;
            vc->mrail.rails[rail_index].recv_cq_hndl = NULL;

            if (info) {
                info->lid[i][rail_index] = lids[hca_index][port_index];
                info->gid[i][rail_index] = gids[hca_index][port_index];
                info->arch_hca_type[i] = proc->arch_hca_type;
                info->qp_num_rdma[i][rail_index] =
                    vc->mrail.rails[rail_index].qp_hndl->qp_num;
#ifdef _ENABLE_HOLLOW_RC_
                info->hollow_srqn[i][rail_index] =
                    proc->hollow_srqn[hca_index];
#endif
                info->vc_addr[i] = (uintptr_t) vc;
                DEBUG_PRINT("[%d->%d from %d] vc %p\n", i, pg_rank, pg_rank,
                            vc);
                DEBUG_PRINT("Setting hca type %d\n", info->arch_hca_type[i]);
            }

            qp_attr.qp_state = IBV_QPS_INIT;
            if (g_atomics_support) {
                qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                IBV_ACCESS_REMOTE_ATOMIC;
#ifdef INFINIBAND_VERBS_EXP_H
                if (g_atomics_support_be) {
                    qp_attr.qp_access_flags |= IBV_EXP_QP_CREATE_ATOMIC_BE_REPLY;
                }
#endif  /* INFINIBAND_VERBS_EXP_H */
            } else {
                qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
            }

            qp_attr.port_num = ports[hca_index][port_index];
            set_pkey_index(&qp_attr.pkey_index, hca_index, qp_attr.port_num);

            if (ibv_ops.modify_qp(vc->mrail.rails[rail_index].qp_hndl, &qp_attr,
                              IBV_QP_STATE |
                              IBV_QP_PKEY_INDEX |
                              IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
                MPIR_ERR_SETFATALANDSTMT1(mpi_errno, MPI_ERR_OTHER, goto err_cq,
                                          "**fail", "**fail %s",
                                          "Failed to modify QP to INIT");
            }
        }
    }

#ifdef RDMA_CM
    if (proc->use_rdma_cm) {
        if ((mpi_errno =
             ib_init_rdma_cm(proc, pg_rank, pg_size)) != MPI_SUCCESS) {
            MPIR_ERR_POP(mpi_errno);
        }
    }
#endif

    DEBUG_PRINT("Return from init hca\n");

  fn_exit:
    return mpi_errno;

  err_cq:
    for (i = 0; i < rdma_num_hcas; ++i) {
        if (proc->cq_hndl[i])
            ibv_ops.destroy_cq(proc->cq_hndl[i]);
    }

  err:
    for (i = 0; i < rdma_num_hcas; i++) {
        if (proc->ptag[i])
            ibv_ops.dealloc_pd(proc->ptag[i]);
    }

    for (i = 0; i < rdma_num_hcas; i++) {
        if (proc->nic_context[i])
            ibv_ops.close_device(proc->nic_context[i]);
    }

  fn_fail:
    goto fn_exit;
}

/* Allocate memory and handlers */
#undef FUNCNAME
#define FUNCNAME rdma_iba_allocate_memory
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int rdma_iba_allocate_memory(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc,
                         int pg_rank, int pg_size)
{
    int mpi_errno = MPI_SUCCESS;

    MPIDI_STATE_DECL(MPID_STATE_RDMA_IBA_ALLOCATE_MEMORY);
    MPIDI_FUNC_ENTER(MPID_STATE_RDMA_IBA_ALLOCATE_MEMORY);

    /* First allocate space for RDMA_FAST_PATH for every connection */
    /*
     * for (; i < pg_size; ++i)
     * {
     * MPIDI_PG_Get_vc(g_cached_pg, i, &vc);
     *
     * vc->mrail.rfp.phead_RDMA_send = 0;
     * vc->mrail.rfp.ptail_RDMA_send = 0;
     * vc->mrail.rfp.p_RDMA_recv = 0;
     * vc->mrail.rfp.p_RDMA_recv_tail = 0;
     * } */

    mv2_MPIDI_CH3I_RDMA_Process.polling_group_size = 0;

#ifdef _ENABLE_HOLLOW_RC_
    mv2_MPIDI_CH3I_RDMA_Process.hollow_retry_capacity = pg_size;
    mv2_MPIDI_CH3I_RDMA_Process.hollow_blocked_rails = 0;
    mv2_MPIDI_CH3I_RDMA_Process.hollow_retry_set =
        (MPIDI_VC_t **)MPIU_Malloc(pg_size * sizeof(MPIDI_VC_t *));
    if (!mv2_MPIDI_CH3I_RDMA_Process.hollow_retry_set) {
        ibv_error_abort(IBV_RETURN_ERR,
                        "Unable to allocate Hollow RC retry set\n");
    }
    MPIU_Memset(mv2_MPIDI_CH3I_RDMA_Process.hollow_retry_set, 0,
                pg_size * sizeof(MPIDI_VC_t *));
#endif

    if (rdma_polling_set_limit > 0) {
        mv2_MPIDI_CH3I_RDMA_Process.polling_set =
            (MPIDI_VC_t **) MPIU_Malloc(rdma_polling_set_limit *
                                        sizeof(MPIDI_VC_t *));
    } else {
        mv2_MPIDI_CH3I_RDMA_Process.polling_set =
            (MPIDI_VC_t **) MPIU_Malloc(pg_size * sizeof(MPIDI_VC_t *));
    }

    if (!mv2_MPIDI_CH3I_RDMA_Process.polling_set) {
        fprintf(stderr,
                "[%s:%d]: %s\n",
                __FILE__,
                __LINE__, "unable to allocate space for polling set\n");
        MPIR_ERR_SETFATALANDJUMP1(mpi_errno, MPI_ERR_OTHER, "**fail",
                                  "**fail %s", "unable to allocate space for polling set");
    }

    /* We need to allocate vbufs for send/recv path */
    if ((mpi_errno= allocate_vbufs(mv2_MPIDI_CH3I_RDMA_Process.ptag))) {
        MPIR_ERR_POP(mpi_errno);
    }
#ifdef _ENABLE_UD_
    if (rdma_enable_hybrid) {
        if ((mpi_errno = allocate_ud_vbufs())) {
            MPIR_ERR_POP(mpi_errno);
        }
    }
#endif /* _ENABLE_UD_ */
    /* See if we need to pre-allocate buffers for RDMA Fast Path */
    if (mv2_rdma_fast_path_preallocate_buffers) {
        if ((mpi_errno = mv2_preallocate_rdma_fp_bufs(mv2_MPIDI_CH3I_RDMA_Process.ptag))) {
            MPIR_ERR_POP(mpi_errno);
        }
    }

#ifdef _ENABLE_UD_
    /* post UD buffers for SRQ or non SRQ */
    if (rdma_enable_hybrid) { 
        if ((mpi_errno =
            rdma_ud_post_buffers(&mv2_MPIDI_CH3I_RDMA_Process)) != 
                MPI_SUCCESS) {
            MPIR_ERR_POP(mpi_errno);
        }
    }
#endif
    /* Post the buffers for the SRQ */
    if (mv2_MPIDI_CH3I_RDMA_Process.has_srq
#ifdef _ENABLE_UD_
        /* we still need this condition to init the locks and mutex's */
        || rdma_use_ud_srq
#endif
        ) { 
        int hca_num = 0;

        pthread_spin_init(&mv2_MPIDI_CH3I_RDMA_Process.srq_post_spin_lock, 0);
        pthread_spin_lock(&mv2_MPIDI_CH3I_RDMA_Process.srq_post_spin_lock);

        mv2_MPIDI_CH3I_RDMA_Process.is_finalizing = 0;

        for (hca_num = 0; hca_num < rdma_num_hcas; ++hca_num) {
            pthread_mutex_init(&mv2_MPIDI_CH3I_RDMA_Process.
                               srq_post_mutex_lock[hca_num], 0);
            pthread_cond_init(&mv2_MPIDI_CH3I_RDMA_Process.
                              srq_post_cond[hca_num], 0);
            mv2_MPIDI_CH3I_RDMA_Process.srq_zero_post_counter[hca_num] = 0;
            /* Create RC SRQ only if requested */
            if (mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
                mv2_MPIDI_CH3I_RDMA_Process.posted_bufs[hca_num] =
                    mv2_post_srq_buffers(mv2_srq_fill_size, hca_num);
                struct ibv_srq_attr srq_attr;
                srq_attr.max_wr = mv2_srq_alloc_size;
                srq_attr.max_sge = 1;
                srq_attr.srq_limit = mv2_srq_limit;

                if (ibv_ops.modify_srq
                    (mv2_MPIDI_CH3I_RDMA_Process.srq_hndl[hca_num], &srq_attr,
                     IBV_SRQ_LIMIT)) {
                    ibv_error_abort(IBV_RETURN_ERR,
                                    "Couldn't modify SRQ limit\n");
                }
            }
#ifdef _ENABLE_UD_
            /* Create UD SRQ if requested */
            if (rdma_use_ud_srq && rdma_enable_hybrid) {
                struct ibv_srq_attr srq_attr;
                srq_attr.max_wr = mv2_ud_srq_alloc_size;
                srq_attr.max_sge = 1;
                srq_attr.srq_limit = mv2_ud_srq_limit;
                if (ibv_ops.modify_srq(proc->ud_srq_hndl[hca_num], &srq_attr,
                                    IBV_SRQ_LIMIT)) {
                    ibv_error_abort(IBV_RETURN_ERR,
                                    "Couldn't modify SRQ limit\n");
                }
            }
#endif
        }
        pthread_spin_unlock(&mv2_MPIDI_CH3I_RDMA_Process.srq_post_spin_lock);
    }

    if (mv2_MPIDI_CH3I_RDMA_Process.has_srq
#ifdef _ENABLE_UD_
        || rdma_use_ud_srq
#endif
        ) { 
        int hca_num = 0; 
        pthread_spin_lock(&mv2_MPIDI_CH3I_RDMA_Process.srq_post_spin_lock);
            
        for (hca_num = 0; hca_num < rdma_num_hcas; ++hca_num) {
            /* Start the async thread which watches for SRQ limit events */
            pthread_attr_t attr;
            int stacksz_ret;
            if (pthread_attr_init(&attr)) {
                ibv_error_abort(IBV_RETURN_ERR,
                                "Couldn't init pthread_attr\n");
            }
            stacksz_ret = pthread_attr_setstacksize(&attr,
                                                    rdma_default_async_thread_stack_size);
            if (stacksz_ret && stacksz_ret != EINVAL) {
                ibv_error_abort(IBV_RETURN_ERR,
                                "Couldn't set pthread stack size\n");
            }
            pthread_create(&mv2_MPIDI_CH3I_RDMA_Process.
                           async_thread[hca_num], &attr,
                           (void *) async_thread,
                           (void *) mv2_MPIDI_CH3I_RDMA_Process.
                           nic_context[hca_num]);
            /* Destroy thread attributes object */
            mpi_errno = pthread_attr_destroy(&attr);
        }
        pthread_spin_unlock(&mv2_MPIDI_CH3I_RDMA_Process.srq_post_spin_lock);
    }
  fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_RDMA_IBA_ALLOCATE_MEMORY);
    return mpi_errno;
  
  fn_fail:
    goto fn_exit;
}

/*
 * TODO add error handling
 */

int
rdma_iba_enable_connections(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc,
                            int pg_rank, MPIDI_PG_t * pg,
                            struct process_init_info *info)
{
    struct ibv_qp_attr qp_attr;
    uint32_t qp_attr_mask = 0;
    int i, j;
    int rail_index, pg_size;
    static int rdma_qos_sl = 0;
    MPIDI_VC_t *vc;

    /**********************  INIT --> RTR  ************************/
    MPIU_Memset(&qp_attr, 0, sizeof qp_attr);
    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.rq_psn = rdma_default_psn;
#ifdef _ENABLE_XRC_
    if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity || USE_XRC)
#else
    if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity)
#endif
    {
        qp_attr.max_dest_rd_atomic = rdma_default_max_rdma_dst_ops;
    } else {
        qp_attr.max_dest_rd_atomic = rdma_supported_max_rdma_dst_ops;
    }

    qp_attr.min_rnr_timer = rdma_default_min_rnr_timer;
    if (rdma_use_qos) {
        qp_attr.ah_attr.sl = rdma_qos_sl;
        rdma_qos_sl = (rdma_qos_sl + 1) % rdma_qos_num_sls;
    } else {
        qp_attr.ah_attr.sl = rdma_default_service_level;
    }
    qp_attr.ah_attr.static_rate = rdma_default_static_rate;
    qp_attr.ah_attr.src_path_bits = rdma_default_src_path_bits;

    qp_attr_mask |= IBV_QP_STATE;
    qp_attr_mask |= IBV_QP_PATH_MTU;
    qp_attr_mask |= IBV_QP_RQ_PSN;
    qp_attr_mask |= IBV_QP_MAX_DEST_RD_ATOMIC;
    qp_attr_mask |= IBV_QP_MIN_RNR_TIMER;
    qp_attr_mask |= IBV_QP_AV;

    pg_size = MPIDI_PG_Get_size(pg);
    for (i = 0; i < pg_size; i++) {
        MPIDI_PG_Get_vc(pg, i, &vc);
        if (!qp_required(vc, pg_rank, i)) {
            continue;
        }

        vc->mrail.remote_vc_addr = info->vc_addr[i];
        DEBUG_PRINT("[%d->%d] from %d received vc %08llx\n",
                    pg_rank, i, pg_rank, info->vc_addr[i]);

        for (rail_index = 0; rail_index < rdma_num_rails; rail_index++) {
#ifdef _ENABLE_HOLLOW_RC_
            vc->mrail.rails[rail_index].hollow_remote_srqn =
                info->hollow_srqn[i][rail_index];
#endif
            if (use_iboeth || vc->mrail.rails[rail_index].is_roce) {
                qp_attr.ah_attr.grh.dgid.global.subnet_prefix = 0;
                qp_attr.ah_attr.grh.dgid.global.interface_id = 0;
                qp_attr.ah_attr.grh.flow_label = 0;
                qp_attr.ah_attr.grh.hop_limit = 1;
                qp_attr.ah_attr.grh.traffic_class = 0;
                qp_attr.ah_attr.grh.sgid_index = vc->mrail.rails[rail_index].gid_index;
                PRINT_DEBUG(DEBUG_INIT_verbose, "gid_index = %d\n", vc->mrail.rails[rail_index].gid_index);
                qp_attr.ah_attr.grh.dgid = info->gid[i][rail_index];
                qp_attr.ah_attr.is_global = 1;
                qp_attr.ah_attr.dlid = 0;
            } else {
                qp_attr.ah_attr.is_global = 0;
            }
            qp_attr.path_mtu = rdma_default_mtu;
            qp_attr.dest_qp_num = info->qp_num_rdma[i][rail_index];
            qp_attr.ah_attr.port_num = vc->mrail.rails[rail_index].port;
            qp_attr.ah_attr.dlid = info->lid[i][rail_index];

            /* If HSAM is enabled, include the source path bits and change
             * the destination LID accordingly */

            /* Both source and destination should have the same value of the
             * bits */

#ifdef _ENABLE_HSAM_
            if (mv2_MPIDI_CH3I_RDMA_Process.has_hsam) {
                qp_attr.ah_attr.src_path_bits = rail_index %
                    power_two(mv2_MPIDI_CH3I_RDMA_Process.lmc);
                qp_attr.ah_attr.dlid = info->lid[i][rail_index]
                    + rail_index % power_two(mv2_MPIDI_CH3I_RDMA_Process.lmc);
            }
#endif /*_ENABLE_HSAM_*/
            qp_attr_mask |= IBV_QP_DEST_QPN;

            if (!(use_iboeth || vc->mrail.rails[rail_index].is_roce)
                    && (rdma_3dtorus_support || rdma_path_sl_query)) {
                /* Path SL Lookup */
                int hca_index = rail_index /
                    (vc->mrail.num_rails / rdma_num_hcas);
                struct ibv_context *context =
                    vc->mrail.rails[rail_index].nic_context;
                struct ibv_pd *pd = proc->ptag[hca_index];
                uint16_t lid = vc->mrail.rails[rail_index].lid;
                uint16_t rem_lid = qp_attr.ah_attr.dlid;
                uint32_t port_num = qp_attr.ah_attr.port_num;
                qp_attr.ah_attr.sl = mv2_get_path_rec_sl(context, pd, port_num,
                                                         lid, rem_lid,
                                                         rdma_3dtorus_support,
                                                         rdma_num_sa_query_retries);
            }

            if (ibv_ops.modify_qp(vc->mrail.rails[rail_index].qp_hndl,
                              &qp_attr, qp_attr_mask)) {
                fprintf(stderr,
                        "[%s:%d] Could not modify qp to RTR: "
                        "rank=%d peer=%d rail=%d qpn=%u errno=%d (%s) "
                        "local_gid=%02x%02x:%02x%02x remote_gid=%02x%02x:%02x%02x\n",
                        __FILE__, __LINE__, pg_rank, i, rail_index,
                        vc->mrail.rails[rail_index].qp_hndl->qp_num,
                        errno, strerror(errno),
                        vc->mrail.rails[rail_index].gid.raw[12],
                        vc->mrail.rails[rail_index].gid.raw[13],
                        vc->mrail.rails[rail_index].gid.raw[14],
                        vc->mrail.rails[rail_index].gid.raw[15],
                        info->gid[i][rail_index].raw[12],
                        info->gid[i][rail_index].raw[13],
                        info->gid[i][rail_index].raw[14],
                        info->gid[i][rail_index].raw[15]);
                return 1;
            }
        }
    }

    /************** RTR --> RTS *******************/
    MPIU_Memset(&qp_attr, 0, sizeof qp_attr);
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = rdma_default_psn;
    qp_attr.timeout = rdma_default_time_out;
    qp_attr.retry_cnt = rdma_default_retry_count;
    qp_attr.rnr_retry = rdma_default_rnr_retry;
#ifdef _ENABLE_XRC_
    if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity || USE_XRC)
#else
    if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity)
#endif
    {
        qp_attr.max_rd_atomic = rdma_default_qp_ous_rd_atom;
    } else {
        qp_attr.max_rd_atomic = rdma_supported_max_qp_ous_rd_atom;
    }

    qp_attr_mask = 0;
    qp_attr_mask = IBV_QP_STATE |
        IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    for (i = 0; i < pg_size; i++) {
        MPIDI_PG_Get_vc(pg, i, &vc);

        if (!qp_required(vc, pg_rank, i)) {
            continue;
        }

        for (j = 0; j < rdma_num_rails - 1; j++) {
            vc->mrail.rails[j].s_weight = DYNAMIC_TOTAL_WEIGHT / rdma_num_rails;
        }

        vc->mrail.rails[rdma_num_rails - 1].s_weight =
            DYNAMIC_TOTAL_WEIGHT -
            (DYNAMIC_TOTAL_WEIGHT / rdma_num_rails) * (rdma_num_rails - 1);

        for (rail_index = 0; rail_index < rdma_num_rails; rail_index++) {
            if (ibv_ops.modify_qp(vc->mrail.rails[rail_index].qp_hndl, &qp_attr,
                              qp_attr_mask)) {
                fprintf(stderr, "[%s:%d] Could not modify rdma qp to RTS\n",
                        __FILE__, __LINE__);
                return 1;
            }

            if (mv2_MPIDI_CH3I_RDMA_Process.has_apm) {
                reload_alternate_path(vc->mrail.rails[rail_index].qp_hndl);
            }
        }
    }

    PRINT_DEBUG(DEBUG_INIT_verbose, "Done enabling connections\n");
    return 0;
}

void MRAILI_RC_Enable(MPIDI_VC_t * vc)
{
    int i, k;
    PRINT_DEBUG(DEBUG_UD_verbose > 0,
                "Enabled to create RC Channel to rank:%d\n", vc->pg_rank);
    vc->mrail.state |= MRAILI_RC_CONNECTED;
    for (i = 0; i < vc->mrail.num_rails; i++) {
        if (!mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
            for (k = 0; k < rdma_initial_prepost_depth; k++) {
                PREPOST_VBUF_RECV(vc, i);
            }
        }
    }
#ifdef _ENABLE_UD_
    if (rdma_enable_hybrid) {
        vc->mrail.rfp.eager_start_cnt = 0;
        mv2_MPIDI_CH3I_RDMA_Process.rc_connections++;
        rdma_hybrid_pending_rc_conn--;
        MPIU_Assert(vc->mrail.state & MRAILI_RC_CONNECTING);
        if (mv2_use_eager_fast_send) {
            vc->eager_fast_max_msg_sz = MIN(DEFAULT_MEDIUM_VBUF_SIZE, rdma_fp_buffer_size);
        } else {
            vc->eager_fast_max_msg_sz = 0;
        }
    }
#endif
}

void MRAILI_Init_vc(MPIDI_VC_t * vc)
{
    int pg_size;
    int i, k;

#ifdef _ENABLE_XRC_
    if (USE_XRC) {
        if (VC_XST_ISSET(vc, XF_INIT_DONE))
            return;
        else
            VC_XST_SET(vc, XF_INIT_DONE);
    }
    PRINT_DEBUG(DEBUG_XRC_verbose > 0, "MRAILI_Init_vc %d\n", vc->pg_rank);
#endif
#ifdef _ENABLE_UD_
    if(rdma_enable_hybrid)
    {
            if (vc->mrail.state & MRAILI_UD_CONNECTED) {
                    MRAILI_RC_Enable(vc);
                    return;
            }
    }
#endif

    UPMI_GET_SIZE(&pg_size);

    vc->mrail.rfp.phead_RDMA_send = 0;
    vc->mrail.rfp.ptail_RDMA_send = 0;
    vc->mrail.rfp.p_RDMA_recv = 0;
    vc->mrail.rfp.p_RDMA_recv_tail = 0;
    vc->mrail.rfp.rdma_failed = 0;
    vc->mrail.num_rails = rdma_num_rails;

    if (!vc->mrail.rails) {
        vc->mrail.rails = MPIU_Malloc
            (sizeof *vc->mrail.rails * vc->mrail.num_rails);
        if (!vc->mrail.rails) {
            ibv_error_abort(GEN_EXIT_ERR,
                            "Fail to allocate resources for multirails\n");
        }
        MPIU_Memset(vc->mrail.rails, 0,
                    (sizeof *vc->mrail.rails * vc->mrail.num_rails));
    }

    if (!vc->mrail.srp.credits) {
        vc->mrail.srp.credits = MPIU_Malloc(sizeof(*vc->mrail.srp.credits) *
                                            vc->mrail.num_rails);
        if (!vc->mrail.srp.credits) {
            ibv_error_abort(GEN_EXIT_ERR,
                            "Fail to allocate resources for credits array\n");
        }
        MPIU_Memset(vc->mrail.srp.credits, 0,
                    (sizeof(*vc->mrail.srp.credits) * vc->mrail.num_rails));
    }

    /* Now we will need to */
    for (i = 0; i < rdma_num_rails; i++) {
        vc->mrail.rails[i].send_wqes_avail = rdma_default_max_send_wqe;
        vc->mrail.rails[i].ext_sendq_head = NULL;
        vc->mrail.rails[i].ext_sendq_tail = NULL;
        vc->mrail.rails[i].ext_sendq_size = 0;
        vc->mrail.rails[i].used_send_cq = 0;
        vc->mrail.rails[i].used_recv_cq = 0;
#ifdef _ENABLE_XRC_
        vc->mrail.rails[i].hca_index = i / (rdma_num_rails / rdma_num_hcas);
#endif
    }

    vc->mrail.outstanding_eager_vbufs = 0;
    vc->mrail.coalesce_vbuf = NULL;

    vc->mrail.rfp.rdma_credit = 0;
#ifndef MV2_DISABLE_HEADER_CACHING
    vc->mrail.rfp.cached_miss = 0;
    vc->mrail.rfp.cached_hit = 0;
    vc->mrail.rfp.cached_incoming = MPIU_Malloc(sizeof(MPIDI_CH3_Pkt_send_t));
    vc->mrail.rfp.cached_outgoing = MPIU_Malloc(sizeof(MPIDI_CH3_Pkt_send_t));
    MPIU_Memset(vc->mrail.rfp.cached_outgoing, 0, sizeof(MPIDI_CH3_Pkt_send_t));
    MPIU_Memset(vc->mrail.rfp.cached_incoming, 0, sizeof(MPIDI_CH3_Pkt_send_t));
#endif

    vc->mrail.cmanager.num_channels = vc->mrail.num_rails;
    vc->mrail.cmanager.num_local_pollings = 0;

    if (pg_size < rdma_eager_limit && !MPIDI_CH3I_Process.has_dpm) {
        vc->mrail.rfp.eager_start_cnt = rdma_polling_set_threshold + 1;
    } else {
        vc->mrail.rfp.eager_start_cnt = 0;
    }

    vc->mrail.rfp.in_polling_set = 0;

    /* extra one channel for later increase the adaptive rdma */
    vc->mrail.cmanager.msg_channels = MPIU_Malloc
        (sizeof *vc->mrail.cmanager.msg_channels
         * (vc->mrail.cmanager.num_channels + 1));
    if (!vc->mrail.cmanager.msg_channels) {
        ibv_error_abort(GEN_EXIT_ERR, "No resource for msg channels\n");
    }
    MPIU_Memset(vc->mrail.cmanager.msg_channels, 0,
                sizeof *vc->mrail.cmanager.msg_channels
                * (vc->mrail.cmanager.num_channels + 1));

    vc->mrail.cmanager.next_arriving = NULL;
    vc->mrail.cmanager.inqueue = 0;
    vc->mrail.cmanager.vc = (void *) vc;

    DEBUG_PRINT("Cmanager total channel %d, local polling %d\n",
                vc->mrail.cmanager.num_channels,
                vc->mrail.cmanager.num_local_pollings);

    vc->mrail.sreq_head = NULL;
    vc->mrail.sreq_tail = NULL;
    vc->mrail.nextflow = NULL;
    vc->mrail.inflow = 0;
#if defined (_ENABLE_CUDA_) && defined(HAVE_CUDA_IPC)
    vc->mrail.device_ipc_sreq_head = NULL;
    vc->mrail.device_ipc_sreq_tail = NULL;
#endif

    for (i = 0; i < vc->mrail.num_rails; i++) {
        if (!mv2_MPIDI_CH3I_RDMA_Process.has_srq
#ifdef _ENABLE_UD_
            && !rdma_enable_hybrid
#endif
) {
            for (k = 0; k < rdma_initial_prepost_depth; k++) {
                PREPOST_VBUF_RECV(vc, i);
            }
        }

        vc->mrail.srp.credits[i].remote_credit = rdma_initial_credits;
        vc->mrail.srp.credits[i].remote_cc = rdma_initial_credits;
        vc->mrail.srp.credits[i].local_credit = 0;
        vc->mrail.srp.credits[i].preposts = rdma_initial_prepost_depth;

        if (!mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
            vc->mrail.srp.credits[i].initialized =
                (rdma_prepost_depth == rdma_initial_prepost_depth);
        } else {
            vc->mrail.srp.credits[i].initialized = 1;
            vc->mrail.srp.credits[i].pending_r3_sends = 0;
        }

        vc->mrail.srp.credits[i].backlog.len = 0;
        vc->mrail.srp.credits[i].backlog.vbuf_head = NULL;
        vc->mrail.srp.credits[i].backlog.vbuf_tail = NULL;

        vc->mrail.srp.credits[i].rendezvous_packets_expected = 0;
    }

    for (i = 0; i < rdma_num_rails - 1; i++) {
        vc->mrail.rails[i].s_weight = DYNAMIC_TOTAL_WEIGHT / rdma_num_rails;
    }

    vc->mrail.rails[rdma_num_rails - 1].s_weight =
        DYNAMIC_TOTAL_WEIGHT -
        (DYNAMIC_TOTAL_WEIGHT / rdma_num_rails) * (rdma_num_rails - 1);

    vc->mrail.seqnum_next_tosend = 0;
    vc->mrail.seqnum_next_torecv = 0;
    vc->mrail.seqnum_next_toack = UINT16_MAX;
#ifdef _ENABLE_UD_
    if (rdma_enable_hybrid && !(vc->mrail.state & MRAILI_UD_CONNECTED)) {

        vc->mrail.ud = NULL;
        vc->mrail.rely.total_messages = 0;
        vc->mrail.rely.ack_pending = 0;

        vc->mrail.state &= ~(MRAILI_RC_CONNECTING | MRAILI_RC_CONNECTING);

        MESSAGE_QUEUE_INIT(&vc->mrail.rely.send_window);
        MESSAGE_QUEUE_INIT(&vc->mrail.rely.ext_window);
        vc->mrail.rely.cntl_acks = 0;
        vc->mrail.rely.resend_count = 0;
        vc->mrail.rely.ext_win_send_count = 0;
    } else
#endif /* _ENABLE_UD_ */
    {
        vc->mrail.state |= MRAILI_RC_CONNECTED;
    }
}

#ifdef _ENABLE_XRC_
int cm_qp_reuse(MPIDI_VC_t * vc, MPIDI_VC_t * orig)
{
    int hca_index = 0;
    int port_index = 0;
    int rail_index = 0;

    VC_XST_SET(vc, XF_INDIRECT_CONN);
    vc->ch.orig_vc = orig;

    vc->mrail.num_rails = rdma_num_rails;
    if (!vc->mrail.rails) {
        vc->mrail.rails = MPIU_Malloc
            (sizeof *vc->mrail.rails * vc->mrail.num_rails);
        if (!vc->mrail.rails) {
            ibv_error_abort(GEN_EXIT_ERR,
                            "Fail to allocate resources for multirails\n");
        }
        MPIU_Memset(vc->mrail.rails, 0,
                    (sizeof *vc->mrail.rails * vc->mrail.num_rails));
    }

    if (!vc->mrail.srp.credits) {
        vc->mrail.srp.credits = MPIU_Malloc(sizeof *vc->mrail.srp.credits *
                                            vc->mrail.num_rails);
        if (!vc->mrail.srp.credits) {
            ibv_error_abort(GEN_EXIT_ERR,
                            "Fail to allocate resources for credits array\n");
        }
        MPIU_Memset(vc->mrail.srp.credits, 0,
                    (sizeof(*vc->mrail.srp.credits) * vc->mrail.num_rails));
    }

    for (rail_index = 0; rail_index < vc->mrail.num_rails; rail_index++) {
        hca_index = rail_index / (vc->mrail.num_rails / rdma_num_hcas);
        port_index = (rail_index / (vc->mrail.num_rails / (rdma_num_hcas *
                                                           rdma_num_ports))) %
            rdma_num_ports;

        if (mv2_MPIDI_CH3I_RDMA_Process.link_layer[hca_index][port_index] == IBV_LINK_LAYER_ETHERNET) {
            vc->mrail.rails[rail_index].is_roce = 1;
            vc->mrail.rails[rail_index].gid_index = mv2_MPIDI_CH3I_RDMA_Process.gid_index[hca_index][port_index];
            PRINT_DEBUG(DEBUG_INIT_verbose, "Automatically enabling RoCE mode, gid_index = %d\n",
                    vc->mrail.rails[rail_index].gid_index);
        } else {
            vc->mrail.rails[rail_index].is_roce = 0;
        }
        vc->mrail.rails[rail_index].qp_hndl =
            orig->mrail.rails[rail_index].qp_hndl;
        vc->mrail.rails[rail_index].nic_context =
            mv2_MPIDI_CH3I_RDMA_Process.nic_context[hca_index];
        vc->mrail.rails[rail_index].hca_index = hca_index;
        vc->mrail.rails[rail_index].port =
            mv2_MPIDI_CH3I_RDMA_Process.ports[hca_index][port_index];
        if (use_iboeth || vc->mrail.rails[rail_index].is_roce) {
            vc->mrail.rails[rail_index].gid =
                mv2_MPIDI_CH3I_RDMA_Process.gids[hca_index][port_index];
        } else {
            vc->mrail.rails[rail_index].lid =
                mv2_MPIDI_CH3I_RDMA_Process.lids[hca_index][port_index];
        }
        vc->mrail.rails[rail_index].cq_hndl =
            mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_index];
        vc->mrail.rails[rail_index].send_cq_hndl = NULL;
        vc->mrail.rails[rail_index].recv_cq_hndl = NULL;
    }

    cm_send_xrc_cm_msg(vc, orig);
    return 0;
}
#endif /* _ENABLE_XRC_ */

static inline int cm_qp_conn_create(MPIDI_VC_t * vc, int qptype)
{
    struct ibv_qp_init_attr attr;
    struct ibv_qp_attr qp_attr;

    int hca_index = 0;
    int port_index = 0;
    int rail_index = 0;


    vc->mrail.num_rails = rdma_num_rails;
#if defined _ENABLE_XRC_ || _ENABLE_UD_
    if (!vc->mrail.rails) {
#endif
        vc->mrail.rails = MPIU_Malloc
            (sizeof *vc->mrail.rails * vc->mrail.num_rails);

        if (!vc->mrail.rails) {
            ibv_error_abort(GEN_EXIT_ERR,
                            "Fail to allocate resources for multirails\n");
        }
        MPIU_Memset(vc->mrail.rails, 0,
                    (sizeof *vc->mrail.rails * vc->mrail.num_rails));
#if defined _ENABLE_XRC_ || _ENABLE_UD_
    }
#endif

#if defined _ENABLE_XRC_ || _ENABLE_UD_
    if (!vc->mrail.srp.credits) {
#endif
        vc->mrail.srp.credits = MPIU_Malloc(sizeof *vc->mrail.srp.credits *
                                            vc->mrail.num_rails);
        if (!vc->mrail.srp.credits) {
            ibv_error_abort(GEN_EXIT_ERR,
                            "Fail to allocate resources for credits array\n");
        }
        MPIU_Memset(vc->mrail.srp.credits, 0,
                    (sizeof(*vc->mrail.srp.credits) * vc->mrail.num_rails));
#if defined _ENABLE_XRC_ || _ENABLE_UD_
    }
#endif

    for (rail_index = 0; rail_index < vc->mrail.num_rails; rail_index++) {
        hca_index = rail_index / (vc->mrail.num_rails / rdma_num_hcas);
        port_index = (rail_index / (vc->mrail.num_rails / (rdma_num_hcas *
                                                           rdma_num_ports))) %
            rdma_num_ports;
        if (mv2_MPIDI_CH3I_RDMA_Process.link_layer[hca_index][port_index] == IBV_LINK_LAYER_ETHERNET) {
            vc->mrail.rails[rail_index].is_roce = 1;
            vc->mrail.rails[rail_index].gid_index =
                    mv2_MPIDI_CH3I_RDMA_Process.gid_index[hca_index][port_index];
            PRINT_DEBUG(DEBUG_INIT_verbose, "Automatically enabling RoCE mode, gid_index = %d\n",
                    vc->mrail.rails[rail_index].gid_index);
        } else {
            vc->mrail.rails[rail_index].is_roce = 0;
        }
        MPIU_Memset(&attr, 0, sizeof(attr));
        attr.cap.max_send_wr = rdma_default_max_send_wqe;
#ifdef _ENABLE_XRC_
        if (USE_XRC && qptype == MV2_QPT_XRC) {
            attr.xrc_domain = mv2_MPIDI_CH3I_RDMA_Process.xrc_domain[hca_index];
            MPIU_Assert(attr.xrc_domain != NULL);
            attr.qp_type = IBV_QPT_XRC;
            attr.srq = NULL;
            attr.cap.max_recv_wr = 0;
        } else
#endif
        {
            if (mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
                attr.cap.max_recv_wr = 0;
                attr.srq = mv2_MPIDI_CH3I_RDMA_Process.srq_hndl[hca_index];
            } else {
                attr.cap.max_recv_wr = rdma_default_max_recv_wqe;
            }
            attr.qp_type = IBV_QPT_RC;
        }
        attr.cap.max_send_sge = rdma_default_max_sg_list;
        attr.cap.max_recv_sge = rdma_default_max_sg_list;
        attr.cap.max_inline_data = rdma_max_inline_size;
        attr.send_cq = mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_index];
        attr.recv_cq = mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_index];
        attr.sq_sig_all = 0;

        #ifdef _ENABLE_HOLLOW_RC_
        vc->mrail.rails[rail_index].qp_hndl = mv2_create_hollow_qp(
            &mv2_MPIDI_CH3I_RDMA_Process, hca_index, &attr);
        #else
        vc->mrail.rails[rail_index].qp_hndl =
            ibv_ops.create_qp(mv2_MPIDI_CH3I_RDMA_Process.ptag[hca_index], &attr);
        #endif

        if (!vc->mrail.rails[rail_index].qp_hndl) {
            ibv_va_error_abort(GEN_EXIT_ERR, "Failed to create QP. "
                                "Error: %d (%s)\n", errno, strerror(errno));
        }
        rdma_max_inline_size = attr.cap.max_inline_data;

        vc->mrail.rails[rail_index].nic_context =
            mv2_MPIDI_CH3I_RDMA_Process.nic_context[hca_index];
        vc->mrail.rails[rail_index].hca_index = hca_index;
        vc->mrail.rails[rail_index].port =
            mv2_MPIDI_CH3I_RDMA_Process.ports[hca_index][port_index];
        if (use_iboeth || vc->mrail.rails[rail_index].is_roce) {
            vc->mrail.rails[rail_index].gid =
                mv2_MPIDI_CH3I_RDMA_Process.gids[hca_index][port_index];
        }
        vc->mrail.rails[rail_index].lid =
            mv2_MPIDI_CH3I_RDMA_Process.lids[hca_index][port_index];
        vc->mrail.rails[rail_index].cq_hndl =
            mv2_MPIDI_CH3I_RDMA_Process.cq_hndl[hca_index];
        vc->mrail.rails[rail_index].send_cq_hndl = NULL;
        vc->mrail.rails[rail_index].recv_cq_hndl = NULL;

        qp_attr.qp_state = IBV_QPS_INIT;
        if (g_atomics_support) {
            qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
            IBV_ACCESS_REMOTE_ATOMIC;
#ifdef INFINIBAND_VERBS_EXP_H
            if (g_atomics_support_be) {
                qp_attr.qp_access_flags |= IBV_EXP_QP_CREATE_ATOMIC_BE_REPLY;
            }
#endif  /* INFINIBAND_VERBS_EXP_H */
        } else {
            qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
        }

        qp_attr.port_num =
            mv2_MPIDI_CH3I_RDMA_Process.ports[hca_index][port_index];
        set_pkey_index(&qp_attr.pkey_index, hca_index, qp_attr.port_num);

        if (ibv_ops.modify_qp(vc->mrail.rails[rail_index].qp_hndl, &qp_attr,
                          IBV_QP_STATE |
                          IBV_QP_PKEY_INDEX |
                          IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
            ibv_error_abort(GEN_EXIT_ERR, "Failed to modify QP to INIT\n");
        }
    }

#ifdef _ENABLE_XRC_
    if (USE_XRC && qptype == MV2_QPT_XRC) {
        PRINT_DEBUG(DEBUG_XRC_verbose > 0, "Added vc to XRC hash\n");
        add_vc_xrc_hash(vc);
    }
#endif /* _ENABLE_XRC_ */
    return 0;
}

/*function to create qps for the connection and move them to INIT state*/
int cm_qp_create(MPIDI_VC_t * vc, int force, int qptype)
{

#ifdef _ENABLE_XRC_
    int match = 0;
    int rail_index = 0;
    int hca_index = 0;
    int port_index = 0;

    PRINT_DEBUG(DEBUG_XRC_verbose > 0, "Talking to %d (force:%d)\n",
                vc->pg_rank, force);
    if (USE_XRC && !force && qptype == MV2_QPT_XRC) {
        int hash;
        xrc_hash_t *iter;

        if (VC_XST_ISSET(vc, XF_REUSE_WAIT)) {
            PRINT_DEBUG(DEBUG_XRC_verbose > 0, "Already waiting for REUSE %d\n",
                        vc->pg_rank);
            return MV2_QP_REUSE;
        }

        hash = compute_xrc_hash(vc->smp.hostid);
        iter = xrc_hash[hash];

        while (iter) {
            if (iter->vc->smp.hostid == vc->smp.hostid &&
                VC_XST_ISUNSET(vc, XF_CONN_CLOSING)) {

                /* Check if processes use same HCA */
                for (rail_index = 0; rail_index < vc->mrail.num_rails;
                     rail_index++) {
                    hca_index = rail_index /
                        (vc->mrail.num_rails / rdma_num_hcas);
                    port_index = (rail_index / (vc->mrail.num_rails /
                                                (rdma_num_hcas *
                                                 rdma_num_ports))) %
                        rdma_num_ports;

                    PRINT_DEBUG(DEBUG_XRC_verbose > 0,
                                "rail_index = %d, old lid = %d, new lid = %d\n",
                                rail_index,
                                iter->vc->mrail.lid[hca_index][port_index],
                                vc->mrail.lid[hca_index][port_index]);
                    if ((vc->mrail.lid[hca_index][port_index] > 0) &&
                        (vc->mrail.lid[hca_index][port_index] ==
                         iter->vc->mrail.lid[hca_index][port_index])) {
                        /* LID is valid and there is a match
                         * i.e Both VC's use same HCA, so we can re-use QP
                         */
                        match = 1;
                        break;
                    } else if ((use_iboeth  || vc->mrail.rails[rail_index].is_roce)
                                      && memcmp(&vc->mrail.gid[hca_index][port_index],
                                      &iter->vc->mrail.
                                      gid[hca_index][port_index],
                                      sizeof(union ibv_gid))) {
                        /* We're using RoCE mode. Check for GID's instead of
                         * LID's. As above if GID's match we can re-use QP
                         */
                        match = 1;
                        break;
                    }
                }

                if (!match) {
                    /* Cannot re-use QP. Need to create a new one */
                    PRINT_DEBUG(DEBUG_XRC_verbose > 0,
                                "Cannot reuse QP to talk to %d\n", vc->pg_rank);
                    break;
                }

                PRINT_DEBUG(DEBUG_XRC_verbose > 0,
                            "Talking to %d Reusing conn to %d XST: 0x%08x\n",
                            vc->pg_rank, iter->vc->pg_rank,
                            iter->vc->ch.xrc_flags);
                MPIU_Assert(vc->smp.hostid != -1);
                if (VC_XST_ISSET(iter->vc, XF_SEND_CONNECTING)) {
                    xrc_pending_conn_t *n;
                    VC_XST_SET(vc, XF_REUSE_WAIT);

                    n = (xrc_pending_conn_t *) MPIU_Malloc(xrc_pending_conn_s);
                    n->vc = vc;
                    n->next = iter->vc->ch.xrc_conn_queue;
                    iter->vc->ch.xrc_conn_queue = n;
                    PRINT_DEBUG(DEBUG_XRC_verbose > 0,
                                "Added %d to pending queue of %d \n",
                                vc->pg_rank, iter->vc->pg_rank);
                } else {
                    cm_qp_reuse(vc, iter->vc);
                }
                return MV2_QP_REUSE;
            } else {
                iter = iter->next;
            }
        }
        PRINT_DEBUG(DEBUG_XRC_verbose > 0, "Not FOUND!\n");
    }
#endif /* _ENABLE_XRC_ */
    /* XRC not in use or no qps found */
    cm_qp_conn_create(vc, qptype);
    return MV2_QP_NEW;
}


/*function to move qps to rtr and prepost buffers*/
int cm_qp_move_to_rtr(MPIDI_VC_t * vc, uint16_t * lids, union ibv_gid *gids,
                      uint32_t * qpns, int is_rqp, uint32_t * rqpn, int is_dpm)
{
    struct ibv_qp_attr qp_attr;
    uint32_t qp_attr_mask = 0;
    int rail_index, hca_index;
    static int rdma_qos_sl = 0;

    for (rail_index = 0; rail_index < rdma_num_rails; rail_index++) {
        hca_index = rail_index / (vc->mrail.num_rails / rdma_num_hcas);
        MPIU_Memset(&qp_attr, 0, sizeof qp_attr);
        qp_attr.qp_state = IBV_QPS_RTR;
        qp_attr.rq_psn = rdma_default_psn;
#ifdef _ENABLE_XRC_
        if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity || USE_XRC)
#else
        if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity)
#endif
        {
            qp_attr.max_dest_rd_atomic = rdma_default_max_rdma_dst_ops;
        } else {
            qp_attr.max_dest_rd_atomic = rdma_supported_max_rdma_dst_ops;
        }
        qp_attr.min_rnr_timer = rdma_default_min_rnr_timer;
        if (rdma_use_qos) {
            qp_attr.ah_attr.sl = rdma_qos_sl;
            rdma_qos_sl = (rdma_qos_sl + 1) % rdma_qos_num_sls;
        } else {
            qp_attr.ah_attr.sl = rdma_default_service_level;
        }
        qp_attr.ah_attr.static_rate = rdma_default_static_rate;
        qp_attr.ah_attr.src_path_bits = rdma_default_src_path_bits;
        qp_attr.ah_attr.port_num = vc->mrail.rails[rail_index].port;

        if (use_iboeth || vc->mrail.rails[rail_index].is_roce) {
            qp_attr.ah_attr.grh.dgid.global.subnet_prefix = 0;
            qp_attr.ah_attr.grh.dgid.global.interface_id = 0;
            qp_attr.ah_attr.grh.flow_label = 0;
            qp_attr.ah_attr.grh.sgid_index = vc->mrail.rails[rail_index].gid_index;
            PRINT_DEBUG(DEBUG_INIT_verbose, "gid_index = %d\n", vc->mrail.rails[rail_index].gid_index);
            qp_attr.ah_attr.grh.hop_limit = 1;
            qp_attr.ah_attr.grh.traffic_class = 0;
            qp_attr.ah_attr.is_global = 1;
            qp_attr.ah_attr.grh.dgid = gids[rail_index];
        } else {
            qp_attr.ah_attr.is_global = 0;
        }
        qp_attr.path_mtu = rdma_default_mtu;
        qp_attr.ah_attr.dlid = lids[rail_index];

#ifdef _ENABLE_XRC_
        if (USE_XRC && !is_rqp && !is_dpm) {
            /* Move send qp to RTR */
            qp_attr.dest_qp_num = vc->ch.xrc_rqpn[rail_index];
            PRINT_DEBUG(DEBUG_XRC_verbose > 0, "%d dlid: %d\n",
                        vc->ch.xrc_rqpn[rail_index], lids[rail_index]);
        } else  /* Move rcv qp to RTR, or no XRC */
#endif
        {
            qp_attr.dest_qp_num = qpns[rail_index];
            PRINT_DEBUG(DEBUG_XRC_verbose > 0, "DQPN: %d dlid: %d\n",
                        rail_index[qpns], lids[rail_index]);
        }

#ifdef _ENABLE_HSAM_
        if (mv2_MPIDI_CH3I_RDMA_Process.has_hsam) {
            qp_attr.ah_attr.src_path_bits = rdma_default_src_path_bits
                + rail_index % power_two(mv2_MPIDI_CH3I_RDMA_Process.lmc);
            qp_attr.ah_attr.dlid = lids[rail_index] + rail_index %
                power_two(mv2_MPIDI_CH3I_RDMA_Process.lmc);
        }
#endif /*_ENABLE_HSAM_*/
        qp_attr_mask |= IBV_QP_STATE;
        qp_attr_mask |= IBV_QP_PATH_MTU;
        qp_attr_mask |= IBV_QP_RQ_PSN;
        qp_attr_mask |= IBV_QP_MAX_DEST_RD_ATOMIC;
        qp_attr_mask |= IBV_QP_MIN_RNR_TIMER;
        qp_attr_mask |= IBV_QP_AV;
        qp_attr_mask |= IBV_QP_DEST_QPN;

        if (!(use_iboeth || vc->mrail.rails[rail_index].is_roce)
              && (rdma_3dtorus_support || rdma_path_sl_query)) {
            /* Path SL Lookup */
            struct ibv_context *context =
                vc->mrail.rails[rail_index].nic_context;
            struct ibv_pd *pd = mv2_MPIDI_CH3I_RDMA_Process.ptag[hca_index];
            uint16_t lid = vc->mrail.rails[rail_index].lid;
            uint16_t rem_lid = qp_attr.ah_attr.dlid;
            uint32_t port_num = qp_attr.ah_attr.port_num;
            qp_attr.ah_attr.sl = mv2_get_path_rec_sl(context, pd, port_num, lid,
                                                     rem_lid,
                                                     rdma_3dtorus_support,
                                                     rdma_num_sa_query_retries);
        }

        /* fprintf(stderr, "!!!Modify qp %d with qpnum %08x, dlid %x, port %d\n",
         * rail_index, qp_attr.dest_qp_num, qp_attr.ah_attr.dlid,
         * qp_attr.ah_attr.port_num); */
#ifdef _ENABLE_XRC_
        if (USE_XRC && is_rqp) {
            /* Move rcv qp to RTR */
            PRINT_DEBUG(DEBUG_XRC_verbose > 0, "%d <-> %d\n",
                        rqpn[rail_index], qp_attr.dest_qp_num);
            if (ibv_ops.modify_xrc_rcv_qp
                (mv2_MPIDI_CH3I_RDMA_Process.xrc_domain[hca_index],
                 rqpn[rail_index], &qp_attr, qp_attr_mask)) {
                ibv_error_abort(GEN_EXIT_ERR, "Failed to modify QP to RTR\n");
            }
        } else  /* Move send qp to RTR */
#endif
        {
            PRINT_DEBUG(DEBUG_XRC_verbose > 0, "dpqn %d\n",
                        qp_attr.dest_qp_num);
            if (ibv_ops.modify_qp(vc->mrail.rails[rail_index].qp_hndl,
                              &qp_attr, qp_attr_mask)) {
                ibv_error_abort(GEN_EXIT_ERR, "Failed to modify QP to RTR\n");
            }
        }
    }

    return 0;
}

/*function to move qps to rts and mark the connection available*/
int cm_qp_move_to_rts(MPIDI_VC_t * vc)
{
    struct ibv_qp_attr qp_attr;
    uint32_t qp_attr_mask = 0;
    int rail_index;

    for (rail_index = 0; rail_index < rdma_num_rails; rail_index++) {
        MPIU_Memset(&qp_attr, 0, sizeof qp_attr);
        qp_attr.qp_state = IBV_QPS_RTS;
        qp_attr.sq_psn = rdma_default_psn;
        qp_attr.timeout = rdma_default_time_out;
        qp_attr.retry_cnt = rdma_default_retry_count;
        qp_attr.rnr_retry = rdma_default_rnr_retry;
#ifdef _ENABLE_XRC_
        if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity || USE_XRC)
#else
        if (mv2_MPIDI_CH3I_RDMA_Process.heterogeneity)
#endif
        {
            qp_attr.max_rd_atomic = rdma_default_qp_ous_rd_atom;
        } else {
            qp_attr.max_rd_atomic = rdma_supported_max_qp_ous_rd_atom;
        }

        qp_attr_mask = 0;
        qp_attr_mask = IBV_QP_STATE |
            IBV_QP_TIMEOUT |
            IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
        PRINT_DEBUG(DEBUG_XRC_verbose > 0, "RTS %d\n", vc->pg_rank);
        if (ibv_ops.modify_qp(vc->mrail.rails[rail_index].qp_hndl, &qp_attr,
                          qp_attr_mask)) {
            ibv_error_abort(GEN_EXIT_ERR, "Failed to modify QP to RTS\n");
        }

        if (mv2_MPIDI_CH3I_RDMA_Process.has_apm) {
            reload_alternate_path(vc->mrail.rails[rail_index].qp_hndl);
        }

    }
    return 0;
}

int get_pkey_index(uint16_t pkey, int hca_num, int port_num, uint16_t * ix)
{
    uint16_t i = 0;
    struct ibv_device_attr dev_attr;

    if (ibv_ops.query_device(mv2_MPIDI_CH3I_RDMA_Process.nic_context[hca_num],
                         &dev_attr)) {

        ibv_error_abort(GEN_EXIT_ERR, "Error getting HCA attributes\n");
    }

    for (; i < dev_attr.max_pkeys; ++i) {
        uint16_t curr_pkey;
        ibv_ops.query_pkey(mv2_MPIDI_CH3I_RDMA_Process.nic_context[hca_num],
                       (uint8_t) port_num, (int) i, &curr_pkey);
        if (pkey == (ntohs(curr_pkey) & PKEY_MASK)) {
            *ix = i;
            return 1;
        }
    }

    return 0;
}

void set_pkey_index(uint16_t * pkey_index, int hca_num, int port_num)
{
    if (rdma_default_pkey == RDMA_DEFAULT_PKEY) {
        *pkey_index = rdma_default_pkey_ix;
    } else if (!get_pkey_index(rdma_default_pkey, hca_num, port_num, pkey_index)) {
        ibv_error_abort(GEN_EXIT_ERR,
                        "Can't find PKEY INDEX according to given PKEY\n");
    }
}

#ifdef CKPT
void MRAILI_Init_vc_network(MPIDI_VC_t * vc)
{
    /*Reinitialize VC after channel reactivation
     *      * No change in cmanager, no change in rndv, not support rdma
     *      fast path*/
    int i;

#ifndef MV2_DISABLE_HEADER_CACHING
    vc->mrail.rfp.cached_miss = 0;
    vc->mrail.rfp.cached_hit = 0;
    vc->mrail.rfp.cached_incoming = MPIU_Malloc(sizeof(MPIDI_CH3_Pkt_send_t));
    vc->mrail.rfp.cached_outgoing = MPIU_Malloc(sizeof(MPIDI_CH3_Pkt_send_t));
    memset(vc->mrail.rfp.cached_outgoing, 0, sizeof(MPIDI_CH3_Pkt_send_t));
    memset(vc->mrail.rfp.cached_incoming, 0, sizeof(MPIDI_CH3_Pkt_send_t));
#endif

    for (i = 0; i < rdma_num_rails; i++) {
        vc->mrail.rails[i].send_wqes_avail = rdma_default_max_send_wqe;
        vc->mrail.rails[i].ext_sendq_head = NULL;
        vc->mrail.rails[i].ext_sendq_tail = NULL;
    }
    for (i = 0; i < vc->mrail.num_rails; i++) {
        int k;
        if (!mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
            for (k = 0; k < rdma_initial_prepost_depth; k++) {
                PREPOST_VBUF_RECV(vc, i);
            }
        }
        vc->mrail.srp.credits[i].remote_credit = rdma_initial_credits;
        vc->mrail.srp.credits[i].remote_cc = rdma_initial_credits;
        vc->mrail.srp.credits[i].local_credit = 0;
        vc->mrail.srp.credits[i].preposts = rdma_initial_prepost_depth;

        if (!mv2_MPIDI_CH3I_RDMA_Process.has_srq) {
            vc->mrail.srp.credits[i].initialized =
                (rdma_prepost_depth == rdma_initial_prepost_depth);
        } else {
            vc->mrail.srp.credits[i].initialized = 1;
            vc->mrail.srp.credits[i].pending_r3_sends = 0;
        }
        vc->mrail.srp.credits[i].backlog.len = 0;
        vc->mrail.srp.credits[i].backlog.vbuf_head = NULL;
        vc->mrail.srp.credits[i].backlog.vbuf_tail = NULL;
        vc->mrail.srp.credits[i].rendezvous_packets_expected = 0;
    }
}

#endif

#ifdef _ENABLE_UD_
int rdma_init_ud(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc)
{
    int mpi_errno = MPI_SUCCESS;
    int hca_index;
    mv2_ud_ctx_t *ud_ctx;
    mv2_ud_qp_info_t qp_info;

    for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
        qp_info.send_cq = qp_info.recv_cq = proc->cq_hndl[hca_index];
        qp_info.sq_psn = rdma_default_psn;
        qp_info.pd = proc->ptag[hca_index];
        qp_info.cap.max_send_sge = rdma_default_max_sg_list;
        qp_info.cap.max_recv_sge = rdma_default_max_sg_list;
        qp_info.cap.max_send_wr = rdma_default_max_ud_send_wqe;
        qp_info.cap.max_recv_wr = rdma_default_max_ud_recv_wqe;
        qp_info.srq = NULL;
        if (rdma_use_ud_srq) {
            /* 
             * the create function takes care of alloc size
             * Don't set the limit here to avoid automatically tripping alarm 
             */
            proc->ud_srq_hndl[hca_index] = create_ud_srq(proc, hca_index);
            qp_info.srq = proc->ud_srq_hndl[hca_index];
        }
        qp_info.cap.max_inline_data = rdma_max_inline_size;
        ud_ctx = mv2_ud_create_ctx(&qp_info, hca_index);
        if (!ud_ctx) {
            fprintf(stderr, "Error in create UD qp\n");
            return MPI_ERR_INTERN;
        }
        if (rdma_use_ud_srq) {
            ud_ctx->credit_preserve = mv2_ud_srq_limit*2;
        } else {
            ud_ctx->credit_preserve = (rdma_default_max_ud_recv_wqe / 4);
        }

        ud_ctx->send_wqes_avail = rdma_default_max_ud_send_wqe;
        MESSAGE_QUEUE_INIT(&ud_ctx->ext_send_queue);
        ud_ctx->hca_num = hca_index;
        ud_ctx->num_recvs_posted = 0;

        proc->ud_rails[hca_index] = ud_ctx;
        proc->rc_connections = 0;
        ud_ctx->ext_sendq_count = 0;
    }
    MESSAGE_QUEUE_INIT(&proc->unack_queue);
    PRINT_DEBUG(DEBUG_UD_verbose > 0, "Finish setting up UD queue pairs\n");
    return mpi_errno;
}

int rdma_ud_post_buffers(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc)
{
    int hca_index = 0;
    int mpi_errno = MPI_SUCCESS;
    mv2_ud_ctx_t *ud_ctx = NULL;
    int max_ud_bufs = 0;

    if (rdma_use_ud_srq) {
        max_ud_bufs = mv2_ud_srq_fill_size;
    } else {
        max_ud_bufs = rdma_default_max_ud_recv_wqe;
    }

    if (rdma_use_ud_zcopy) {
        proc->zcopy_info.grh_mr =
            (void *) dreg_register(proc->zcopy_info.grh_buf, MV2_UD_GRH_LEN);
    }

    for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
        ud_ctx = proc->ud_rails[hca_index];
        ud_ctx->num_recvs_posted += mv2_post_ud_recv_buffers((max_ud_bufs - ud_ctx->num_recvs_posted), ud_ctx);
    }
    PRINT_DEBUG(DEBUG_UD_verbose > 0, "Finish posting UD buffers\n");

    return mpi_errno;
}

/* create ud vc */
static inline struct ibv_ah* mv2_ud_set_vc_info (mv2_ud_exch_info_t *rem_info, union ibv_gid gid,
                        struct ibv_pd *pd, int port, int hca_index)
{
    struct ibv_ah_attr ah_attr;

    MPIU_Memset(&ah_attr, 0, sizeof(ah_attr));

    PRINT_DEBUG(DEBUG_UD_verbose>0, "Creating AH: hca: %d, lid:%d, qpn:%d, port: %d, gid_index: %d, roce: %d\n",
                hca_index, rem_info->lid,rem_info->qpn, port,
                mv2_MPIDI_CH3I_RDMA_Process.gid_index[hca_index][0],
                mv2_MPIDI_CH3I_RDMA_Process.link_layer[hca_index][0]);

    if (use_iboeth ||
        (mv2_MPIDI_CH3I_RDMA_Process.link_layer[hca_index][0] == IBV_LINK_LAYER_ETHERNET)) {
        ah_attr.grh.dgid.global.subnet_prefix = 0;
        ah_attr.grh.dgid.global.interface_id = 0;
        ah_attr.grh.flow_label = 0;
        ah_attr.grh.sgid_index = mv2_MPIDI_CH3I_RDMA_Process.gid_index[hca_index][0];
        ah_attr.grh.hop_limit = 1;
        ah_attr.grh.traffic_class = 0;
        ah_attr.is_global = 1;
        ah_attr.dlid = 0;
        ah_attr.grh.dgid = gid;
    } else {
        ah_attr.is_global = 0;
        ah_attr.dlid = rem_info->lid;
        ah_attr.sl = 0;
    }

    ah_attr.src_path_bits = 0;
    ah_attr.port_num = port;

    /* Sanity check to ensure that we are not creating more AH than optimally
     * needed */
    mv2_num_ud_ah_created++;
    MPIU_Assert(mv2_num_ud_ah_created <= MPIDI_Get_num_nodes()*MAX_NUM_HCAS);

    return ibv_ops.create_ah(pd, &ah_attr);
}

int MPIDI_CH3I_UD_Generate_addr_handle_for_rank(MPIDI_PG_t * pg, int tgt_rank)
{
    int idx         = 0;
    int offset      = 0;
    int hca_index   = 0;
    int found_index = 0;
    union ibv_gid null_gid;
    MPIDI_VC_t *vc  = NULL;
    MPID_Node_id_t node_id;

    MPIU_Memset(&null_gid, 0, sizeof(union ibv_gid));

    MPIDI_PG_Get_vc(pg, tgt_rank, &vc);

    /* Get the unique node_id for the peer process */
    node_id = pg->ch.mrail->cm_shmem.ud_cm[tgt_rank].xrc_hostid;
    /* Get the offset into the ud_ah array for the peer process */
    offset = node_id * MAX_NUM_HCAS;

    vc->mrail.ud = MPIU_Malloc(sizeof(mv2_ud_vc_info_t) * rdma_num_hcas);

    for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
        /* Initialize found_index */
        found_index = -1;
        /* In multi-rail scenarios when processes use rail-binding, different
         * processes will use different HCAs and thus will be listening on
         * different LIDs. Handle this scenario */
        for (idx = 0; idx < MAX_NUM_HCAS; ++idx) {
            /* We initialized the array to 0. If entry is 0, it is not used. */
            if ((mv2_system_has_ib && pg->ch.mrail->cm_shmem.ud_lid[offset] == 0) ||
                (mv2_system_has_roce &&
                 !memcmp(&pg->ch.mrail->cm_shmem.ud_gid[offset], &null_gid,
                         sizeof(union ibv_gid)))) {
                break;
            }
            if ((mv2_system_has_ib &&
                (pg->ch.mrail->cm_shmem.ud_lid[offset] ==
                 pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].lid)) ||
                (mv2_system_has_roce &&
                 !memcmp(&pg->ch.mrail->cm_shmem.ud_gid[offset],
                         &pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].gid,
                         sizeof(union ibv_gid)))) {
                found_index = offset;
                break;
            }
            /* Entry at 'offset' is used. Increment 'offset' so that it does not
             * get overwritten in the segment below when 'found_index == -1' */
            offset++;
        }

        /* Sanity checks */
        MPIU_Assert(offset < (MPIDI_Get_num_nodes() * MAX_NUM_HCAS));
        MPIU_Assert(offset <= ((node_id+1) * MAX_NUM_HCAS));
        if (found_index == -1) {
            PRINT_DEBUG(DEBUG_UD_verbose,
                        "Generating UD_AH for lid %u gid %"PRIx64 ":%"PRIx64 " for node_id %d, tgt_rank %d\n",
                        pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].lid,
                        (unsigned long) pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].gid.global.subnet_prefix,
                        (unsigned long) pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].gid.global.interface_id,
                        node_id, tgt_rank);
            /* Store the LID */
            pg->ch.mrail->cm_shmem.ud_lid[offset] =
                                pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].lid;
            /* Store the GID */
            MPIU_Memcpy(&pg->ch.mrail->cm_shmem.ud_gid[offset],
                        &pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].gid,
                        sizeof(union ibv_gid));
            /* Store the UD_AH */
            pg->ch.mrail->cm_shmem.ud_ah[offset] =
                mv2_ud_set_vc_info(&pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index],
                                pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].gid,
                                mv2_MPIDI_CH3I_RDMA_Process.ptag[hca_index],
                                mv2_MPIDI_CH3I_RDMA_Process.ports[hca_index][0], hca_index);
            found_index = offset;
        }
        PRINT_DEBUG(DEBUG_CM_verbose > 0, "node_id for peer %d is %d. AH for hca_index %d = %p\n",
                    tgt_rank, node_id, hca_index, pg->ch.mrail->cm_shmem.ud_ah[found_index]);
        vc->mrail.ud[hca_index].ah  = pg->ch.mrail->cm_shmem.ud_ah[found_index];
        vc->mrail.ud[hca_index].lid = pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].lid;
        vc->mrail.ud[hca_index].qpn = pg->ch.mrail->cm_shmem.remote_ud_info[tgt_rank][hca_index].qpn;
    }

#ifdef _ENABLE_XRC_
    VC_XST_SET(vc, XF_UD_CONNECTED);
#endif
    vc->state = MPIDI_VC_STATE_ACTIVE;

    PRINT_DEBUG(DEBUG_CM_verbose > 0, "Created UD Address handle for rank %d\n", tgt_rank);

    return MPI_SUCCESS;
}

int MPIDI_CH3I_UD_Generate_addr_handles(MPIDI_PG_t * pg, int pg_rank,
                                        int pg_size)
{
    int i = 0;
    MPIDI_VC_t *vc  = NULL;

    for (i = 0; i < pg_size; i++) {
        if (i == pg_rank) {
            continue;
        }
        MPIDI_PG_Get_vc(pg, i, &vc);

        MRAILI_Init_vc(vc);
        /* Change vc state to avoid UD CM connection establishment */
        vc->ch.state = MPIDI_CH3I_VC_STATE_IDLE;
#ifdef _ENABLE_XRC_
        VC_XST_SET(vc, XF_SEND_IDLE);
#endif
        vc->mrail.state |= MRAILI_UD_CONNECTED;
    }

    PRINT_DEBUG(DEBUG_UD_verbose > 0, "Created UD Address handles \n");

    return MPI_SUCCESS;
}

int mv2_ud_setup_zcopy_rndv(struct mv2_MPIDI_CH3I_RDMA_Process_t *proc)
{
    int i, hca_index;
    mv2_ud_zcopy_info_t *zcopy_info = &proc->zcopy_info;
    mv2_ud_qp_info_t qp_info;
    mv2_ud_ctx_t *ud_ctx;

    zcopy_info->rndv_qp_pool = (mv2_rndv_qp_t *)
        MPIU_Malloc(sizeof(mv2_rndv_qp_t) * rdma_ud_num_rndv_qps);
    zcopy_info->rndv_ud_cqs = (struct ibv_cq **)
        MPIU_Malloc(sizeof(struct ibv_cq *) * rdma_num_hcas);
    zcopy_info->rndv_ud_qps = (mv2_ud_ctx_t **)
        MPIU_Malloc(sizeof(mv2_ud_ctx_t *) * rdma_num_hcas);

    for (i = 0; i < rdma_ud_num_rndv_qps; i++) {
        for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
            /* creating 256 extra cq entries than max possible recvs for safer side */
            zcopy_info->rndv_qp_pool[i].ud_cq[hca_index] =
                ibv_ops.create_cq(proc->nic_context[hca_index],
                              (rdma_ud_zcopy_rq_size + 256), NULL, NULL, 0);
            if (!zcopy_info->rndv_qp_pool[i].ud_cq[hca_index]) {
                fprintf(stderr, "Error in creating ZCOPY Rndv CQ\n");
                return MPI_ERR_INTERN;
            }
    
            qp_info.send_cq = qp_info.recv_cq = zcopy_info->rndv_qp_pool[i].ud_cq[hca_index];
            qp_info.sq_psn = rdma_default_psn;
            qp_info.pd = proc->ptag[hca_index];
            qp_info.cap.max_send_sge = 1;
            qp_info.cap.max_recv_sge = 2;
            qp_info.cap.max_send_wr = 1;
            qp_info.cap.max_recv_wr = rdma_ud_zcopy_rq_size;
            qp_info.srq = NULL;
            qp_info.cap.max_inline_data = 0;
            zcopy_info->rndv_qp_pool[i].ud_qp[hca_index] = mv2_ud_create_qp(&qp_info, hca_index);
            if (!zcopy_info->rndv_qp_pool[i].ud_qp[hca_index]) {
                fprintf(stderr, "Error in creating ZCOPY Rndv QP\n");
                return MPI_ERR_INTERN;
            }
    
            zcopy_info->rndv_qp_pool[i].seqnum = 0;
            zcopy_info->rndv_qp_pool[i].next = zcopy_info->rndv_qp_pool[i].prev = NULL;
        }
    }

    for (i = 0; i < rdma_ud_num_rndv_qps - 1; i++) {
        zcopy_info->rndv_qp_pool[i].next = &zcopy_info->rndv_qp_pool[i + 1];
    }
    zcopy_info->rndv_qp_pool_free_head = &zcopy_info->rndv_qp_pool[0];

    /* allocate and register GRH buffer */
    zcopy_info->grh_buf = MPIU_Malloc(MV2_UD_GRH_LEN);
    zcopy_info->no_free_rndv_qp = 0;

    /* Setup QP for sending zcopy rndv messages */
    for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
        zcopy_info->rndv_ud_cqs[hca_index] =
            ibv_ops.create_cq(proc->nic_context[hca_index], 16384, NULL, NULL, 0);
        if (!zcopy_info->rndv_ud_cqs[hca_index]) {
            fprintf(stderr, "Error in creating ZCOPY Rndv CQ\n");
            return MPI_ERR_INTERN;
        }
    }

    for (hca_index = 0; hca_index < rdma_num_hcas; hca_index++) {
        qp_info.send_cq = qp_info.recv_cq = zcopy_info->rndv_ud_cqs[hca_index];
        qp_info.sq_psn = rdma_default_psn;
        qp_info.pd = proc->ptag[hca_index];
        qp_info.cap.max_send_sge = rdma_default_max_sg_list;
        qp_info.cap.max_recv_sge = rdma_default_max_sg_list;
        qp_info.cap.max_send_wr = rdma_default_max_ud_send_wqe;
        qp_info.cap.max_recv_wr = 1;
        qp_info.srq = NULL;
        /* Since the rndv zcopy QP will also be used to send the ZCopy fin
         * message which is less than inline size, we should ensure that the QP
         * is capable of handling it */
        qp_info.cap.max_inline_data = rdma_max_inline_size;
        ud_ctx = mv2_ud_create_ctx(&qp_info, hca_index);
        if (!ud_ctx) {
            fprintf(stderr, "Error in create UD qp\n");
            return MPI_ERR_INTERN;
        }

        ud_ctx->send_wqes_avail = rdma_default_max_ud_send_wqe - 50;
        MESSAGE_QUEUE_INIT(&ud_ctx->ext_send_queue);
        ud_ctx->hca_num = hca_index;
        ud_ctx->num_recvs_posted = 0;
        ud_ctx->credit_preserve = (rdma_default_max_ud_recv_wqe / 4);
        zcopy_info->rndv_ud_qps[hca_index] = ud_ctx;
    }

    PRINT_DEBUG(DEBUG_ZCY_verbose > 2,
                "ZCOPY Rndv setup done num rndv qps:%d\n",
                rdma_ud_num_rndv_qps);

    return MPI_SUCCESS;
}

void MPIDI_CH3I_UD_Stats(MPIDI_PG_t * pg)
{

    int i;
    mv2_ud_ctx_t *ud_ctx;
    mv2_ud_reliability_info_t *ud_vc;
    MPIDI_VC_t *vc;

    int pg_size = MPIDI_PG_Get_size(pg);
    int pg_rank = MPIDI_Process.my_pg_rank;

    if (pg_rank != 0 && DEBUG_UDSTAT_verbose < 3) {
        return;
    }

    PRINT_INFO(DEBUG_UDSTAT_verbose > 0, "RC conns: %u pending:%d "
               "zcopy_fallback_count:%u\n",
               mv2_MPIDI_CH3I_RDMA_Process.rc_connections,
               rdma_hybrid_pending_rc_conn,
               mv2_MPIDI_CH3I_RDMA_Process.zcopy_info.no_free_rndv_qp);

    for (i = 0; i < rdma_num_hcas; i++) {
        ud_ctx = mv2_MPIDI_CH3I_RDMA_Process.ud_rails[i];
        if (ud_ctx->ext_sendq_count) {
            PRINT_INFO(DEBUG_UDSTAT_verbose > 0, "rail:%d "
                       " ext send queue sends: %lu\n", i,
                       ud_ctx->ext_sendq_count);
        }
    }
    for (i = 0; i < pg_size; i++) {
        MPIDI_PG_Get_vc(pg, i, &vc);
        if (rdma_use_smp && (vc->smp.local_rank != -1))
            continue;
        ud_vc = &vc->mrail.rely;
        if (ud_vc->resend_count) {
            PRINT_INFO(DEBUG_UDSTAT_verbose > 1, "\t[-> %d]: resends:%lu "
                       "cntl msg:%lu extwin_msgs:%lu tot_ud_msgs:%llu\n",
                       vc->pg_rank, ud_vc->resend_count, ud_vc->cntl_acks,
                       ud_vc->ext_win_send_count, ud_vc->total_messages);
        }
    }
}
#endif /* _ENABLE_UD_ */

/* vi:set sw=4 */
