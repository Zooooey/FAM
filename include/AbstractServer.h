#ifndef AbstractServer_H
#define AbstractServer_H
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <boost/program_options.hpp>

#define TIMEOUT_IN_MS 500;
class AbstractServer{

protected:
struct context
{
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;
  volatile unsigned long connections;

  pthread_t cq_poller_thread;
};
  struct context *s_ctx = NULL;
  void build_params(struct rdma_conn_param *params);
  void build_connection(struct rdma_cm_id *id, bool is_qp0);
  void build_context(struct ibv_context *verbs);
  void build_qp_attr(struct ibv_qp_init_attr *qp_attr, bool is_qp0);
  static void *poll_cq(void *ctx);
public:
  virtual void on_pre_conn(struct rdma_cm_id *id) = 0;
  virtual void on_connection(struct rdma_cm_id *id) = 0;
  virtual void on_completion(struct ibv_wc *wc) = 0;
  virtual void on_disconnect(struct rdma_cm_id *id) = 0;
  virtual void run(boost::program_options::variables_map const &vm) = 0;
  void event_loop(struct rdma_event_channel *ec, int exit_on_disconnect);
};



#endif