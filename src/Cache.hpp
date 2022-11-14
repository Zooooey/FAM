#ifndef Cache_h
#define Cache_h
#include <sys/mman.h>
#include <iostream>
#include <string.h>
using namespace std;
class CacheElem
{
private:
  uint32_t vertex_id;
  uint32_t *neighbors;
  uint64_t out_degree;

public:
  CacheElem(uint32_t v_id, uint64_t out_deg)
  {
    vertex_id = v_id;
    out_degree = out_deg;
    neighbors = new uint32_t[out_deg];
    /*void *p =  mmap(0, sizeof(uint32_t)*out_deg, PROT_READ | PROT_WRITE , MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(p != MAP_FAILED){
		neighbors = static_cast<uint32_t*>(p);
	}else {
		cout<<"mmap failed for cache elem!"<<endl;
		printf("Error opening the file: %s\n", strerror(errno));
		exit(-1);
	}*/
  }

  uint64_t get_out_degree() { return out_degree; }

  uint32_t get_vertex_id() { return vertex_id; }

  uint32_t get_neighbor_at(uint32_t i) { return neighbors[i]; }

  uint32_t* get_neighbors(){
    return neighbors;
  }

  void set_neighbor_at(uint32_t i, uint32_t target_vertex_id)
  {
    neighbors[i] = target_vertex_id;
  }

  ~CacheElem() { delete[] neighbors; }
};
class CacheMap
{
private:
  CacheElem **all_cache = nullptr;
  uint32_t total_vert = 0;

public:
  CacheMap(uint32_t total_v)
  {
    total_vert = total_v;
    all_cache = new CacheElem*[total_vert];
	//(CacheElem*)sizeof(CacheElem*) * total_verts;
    for (uint32_t i = 0; i < total_vert; i++) { all_cache[i] = nullptr; }
  }

  void put(uint32_t vertex_id, CacheElem *elem)
  {
    if (all_cache[vertex_id] != nullptr) { delete all_cache[vertex_id]; }
    all_cache[vertex_id] = elem;
  }

  CacheElem *get(uint32_t vertex_id) { return all_cache[vertex_id]; }

  size_t size() { return total_vert; }

  void clear_all()
  {
    for (uint32_t i = 0; i < total_vert; i++) {
      if (all_cache[i] != nullptr) {
        delete all_cache[i];
        all_cache[i] = nullptr;
      }
    }
  }

  ~CacheMap() { delete[] all_cache; }
};

#endif
