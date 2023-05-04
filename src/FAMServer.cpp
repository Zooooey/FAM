#include <iostream>
#include <stdexcept>

#include <server_runtime.hpp>

#include <boost/program_options.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>//Remove when refactor edgelist init
#include <boost/filesystem/fstream.hpp>//Remove when refactor edgelist init

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>//for mmap, munmap
#include <sys/types.h>//for open
#include <sys/stat.h>//for open
#include <fcntl.h>//for open

#include "mmap_util.hpp"
#include "connection_utils.hpp"
#include "messages.hpp"
#include "fam_common.hpp"
#include "AbstractServer.h"
#include "FAMServer.h"

struct server_context// consider renaming server context
{
  std::string adj_filename;
  std::vector<std::unique_ptr<uint32_t, famgraph::RDMA_mmap_deleter>> v;

  struct message *tx_msg;
  struct ibv_mr *tx_msg_mr;

  struct message *rx_msg;
  struct ibv_mr *rx_msg_mr;

  bool use_hp{ false };

  int fam_thp_flag{ 0 };

  server_context(std::string const &file) : adj_filename{ file } {}

  server_context &operator=(const server_context &) = delete;
  server_context(const server_context &) = delete;
};


class file_mapper
{
  constexpr static uint64_t default_chunk = 30UL * (1 << 30);// 30 GB
  uint64_t const chunk_size{ default_chunk };
  uint64_t offset{ 0 };
  uint64_t filesize;
  int fd;
  bool use_HP;

public:
  file_mapper(std::string const &file, uint64_t t_fsize, bool huge_page)
    : filesize{ t_fsize }, fd{ open(file.c_str(), O_RDONLY) }, use_HP{ huge_page }
  {
    if (this->fd == -1) { throw std::runtime_error("open() failed on .adj file"); }
  }

  ~file_mapper() { close(this->fd); }

  file_mapper(const file_mapper &) = delete;
  file_mapper &operator=(const file_mapper &) = delete;

  bool has_next() noexcept { return this->offset < this->filesize; }

  auto operator()()
  {
    auto length = std::min(this->chunk_size, this->filesize - this->offset);
    if (this->use_HP) {
      auto constexpr HP_align = 1 << 21;// 2 MB huge pages
      length = boost::alignment::align_up(length, HP_align);
    }
    auto del = [length](void *p) {
      auto r = munmap(p, length);
      if (r) BOOST_LOG_TRIVIAL(fatal) << "munmap chunk failed";
    };

    auto flags = MAP_PRIVATE | MAP_POPULATE;
    if (this->use_HP) {
      flags = flags | MAP_HUGETLB;
      auto constexpr HP_align = 1 << 21;// 2 MB huge pages
    }
    auto ptr =
      mmap(0, length, PROT_READ, flags, this->fd, static_cast<long>(this->offset));
    if (!ptr) { BOOST_LOG_TRIVIAL(fatal) << "munmap chunk failed"; }
    this->offset += length;

    return make_pair(std::unique_ptr<void, decltype(del)>(ptr, del), length);
  }
};

namespace {
template<typename T> auto num_elements(boost::filesystem::path const &p)
{
  const auto file_size = boost::filesystem::file_size(p);
  const auto n = file_size / sizeof(T);
  return n;
}
void post_receive(struct rdma_cm_id *id)
{
  struct server_context *ctx = static_cast<struct server_context *>(id->context);
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));

  wr.wr_id = reinterpret_cast<uintptr_t>(id);
  wr.sg_list = &sge;
  wr.num_sge = 1;

  sge.addr = reinterpret_cast<uintptr_t>(ctx->rx_msg);
  sge.length = sizeof(*ctx->rx_msg);
  sge.lkey = ctx->rx_msg_mr->lkey;

  TEST_NZ(ibv_post_recv(id->qp, &wr, &bad_wr));
}

auto get_edge_list(std::string adj_file, ibv_pd *pd, bool use_HP, int fam_thp_flag)
{
  namespace fs = boost::filesystem;
  BOOST_LOG_TRIVIAL(info) << "Reading adj file: " << adj_file;

  fs::path p(adj_file);
  if (!(fs::exists(p) && fs::is_regular_file(p)))
    throw std::runtime_error(".adj file not found");

  auto const edges_count = num_elements<uint32_t>(p);
  BOOST_LOG_TRIVIAL(info) << "adj file edges count: " << edges_count;
  auto ptr = famgraph::RDMA_mmap_unique<uint32_t>(edges_count, pd, use_HP, fam_thp_flag);
  auto array = reinterpret_cast<char *>(ptr.get());
  auto const filesize = edges_count * sizeof(uint32_t);
  file_mapper get_mapped_chunk{ adj_file, filesize, use_HP };

  while (get_mapped_chunk.has_next()) {
    auto const [fptr, len] = get_mapped_chunk();
    std::memcpy(array, fptr.get(), len);
    array += len;
    std::cout << "#" << std::flush;
  }

  auto mr = ptr.get_deleter().mr;
  return std::make_tuple(std::move(ptr), mr, edges_count);
}

