#ifndef __PROJ_BFS_H__
#define __PROJ_BFS_H__

#include "graph_kernel.hpp"
#include "vertex_table.hpp"
#include "graph_types.hpp"
#include <client_runtime.hpp>
#include <atomic>
#include <memory>

#include "communication_runtime.hpp"
#include "bitmap.hpp"
//#include "Cache.hpp"
#include "/home/ccy/Develop/GraphTools/COrder/Binning.hpp"

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <iostream>

#pragma GCC diagnostic push
//#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#include <oneapi/tbb.h>
#pragma GCC diagnostic pop
#define TRACE_CACHE 0
#define TRACE_VERTEX_ID 1
#define DEBUG_EVERY_VERTEX 0
#define STOP_ROUND 0

using namespace std;
namespace bfs {
constexpr uint32_t NULLVERT = 0xFFFFFFFF;

struct bfs_vertex
{
  std::atomic<uint32_t> parent{ NULLVERT };
  // bool visited = false;
  std::atomic<bool> visited{ false };

  /*bool cas_visited(){
    return visited.compare_exchange_strong(false, true)
  }*/

  bool update_atomic(uint32_t const t_parent) noexcept
  {// returns true if update succeeded
    uint32_t expect = NULLVERT;
    return parent.compare_exchange_strong(
      expect, t_parent, std::memory_order_relaxed, std::memory_order_relaxed);
  }
};

template<famgraph::Buffering b> struct bfs_kernel
{
public:
  famgraph::Generic_ctx<bfs::bfs_vertex> c;
  uint32_t const start_v;
  bfs_kernel(struct client_context &ctx)
    : c(ctx, b), start_v{ (*ctx.vm)["start-vertex"].as<uint32_t>() }
  {}
  void operator()()
  {

    uint32_t total_verts = c.num_vertices;

    /*
    1. cache_file_path
    2. cache_ratio
    */

    CacheMap * cache_map = c.context->cacheMap;
    CacheManager *cacheManager = c.context->cacheManager;

    tbb::tick_count tbbt0 = tbb::tick_count::now();
    struct timespec t1, t2, res;
    // clock_gettime(CLOCK_MONOTONIC, &t1);
    auto vtable = c.p.second.get();
    auto *frontier = &c.frontierA;
    auto *next_frontier = &c.frontierB;
    tbb::blocked_range<uint32_t> const my_range(0,total_verts );

    //===========TRACE file to debug =============
    ofstream TRACE("trace.log", ios::out);
    if (!TRACE.good()) { cout << "open file trace.log failed!" << endl; }
    //===========================================

    BOOST_LOG_TRIVIAL(info) << "bfs start vertex: " << start_v;
    cout << "start vertex: " << start_v << endl;
    frontier->clear();
    frontier->set_bit(start_v);
    tbb::parallel_for(my_range, [&](auto const &range) {
      for (uint32_t i = range.begin(); i < range.end(); ++i) {
        vtable[i].visited = false;
      }
    });
    vtable[start_v].update_atomic(0);// 0 distance to self
    uint32_t round = 0;
    // struct timespec atomic_t1, atomic_t2, atomic_res;
    auto bfs_push = [&](
                      uint32_t const, uint32_t *const edges, uint32_t const n) noexcept {
      for (uint32_t i = 0; i < n; ++i) {// push out updates //make parallel
        uint32_t w = edges[i];
        // clock_gettime(CLOCK_MONOTONIC, &atomic_t1);
        if (!vtable[w].visited && vtable[w].update_atomic(round)) {
          // if(vtable[w].cas_visited()){
          vtable[w].visited = true;
          next_frontier->set_bit(w);// activate w
        }
        // clock_gettime(CLOCK_MONOTONIC, &atomic_t2);
        // famgraph::timespec_diff(&atomic_t2, &atomic_t1, &atomic_res);
        // c.context->stats.atomic_time.local() +=
        //  atomic_res.tv_sec * 1000000000L + atomic_res.tv_nsec;
      }
    };
    while (!frontier->is_empty()) {
      ++round;
      // if (round == 5) {
      //   cout << "stop in round 5!" << endl;
      //   exit(-1);
      // }
      /*uint64_t round_frontier_count = 0;
/tbb::parallel_for(my_range, [&](auto const &range) {
for (uint32_t i = range.begin(); i < range.end(); ++i) {
  if (frontier->get_bit(i)) { round_frontier_count++; }
}
});
      cout<<"round:"<<round<<" frontier_count:"<<round_frontier_count<<endl;*/
      /*if (round == 4) {
        ofstream round_file("round_4.txt", ios::out);
        tbb::parallel_for(my_range, [&](auto const &range) {
          for (uint32_t i = range.begin(); i < range.end(); ++i) {
            if (frontier->get_bit(i)) { round_file << i << endl; }
          }
        });
        round_file.close();
      }*/


      if (DEBUG && STOP_ROUND != 0 && round == STOP_ROUND) {
        cout << "STOP_ROUND was set to " << STOP_ROUND << ", so we stop to debuging!"
             << endl;
        exit(-1);
      }
      if (DEBUG && DEBUG_EVERY_VERTEX) {
        cout << "round " << round << " ready go!" << endl;
        tbb::parallel_for(my_range, [&](auto const &range) {
          for (uint32_t i = range.begin(); i < range.end(); ++i) {
            if (frontier->get_bit(i)) { cout << " " << i << " "; }
          }
        });
        cout << endl;
      }
	  //cout<<"here?"<<endl;
      // struct timespec t1, t2, res;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      famgraph::single_buffer::ccy_for_each_active_batch(
        cache_map,cacheManager, *frontier, my_range, c, bfs_push);
      if (DEBUG) {
        cout << "next_frontier collide count:" << next_frontier->collide_count << endl;
        cout << "next_frontier no_collide count:" << next_frontier->no_collide_count
             << endl;
        cout << "next_frontier size:" << next_frontier->num_set() << endl;
      }
      frontier->clear();
      std::swap(frontier, next_frontier);
      // clock_gettime(CLOCK_MONOTONIC, &t2);
      // famgraph::timespec_diff(&t2, &t1, &res);
      // BOOST_LOG_TRIVIAL(info) << "round:" << round << " bfs time(milli seconds):"
      //                        << (res.tv_sec * 1000000000L + res.tv_nsec) / 1000000;
      if (DEBUG) {
        famgraph::print_stats_summary(c.context->stats);
        famgraph::clear_all(c.context->stats);
      }
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    famgraph::timespec_diff(&t2, &t1, &res);
    BOOST_LOG_TRIVIAL(info) << "time consume without cache reading(s):"
                            << static_cast<double>(res.tv_sec * 1000000000L + res.tv_nsec)
                                 / 1000000000;
    BOOST_LOG_TRIVIAL(info) << "bfs rounds " << round;
  	tbb::tick_count tbbt1 = tbb::tick_count::now();
  	BOOST_LOG_TRIVIAL(info) << "BFS Running(without cache reading) Time(s): " << (tbbt1 - tbbt0).seconds();
  }
  void print_result() {}
};


void run_bfs_sb(std::unique_ptr<famgraph::vertex, famgraph::mmap_deleter> index,
  std::unique_ptr<bfs_vertex, famgraph::mmap_deleter> vertex_table,
  struct client_context *context);

void run_bfs_db(std::unique_ptr<famgraph::vertex, famgraph::mmap_deleter> index,
  std::unique_ptr<bfs_vertex, famgraph::mmap_deleter> vertex_table,
  struct client_context *context);
}// namespace bfs
#endif// __PROJ_BFS_H__
