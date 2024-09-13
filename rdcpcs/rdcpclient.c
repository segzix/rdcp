#include "rdcp.h"
#include "utils.h"
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <unistd.h>

static int rdcp_bind_client(struct rdcp_cb *cb);
static int rdcp_connect_client(struct rdcp_cb *cb);
static int rdcp_test_client(struct rdcp_cb *cb);

int rdcp_run_client(struct rdcp_cb *cb) {
    struct ibv_recv_wr *bad_wr;
    int ret;

    if (!cb->use_null) {
        int fd = open(cb->metadata.src_path, O_RDONLY);

        close(fd);
        if (fd < 0) {
            perror("failed to open source file");
            return errno;
        }
    }

    ret = rdcp_bind_client(cb);
    if (ret)
        return ret;

    ret = rdcp_setup_qp(cb, cb->cm_id);
    if (ret) {
        fprintf(stderr, "setup_qp failed: %d\n", ret);
        return ret;
    }

    ret = rdcp_setup_buffers(cb);
    if (ret) {
        fprintf(stderr, "rdcp_setup_buffers failed: %d\n", ret);
        goto free_qp;
    }

    /** 接收端的工作全部准备好 */
    ret = ibv_post_recv(cb->qp, &cb->md_recv_wr, &bad_wr);
    if (ret) {
        perror("error post recv metadata");
        goto free_buffers;
    }

    ret = ibv_post_recv(cb->qp, &cb->recv_tasks[0].rq_wr, &bad_wr);
    if (ret) {
        fprintf(stderr, "ibv_post_recv tasks failed: %d\n", ret);
        goto free_buffers;
    }

    pthread_create(&cb->cqthread, NULL, cq_thread, cb);

    ret = rdcp_connect_client(cb);
    if (ret) {
        fprintf(stderr, "connect error %d\n", ret);
        cb->state = ERROR;
        goto join_cq_thread;
    }

    ret = rdcp_test_client(cb);
    if (ret) {
        fprintf(stderr, "rdcp client failed: %d\n", ret);
        goto discon;
    }

    ret = 0;
discon:
    if (cb->cm_id)
        rdma_disconnect(cb->cm_id);
join_cq_thread:
    pthread_join(cb->cqthread, NULL);
    // TODO: wait for flush
    sleep(1);
free_buffers:
    rdcp_free_buffers(cb);
free_qp:
    rdcp_free_qp(cb);

    return ret;
}

/**
 * rdma绑定客户端
 */
int rdcp_bind_client(struct rdcp_cb *cb) {
    int ret;

    if (cb->sin.ss_family == AF_INET)
        ((struct sockaddr_in *)&cb->sin)->sin_port = cb->port;
    else
        ((struct sockaddr_in6 *)&cb->sin)->sin6_port = cb->port;

    /** 解析地址信息进行绑定 */
    ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&cb->sin, 2000);
    if (ret) {
        perror("rdma_resolve_addr");
        return ret;
    }

    sem_wait(&cb->sem);
    if (cb->state != ROUTE_RESOLVED) {
        fprintf(stderr, "waiting for addr/route resolution state %d\n", cb->state);
        return -1;
    }

    VERBOSE_LOG(3, "rdma_resolve_addr - rdma_resolve_route successful\n");
    return 0;
}

/**
 * 建立连接
 * sem_wait(&cb->sem)相当于一个同步操作，用这个同步操作来保证所有必须动作的同步(按顺序进行)
 * 建立连接->发送文件元数据->发送完一轮的send_tasks(标记client端rdma的区域，方便远程访问)
 */
int rdcp_connect_client(struct rdcp_cb *cb) {
    struct rdma_conn_param conn_param;
    int ret;

    memset(&conn_param, 0, sizeof conn_param);
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 7;
    // if we need retry because we miss recv wr
    //	conn_param.rnr_retry_count = 6;

    ret = rdma_connect(cb->cm_id, &conn_param);
    if (ret) {
        perror("rdma_connect");
        return ret;
    }

    sem_wait(&cb->sem);
    if (cb->state != CONNECTED) {
        fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
        return -1;
    }

    VERBOSE_LOG(3, "rmda_connect successful\n");
    return 0;
}

