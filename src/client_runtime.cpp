#include <client_runtime.hpp>

#include <boost/program_options.hpp>
#include <boost/log/trivial.hpp>

#include <fcntl.h>

#include "connection_utils.hpp"
#include "messages.hpp"
#include "communication_runtime.hpp"
#include <tcmalloc_extensions.hpp>

#include <iostream>//REMOVE
#include <stdexcept>
#include <functional>

#include "graph_kernel.hpp"
#include "bfs.hpp"
#include "pagerank_delta.hpp"
#include "connected_components.hpp"
#include "kcore.hpp"
#include "mis.hpp"
#include "vertex_table.hpp"//REMOVE -dont call directly

#include <numa.h>//for numa_bind
#include <numaif.h>
#include <string.h>

#include "ccy-csr-cache/CacheManager.h"

char CACHE_FILE_PATH[] = "/home/ccy/data_set/MOLIERE_2016_FAM_GRAPH/bin_order.cache";

namespace {

bool endsWith(const char* str, const char* suffix) {
    std::string s(str);
    std::string sfx(suffix);

    if (sfx.length() > s.length()) {
        return false;
    }

    return s.substr(s.length() - sfx.length()) == sfx;
}


void validate_params(boost::program_options::variables_map const &vm)
{
  if (!vm.count("server-addr"))
    throw boost::program_options::validation_error(
      boost::program_options::validation_error::invalid_option_value, "server-addr");
  if (!vm.count("port"))
    throw boost::program_options::validation_error(
      boost::program_options::validation_error::invalid_option_value, "port");
  if (!vm.count("indexfile"))
    throw boost::program_options::validation_error(
      boost::program_options::validation_error::invalid_option_value, "index file");
}

void send_message(struct rdma_cm_id *id)
{
  struct client_context *ctx = static_cast<struct client_context *>(id->context);

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
  int ret = ibv_post_send(id->qp, &wr, &bad_wr);
  if(ret!=0){
		printf("Error ibv_post_send: %s\n", strerror(errno));
	}
  
  TEST_NZ(ret);
}

void post_receive(struct rdma_cm_id *id)
{
  struct client_context *ctx = static_cast<struct client_context *>(id->context);
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));

  wr.wr_id = reinterpret_cast<uintptr_t>(id);
  wr.sg_list = &sge;
  wr.num_sge = 1;

  sge.addr = reinterpret_cast<uintptr_t>(ctx->rx_msg);
  sge.length = sizeof(*ctx->rx_msg);
  sge.lkey = ctx->rx_msg_mr->lkey;
  int ret = ibv_post_recv(id->qp, &wr, &bad_wr);
  if(ret!=0){
		printf("Error ibv_post_recv: %s\n", strerror(errno));
	}
  TEST_NZ(ret);
}

void on_pre_conn(struct rdma_cm_id *id)
{
  BOOST_LOG_TRIVIAL(debug) << "precon";
  struct client_context *ctx = static_cast<struct client_context *>(id->context);
  // tx buffer
  if (posix_memalign(reinterpret_cast<void **>(&ctx->tx_msg),
        static_cast<size_t>(sysconf(_SC_PAGESIZE)),
        sizeof(*ctx->tx_msg))) {
    throw std::runtime_error("posix memalign failed");
  }
  TEST_Z(ctx->tx_msg_mr = ibv_reg_mr(rc_get_pd(), ctx->tx_msg, sizeof(*ctx->tx_msg), 0));

  // rx buffer
  if (posix_memalign(reinterpret_cast<void **>(&ctx->rx_msg),
        static_cast<size_t>(sysconf(_SC_PAGESIZE)),
        sizeof(*ctx->rx_msg))) {
    throw std::runtime_error("posix memalign failed");
  }
  TEST_Z(ctx->rx_msg_mr = ibv_reg_mr(
           rc_get_pd(), ctx->rx_msg, sizeof(*ctx->rx_msg), IBV_ACCESS_LOCAL_WRITE));

  post_receive(id);// prepare to recv MSG_MR
}


