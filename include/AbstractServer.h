#ifndef AbstractServer_H
#define AbstractServer_H
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

  
class AbstractServer{
private:
  void build_params(struct rdma_conn_param *params);
public:
  virtual void on_pre_conn(struct rdma_cm_id *id) = 0;
  virtual void on_connection(struct rdma_cm_id *id) = 0;
  virtual void on_completion(struct ibv_wc *wc) = 0;
  virtual void on_disconnect(struct rdma_cm_id *id) = 0;
  virtual void run(boost::program_options::variables_map const &vm) = 0;
  void event_loop(struct rdma_event_channel *ec, int exit_on_disconnect);
};



#endif