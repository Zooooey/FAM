#ifndef FAMServer_H
#define FAMServer_H

#include <iostream>
#include <stdexcept>

#include "AbstractServer.h"

class FAMServer : public AbstractServer
{
private:
  template<typename T> auto num_elements(boost::filesystem::path const &p);
  void post_receive(struct rdma_cm_id *id);
  auto get_edge_list(std::string adj_file, ibv_pd *pd, bool use_HP, int fam_thp_flag);
  void send_message(struct rdma_cm_id *id);

public:
  static void validate_params(boost::program_options::variables_map const &vm);
  FAMServer();
};

#endif