void test_all_qp(std::vector<rdma_cm_id*> * cm_ids)
{
  struct ibv_send_wr *bad_wr = NULL;
  struct ibv_send_wr wr;
  struct ibv_sge sge;
  struct ibv_wc wc;

  for (int i = 0; i < cm_ids->size(); i++) {
    struct rdma_cm_id *id = cm_ids->at(i);
    struct client_context *ctx = static_cast<struct client_context *>(id->context);
    BOOST_LOG_TRIVIAL(info) << "Testing " << (i + 1) << "th qp";
    memset(&wr, 0, sizeof(wr));
    memset(&sge, 0, sizeof(sge));
    memset(&wc, 0, sizeof(wc));

    // wr.wr_id = reinterpret_cast<uintptr_t>(av);
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED;// can change for selective signaling
    
    wr.wr.rdma.remote_addr = ctx->peer_addr + 0;
    wr.wr.rdma.rkey = ctx->peer_rkey;
    BOOST_LOG_TRIVIAL(info)<<  "remote_addr:"<<ctx->peer_addr;
    BOOST_LOG_TRIVIAL(info)<<  "rkey:"<<ctx->peer_rkey;

    wr.sg_list = &sge;
    wr.num_sge = 1;

    uint32_t test_target_id;

    struct ibv_mr *mr = ibv_reg_mr(ctx->pd,
      &test_target_id,
      sizeof(uint32_t),
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) { BOOST_LOG_TRIVIAL(fatal) << "ibv_reg_mr for test RDMA_READ failed!"; }

    sge.addr = reinterpret_cast<uintptr_t>(&test_target_id);
    sge.length = sizeof(uint32_t);
    sge.lkey = mr->lkey;

    /*sge.addr = reinterpret_cast<uintptr_t>(buffer);
    sge.length = length;
    sge.lkey = ctx->heap_mr->lkey;*/

    int ret = ibv_post_send(cm_ids->at(i)->qp, &wr, &bad_wr);
    if (ret != 0) {
      BOOST_LOG_TRIVIAL(fatal)
        << i << "th qp sending a test RDMA_READ request to server failed!";
    }
    bool is_done = false;
    while (!is_done) {
      int cq_received_num = ibv_poll_cq(id->send_cq, 1, &wc);
      if (cq_received_num != 0) {
        is_done = true;
        if (wc.status == IBV_WC_SUCCESS) {
          BOOST_LOG_TRIVIAL(info) << "cq_received_num:" << cq_received_num;
          BOOST_LOG_TRIVIAL(info)
            << i << "th qp sending a test RDMA_READ request to server successful!";
          BOOST_LOG_TRIVIAL(info)
            << "The target id read from RDMA server is :" << test_target_id;
        } else {
          BOOST_LOG_TRIVIAL(fatal) << "test failed! status:" << ibv_wc_status_str(wc.status);
          rc_die("poll_cq: test status is not IBV_WC_SUCCESS");
        }
      }
    }

    if (ibv_dereg_mr(mr)) {
      BOOST_LOG_TRIVIAL(fatal) << "error unmapping test RDMA buffer";
    }

  }
  BOOST_LOG_TRIVIAL(info) << "Test done!";
}

