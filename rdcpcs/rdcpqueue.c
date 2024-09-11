#include "rdcp.h"
#include "utils.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * 根据完成队列来创建接收和发送队列
 */
int rdcp_create_qp(struct rdcp_cb *cb) {
    struct ibv_qp_init_attr init_attr;
    int ret;

    // TODO: check values
    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = MAX_WR;
    init_attr.cap.max_recv_wr = MAX_WR;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_send_sge = 1; // 每次只能有一个缓冲区中的数据发送给对方
    init_attr.qp_type = IBV_QPT_RC; // 可靠连接
    init_attr.send_cq = cb->cq;     // 发送端完成队列
    init_attr.recv_cq = cb->cq;     // 接收端完成队列

    // 根据是否是服务端来判断根据cm_id还是child_cm_id来判断
    if (cb->server) {
        ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
        if (!ret)
            cb->qp = cb->child_cm_id->qp;
    } else {
        ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
        if (!ret)
            cb->qp = cb->cm_id->qp;
    }

    return ret;
}

/**
 * 创建完成队列和对应的接受发送队列
 */
int rdcp_setup_qp(struct rdcp_cb *cb, struct rdma_cm_id *cm_id) {
    int ret;

    // 申请一个保护域
    cb->pd = ibv_alloc_pd(cm_id->verbs);
    if (!cb->pd) {
        // TODO use perror for verbs error prints
        fprintf(stderr, "ibv_alloc_pd failed\n");
        return errno;
    }
    VERBOSE_LOG(3, "created pd %p\n", cb->pd);

    /**
     * 在 RDMA 操作完成时，相应的完成事件会被通知到完成通道，
     * 应用程序可以通过轮询完成队列或者等待通知来处理这些完成事件，
     * 以便执行后续的操作或者进行错误处理
     */

    /** ibv_create_comp_channel 创建一个完成通道，用于接收 RDMA 完成事件的通知 */
    cb->channel = ibv_create_comp_channel(cm_id->verbs);
    if (!cb->channel) {
        fprintf(stderr, "ibv_create_comp_channel failed\n");
        ret = errno;
        // TODO: use real labels. e.g. err_create_channel or free_pd
        goto free_pd;
    }
    VERBOSE_LOG(3, "created channel %p\n", cb->channel);

    // TODO: do we really need *2 ?
    /** ibv_create_cq 用于创建一个完成队列（CQ）对象 */
    cb->cq = ibv_create_cq(cm_id->verbs, CQ_DEPTH, cb, cb->channel, 0);
    if (!cb->cq) {
        fprintf(stderr, "ibv_create_cq failed\n");
        ret = errno;
        goto free_comp_chan;
    }
    VERBOSE_LOG(3, "created cq %p\n", cb->cq);

    /** ibv_req_notify_cq 请求在CQ中有新的完成事件时产生通知 */
    ret = ibv_req_notify_cq(cb->cq, 0);
    if (ret) {
        fprintf(stderr, "ibv_create_cq failed\n");
        ret = errno;
        goto free_cq;
    }

    ret = rdcp_create_qp(cb);
    if (ret) {
        perror("rdma_create_qp");
        goto free_cq;
    }
    VERBOSE_LOG(3, "created qp %p\n", cb->qp);
    return 0;

free_cq:
    ibv_destroy_cq(cb->cq);
free_comp_chan:
    ibv_destroy_comp_channel(cb->channel);
free_pd:
    ibv_dealloc_pd(cb->pd);
    return ret;
}

/**
 * 销毁完成队列和对应的接收发送队列对
 */
void rdcp_free_qp(struct rdcp_cb *cb) {
    ibv_destroy_qp(cb->qp);
    ibv_destroy_cq(cb->cq);
    ibv_destroy_comp_channel(cb->channel);
    ibv_dealloc_pd(cb->pd);
}