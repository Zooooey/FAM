#ifndef AbstractServer_H
#define AbstractServer_H
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

  
class AbstractServer{
  virtual void on_pre_conn(struct rdma_cm_id *id) = 0;
  virtual void on_connection(struct rdma_cm_id *id) = 0;
  virtual void on_completion(struct ibv_wc *wc) = 0;
  virtual void on_disconnect(struct rdma_cm_id *id) = 0;
};



#endif