void temporary_test(struct ibv_wc *wc, struct ibv_cq * cq){
     struct rdma_cm_id *id = reinterpret_cast<struct rdma_cm_id *>(wc->wr_id);
     struct client_context *ctx = static_cast<struct client_context *>(id->context);
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

      struct ibv_mr *mr = ibv_reg_mr(ctx->pd, &test_target_id, sizeof(uint32_t), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
      if (!mr)	{
        BOOST_LOG_TRIVIAL(fatal) << "ibv_reg_mr for test RDMA_READ failed!";
      }

      sge.addr = reinterpret_cast<uintptr_t>(&test_target_id);
      sge.length = sizeof(uint32_t);
	  sge.lkey =  mr->lkey;

	  
      int ret = ibv_post_send(id->qp, &wr, &bad_wr);
      if(ret != 0 ){
        BOOST_LOG_TRIVIAL(fatal) << "Sending a test RDMA_READ request to server failed!";
      }
	  bool is_done = false;
      while(!is_done){
		int cq_received_num = ibv_poll_cq(cq, 1, wc);
		if(cq_received_num!=0){
			is_done=true;
        if (wc->status == IBV_WC_SUCCESS){
          BOOST_LOG_TRIVIAL(info) << "cq_received_num:"<<cq_received_num;
          BOOST_LOG_TRIVIAL(info) << "Sending a test RDMA_READ request to server successful!";
          BOOST_LOG_TRIVIAL(info) << "The target id read from RDMA server is :"<<test_target_id;
        }
      	else{
        BOOST_LOG_TRIVIAL(fatal) << "test failed! status:"<<wc->status;
        rc_die("poll_cq: test status is not IBV_WC_SUCCESS");
        }
      }}
      if (ibv_dereg_mr(mr)) { BOOST_LOG_TRIVIAL(fatal) << "error unmapping test RDMA buffer"; }
          BOOST_LOG_TRIVIAL(info) << "Test done!";

    //TODO:end
}

void on_completion(struct ibv_wc *wc, struct ibv_cq * cq)
{
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

      
      // init_rdma_heap(ctx);
      ctx->pd = rc_get_pd();// grab a ref to the pd
      //temporary_test(wc, cq);

      uint32_t const num_vertices = famgraph::get_num_verts(ctx->index_file);
      BOOST_LOG_TRIVIAL(info) << "|V| " << num_vertices;
      BOOST_LOG_TRIVIAL(info) << "|E| " << num_edges;

      ctx->app = std::make_unique<famgraph::application>(num_vertices, num_edges);

      for (auto &conn_ptr : ctx->cm_ids) {
        TEST_NZ(rdma_resolve_addr(conn_ptr, NULL, ctx->addr->ai_addr, TIMEOUT_IN_MS));
      }

      BOOST_LOG_TRIVIAL(info) << "Waiting all " << (ctx->connections+1) <<" connections ready...";
      while (rc_get_num_connections() < ctx->connections + 1) {}
      //test_all_qp(&ctx->cm_ids);


      BOOST_LOG_TRIVIAL(info) << "connections: " << rc_get_num_connections();

      ctx->comm_threads.push_back(std::thread(
        famgraph::comm_runtime_worker2, std::ref(ctx->cm_ids), ctx->app.get()));
	 

      if (ctx->vm->count("double-buffer")) {
        throw std::runtime_error("double buffering deprecated");
      } else {

        tbb::tick_count t0 = tbb::tick_count::now();
        if(ctx->cache_ratio != 0){
          if(!ctx->vm->count("cache_file_path")){
            throw std::runtime_error("Not providing cache file path!");
          }
          const char * cache_file_path = ctx->vm->operator[]("cache_file_path").as<std::string>().c_str();
          if(endsWith(cache_file_path, ".bin_cache")){
            BOOST_LOG_TRIVIAL(info)
            << "Cache file:"<<  cache_file_path <<". Using bin_cache mode!" << endl;
            CacheMap *cache_map =
            BinReader::read_bin_cache(ctx->vm->operator[]("cache_file_path").as<std::string>().c_str(), ctx->cache_ratio, ctx->app->num_vertices);
            BOOST_LOG_TRIVIAL(info)
            << "read_cache done! cache_map size is :" << cache_map->size() << endl;
            //FIXME:merge into one
            ctx->cacheMap = cache_map;
            ctx->cacheManager = nullptr;
          } else if (endsWith(cache_file_path, ".cache")) {
            BOOST_LOG_TRIVIAL(info)
            << "Cache file:"<<  cache_file_path <<". Using original cache mode!" << endl;

            ctx->cacheMap = nullptr;
			//FIXME: the const can't pass as non-const char*
            char* non_const_str = new char[strlen(cache_file_path) + 1]; // allocate buffer
            strcpy(non_const_str, cache_file_path); // copy string to buffer
            ctx->cacheManager = new CacheManager(non_const_str, ctx->app->num_vertices);

            ctx->cacheManager->load(ctx->cache_ratio);
            BOOST_LOG_TRIVIAL(info)
            << "read_cache done! cache vertices count :" << ctx->cacheManager->cached_vertices_count() << endl;
          }
        } else {
          ctx->cacheMap = nullptr;
          ctx->cacheManager = nullptr;
        }
        tbb::tick_count t1 = tbb::tick_count::now();
        char temp_str[255];
        sprintf(temp_str, "%.2f", (t1 - t0).seconds());
        BOOST_LOG_TRIVIAL(info) << "Reading Cache Time(s): " << temp_str <<endl ;
        if (ctx->kernel == "bfs") {
          ctx->app_thread = std::thread(
            famgraph::run_kernel<bfs::bfs_kernel<famgraph::Buffering::SINGLE>>,
            std::ref(*ctx));
        } else if (ctx->kernel == "pagerank_delta") {
          ctx->app_thread = std::thread(
            famgraph::run_kernel<
              pagerank_delta::pagerank_delta_kernel<famgraph::Buffering::SINGLE>>,
            std::ref(*ctx));
        } else if (ctx->kernel == "CC") {
          ctx->app_thread = std::thread(
            famgraph::run_kernel<connected_components::connected_components_kernel<
              famgraph::Buffering::SINGLE>>,
            std::ref(*ctx));
        } else if (ctx->kernel == "kcore") {
          ctx->app_thread = std::thread(
            famgraph::run_kernel<kcore::kcore_kernel<famgraph::Buffering::SINGLE>>,
            std::ref(*ctx));
        } else if (ctx->kernel == "MIS") {
          ctx->app_thread = std::thread(
            famgraph::run_kernel<mis::mis_kernel<famgraph::Buffering::SINGLE>>,
            std::ref(*ctx));
        } else {
          BOOST_LOG_TRIVIAL(fatal) << "Unrecognized Kernel";
          throw std::runtime_error("Unrecognized Kernel");
        }
      }
    } else if (ctx->rx_msg->id == MSG_READY) {// client never receives this
      BOOST_LOG_TRIVIAL(trace) << "received READY";
      post_receive(id);
    } else if (ctx->rx_msg->id == MSG_DONE) {
      BOOST_LOG_TRIVIAL(trace) << "received DONE";
      ctx->app_thread.join();
      for (auto &thread : ctx->comm_threads) { thread.join(); }
      BOOST_LOG_TRIVIAL(info) << "Joined app thread";
      rc_disconnect(id);// end server connection
      // disconnect comm threads maybe
      famgraph::print_stats_summary(ctx->stats);
      return;
    }
  }
}

