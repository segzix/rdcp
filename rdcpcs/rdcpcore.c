
#include "rdcp.h"
#include "utils.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static int rdcp_cq_event_handler(struct rdcp_cb *cb);
static int handle_wc(struct rdcp_cb *cb, struct ibv_wc *wc);
static int rearm_completions(struct rdcp_cb *cb);

/**
 * 核心线程(用来掌管rdma的接收与发送)
 * 用户端与服务端负责轮询并处理完成队列相关事件的线程
 */
void *cq_thread(void *arg) {
    struct rdcp_cb *cb = arg;
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    int ret;
    long long tick1, tick2;

    VERBOSE_LOG(3, "cq_thread started.\n");

    /**
     * 创建的线程在这里循环，不断轮询完成事件
     */
    while (1) {

        //查询当前线程是否有被取消
        pthread_testcancel();

        tick1 = current_timestamp();
        VERBOSE_LOG(3, "\nwait for cq event\n");

        //通过完成通道cb->channel获取完成队列里的完成事件(队列存储在ev_cq中，事件上下文信息存储在ev_ctx中)
        ret = ibv_get_cq_event(cb->channel, &ev_cq, &ev_ctx);
        if (ret) {
            fprintf(stderr, "Failed to get cq event!\n");
            pthread_exit(NULL);
        }
        VERBOSE_LOG(3, "get cq event\n");

        tick2 = current_timestamp();
        if (tick2 - tick1 > 1000) {
            VERBOSE_LOG(1, "got pause %f\n", (tick2 - tick1) / 1000.0);
        }
        VERBOSE_LOG(3, "skip pause\n");

        //完成队列应该和cb结构体中规定的一致，否则报错
        if (ev_cq != cb->cq) {
            fprintf(stderr, "Unknown CQ!\n");
            pthread_exit(NULL);
        }
        VERBOSE_LOG(3, "consistent\n");

        //确认一个完成事件
        ibv_ack_cq_events(cb->cq, 1);

        //处理完成事件
        VERBOSE_LOG(3, "handle cq event\n");
        ret = rdcp_cq_event_handler(cb);
        if (ret) {
            VERBOSE_LOG(3, "ERROR: handle event, killing cq_thread\n");
            pthread_exit(NULL);
        }

        if (cb->state == ERROR)
            printf("ERROR state\n");
        if (cb->state >= DISCONNECTED)
            pthread_exit(NULL);

        ret = ibv_req_notify_cq(cb->cq, 0);
        if (ret) {
            fprintf(stderr, "Failed to set notify!\n");
            pthread_exit(NULL);
        }
    }
}

/**
 * 轮询完成事件队列并调用handle_wc进行处理
 */
int rdcp_cq_event_handler(struct rdcp_cb *cb) {
    struct ibv_wc wcs[MAX_WC];
    int i, n;

    VERBOSE_LOG(3, "poll\n");
    while ((n = ibv_poll_cq(cb->cq, MAX_WC, wcs)) > 0) {
        for (i = 0; i < n; i++) {
            handle_wc(cb, &wcs[i]);
        }
    }

    if (n < 0)
        return n;

    return 0;
}

/**
 * 处理完成事件
 */
int handle_wc(struct rdcp_cb *cb, struct ibv_wc *wc) {
    int ret = 0;
    int i;
    int size;

    VERBOSE_LOG(3, "wc->wr_id: %lu\n", wc->wr_id);
    VERBOSE_LOG(3, "wc->status: %u\n", wc->status);
    VERBOSE_LOG(3, "cb->recv_count: %u\n", cb->recv_count);
    VERBOSE_LOG(3, "cb->sent_count: %u\n", cb->sent_count);
    if (wc->status) {
        if (wc->status != IBV_WC_WR_FLUSH_ERR) {
            fprintf(stderr, "cq completion id %lu failed with status %d (%s)\n",
                    (unsigned long)wc->wr_id, wc->status, ibv_wc_status_str(wc->status));
            ret = -1;
        }
        goto error;
    }

    switch (wc->opcode) {
    case IBV_WC_SEND:
        VERBOSE_LOG(3, "send completion\n");
        cb->sent_count++;
        break;

    case IBV_WC_RDMA_WRITE:
        VERBOSE_LOG(3, "rdma write completion\n");
        cb->state = RDMA_WRITE_COMPLETE;
        sem_post(&cb->sem);
        break;

    /** RDMA读取操作完成，准备写入文件 */
    /** RDMA读取操作是底层RDMA完成的，具体流程是通过先前客户端传来的send
     * task获悉远程的rdma内存注册区域，然后写到本地的RDMA内存上，并提交一个完成事件*/
    case IBV_WC_RDMA_READ:
        VERBOSE_LOG(3, "rdma read completion\n");
        i = wc->wr_id;

        VERBOSE_LOG(1, "fd %d i %d rdma_buf %p\n", cb->fd, i, &cb->rdma_buf[i * BUF_SIZE]);
        if (!cb->use_null) {
            VERBOSE_LOG(3, "cb->fd: %d cb->rdma_buf: %s size: %d\n", cb->fd,
                        &cb->rdma_buf[i * BUF_SIZE], cb->recv_tasks[i].rdmaInfo.size);
            size = write(cb->fd, &cb->rdma_buf[i * BUF_SIZE], cb->recv_tasks[i].rdmaInfo.size);

            if (size < 0) {
                printf("error writing data\n");
                ret = size;
                goto error;
            }
        }
        ret = server_response(cb, wc);
        if (ret)
            goto error;
        break;

    /** 处理recv请求(发送的数据暂存到recv_tasks中) */
    case IBV_WC_RECV:
        VERBOSE_LOG(3, "recv completion\n");
        ret = cb->server ? server_recv(cb, wc) : client_recv(cb, wc);
        if (ret) {
            perror("recv wc error");
            goto error;
        }

        if (wc->wr_id == METADATA_WR_ID)
            break;

        rearm_completions(cb);

        cb->recv_count++;
        VERBOSE_LOG(3, "recv_count: %d\n", cb->recv_count);
        if (cb->recv_count >= cb->sent_count)
            // wakeup recv <= MAXTASKS sem wait
            sem_post(&cb->sem);
        break;

    default:
        VERBOSE_LOG(3, "unknown!!!!! completion\n");
        ret = -1;
        goto error;
    }

    if (ret) {
        fprintf(stderr, "poll error %d\n", ret);
        goto error;
    }

    return 0;

error:
    if (ret < 0) {
        cb->state = ERROR;
        //		if (cb->server)
        //			rdma_disconnect(cb->child_cm_id);
        //		else
        //			rdma_disconnect(cb->cm_id);
    }
    sem_post(&cb->sem);
    return ret;
}

/**
 * 在所有的recv tasks处理完后，重置所有recv_tasks
 */
int rearm_completions(struct rdcp_cb *cb) {
    int ret;
    static int rearm = 0;

    rearm++;
    if (rearm == MAX_TASKS) {
        ret = ibv_post_recv(cb->qp, &cb->recv_tasks[0].rq_wr, NULL);
        if (ret) {
            perror("post recv error");
            goto error;
        }
        rearm = 0;
    }

    return 0;

error:
    return ret;
}