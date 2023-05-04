#ifndef FAMServer_H
#define FAMServer_H

#include <iostream>
#include <stdexcept>

#include "AbstractServer.h"
#include <boost/filesystem.hpp>//Remove when refactor edgelist init
#include <boost/filesystem/fstream.hpp>//Remove when refactor edgelist init

class FAMServer : public AbstractServer
{
private:
  /*void post_receive(struct rdma_cm_id *id);
  auto get_edge_list(std::string adj_file, ibv_pd *pd, bool use_HP, int fam_thp_flag);
  void send_message(struct rdma_cm_id *id);*/
    struct server_context *server_ctx = 0;
public:
  virtual void on_pre_conn(struct rdma_cm_id *id) override;
  virtual void on_connection(struct rdma_cm_id *id) override;
  virtual void on_completion(struct ibv_wc *wc) override;
  virtual void on_disconnect(struct rdma_cm_id *id) override;
  virtual void run(boost::program_options::variables_map const &vm) override;
  FAMServer();
};

#endif