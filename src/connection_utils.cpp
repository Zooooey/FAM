#include <connection_utils.hpp>

#include <boost/log/trivial.hpp>

#include <client_runtime.hpp>
#include "fam_common.hpp"

struct context
{
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;
  volatile unsigned long connections;

  pthread_t cq_poller_thread;
};

static struct context *s_ctx = NULL;
static pre_conn_cb_fn s_on_pre_conn_cb = NULL;
static connect_cb_fn s_on_connect_cb = NULL;
static completion_cb_fn s_on_completion_cb = NULL;
static disconnect_cb_fn s_on_disconnect_cb = NULL;

static void build_context(struct ibv_context *verbs);
static void build_qp_attr(struct ibv_qp_init_attr *qp_attr, bool is_qp0);
static void event_loop(struct rdma_event_channel *ec, int exit_on_disconnect);
static void event_loop(struct rdma_event_channel *ec, int exit_on_disconnect, AbstractServer* server);
static void *poll_cq(void *);

unsigned long rc_get_num_connections() { return s_ctx->connections; }

void build_connection(struct rdma_cm_id *id, bool is_qp0)
{
  struct ibv_qp_init_attr qp_attr;

  build_context(id->verbs);// guaranteed to only go thru on qp0
  build_qp_attr(&qp_attr, is_qp0);// do special handling on qp > 0

  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));
}

void build_context(struct ibv_context *verbs)
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

  TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, s_ctx));
}

void build_params(struct rdma_conn_param *params)
{
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 7; /* infinite retry */
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr, bool is_qp0)// take index as param
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

  qp_attr->cap.max_send_wr = 40;// max from ibv_devinfo: max_qp_wr: 16351
  qp_attr->cap.max_recv_wr = 1600;
  qp_attr->cap.max_send_sge = 1;
  qp_attr->cap.max_recv_sge = 1;
  qp_attr->sq_sig_all = 0;// shouldn't need this explicitly
}

namespace {
std::string cm_event_to_string(rdma_cm_event_type e)
{
  switch (e) {
  case RDMA_CM_EVENT_ADDR_RESOLVED:
    return "RDMA_CM_EVENT_ADDR_RESOLVED";
    break;
  case RDMA_CM_EVENT_ADDR_ERROR:
    return "RDMA_CM_EVENT_ADDR_ERROR";
    break;
  case RDMA_CM_EVENT_ROUTE_RESOLVED:
    return "RDMA_CM_EVENT_ROUTE_RESOLVED";
    break;
  case RDMA_CM_EVENT_ROUTE_ERROR:
    return "RDMA_CM_EVENT_ROUTE_ERROR";
    break;
  case RDMA_CM_EVENT_CONNECT_REQUEST:
    return "RDMA_CM_EVENT_CONNECT_REQUEST";
    break;
  case RDMA_CM_EVENT_CONNECT_RESPONSE:
    return "RDMA_CM_EVENT_CONNECT_RESPONSE";
    break;
  case RDMA_CM_EVENT_CONNECT_ERROR:
    return "RDMA_CM_EVENT_CONNECT_ERROR";
    break;
  case RDMA_CM_EVENT_UNREACHABLE:
    return "RDMA_CM_EVENT_UNREACHABLE";
    break;
  case RDMA_CM_EVENT_REJECTED:
    return "RDMA_CM_EVENT_REJECTED";
    break;
  case RDMA_CM_EVENT_ESTABLISHED:
    return "RDMA_CM_EVENT_ESTABLISHED";
    break;
  case RDMA_CM_EVENT_DISCONNECTED:
    return "RDMA_CM_EVENT_DISCONNECTED";
    break;
  case RDMA_CM_EVENT_DEVICE_REMOVAL:
    return "RDMA_CM_EVENT_DEVICE_REMOVAL";
    break;
  case RDMA_CM_EVENT_MULTICAST_JOIN:
    return "RDMA_CM_EVENT_MULTICAST_JOIN";
    break;
  case RDMA_CM_EVENT_MULTICAST_ERROR:
    return "RDMA_CM_EVENT_MULTICAST_ERROR";
    break;
  case RDMA_CM_EVENT_ADDR_CHANGE:
    return "RDMA_CM_EVENT_ADDR_CHANGE";
    break;
  case RDMA_CM_EVENT_TIMEWAIT_EXIT:
    return "RDMA_CM_EVENT_TIMEWAIT_EXIT";
    break;
  }
  return "To string event unknown";
}
}// namespace

//FIXME: merge it!!
void event_loop(struct rdma_event_channel *ec, int exit_on_disconnect)
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
      if (s_on_pre_conn_cb && !latch0) s_on_pre_conn_cb(event_copy.id);

      TEST_NZ(rdma_resolve_route(event_copy.id, TIMEOUT_IN_MS));

      latch0 = true;
    } else if (event_copy.event == RDMA_CM_EVENT_ROUTE_RESOLVED) {// Runs on client
      TEST_NZ(rdma_connect(event_copy.id, &cm_params));
      BOOST_LOG_TRIVIAL(debug) << "CLIENT2";
    } else if (event_copy.event == RDMA_CM_EVENT_CONNECT_REQUEST) {// Runs on server
      build_connection(event_copy.id, !latch2);
      BOOST_LOG_TRIVIAL(info) << "RDMA_CM Connect request received!";
      if (s_on_pre_conn_cb && !latch2) s_on_pre_conn_cb(event_copy.id);

      TEST_NZ(rdma_accept(event_copy.id, &cm_params));
      latch2 = true;
    } else if (event_copy.event == RDMA_CM_EVENT_ESTABLISHED) {// Runs on both
      if (s_on_connect_cb && !latch3) s_on_connect_cb(event_copy.id);
      BOOST_LOG_TRIVIAL(info) << "RDMA_CM Established, id:"<<event_copy.id;
      latch3 = true;
      s_ctx->connections++;
    } else if (event_copy.event == RDMA_CM_EVENT_DISCONNECTED) {// Runs on both
      rdma_destroy_qp(event_copy.id);
      BOOST_LOG_TRIVIAL(info) << "Connection disconnected, id:"<<event_copy.id;
      if (s_on_disconnect_cb && !latch4) s_on_disconnect_cb(event_copy.id);

      rdma_destroy_id(event_copy.id);

      if (exit_on_disconnect) break;
      latch4 = true;
    } else {
      BOOST_LOG_TRIVIAL(fatal) << cm_event_to_string(event_copy.event);
      throw std::runtime_error("RDMA event not handled");
    }
  }
}

