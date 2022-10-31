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
#include "types.hpp"

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
#define USE_CACHE 1
#define CACHE_RATIO 1
#define TRACE_CACHE 0
#define TRACE_VERTEX_ID 1
#define DEBUG 0
#define DEBUG_EVERY_VERTEX 0
#define STOP_ROUND 0

using namespace std;
//const char *CACHE_FILE_PATH = "/home/ccy/data_set/soc-LiveJournal1/corder_cache_file";
const char *CACHE_FILE_PATH = "/home/ccy/data_set/MOLIERE_2016_FAM_GRAPH/out_degree_first.cache";
//const char *CACHE_FILE_PATH = "/home/ccy/data_set/soc-LiveJournal1/out_degree_first.cache";

namespace bfs {
constexpr uint32_t NULLVERT = 0xFFFFFFFF;

// ccy code

static CacheMap *read_cache(ifstream &cache_file,
  size_t capacity, uint32_t total_vert)
{
  char buff[256];
  size_t cur_have_been_read_bytes = 0;
  ofstream CACHE_TRACE("cache_logic.trace", ios::out);
  CacheMap *to_return = new CacheMap(total_vert);
  while (cache_file.good() && cur_have_been_read_bytes < capacity) {
    cache_file.read(buff, sizeof(unsigned int) + sizeof(unsigned long));
    // unsigned int vertex_id = *(unsigned int *)buff;
    unsigned int vertex_id = *reinterpret_cast<unsigned int *>(buff);
    // unsigned long out_vertices_num = *((unsigned int *)buff + sizeof(unsigned int));
    unsigned long out_vertices_num =
      *(reinterpret_cast<unsigned long *>(buff + sizeof(unsigned int)));
    if (vertex_id == TRACE_VERTEX_ID && TRACE_CACHE) {
      CACHE_TRACE << "reading cache, vertex_id:" << vertex_id
                  << " out_num:" << out_vertices_num << endl;
    }
    cur_have_been_read_bytes += sizeof(unsigned int) + sizeof(unsigned long);
    // exceed capacity.
    if (out_vertices_num * sizeof(unsigned long) + cur_have_been_read_bytes > capacity) {
      break;
    }

    CacheElem *v_struct = new CacheElem(vertex_id, out_vertices_num);
    if (vertex_id == TRACE_VERTEX_ID && TRACE_CACHE) {
      CACHE_TRACE << "displaying target_verices of vertex_id:" << TRACE_VERTEX_ID << endl;
    }
    for (uint32_t i = 0; i < out_vertices_num; i++) {
      cache_file.read(buff, sizeof(unsigned int));
      uint32_t target_vertex = *(reinterpret_cast<uint32_t *>(buff));
      v_struct->set_neighbor_at(i, target_vertex);
      //*(v_struct->neightbors + static_cast<uint32_t>(i)) = target_vertex;
      if (vertex_id == TRACE_VERTEX_ID && TRACE_CACHE) {
        CACHE_TRACE << " " << target_vertex;
      }
      cur_have_been_read_bytes += sizeof(unsigned int);
    }
    if (vertex_id == TRACE_VERTEX_ID && TRACE_CACHE) { CACHE_TRACE << endl; }
    to_return->put(v_struct->get_vertex_id(), v_struct);
    //to_return->insert({ v_struct->vertex_id, v_struct });
  }
  return to_return;
}

// ccy end

struct bfs_vertex
{
  std::atomic<uint32_t> parent{ NULLVERT };
  bool visited = false;

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
    // ifstream cache_file_instream("cache_file", ios::in | ios::binary);
    auto const total_verts = c.num_vertices;
    // ifstream cache_file_instream("/home/ccy/data_set/soc-LiveJournal1/cache_file",
    // ios::in | ios::binary);
    ifstream cache_file_instream(CACHE_FILE_PATH, ios::in | ios::binary);
    if (!cache_file_instream.good()) {
      cout << "FATAL: open file cache_file failed!" << endl;
      exit(-1);
    }
    cache_file_instream.seekg(0, cache_file_instream.end);
    long cache_file_size = cache_file_instream.tellg();
    cache_file_instream.seekg(0, cache_file_instream.beg);
    cout<<"Cache size is "  << cache_file_size<<" bytes"<<endl;
    cout<<"Cache ratio is "<<CACHE_RATIO<<endl;
	  double ret = floor(static_cast<double>(cache_file_size)*CACHE_RATIO);
    uint64_t cache_pool_capacity = static_cast<uint64_t>(ret);
    cout<<"Cache capacity is :"<<cache_pool_capacity<<endl;

