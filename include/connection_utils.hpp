#ifndef RDMA_CONNECTION_UTILS_H
#define RDMA_CONNECTION_UTILS_H

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "AbstractServer.h"
#include <rdma/rdma_cma.h>


typedef void (*pre_conn_cb_fn)(struct rdma_cm_id *id);
typedef void (*connect_cb_fn)(struct rdma_cm_id *id);
typedef void (*completion_cb_fn)(struct ibv_wc *wc, struct ibv_cq * cq);
typedef void (*disconnect_cb_fn)(struct rdma_cm_id *id);

void rc_init(pre_conn_cb_fn, connect_cb_fn, completion_cb_fn, disconnect_cb_fn);
void rc_client_loop(const char *host, const char *port, struct client_context* context);
void rc_disconnect(struct rdma_cm_id *id);
struct ibv_pd * rc_get_pd();
unsigned long rc_get_num_connections();

void rc_server_loop(const char *port);
//void rc_server_loop(const char *port, AbstractServer * server);

constexpr int TIMEOUT_IN_MS = 500;

#endif
