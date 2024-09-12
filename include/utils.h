#ifndef _INCLUDE_UTILS_H
#define _INCLUDE_UTILS_H

#include "rdcp.h"

/** utils */
void print_send_status(struct rdcp_cb *cb, long *_total_size, int print);
long long current_timestamp();
int get_addr(char *dst, struct sockaddr *addr);
void usage();

/** rdcp utils */
int rdcp_create_event_channel(struct rdcp_cb *cb);
void rdcp_destroy_event_channel(struct rdcp_cb *cb);

#endif