// thread blocks waiting for control plane rpc's
void *poll_cq(void *ctx)
{
  struct context *ss_ctx = static_cast<struct context *>(ctx);

  struct ibv_cq *cq = ss_ctx->cq;
  struct ibv_wc wc;

  while (1) {//! should_disconnect
    TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx));
    ibv_ack_cq_events(cq, 1);
    TEST_NZ(ibv_req_notify_cq(cq, 0));

    while (ibv_poll_cq(cq, 1, &wc)) {
      if (wc.status == IBV_WC_SUCCESS){
        temporary_test(&wc,cq);
        s_on_completion_cb(&wc);
      }
      else
        rc_die("poll_cq: status is not IBV_WC_SUCCESS");
    }
  }

  return NULL;
}


void temporary_test(struct ibv_wc *wc, struct ibv_cq * cq){
    BOOST_LOG_TRIVIAL(debug) << "on completion";
  struct rdma_cm_id *id = reinterpret_cast<struct rdma_cm_id *>(wc->wr_id);
  struct client_context *ctx = static_cast<struct client_context *>(id->context);

  if (wc->opcode & IBV_WC_RECV) {
    if (ctx->rx_msg->id == MSG_MR) {
      ctx->peer_addr = ctx->rx_msg->data.mr.addr;
      ctx->peer_rkey = ctx->rx_msg->data.mr.rkey;
      uint64_t const num_edges = ctx->rx_msg->data.mr.total_edges;
      ctx->num_edges = num_edges;
      post_receive(id);
      BOOST_LOG_TRIVIAL(info) << "RDMA: Received server MR, remote_addr:"<<ctx->peer_addr<<" rkey:"<<ctx->peer_rkey<<" remote edges:"<<num_edges;

      BOOST_LOG_TRIVIAL(info) << "Sending a test RDMA_READ request to server...";
      //TODO
      struct ibv_send_wr *bad_wr = NULL;
      struct ibv_send_wr wr;
      memset(&wr, 0, sizeof(wr));// maybe optimize away
      wr.opcode = IBV_WR_RDMA_READ;
      wr.send_flags = IBV_SEND_SIGNALED;// can change for selective signaling
      wr.wr.rdma.remote_addr = ctx->peer_addr + 0;
      wr.wr.rdma.rkey = ctx->peer_rkey;

      struct ibv_sge sge;
      wr.sg_list=&sge;
      wr.num_sge = 1;

      uint32_t test_target_id;
      sg.addr = reinterpret_cast<uintptr_t>(&test_target_id);
      sg.length = sizeof(uint32_t);

      int ret = ibv_post_send(id->qp, &wr, &bad_wr);
      if(ret != 0 ){
        BOOST_LOG_TRIVIAL(fatal) << "Sending a test RDMA_READ request to server failed!";
      }
      while (ibv_poll_cq(cq, 1, &wc)) {
        if (wc.status == IBV_WC_SUCCESS){
          BOOST_LOG_TRIVIAL(info) << "Sending a test RDMA_READ request to server successful!";
          BOOST_LOG_TRIVIAL(info) << "The target id read from RDMA server is :"<<test_target_id;
        }
      else
        rc_die("poll_cq: status is not IBV_WC_SUCCESS");
    }

    }
  }
}

void rc_init(pre_conn_cb_fn pc,
  connect_cb_fn conn,
  completion_cb_fn comp,
  disconnect_cb_fn disc)
{
  s_on_pre_conn_cb = pc;
  s_on_connect_cb = conn;
  s_on_completion_cb = comp;
  s_on_disconnect_cb = disc;
}

void rc_client_loop(const char *host, const char *port, struct client_context *context)
{
  struct addrinfo *addr;
  struct rdma_cm_id *conn = NULL;
  struct rdma_event_channel *ec = NULL;

  TEST_NZ(getaddrinfo(host, port, NULL, &addr));

  TEST_Z(ec = rdma_create_event_channel());
  TEST_NZ(rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP));
  TEST_NZ(rdma_resolve_addr(conn, NULL, addr->ai_addr, TIMEOUT_IN_MS));

  context->base_id = conn;
  for (auto &conn_ptr : context->cm_ids) {
    TEST_NZ(rdma_create_id(ec, &conn_ptr, NULL, RDMA_PS_TCP));
    // TEST_NZ(rdma_resolve_addr(conn_ptr, NULL, addr->ai_addr, TIMEOUT_IN_MS));
    conn_ptr->context = context;
  }

  // freeaddrinfo(addr);

  context->addr = addr;
  conn->context = context;

  event_loop(ec, 1);// exit on disconnect

  rdma_destroy_event_channel(ec);
}


void rc_disconnect(struct rdma_cm_id *id) { rdma_disconnect(id); }

struct ibv_pd *rc_get_pd() { return s_ctx->pd; }