void on_disconnect(struct rdma_cm_id *id)
{
  BOOST_LOG_TRIVIAL(debug) << "on disconnect";
  struct client_context *ctx = static_cast<struct client_context *>(id->context);
  ibv_dereg_mr(ctx->rx_msg_mr);
  ibv_dereg_mr(ctx->tx_msg_mr);
  free(ctx->rx_msg);
  free(ctx->tx_msg);
  BOOST_LOG_TRIVIAL(info) << "Client Disconnect";
}

void do_numa_map(unsigned long threads)
{
  if (numa_available() == -1) {
    BOOST_LOG_TRIVIAL(warning) << "numa is not supported on this platform!";
    return;
  }

  auto const requested_cpus = static_cast<int>(threads);
  auto const available_cpus = numa_num_configured_cpus();
  auto const use_cpus = requested_cpus > available_cpus ? available_cpus : requested_cpus;
  BOOST_LOG_TRIVIAL(debug) << "Available CPUs: " << available_cpus
                           << ", Using: " << use_cpus;

  auto const available_nodes =
    numa_num_configured_nodes();// returns n, where nodes are [0,n)
  BOOST_LOG_TRIVIAL(debug) << "Available NUMA nodes: " << available_nodes;

  auto const cpus_per_node = available_cpus / available_nodes;

  auto const use_nodes = (use_cpus + cpus_per_node - 1) / cpus_per_node;
  BOOST_LOG_TRIVIAL(debug) << "using nodes NUMA nodes [0," << use_nodes << ")";

  auto nodemask = numa_allocate_nodemask();
  if (nodemask == 0) {
    BOOST_LOG_TRIVIAL(fatal) << "nodemask alloc failed!";
    throw std::runtime_error("numa alloc");
  }

  nodemask = numa_bitmask_clearall(nodemask);
  for (unsigned int i = 0; i < static_cast<unsigned int>(use_nodes); ++i)
    nodemask = numa_bitmask_setbit(nodemask, i);

  if (set_mempolicy(MPOL_INTERLEAVE, nodemask->maskp, nodemask->size))
    throw std::runtime_error("set_mempolicy() failed");

  numa_bind(nodemask);
  numa_bitmask_free(nodemask);
}
}// namespace

void run_client(boost::program_options::variables_map &vm)
{
  validate_params(vm);
  std::string server_ip = vm["server-addr"].as<std::string>();
  std::string server_port = vm["port"].as<std::string>();
  std::string ifile = vm["indexfile"].as<std::string>();
  std::string kernel = vm["kernel"].as<std::string>();
  std::string ofile = vm["ofile"].as<std::string>();
  auto const threads = vm["threads"].as<unsigned long>();
  double cache_ratio = vm["cache_ratio"].as<double>();
  unsigned long const max_cores =
    static_cast<unsigned long>(std::thread::hardware_concurrency());
  auto const num_connections = threads == 0 || threads > max_cores ? max_cores : threads;
  bool const print_vtable = vm.count("print-table") ? true : false;
  auto const numa_bind = vm.count("no-numa-bind") ? false : true;

  if (numa_bind) do_numa_map(num_connections);

  BOOST_LOG_TRIVIAL(info) << "Starting client";
  BOOST_LOG_TRIVIAL(info) << "PID:"<<getpid();
  
  BOOST_LOG_TRIVIAL(info) << "Server IPoIB address: " << server_ip
                          << " port: " << server_port;
  BOOST_LOG_TRIVIAL(info) << "Index File: " << ifile;
  BOOST_LOG_TRIVIAL(info) << "Cache Ratio: " << cache_ratio;

  struct client_context ctx
  {
    ifile, num_connections, kernel, ofile, print_vtable, &vm, cache_ratio
  };
  rc_init(on_pre_conn,
    NULL,// on connect
    on_completion,
    on_disconnect);// on disconnect

  rc_client_loop(server_ip.c_str(), server_port.c_str(), &ctx);
}

void client_context::finish_application()
{
  app->should_stop = true;
  tx_msg->id = MSG_READY;// well get a MSG_DONE BACK
  send_message(base_id);
}
