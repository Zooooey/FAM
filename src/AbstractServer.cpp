#include "AbstractServer.h"

#define TEST_NZ(x) do { if ( (x)) rc_die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) rc_die("error: " #x " failed (returned zero/null)."); } while (0)

void AbstractServer::rc_die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

void * AbstractServer::poll_cq(void *arg)
{

  AbstractServer* instance = static_cast<AbstractServer*>(arg);

  struct context *ss_ctx = static_cast<struct context *>(instance->s_ctx);

  struct ibv_cq *cq = ss_ctx->cq;
  struct ibv_wc wc;

  while (1) {//! should_disconnect
    TEST_NZ(ibv_get_cq_event(ss_ctx->comp_channel, &cq, static_cast<void**>(&(instance->s_ctx))));
    ibv_ack_cq_events(cq, 1);
    TEST_NZ(ibv_req_notify_cq(cq, 0));

    while (ibv_poll_cq(cq, 1, &wc)) {
      if (wc.status == IBV_WC_SUCCESS)
        instance->on_completion(&wc);
      else
        rc_die("poll_cq: status is not IBV_WC_SUCCESS");
    }
  }

  return NULL;
}

void AbstractServer::build_params(struct rdma_conn_param *params)
{
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 7; /* infinite retry */
}

void AbstractServer::build_connection(struct rdma_cm_id *id, bool is_qp0)
{
  struct ibv_qp_init_attr qp_attr;

  build_context(id->verbs);// guaranteed to only go thru on qp0
  build_qp_attr(&qp_attr, is_qp0);// do special handling on qp > 0

  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));
}


void AbstractServer::build_qp_attr(struct ibv_qp_init_attr *qp_attr, bool is_qp0)// take index as param
{
  memset(qp_attr, 0, sizeof(*qp_attr));

  if (is_qp0) {
    qp_attr->send_cq = s_ctx->cq;// index into cq array /or just make a new qp
    qp_attr->recv_cq = s_ctx->cq;// reuse from above
  } else {
    // struct ibv_cq * cq;
    // TEST_Z(cq = ibv_create_cq(s_ctx->ctx, 10, NULL, NULL, 0)); /* cqe=10 is arbitrary
    // */ qp_attr->send_cq = cq; //index into cq array /or just make a new qp
    // qp_attr->recv_cq = cq; //reuse from above
  }

  qp_attr->qp_type = IBV_QPT_RC;

  qp_attr->cap.max_send_wr = 1600;// max from ibv_devinfo: max_qp_wr: 16351
  qp_attr->cap.max_recv_wr = 40;
  qp_attr->cap.max_send_sge = 1;
  qp_attr->cap.max_recv_sge = 1;
  qp_attr->sq_sig_all = 0;// shouldn't need this explicitly
}

void AbstractServer::build_context(struct ibv_context *verbs)
{
  if (s_ctx) {
    if (s_ctx->ctx != verbs) rc_die("cannot handle events in more than one context.");

    return;
  }

  s_ctx = static_cast<struct context *>(malloc(sizeof(struct context)));

  s_ctx->ctx = verbs;
  s_ctx->connections = 0;

  TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
  TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));
  TEST_Z(s_ctx->cq = ibv_create_cq(
           s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0)); /* cqe=10 is arbitrary */
  TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));// can flip to solicited only

  TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, this));
}

//An event_loop both being used in server and client
void AbstractServer::event_loop(struct rdma_event_channel *ec, int exit_on_disconnect)
{
  struct rdma_cm_event *event = NULL;
  struct rdma_conn_param cm_params;

  build_params(&cm_params);

  // only run custom handlers on connection 0
  bool latch0 = false;
  // bool latch1 = false;
  bool latch2 = false;
  bool latch3 = false;
  bool latch4 = false;

  while (rdma_get_cm_event(ec, &event) == 0) {
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);

    if (event_copy.event == RDMA_CM_EVENT_ADDR_RESOLVED) {// Runs on client
      build_connection(event_copy.id, !latch0);
      BOOST_LOG_TRIVIAL(debug) << "CLIENT1";

      if (!latch0) on_pre_conn(event_copy.id);
      TEST_NZ(rdma_resolve_route(event_copy.id, TIMEOUT_IN_MS));
      latch0 = true;
    } else if (event_copy.event == RDMA_CM_EVENT_ROUTE_RESOLVED) {// Runs on client
      TEST_NZ(rdma_connect(event_copy.id, &cm_params));
      BOOST_LOG_TRIVIAL(debug) << "CLIENT2";
    } else if (event_copy.event == RDMA_CM_EVENT_CONNECT_REQUEST) {// Runs on server
      build_connection(event_copy.id, !latch2);
      BOOST_LOG_TRIVIAL(info) << "RDMA_CM Connect request received!";
      if (!latch2)on_pre_conn(event_copy.id);

      TEST_NZ(rdma_accept(event_copy.id, &cm_params));
      latch2 = true;
    } else if (event_copy.event == RDMA_CM_EVENT_ESTABLISHED) {// Runs on both
      if(!latch3) on_connection(event_copy.id);
      BOOST_LOG_TRIVIAL(info) << "RDMA_CM Established, id:"<<event_copy.id;
      latch3 = true;
      s_ctx->connections++;
    } else if (event_copy.event == RDMA_CM_EVENT_DISCONNECTED) {// Runs on both
      rdma_destroy_qp(event_copy.id);
      BOOST_LOG_TRIVIAL(info) << "Connection disconnected, id:"<<event_copy.id;
      if(!latch4) on_disconnect(event_copy.id);
      rdma_destroy_id(event_copy.id);

      if (exit_on_disconnect) break;
      latch4 = true;
    } else {
      BOOST_LOG_TRIVIAL(fatal) << cm_event_to_string(event_copy.event);
      throw std::runtime_error("RDMA event not handled");
    }
  }
}