void send_message(struct rdma_cm_id *id)
{
  struct server_context *ctx = static_cast<struct server_context *>(id->context);

  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));

  wr.wr_id = reinterpret_cast<uintptr_t>(id);
  wr.opcode = IBV_WR_SEND;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  sge.addr = reinterpret_cast<uintptr_t>(ctx->tx_msg);
  sge.length = sizeof(*ctx->tx_msg);
  sge.lkey = ctx->tx_msg_mr->lkey;

  TEST_NZ(ibv_post_send(id->qp, &wr, &bad_wr));
}

 void validate_params(boost::program_options::variables_map const &vm)
{
  if (!vm.count("server-addr"))
    throw boost::program_options::validation_error(
      boost::program_options::validation_error::invalid_option_value, "server-addr");
  if (!vm.count("port"))
    throw boost::program_options::validation_error(
      boost::program_options::validation_error::invalid_option_value, "port");
  if (!vm.count("edgefile"))
    throw boost::program_options::validation_error(
      boost::program_options::validation_error::invalid_option_value, "edgefile");
}
}// namespace


FAMServer::FAMServer() {}

void FAMServer::on_pre_conn(struct rdma_cm_id *id)
{
  BOOST_LOG_TRIVIAL(debug) << "precon";
  struct server_context *ctx = g_ctx;// find a better way later

  id->context = ctx;

  if (posix_memalign(reinterpret_cast<void **>(&ctx->tx_msg),
        static_cast<size_t>(sysconf(_SC_PAGESIZE)),
        sizeof(*ctx->tx_msg))) {
    throw std::runtime_error("posix memalign failed");
  }

  TEST_Z(ctx->tx_msg_mr = ibv_reg_mr(s_ctx->pd, ctx->tx_msg, sizeof(*ctx->tx_msg), 0));

  if (posix_memalign(reinterpret_cast<void **>(&ctx->rx_msg),
        static_cast<size_t>(sysconf(_SC_PAGESIZE)),
        sizeof(*ctx->rx_msg))) {
    throw std::runtime_error("posix memalign failed");
  }
  TEST_Z(ctx->rx_msg_mr = ibv_reg_mr(
           s_ctx->pd, ctx->rx_msg, sizeof(*ctx->rx_msg), IBV_ACCESS_LOCAL_WRITE));

  post_receive(id);
}
void FAMServer::on_connection(struct rdma_cm_id *id)
{
  BOOST_LOG_TRIVIAL(debug) << "on connection";
  struct server_context *ctx = static_cast<struct server_context *>(id->context);

  auto [ptr, mr, edges] =
    get_edge_list(ctx->adj_filename, s_ctx->pd, ctx->use_hp, ctx->fam_thp_flag);
  ctx->v.emplace_back(std::move(ptr));

  ctx->tx_msg->id = MSG_MR;
  ctx->tx_msg->data.mr.addr = reinterpret_cast<uintptr_t>(mr->addr);
  ctx->tx_msg->data.mr.rkey = mr->rkey;
  ctx->tx_msg->data.mr.total_edges = edges;

  send_message(id);
}
void FAMServer::on_completion(struct ibv_wc *wc)
{
  BOOST_LOG_TRIVIAL(debug) << "completion";
  struct rdma_cm_id *id = reinterpret_cast<struct rdma_cm_id *>(wc->wr_id);
  struct server_context *ctx = static_cast<struct server_context *>(id->context);

  if (wc->opcode & IBV_WC_RECV) {
    if (ctx->rx_msg->id == MSG_READY) {
      post_receive(id);
      BOOST_LOG_TRIVIAL(debug) << "received READY";
      ctx->tx_msg->id = MSG_DONE;
      send_message(id);
    } else if (ctx->rx_msg->id == MSG_DONE) {// server never receives this
      printf("received DONE\n");
      post_receive(id);
      ctx->tx_msg->id = MSG_DONE;
      send_message(id);
      // rc_disconnect(id);//should never recv this...
      return;
    }
  }
}
void FAMServer::on_disconnect(struct rdma_cm_id *id)
{
  struct server_context *ctx = static_cast<struct server_context *>(id->context);

  ibv_dereg_mr(ctx->rx_msg_mr);
  ibv_dereg_mr(ctx->tx_msg_mr);
  free(ctx->rx_msg);
  free(ctx->tx_msg);
}
void FAMServer::run(boost::program_options::variables_map const &vm)
{
  // 校验参数
  validate_params(vm);
  // 获取服务端的ip和port，以及边文件
  std::string server_ip = vm["server-addr"].as<std::string>();
  std::string server_port = vm["port"].as<std::string>();
  std::string file = vm["edgefile"].as<std::string>();

  BOOST_LOG_TRIVIAL(info) << "Starting server";
  BOOST_LOG_TRIVIAL(info) << "Server IPoIB address: " << server_ip
                          << " port: " << server_port;

  BOOST_LOG_TRIVIAL(info) << "Reading in edgelist";

  struct server_context ctx
  {
    file
  };
  this->server_ctx = &ctx;
  // 判断有没有在参数里指定使用大页
  ctx.use_hp = vm.count("hp") ? true : false;
  ctx.fam_thp_flag = vm["madvise_thp"].as<uint32_t>();
  BOOST_LOG_TRIVIAL(info) << "hugepages? " << ctx.use_hp;

  BOOST_LOG_TRIVIAL(info) << "waiting for connections. interrupt (^C) to exit.";


  // loop
  struct sockaddr_in6 addr;
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;

  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(static_cast<uint16_t>(atoi(server_port.c_str())));

  TEST_Z(ec = rdma_create_event_channel());
  TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));
  TEST_NZ(rdma_bind_addr(listener, reinterpret_cast<struct sockaddr *>(&addr)));
  TEST_NZ(rdma_listen(listener, 10)); /* backlog=10 is arbitrary */

  event_loop(ec, 0);// exit on disconnect

  rdma_destroy_id(listener);
  rdma_destroy_event_channel(ec);
}