    //build a contigous memory array for cache
    CacheMap* cache_map = read_cache(cache_file_instream, cache_pool_capacity, total_verts);
    cout << "read_cache done! cache_map size is :"<<cache_map->size() << endl;
    struct timespec t1, t2, res;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    auto vtable = c.p.second.get();
    auto *frontier = &c.frontierA;
    auto *next_frontier = &c.frontierB;
    tbb::blocked_range<uint32_t> const my_range(0, total_verts);

    //===========TRACE file to debug =============
    ofstream TRACE("trace.log", ios::out);
    if (!TRACE.good()) { cout << "open file trace.log failed!" << endl; }
    //===========================================

    if (USE_CACHE) {
      cout << "Using CACHE to run FAM" << endl;
    } else {
      cout << "no CAHCE for FAM" << endl;
    }
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
    //struct timespec atomic_t1, atomic_t2, atomic_res;
    auto bfs_push = [&](
                      uint32_t const, uint32_t *const edges, uint32_t const n) noexcept {
      for (uint32_t i = 0; i < n; ++i) {// push out updates //make parallel
        uint32_t w = edges[i];
        //clock_gettime(CLOCK_MONOTONIC, &atomic_t1);
        if (!vtable[w].visited && vtable[w].update_atomic(round)) {
          vtable[w].visited = true;
          next_frontier->set_bit(w);// activate w
        }
        //clock_gettime(CLOCK_MONOTONIC, &atomic_t2);
        //famgraph::timespec_diff(&atomic_t2, &atomic_t1, &atomic_res);
        //c.context->stats.atomic_time.local() +=
         // atomic_res.tv_sec * 1000000000L + atomic_res.tv_nsec;
      }
    };
    while (!frontier->is_empty()) {
      ++round;
      //if (round == 5) {
      //  cout << "stop in round 5!" << endl;
      //  exit(-1);
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
      if (!USE_CACHE) { 
        cache_map->clear_all();
      }
      //struct timespec t1, t2, res;
      //clock_gettime(CLOCK_MONOTONIC, &t1);
      famgraph::single_buffer::ccy_for_each_active_batch(
        cache_map, *frontier, my_range, c, bfs_push);
	  if(DEBUG){
      cout << "next_frontier collide count:" << next_frontier->collide_count << endl;
      cout << "next_frontier no_collide count:" << next_frontier->no_collide_count
           << endl;
      cout << "next_frontier size:" << next_frontier->num_set() << endl;
      }
      frontier->clear();
      std::swap(frontier, next_frontier);
      //clock_gettime(CLOCK_MONOTONIC, &t2);
      //famgraph::timespec_diff(&t2, &t1, &res);
      //BOOST_LOG_TRIVIAL(info) << "round:" << round << " bfs time(milli seconds):"
       //                       << (res.tv_sec * 1000000000L + res.tv_nsec) / 1000000;
      if(DEBUG){
      	famgraph::print_stats_summary(c.context->stats);
      	famgraph::clear_all(c.context->stats);
		}
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    famgraph::timespec_diff(&t2, &t1, &res);
    BOOST_LOG_TRIVIAL(info) << "time consume without cache reading(s):" 
                            << static_cast<double>(res.tv_sec * 1000000000L + res.tv_nsec) / 1000000000;
    BOOST_LOG_TRIVIAL(info) << "bfs rounds " << round;
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
