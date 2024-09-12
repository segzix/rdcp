#ifndef _INCLUDE_RDCP_H
#define _INCLUDE_RDCP_H

#include "list.h"
#include <infiniband/arch.h>
#include <linux/limits.h>
#include <rdma/rdma_cma.h>
#include <semaphore.h>
#include <stdio.h>

enum test_state {
    IDLE = 1,
    CONNECT_REQUEST,
    ADDR_RESOLVED,
    ROUTE_RESOLVED,
    CONNECTED,
    SEND_METADATA,
    SEND_DATA,
    RDMA_READ_ADV,
    RDMA_READ_COMPLETE,
    RDMA_WRITE_ADV,
    RDMA_WRITE_COMPLETE,
    DISCONNECTED,
    ERROR
};

struct rdma_info {
    int id;
    uint64_t buf;
    uint32_t rkey;
    uint32_t size;
};

struct metadata_info {
    int version; // protocol version
    long size;
    char src_path[PATH_MAX];
    char dst_path[PATH_MAX];
};

#define RDCP_PORT 7171
#define MAX_TASKS 112
#define MAX_WC 16
#define MAX_WR (MAX_TASKS + 1)
#define BUF_SIZE (64 * 1024)
#define METADATA_WR_ID 0xfffffffffffffffaULL
#define CQ_DEPTH ((MAX_TASKS + 1) * 2)
#define VERBOSE_LOG(level, fmt, ...)                                                               \
    if (verbose >= level) {                                                                        \
        printf(fmt, ##__VA_ARGS__);                                                                \
    }
#define uint64_from_ptr(p) (uint64_t)(uintptr_t)(p)
#define ptr_from_int64(p) (void *)(unsigned long)(p)

struct rdcp_task {
    struct rdma_info buf;
    struct ibv_sge sgl;
    struct ibv_mr *mr;
    union {
        struct ibv_recv_wr rq_wr;
        struct ibv_send_wr sq_wr;
    };
    struct list_head task_list;
};

/**
 * Control block struct.
 */
struct rdcp_cb {
    int server; /** 0 iff client */
    pthread_t cqthread;
    struct ibv_comp_channel *channel;
    struct ibv_cq *cq;
    struct ibv_pd *pd;
    struct ibv_qp *qp;

    int fd;
    FILE *fp;
    int sent_count;
    int recv_count;
    int use_null;

    /** 元数据的发送请求，接收请求与句柄(均只有一个)，全部绑定元数据 */
    struct metadata_info metadata;
    struct ibv_mr *metadata_mr;
    struct ibv_sge metadata_sgl;
    struct ibv_recv_wr md_recv_wr;
    struct ibv_send_wr md_send_wr;

    /** 接收任务队列与发送任务队列，发送的任务绑定至start_buf，接收的任务绑定至自己的buf(初始化后) */
    struct rdcp_task recv_tasks[MAX_TASKS];
    struct rdcp_task send_tasks[MAX_TASKS];

    /** 两个队列 */
    struct list_head task_free;
    struct list_head task_alloc;

    /** rdma的数据请求队列，句柄数组，每一个句柄绑定rdma_buf中的一块内存 */
    struct ibv_send_wr rdma_sq_wr[MAX_TASKS]; /** rdma work request record */
    struct ibv_sge rdma_sgl[MAX_TASKS];       /** rdma single SGE */
    char *rdma_buf;                           /** used as rdma sink */
    struct ibv_mr *rdma_mr;

    uint32_t remote_rkey; /** remote guys RKEY */
    uint64_t remote_addr; /** remote guys TO */
    uint32_t remote_len;  /** remote guys LEN */

    char *start_buf; /** rdma read src */
    struct ibv_mr *start_mr;

    enum test_state state; /** used for cond/signalling */
    sem_t sem;

    struct sockaddr_storage sin;
    uint16_t port; /** dst port in NBO */
    int size;      /** ping data size */

    /** CM stuff */
    pthread_t cmthread;
    struct rdma_event_channel *cm_channel;
    struct rdma_cm_id *cm_id; /** connection on client side,*/
    /** listener on service side. */
    struct rdma_cm_id *child_cm_id; /** connection on server side */
};

extern int verbose;

/** server */
int rdcp_run_server(struct rdcp_cb *cb);
int server_response(struct rdcp_cb *cb, struct ibv_wc *wc);
int server_recv(struct rdcp_cb *cb, struct ibv_wc *wc);

/** client */
int rdcp_run_client(struct rdcp_cb *cb);
int client_recv(struct rdcp_cb *cb, struct ibv_wc *wc);

/** rdcp core */
void *cq_thread(void *arg);

/** rdcp buffers */
int rdcp_setup_buffers(struct rdcp_cb *cb);
void rdcp_free_buffers(struct rdcp_cb *cb);

/** rdcp queues */
int rdcp_create_qp(struct rdcp_cb *cb);
int rdcp_setup_qp(struct rdcp_cb *cb, struct rdma_cm_id *cm_id);
void rdcp_free_qp(struct rdcp_cb *cb);

#endif