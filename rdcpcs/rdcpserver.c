#include "rdcp.h"
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <unistd.h>

/**
 * 服务端回复客户端(将客户端的文件大小写入)
 */
int server_response(struct rdcp_cb *cb, struct ibv_wc *wc) {
    int ret;
    int id = wc->wr_id;
    struct ibv_send_wr *bad_wr;
    struct rdcp_task *send_task = &cb->send_tasks[id];
    struct rdcp_task *recv_task = &cb->recv_tasks[id];

    send_task->buf.size = recv_task->buf.size;
    ret = ibv_post_send(cb->qp, &send_task->sq_wr, &bad_wr);
    if (ret) {
        perror("server response error");
        return ret;
    }
    VERBOSE_LOG(1, "server posted go ahead\n");

    return 0;
}

/**
 * 服务端打开要写入的文件准备进行写入
 */
int server_open_dest(struct rdcp_cb *cb) {
    DIR *d;

    //这里的cb->metadata是直接由客户端传进来了
    d = opendir(cb->metadata.dst_path);
    if (d) {
        closedir(d);
        // XXX: can overflow dst_path
        strcat(cb->metadata.dst_path, "/");
        strcat(cb->metadata.dst_path, basename(cb->metadata.src_path));
    } else if (ENOENT == errno) {
        // ok. create a file.
    } else if (ENOTDIR == errno) {
        // ok. It's not a dir. overwrite
    } else {
        perror("open error");
        return errno;
    }

    printf("Content of metadata src: %s dst: %s\n", cb->metadata.src_path, cb->metadata.dst_path);
    fflush(stdout);

    cb->fd = open(cb->metadata.dst_path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (cb->fd < 0) {
        perror("failed to open file");
        return errno;
    }

    VERBOSE_LOG(1, "open fd %d\n", cb->fd);

    return 0;
}

/**
 * 服务端处理客户端发过来的元数据，同时将自己的元数据发送给客户端
 */
int server_recv_metadata(struct rdcp_cb *cb, struct ibv_wc *wc) {
    int ret = 0;

    VERBOSE_LOG(1, "Got metadata, replying\n");
    // TODO: Send protocol version to client to verify
    if (!cb->use_null) {
        ret = server_open_dest(cb);
        if (ret)
            goto out;
    }

    /** 建立元数据发送请求(send请求已经在rdcp_setup_wr中初始化) */
    ret = ibv_post_send(cb->qp, &cb->md_send_wr, NULL);
    if (ret) {
        perror("failed reply metadata");
        close(cb->fd);
    }

out:
    return ret;
}

/**
 * 服务端处理接收请求
 */
int server_recv(struct rdcp_cb *cb, struct ibv_wc *wc) {
    struct rdcp_task *task;
    int ret;
    struct ibv_send_wr *bad_wr;
    int i;

    //可能是需要接收元数据(元数据主要包含了文件的相关信息)
    if (wc->wr_id == METADATA_WR_ID) {
        return server_recv_metadata(cb, wc);
    }

    /** 非元数据(此时传输的数据已经放入recv_tasks缓冲区了) */
    i = wc->wr_id;
    task = &cb->recv_tasks[i];

    if (wc->byte_len != sizeof(struct rdma_info)) {
        fprintf(stderr, "Received bogus data, size %d\n", wc->byte_len);
        return -1;
    }

    /** 远程文件的key addr等信息*/
    cb->remote_rkey = task->buf.rkey;
    cb->remote_addr = task->buf.buf;
    cb->remote_len = task->buf.size;
    VERBOSE_LOG(1, "Received rkey %x addr %" PRIx64 " len %d from peer\n", cb->remote_rkey,
                task->buf.buf, cb->remote_len);
    VERBOSE_LOG(1, "rdma read %d %p\n", i, &cb->rdma_buf[i * BUF_SIZE]);

    /** Issue RDMA Read. */
    /** 处理完recv请求，通过send队列发回(注意现在是READ请求)(注意请求中一些信息是参照了远程send请求的信息) */
    cb->rdma_sq_wr[i].opcode = IBV_WR_RDMA_READ;
    cb->rdma_sq_wr[i].wr.rdma.rkey = cb->remote_rkey;
    cb->rdma_sq_wr[i].wr.rdma.remote_addr = cb->remote_addr;
    cb->rdma_sq_wr[i].sg_list->length = cb->remote_len;
    cb->rdma_sq_wr[i].wr_id = i;
    ret = ibv_post_send(cb->qp, &cb->rdma_sq_wr[i], &bad_wr);
    if (ret) {
        perror("post rdma read failed");
        return ret;
    }
    VERBOSE_LOG(3, "server posted rdma read req \n");

    if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
        cb->state = RDMA_READ_ADV;
    else
        cb->state = RDMA_WRITE_ADV;

    return 0;
}

/**
 * test server?为什么要一直sem_wait等待
 */
int rdcp_test_server(struct rdcp_cb *cb) {
    int ret = 0;

    while (1) {
        /** Wait for client's Start STAG/TO/Len */
        sem_wait(&cb->sem);
    }

    close(cb->fd);
    cb->fd = -1;

    return (cb->state == DISCONNECTED) ? 0 : ret;
}

int rdcp_bind_server(struct rdcp_cb *cb) {
    int ret;

    // 判断是ipv4还是ipv6
    if (cb->sin.ss_family == AF_INET)
        ((struct sockaddr_in *)&cb->sin)->sin_port = cb->port;
    else
        ((struct sockaddr_in6 *)&cb->sin)->sin6_port = cb->port;

    // 将该地址绑定到对应的rdma id端口
    ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&cb->sin);
    if (ret) {
        perror("rdma_bind_addr");
        return ret;
    }
    VERBOSE_LOG(3, "rdma_bind_addr successful\n");
    VERBOSE_LOG(3, "rdma_listen\n");
    // 监听rdma端口(超过3个连接请求开始拒绝)，现在只是设置状态
    ret = rdma_listen(cb->cm_id, 3);
    if (ret) {
        perror("rdma_listen");
        return ret;
    }

    return 0;
}

/**
 * 建立一个新的rdma连接(接收客户端的连接，并等待资源被释放)
 */
int rdcp_accept(struct rdcp_cb *cb) {
    int ret;

    VERBOSE_LOG(3, "accepting client connection request\n");

    ret = rdma_accept(cb->child_cm_id, NULL);
    if (ret) {
        perror("rdma_accept");
        return ret;
    }

    sem_wait(&cb->sem);
    if (cb->state == ERROR) {
        fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
        return -1;
    }
    return 0;
}
