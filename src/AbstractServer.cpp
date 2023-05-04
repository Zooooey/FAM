#include "AbstractServer.h"

void AbstractServer::build_params(struct rdma_conn_param *params)
{
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 7; /* infinite retry */
}

void AbstractServer::build_connection(struct rdma_cm_id *id, bool is_qp0);
{
  struct ibv_qp_init_attr qp_attr;

  build_context(id->verbs);// guaranteed to only go thru on qp0
  build_qp_attr(&qp_attr, is_qp0);// do special handling on qp > 0

  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));
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
