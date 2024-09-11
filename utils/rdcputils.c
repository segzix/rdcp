#include "rdcp.h"
#include "utils.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * 建立rdma通信管道，并给出id
 */
int rdcp_create_event_channel(struct rdcp_cb *cb) {
    int ret;

    cb->cm_channel = rdma_create_event_channel();
    if (!cb->cm_channel) {
        perror("rdma_create_event_channel");
        return errno;
    }

    ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
    if (ret) {
        perror("rdma_create_id");
        goto destroy_event_chan;
    }
    VERBOSE_LOG(3, "created cm_id %p\n", cb->cm_id);

    return 0;

destroy_event_chan:
    rdma_destroy_event_channel(cb->cm_channel);
    return ret;
}

/**
 * 销毁rdma通信管道
 */
void rdcp_destroy_event_channel(struct rdcp_cb *cb) {
    VERBOSE_LOG(3, "destroy cm_id %p\n", cb->cm_id);
    rdma_destroy_id(cb->cm_id);
    rdma_destroy_event_channel(cb->cm_channel);
}