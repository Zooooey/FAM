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
#define TRACE_CACHE 0
#define TRACE_VERTEX_ID 1
#define DEBUG 1
#define STOP_ROUND 3

using namespace std;

namespace bfs {
constexpr uint32_t NULLVERT = 0xFFFFFFFF;

// ccy code

static map<unsigned int, edges_cache *> *read_cache(ifstream &cache_file, size_t capacity)
{
  char buff[256];
  size_t cur_have_been_read_bytes = 0;
  ofstream CACHE_TRACE("cache_logic.trace", ios::out);
  map<unsigned int, edges_cache *> *to_return = new map<unsigned int, edges_cache *>();
  while (cur_have_been_read_bytes < capacity) {
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

    edges_cache *v_struct = new edges_cache();
    v_struct->vertex_id = vertex_id;
    v_struct->out_vertices_num = out_vertices_num;
    v_struct->out_vertices_array = new unsigned int[out_vertices_num];
    if (vertex_id == TRACE_VERTEX_ID && TRACE_CACHE) {
      CACHE_TRACE << "displaying target_verices of vertex_id:" << TRACE_VERTEX_ID << endl;
    }
    for (unsigned long i = 0; i < out_vertices_num; i++) {
      cache_file.read(buff, sizeof(unsigned int));
      unsigned int target_vertex = *(reinterpret_cast<unsigned int *>(buff));
      *(v_struct->out_vertices_array + i) = target_vertex;
      if (vertex_id == TRACE_VERTEX_ID && TRACE_CACHE) {
        CACHE_TRACE << " " << target_vertex;
      }
      cur_have_been_read_bytes += sizeof(unsigned int);
    }
    if (vertex_id == TRACE_VERTEX_ID && TRACE_CACHE) { CACHE_TRACE << endl; }
    to_return->insert({ v_struct->vertex_id, v_struct });
  }
  return to_return;
}

// ccy end

struct bfs_vertex
{
  std::atomic<uint32_t> parent{ NULLVERT };

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
    auto const total_verts = c.num_vertices;
    // ccy code;
    // famgraph::Bitmap *cache_frontier = new famgraph::Bitmap(total_verts);
    // famgraph::Bitmap *no_cache_frontier = new famgraph::Bitmap(total_verts);
    map<unsigned int, edges_cache *> *cache_map;

    // raed cache file here
    ifstream cache_file_instream("cache_file", ios::in | ios::binary);
    // ifstream cache_file_instream("bcs_cache_file", ios::in | ios::binary);
    if (!cache_file_instream.good()) {
      cout << "FATAL: open file cache_file failed!" << endl;
      exit(-1);
    }
    cache_map = read_cache(cache_file_instream, 66899148);
    // cache_map = read_cache(cache_file_instream, 40);
    //  ccy end;
    cout << "read_cache done!" << endl;
    /*for(auto it = cache_map->begin();it!=cache_map->end();it++){
            cout<<"cache vertex_id"<<it->first<<endl;
    }*/

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
    // cache_frontier->clear();
    // no_cache_frontier->clear();
    frontier->clear();
    // next_frontier->clear();
    frontier->set_bit(start_v);
    vtable[start_v].update_atomic(0);// 0 distance to self
    uint32_t round = 0;
    auto bfs_push = [&](
                      uint32_t const, uint32_t *const edges, uint32_t const n) noexcept {
      for (uint32_t i = 0; i < n; ++i) {// push out updates //make parallel
        uint32_t w = edges[i];
        if (vtable[w].update_atomic(round)) {
          // cout<<"round:"<<round<<endl;
          // cout<<"next_frontier_bit:"<<w<<endl;
          next_frontier->set_bit(w);// activate w
        }
      }
    };
    while (!frontier->is_empty()) {
      ++round;
      if (DEBUG && STOP_ROUND!=0 && round == STOP_ROUND){
        cout "STOP_ROUND was set to "<<STOP_ROUND<<", so we stop to debuging!"<<endl;
        exit(-1);
      }

      if (DEBUG && STOP_ROUND != 0) {
        cout << "round " << round << " ready go!" << endl;
        tbb::parallel_for(my_range, [&](auto const &range) {
          for (uint32_t i = range.begin(); i < range.end(); ++i) {
            if (frontier->get_bit(i)) {
              cout << " " << i << " ";
            }
          }
          cout<<endl;
        });
      }


      // ccy code
      /*cache_frontier->clear();
      no_cache_frontier->clear();
      //build cache_fontier and no_cache_fontier


      // cout<<"========vertex num this round:"<<num<<"=========="<<endl;
      TRACE << endl;
      // frontier become the struct containts no cache vertices!
      // if(USE_CACHE){
      // frontier->clear();
      // std::swap(no_cache_frontier, frontier);
      //  }
      // ccy end
      if (!USE_CACHE) {
        // cache_frontier->clear();
        cache_map->clear();
        // famgraph::single_buffer::for_each_active_batch(*frontier, my_range, c,
        // bfs_push);
      }
      famgraph::single_buffer::ccy_for_each_active_batch(
        cache_map, *frontier, my_range, c, bfs_push);
      /*
                cout<<"at the end of loop we check frontier and next_frontier!"<<endl;
                cout<<"frontier :";
            tbb::parallel_for(my_range, [&](auto const &range) {
              for (uint32_t i = range.begin(); i < range.end(); ++i) {
                if(frontier->get_bit(i)){
                                cout<<i<<",";
                }
              }
            });
                cout<<endl<<"next frontier:";
            tbb::parallel_for(my_range, [&](auto const &range) {
              for (uint32_t i = range.begin(); i < range.end(); ++i) {
                if(next_frontier->get_bit(i)){
                                ++num;
                                cout<<i<<",";
                }
              }
            });
              cout<<endl;*/
      frontier->clear();
      std::swap(frontier, next_frontier);
    }

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
