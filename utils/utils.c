#include "utils.h"
#include "rdcp.h"
#include <libgen.h>
#include <netdb.h>
#include <sys/time.h>

void print_send_status(struct rdcp_cb *cb, long *_total_size, int print) {
    static long long tick1 = 0, tick2 = 0;
    static long sent_size = 0;
    long total_size = *_total_size;

    if (tick1 == 0)
        tick1 = current_timestamp();

    tick2 = current_timestamp();
    VERBOSE_LOG(3, "tick %lld\n", tick2);

    if (tick2 - tick1 >= 1000 || print) {
        float sec = (tick2 - tick1) / 1000.0;
        long mb = 1024 * 1024;
        long gb = 1024 * mb;
        int percent = 100;
        long eta = 0;
        char size_spec[3] = "MB";
        long div = mb;
        float bytes_per_sec = total_size / sec;

        sent_size += total_size;
        if (cb->metadata.size > 0) {
            percent = sent_size * 100 / cb->metadata.size;
            eta = ((cb->metadata.size - sent_size) / bytes_per_sec);
        }
        if (sent_size > 10 * gb) {
            strcpy(size_spec, "GB");
            div = gb;
        }
        printf("\r%-20s %d%% %5d%s %5dMB/s  %02d:%02d ETA", basename(cb->metadata.src_path),
               percent, (int)(sent_size / div), size_spec, (int)(bytes_per_sec / mb),
               (int)(eta / 60), (int)(eta % 60));
        fflush(stdout);
        total_size = 0;
        *_total_size = 0;
        tick1 = tick2;
    }
}

long long current_timestamp() {
    long long milliseconds;
    struct timeval te;
    gettimeofday(&te, NULL);                               // get current time
    milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000; // caculate milliseconds
    return milliseconds;
}

int get_addr(char *dst, struct sockaddr *addr) {
    struct addrinfo *res;
    int ret;

    ret = getaddrinfo(dst, NULL, NULL, &res);
    if (ret) {
        printf("getaddrinfo failed - invalid hostname or IP address\n");
        return ret;
    }

    if (res->ai_family == PF_INET)
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
    else if (res->ai_family == PF_INET6)
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
    else
        ret = -1;

    freeaddrinfo(res);
    return ret;
}

void usage() {
    printf("usage: rdcp -s [-vt] [-p port]\n");
    printf("       rdcp [-vt] [-p port] file1 host2:file2\n\n");
    printf("  -s        server mode\n");
    printf("  -t        null device\n");
    printf("  -v        verbose mode\n");
    printf("  -p port   port\n");
}