/**
 * 客户端处理接收请求
 */
int client_recv(struct rdcp_cb *cb, struct ibv_wc *wc) {
    // wakeup metadata sem wait
    if (wc->wr_id == METADATA_WR_ID) {
        sem_post(&cb->sem);
        return 0;
    }

    if (wc->byte_len != sizeof(struct rdma_info)) {
        fprintf(stderr, "Received bogus data, size %d\n", wc->byte_len);
        return -1;
    }

    if (cb->state == RDMA_READ_ADV)
        cb->state = RDMA_WRITE_ADV;
    else
        cb->state = RDMA_WRITE_COMPLETE;

    return 0;
}

int rdcp_test_client(struct rdcp_cb *cb) {
    int i, ret = 0;
    struct ibv_send_wr *bad_wr;
    int size;
    long total_size = 0;

    if (!cb->use_null) {
        VERBOSE_LOG(3, "src_path: %s\n", cb->metadata.src_path)
        cb->fd = open(cb->metadata.src_path, O_RDONLY);
        VERBOSE_LOG(1, "open fd %d\n", cb->fd);
        if (cb->fd < 0) {
            perror("Couldn't open file");
            goto out;
        }
        cb->fp = fdopen(cb->fd, "rb");
        fseek(cb->fp, 0, SEEK_END);
        cb->metadata.size = ftell(cb->fp);
        if (cb->metadata.size <= 0) {
            perror("size error");
            goto out;
        }
        fseek(cb->fp, 0, SEEK_SET);
    } else {
        cb->metadata.size = 0;
    }

    // send meta
    //
    VERBOSE_LOG(1, "Sending metadata to server\n");
    ret = ibv_post_send(cb->qp, &cb->md_send_wr, &bad_wr);
    sem_wait(&cb->sem);
    // TODO check state we got metata and not error/disconnect

    if (cb->state >= DISCONNECTED) {
        ret = -1;
        goto out;
    }
    // send file
    //
    VERBOSE_LOG(1, "start\n");

    /** 一直发送到整个文件全部被读取完(所以2GB问题出在哪？) */
    do {
        cb->state = RDMA_READ_ADV;
        cb->recv_count = 0;
        cb->sent_count = 0;

        /** 一次性发送MAX_TASKS个send task，然后一直等待回应到server段对所有的send
         * task均进行了相应，进入下一个发送周期 */
        VERBOSE_LOG(1, "send tasks\n");
        for (i = 0; i < MAX_TASKS; i++) {
            struct rdcp_task *send_task = &cb->send_tasks[i];
            struct rdma_info *info = &send_task->rdmaInfo;

            /**
             * 读取剩余的文件大小
             */
            if (cb->use_null) {
                size = BUF_SIZE;
            } else {
                size = read(cb->fd, ptr_from_int64(info->buf), BUF_SIZE);
                VERBOSE_LOG(1, "Read size = %d\n", size);
            }
            if (size == 0)
                break;

            if (size < 0) {
                perror("error reading file\n");
                break;
            }

            VERBOSE_LOG(3, "RDMA addr %" PRIx64 " rkey %x len %d\n", info->buf, info->rkey,
                        info->size);

            // 如果文件剩下的大小已经不大了，这里会改一下
            info->size = size;
            // 发送send task
            ret = ibv_post_send(cb->qp, &send_task->sq_wr, &bad_wr);
            if (ret) {
                perror("send task failed");
                break;
            }

            total_size += size;
        }

        /** Wait for server to ACK */
        VERBOSE_LOG(1, "wait for server respond\n");
        VERBOSE_LOG(1, "recv_count: %d cb->sem: %ld\n", cb->recv_count, cb->sem);
        while (cb->recv_count < i) {
            sem_wait(&cb->sem);
        }

        print_send_status(cb, &total_size, size < BUF_SIZE);
    } while (size > 0);

    printf("\n");
    VERBOSE_LOG(1, "done\n");

out:
    fclose(cb->fp);
    cb->fd = -1;

    return (cb->state == DISCONNECTED) ? 0 : ret;
}