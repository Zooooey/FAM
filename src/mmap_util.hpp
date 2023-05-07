#ifndef __FG_MMAP_UTIL_H__
#define __FG_MMAP_UTIL_H__

#include <infiniband/verbs.h>
#include <sys/mman.h>
#include <boost/log/trivial.hpp>
#include <memory>
#include <stdexcept>
#include <boost/align/align_up.hpp>
#include "fam_common.hpp"

namespace famgraph {
constexpr auto PROT_RW = PROT_READ | PROT_WRITE;
constexpr auto MAP_ALLOC = MAP_PRIVATE | MAP_ANONYMOUS;
constexpr auto IB_FLAGS = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;

struct RDMA_mmap_deleter
{
  std::size_t const m_size;
  struct ibv_mr *const mr;

  RDMA_mmap_deleter(std::size_t size, struct ibv_mr *t_mr) : m_size{ size }, mr{ t_mr } {}

  void operator()(void *ptr) const
  {
    if (ibv_dereg_mr(mr)) { BOOST_LOG_TRIVIAL(fatal) << "error unmapping RDMA buffer"; }
    munmap(ptr, m_size);
    BOOST_LOG_TRIVIAL(info)<<"MemoryRegion: free memory region success!";
  }
};
 
 //TODO: change usage of this function
//Store Edge Array
template<typename T> auto RDMA_mmap_unique(uint64_t array_size, ibv_pd *pd, bool use_HP, int fam_thp_flag)
{
  // auto constexpr HP_align = 1 << 30;// 1 GB huge pages
  auto constexpr HP_align = 1 << 21;// 2 MB huge pages
  auto const HP_FLAGS = use_HP ? MAP_HUGETLB : 0;
  auto const req_size = sizeof(T) * array_size;
  auto const aligned_size =
    use_HP ? boost::alignment::align_up(req_size, HP_align) : req_size;

  BOOST_LOG_TRIVIAL(info) << "MemoryRegion: aligned size=" << aligned_size << " use_HP=" << use_HP;
  if (auto ptr = mmap(0, aligned_size, PROT_RW, MAP_ALLOC | HP_FLAGS, -1, 0)) {
    //madvice for THP 
    fam_thp::advice_edge_thp(ptr, aligned_size, fam_thp_flag);

    //int access = use_HP? IB_FLAGS | IBV_ACCESS_HUGETLB : IB_FLAGS;
    //FIXME: We are currently testing THP, so RDMA always turn on HUGETLB optimization.
    int access = use_HP? IB_FLAGS | IBV_ACCESS_HUGETLB : IB_FLAGS;
    struct ibv_mr *mr = ibv_reg_mr(pd, ptr, aligned_size, access);
    if (!mr) {
      BOOST_LOG_TRIVIAL(fatal) << "ibv_reg_mr failed" <<"strerror(errno):"<<strerror(errno);
      throw std::runtime_error("ibv_reg_mr failed");
    } else {
      BOOST_LOG_TRIVIAL(info) << "MemoryRegion: ibv_reg_mr success!";
    }

    auto del = RDMA_mmap_deleter(aligned_size, mr);
    return std::unique_ptr<T, RDMA_mmap_deleter>(static_cast<T *>(ptr), del);
  }

  throw std::bad_alloc();
}

class mmap_deleter
{
  std::size_t m_size;

public:
  mmap_deleter(std::size_t size) : m_size{ size } {}

  void operator()(void *ptr) const { munmap(ptr, m_size); }
};
//TODO: change usage of this function
//Store Vertex Array
template<typename T> auto mmap_unique(uint64_t const array_size, bool const use_HP, int fam_thp_flag)
{
  // auto constexpr HP_align = 1 << 30;// 1 GB huge pages
  auto constexpr HP_align = 1 << 21;// 2 MB huge pages
  auto const HP_FLAGS = use_HP ? MAP_HUGETLB : 0;
  auto const req_size = sizeof(T) * array_size;
  auto const aligned_size =
    use_HP ? boost::alignment::align_up(req_size, HP_align) : req_size;

  BOOST_LOG_TRIVIAL(debug) << "nonRDMA aligned size: " << aligned_size
                           << " use_HP: " << use_HP;
  if (auto ptr = mmap(0, aligned_size, PROT_RW, MAP_ALLOC | HP_FLAGS, -1, 0)) {
    fam_thp::advice_vertex_thp(ptr, aligned_size, fam_thp_flag);
    for (uint64_t i = 0; i < array_size; ++i) {
      (void)new (static_cast<T *>(ptr) + i) T{};// T must be default constructable
    }
    auto del = mmap_deleter(aligned_size);
    return std::unique_ptr<T, mmap_deleter>(static_cast<T *>(ptr), del);
  }

  throw std::bad_alloc();
}
}// namespace famgraph

#endif//__FG_MMAP_UTIL_H__
