#include "rdcp.h"
#include "utils.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int rdcp_setup_wr(struct rdcp_cb *cb);

int rdcp_setup_buffers(struct rdcp_cb *cb) {
    int ret;
    int i;

    VERBOSE_LOG(3, "rdcp_setup_buffers called on cb %p\n", cb);

    // TODO: server use rdma_buf and not start_buf but still use send_tasks
    // TODO: client use start_buf and not rdma_buf
    // TODO: handle error
    for (i = 0; i < MAX_TASKS; i++) {
        struct rdcp_task *recv_task = &cb->recv_tasks[i];
        struct rdcp_task *send_task = &cb->send_tasks[i];

        recv_task->buf.id = i;
        //注册recv_task缓冲区的内存
        recv_task->mr =
            ibv_reg_mr(cb->pd, &recv_task->buf, sizeof(struct rdma_info), IBV_ACCESS_LOCAL_WRITE);
        if (!recv_task->mr) {
            fprintf(stderr, "recv_buf reg_mr failed\n");
            ret = errno;
            goto error;
        }
        /**
         * 初始化接收缓冲区的句柄(指向缓冲区)
         * 1.地址 2.长度 3.描述内存区域的标志key
         */
        recv_task->sgl.addr = uint64_from_ptr(&recv_task->buf);
        recv_task->sgl.length = sizeof(struct rdma_info);
        recv_task->sgl.lkey = recv_task->mr->lkey;
        /**
         * 初始化一个接收请求
         * 1.指向请求缓冲区的第一个句柄结构体
         * 2.总共的句柄个数(可能会从链表出发有多个句柄)
         * 3.请求的id号
         */
        recv_task->rq_wr.sg_list = &recv_task->sgl;
        recv_task->rq_wr.num_sge = 1;
        recv_task->rq_wr.wr_id = i;

        send_task->buf.id = i;
        //注册send_task缓冲区的内存
        send_task->mr = ibv_reg_mr(cb->pd, &send_task->buf, sizeof(struct rdma_info), 0);
        if (!send_task->mr) {
            fprintf(stderr, "send_buf reg_mr failed\n");
            ret = errno;
            goto error;
        }
    }

    // valloc申请内存，刷0，rdma注册内存区域进行保护
    cb->rdma_buf = valloc(BUF_SIZE * MAX_TASKS);
    if (!cb->rdma_buf) {
        fprintf(stderr, "rdma_buf alloc failed\n");
        ret = -ENOMEM;
        goto error;
    }
    memset(cb->rdma_buf, 0, BUF_SIZE * MAX_TASKS);
    // printf("cb->pd->handle: %u\ncb->rdma_buf: %x\n", cb->pd->handle, cb->rdma_buf);
    cb->rdma_mr =
        ibv_reg_mr(cb->pd, cb->rdma_buf, BUF_SIZE * MAX_TASKS,
                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!cb->rdma_mr) {
        fprintf(stderr, "rdma_buf reg_mr failed\n");
        ret = errno;
        goto error;
    }

    VERBOSE_LOG(1, "cb->server: %d", cb->server);
    if (!cb->server) {
        char *start_buf;

        // valloc申请内存，刷0，rdma注册内存区域进行保护
        cb->start_buf = valloc(BUF_SIZE * MAX_TASKS);
        if (!cb->start_buf) {
            fprintf(stderr, "start_buf malloc failed\n");
            ret = -ENOMEM;
            goto error;
        }
        start_buf = cb->start_buf;
        memset(cb->start_buf, 0, BUF_SIZE * MAX_TASKS);
        // printf("cb->pd->handle: %u\ncb->start_buf: %x\n", cb->pd->handle, cb->start_buf);
        cb->start_mr =
            ibv_reg_mr(cb->pd, cb->start_buf, BUF_SIZE * MAX_TASKS,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
        if (!cb->start_mr) {
            fprintf(stderr, "start_buf reg_mr failed\n");
            ret = errno;
            goto error;
        }

        INIT_LIST_HEAD(&cb->task_free);
        INIT_LIST_HEAD(&cb->task_alloc);

        /** 客户端初始化send_tasks(将类型也设置好为IBV_WR_SEND) */
        for (i = 0; i < MAX_TASKS; i++) {
            struct rdcp_task *send_task = &cb->send_tasks[i];
            list_add_tail(&send_task->task_list, &cb->task_free);

            /** send_task的buf来自start_buf的切分 */
            send_task->buf.buf = uint64_from_ptr(start_buf);
            send_task->buf.size = BUF_SIZE;
            send_task->buf.rkey = cb->start_mr->rkey;

            /** 初始化send_task的句柄与请求(可能包含多个句柄)，绑定至send_task的buf，实际上就是绑定至start_buf的某块区域 */
            send_task->sgl.addr = uint64_from_ptr(&send_task->buf);
            send_task->sgl.length = sizeof(struct rdma_info);
            send_task->sgl.lkey = send_task->mr->lkey;

            send_task->sq_wr.opcode = IBV_WR_SEND;
            send_task->sq_wr.send_flags = IBV_SEND_SIGNALED;
            send_task->sq_wr.sg_list = &send_task->sgl;
            send_task->sq_wr.num_sge = 1;
            send_task->sq_wr.wr_id = 200 + i;

            start_buf += BUF_SIZE;
        }
    } else {
        // /** 客户端初始化send_tasks(将类型也设置好为IBV_WR_SEND) */
        // for (i = 0; i < MAX_TASKS; i++) {
        //     struct rdcp_task *send_task = &cb->send_tasks[i];
        //     list_add_tail(&send_task->task_list, &cb->task_free);

        //     /**
        //      * 初始化接收缓冲区的句柄(指向缓冲区)
        //      * 1.地址 2.长度 3.描述内存区域的标志key
        //      */
        //     send_task->sgl.addr = uint64_from_ptr(&send_task->buf);
        //     send_task->sgl.length = sizeof(struct rdma_info);
        //     send_task->sgl.lkey = send_task->mr->lkey;
            
        //     VERBOSE_LOG(1, "send task sgl\n");
        //     fprintf(stderr, "send task sgl\n");
        //     /**
        //      * 初始化一个接收请求
        //      * 1.指向请求缓冲区的第一个句柄结构体
        //      * 2.总共的句柄个数(可能会从链表出发有多个句柄)
        //      * 3.请求的id号
        //      */
        //     send_task->sq_wr.opcode = IBV_WR_SEND;
        //     send_task->sq_wr.send_flags = IBV_SEND_SIGNALED;
        //     send_task->sq_wr.sg_list = &send_task->sgl;
        //     send_task->sq_wr.num_sge = 1;
        //     send_task->sq_wr.wr_id = 200 + i;
            
        //     VERBOSE_LOG(1, "send task sgl\n");
        // }
    }

    for (unsigned i = 0; i < MAX_TASKS; i++) {
        VERBOSE_LOG(1, "rdma send buf addr: %lx size: %u id: %d rkey: %u\n",
                    cb->send_tasks[i].buf.buf, cb->send_tasks[i].buf.size, cb->send_tasks[i].buf.id,
                    cb->send_tasks[i].buf.rkey);
    }
    for (unsigned i = 0; i < MAX_TASKS; i++) {
        VERBOSE_LOG(1, "rdma recv buf addr: %lu size: %u id: %d rkey: %u\n",
                    cb->recv_tasks[i].buf.buf, cb->recv_tasks[i].buf.size, cb->recv_tasks[i].buf.id,
                    cb->recv_tasks[i].buf.rkey);
    }

    ret = rdcp_setup_wr(cb);
    if (ret)
        goto error;

    VERBOSE_LOG(3, "allocated & registered buffers...\n");
    return 0;

error:
    rdcp_free_buffers(cb);
    return ret;
}

/**
 * free rdma start recv send metedata
 */
void rdcp_free_buffers(struct rdcp_cb *cb) {
    int i;

    VERBOSE_LOG(3, "rdcp_free_buffers called on cb %p\n", cb);
    if (cb->start_buf)
        free(cb->start_buf);
    if (cb->rdma_mr)
        ibv_dereg_mr(cb->rdma_mr);
    if (cb->rdma_buf)
        free(cb->rdma_buf);

    for (i = 0; i < MAX_TASKS; i++) {
        struct rdcp_task *recv_task = &cb->recv_tasks[i];
        struct rdcp_task *send_task = &cb->send_tasks[i];

        if (recv_task->mr)
            ibv_dereg_mr(recv_task->mr);
        if (send_task->mr)
            ibv_dereg_mr(send_task->mr);
    }
    if (cb->metadata_mr)
        ibv_dereg_mr(cb->metadata_mr);
    //	if (!cb->server) {
    ibv_dereg_mr(cb->start_mr);
    //	}
}

int rdcp_setup_wr(struct rdcp_cb *cb) {
    int i;
    char *buf = cb->rdma_buf;

    /** 元数据信息注册 */
    cb->metadata_mr =
        ibv_reg_mr(cb->pd, &cb->metadata, sizeof(struct metadata_info), IBV_ACCESS_LOCAL_WRITE);
    if (!cb->metadata_mr) {
        fprintf(stderr, "metadata reg_mr failed\n");
        return errno;
    }

    /** 元数据的句柄(指向缓冲区) */
    cb->metadata_sgl.addr = uint64_from_ptr(&cb->metadata);
    cb->metadata_sgl.length = sizeof(struct metadata_info);
    cb->metadata_sgl.lkey = cb->metadata_mr->lkey;

    /** 元数据send请求的初始化(指向元数据句柄) */
    cb->md_send_wr.opcode = IBV_WR_SEND;
    cb->md_send_wr.send_flags = IBV_SEND_SIGNALED;
    cb->md_send_wr.sg_list = &cb->metadata_sgl;
    cb->md_send_wr.num_sge = 1;
    cb->md_send_wr.wr_id = METADATA_WR_ID;

    /** 元数据recv请求的初始化(指向元数据句柄) */
    cb->md_recv_wr.sg_list = &cb->metadata_sgl;
    cb->md_recv_wr.num_sge = 1;
    cb->md_recv_wr.wr_id = METADATA_WR_ID;

    /** rdma句柄与请求初始化，绑定至rdma_buf */
    for (i = 0; i < MAX_TASKS; i++) {
        cb->rdma_sgl[i].addr = uint64_from_ptr(buf);
        cb->rdma_sgl[i].length = BUF_SIZE;
        cb->rdma_sgl[i].lkey = cb->rdma_mr->lkey;
        cb->rdma_sq_wr[i].send_flags = IBV_SEND_SIGNALED;
        cb->rdma_sq_wr[i].sg_list = &cb->rdma_sgl[i];
        cb->rdma_sq_wr[i].num_sge = 1;
        buf += BUF_SIZE;
    }

    return 0;
}