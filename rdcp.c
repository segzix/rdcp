// vim: set ai ts=8 sw=8 softtabstop=8:
/**
 * Copyright (c) 2015 Roi Dayan, Slava Shwartsman.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rdcp.h"
#include "utils.h"
#include <arpa/inet.h>
#include <byteswap.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <libgen.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

int verbose = 3;

static void *cm_thread(void *arg);
static int rdcp_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event);

int main(int argc, char *argv[]) {
    struct rdcp_cb *cb;
    int op;
    int ret = 0;

    cb = malloc(sizeof(*cb));
    if (!cb)
        return -ENOMEM;

    memset(cb, 0, sizeof(*cb));
    cb->fd = -1;
    cb->server = 0;
    cb->state = IDLE;
    cb->sin.ss_family = AF_INET;
    // TODO: use htons when set in sockaddr and not here
    cb->port = htons(RDCP_PORT);
    sem_init(&cb->sem, 0, 0);

    opterr = 0;
    while ((op = getopt(argc, argv, "Pp:tsv")) != -1) {
        switch (op) {
        case 't':
            cb->use_null = 1;
            break;
        case 'p':
            cb->port = htons(atoi(optarg));
            VERBOSE_LOG(3, "port %d\n", (int)atoi(optarg));
            break;
        case 's':
            cb->server = 1;
            VERBOSE_LOG(3, "server\n");
            break;
        case 'v':
            verbose++;
            VERBOSE_LOG(3, "verbose\n");
            break;
        default:
            usage();
            ret = -EINVAL;
            goto out;
        }
    }
    if (ret)
        goto out;

    /**
     * 1.服务端：考虑保证没有文件相关的参数，否则进入usage()
     * 2.客户端：1)必须要有两个文件参数，否则usage()
     * 			2)目前似乎只能保证远程主机(带:)作为第二个文件参数
     *	处理完命令行参数，写入cb结构体中
     */
    if (cb->server) {
        if (optind < argc) {
            usage();
            ret = -EINVAL;
            goto out;
        }
    } else {
        if (optind + 1 >= argc) {
            usage();
            ret = -EINVAL;
            goto out;
        }
        char *p;

        strncpy(cb->metadata.src_path, argv[optind], PATH_MAX);
        optind++;
        p = strchr(argv[optind], ':');
        if (!p) {
            usage();
            ret = -EINVAL;
            goto out;
        }
        *p = '\0';
        ret = get_addr(argv[optind], (struct sockaddr *)&cb->sin);
        strncpy(cb->metadata.dst_path, p + 1, PATH_MAX);

        VERBOSE_LOG(1, "addr %s\n", argv[optind]);
        VERBOSE_LOG(1, "src %s\n", cb->metadata.src_path);
        VERBOSE_LOG(1, "dst %s\n", cb->metadata.dst_path);
    }

    if (cb->server == -1) {
        usage();
        ret = -EINVAL;
        goto out;
    }

    ret = rdcp_create_event_channel(cb);
    if (ret)
        goto out;

    pthread_create(&cb->cmthread, NULL, cm_thread, cb);

    if (cb->server) {
        ret = rdcp_run_server(cb);
    } else {
        ret = rdcp_run_client(cb);
    }

    rdcp_destroy_event_channel(cb);

out:
    free(cb);
    return ret;
}

int rdcp_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event) {
    int ret = 0;
    struct rdcp_cb *cb = cma_id->context;

    VERBOSE_LOG(3, "cma_event type %s cma_id %p (%s)\n", rdma_event_str(event->event), cma_id,
                (cma_id == cb->cm_id) ? "me" : "remote");

    switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        cb->state = ADDR_RESOLVED;
        ret = rdma_resolve_route(cma_id, 2000);
        if (ret) {
            cb->state = ERROR;
            perror("rdma_resolve_route");
            sem_post(&cb->sem);
        }
        break;

    case RDMA_CM_EVENT_ROUTE_RESOLVED:
        cb->state = ROUTE_RESOLVED;
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_CONNECT_REQUEST:
        cb->state = CONNECT_REQUEST;
        cb->child_cm_id = cma_id;
        VERBOSE_LOG(3, "child cma %p\n", cb->child_cm_id);
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_ESTABLISHED:
        VERBOSE_LOG(3, "ESTABLISHED\n");

        /**
         * Server will wake up when first RECV completes.
         */
        if (!cb->server) {
            cb->state = CONNECTED;
        }
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
        fprintf(stderr, "cma event %s, error %d\n", rdma_event_str(event->event), event->status);
        sem_post(&cb->sem);
        ret = -1;
        break;

    case RDMA_CM_EVENT_DISCONNECTED:
        VERBOSE_LOG(3, "%s DISCONNECT EVENT...\n", cb->server ? "server" : "client");
        cb->state = DISCONNECTED;
        // TODO nooo
        sync();
        sem_post(&cb->sem);
        break;

    case RDMA_CM_EVENT_DEVICE_REMOVAL:
        fprintf(stderr, "cma detected device removal!!!!\n");
        ret = -1;
        break;

    default:
        fprintf(stderr, "unhandled event: %s, ignoring\n", rdma_event_str(event->event));
        break;
    }

    return ret;
}

void *cm_thread(void *arg) {
    struct rdcp_cb *cb = arg;
    struct rdma_cm_event *event;
    int ret;

    while (1) {
        ret = rdma_get_cm_event(cb->cm_channel, &event);
        if (ret) {
            perror("rdma_get_cm_event");
            exit(ret);
        }
        ret = rdcp_cma_event_handler(event->id, event);
        rdma_ack_cm_event(event);
        if (ret)
            exit(ret);
        if (cb->state >= DISCONNECTED) {
            if (cb->server)
                rdma_disconnect(cb->child_cm_id);
            VERBOSE_LOG(1, "post disconnect\n");
            // wakeup cq_thread
            ibv_post_send(cb->qp, &cb->send_tasks[0].sq_wr, NULL);
            exit(ret);
        }
    }
}
