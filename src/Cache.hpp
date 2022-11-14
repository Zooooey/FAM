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
  CacheElem **all_cache;
  uint32_t total_vertices = 0;

public:
  CacheMap(uint32_t total_verts)
  {
    total_vertices = total_verts;
    all_cache = new CacheElem*[total_verts];
	//(CacheElem*)sizeof(CacheElem*) * total_verts;
    for (uint32_t i = 0; i < total_verts; i++) { all_cache[i] = nullptr; }
  }

  void put(uint32_t vertex_id, CacheElem *elem)
  {
    if (all_cache[vertex_id] != nullptr) { delete all_cache[vertex_id]; }
    all_cache[vertex_id] = elem;
  }

  CacheElem *get(uint32_t vertex_id) { return all_cache[vertex_id]; }

  size_t size() { return total_vertices; }

  void clear_all()
  {
    for (uint32_t i = 0; i < total_vertices; i++) {
      if (all_cache[i] != nullptr) {
        delete all_cache[i];
        all_cache[i] = nullptr;
      }
    }
  }

  ~CacheMap() { delete[] all_cache; }
};

class BinReader
{
public:
    /**
     * @brief for UT only
     *
     * @param path_to_read
     */
    static CacheMap* read(char *path_to_read, uint32_t total_vert, uint64_t capacity)
    {
        //cout<<"read bin cache!!!!!!!!!!!!"<<endl;
        CacheMap* cacheMap = new CacheMap(total_vert);
        uint64_t current_cache_size = 0;
        ifstream bin_in(path_to_read, ios::in | ios::binary);
        if (!bin_in.good())
        {
            cout << "open " << path_to_read << " failed!" << endl;
            exit(-1);
        }
        char buff[4096];
        bin_in.read(buff, 8);
        uint64_t bin_num = *reinterpret_cast<uint64_t *>(buff);
        //cout<<"bin num is :"<<bin_num<<endl;
        //Foreach every bins.
        for (uint64_t i = 0; i < bin_num; i++)
        {
            bin_in.read(buff, 8);
            uint64_t bin_vertex_num = *reinterpret_cast<uint64_t *>(buff);
            
            bin_in.read(buff, 8);
            uint64_t edges_bytes = *reinterpret_cast<uint64_t *>(buff);
            //cout<<"bin number:"<<i<<" vertex_num on bin:"<<bin_vertex_num<<"edges_bytes:"<<edges_bytes<<endl;
            if(current_cache_size + edges_bytes > capacity){
                break;
            }else {
                current_cache_size+=edges_bytes;
            }
            //Read each vertex of current Bin.
            for (uint64_t j = 0; j < bin_vertex_num; j++)
            {
                bin_in.read(buff, 4);
                uint32_t vertex_id = *reinterpret_cast<uint32_t *>(buff);
                bin_in.read(buff, 8);
                uint64_t out_degree = *reinterpret_cast<uint64_t *>(buff);
                //cout<<" vertex_id:"<<vertex_id<<" out_degree:"<<out_degree<<endl;
                CacheElem *vertexCache  = new CacheElem(vertex_id, out_degree);
                //Read neighbors id of one vertex.
                for (uint64_t k = 0; k < out_degree; k++)
                {
                    bin_in.read(buff, 4);
                    uint32_t neighbor_id = *reinterpret_cast<uint32_t *>(buff);
                    //cout<<"     neighbor_id:"<<neighbor_id<<endl;
                    vertexCache->set_neighbor_at(k, neighbor_id);
                }
                cacheMap->put(vertex_id, vertexCache);
            }
            
        }
        bin_in.close();
        return cacheMap;
    }
};



#endif
