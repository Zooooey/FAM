#ifndef FAM_COMMON_H
#define FAM_COMMON_H
#include <sys/mman.h>
#include <boost/log/trivial.hpp>
#include <string.h>
enum FAM_THP_FLAG{
    THP_VERTEX_ARRAY = (1 << 1),
    THP_EDGE_ARRAY = (1 << 2),
    THP_PROPERTY_ARRAY = (1 << 3),
    THP_ALL = THP_VERTEX_ARRAY|THP_EDGE_ARRAY|THP_PROPERTY_ARRAY
};


namespace fam_thp{
    inline int huge_page_select(void *addr, size_t length, int user_select, FAM_THP_FLAG current_select){
        int mad_ret;
        if(user_select & current_select){
            mad_ret = madvise(addr, length, MADV_HUGEPAGE);
        }else {
            mad_ret = madvise(addr, length, MADV_NOHUGEPAGE);
        }
        return mad_ret;
    }

    inline int advice_edge_thp(void *addr, size_t length, int fam_thp_flag){
        if(0 != huge_page_select(addr, length, fam_thp_flag, THP_EDGE_ARRAY)){
            BOOST_LOG_TRIVIAL(fatal) << "madvise for EDGE array of FAM failed! " <<"strerror(errno):"<<strerror(errno);  
        }else {
            BOOST_LOG_TRIVIAL(info) << "madvise for EDGE array to use huge page success! ";
        }

    }
    inline int advice_vertex_thp(void *addr, size_t length, int fam_thp_flag){
        if(0 != huge_page_select(addr, length, fam_thp_flag, THP_VERTEX_ARRAY)){
            BOOST_LOG_TRIVIAL(fatal) << "madvise for VERTEX array of FAM failed! " <<"strerror(errno):"<<strerror(errno);  
        }else {
            BOOST_LOG_TRIVIAL(info) << "madvise for VERTEX array to use huge page success! ";
        }
    }

    inline void advice_prop_thp(void *addr, size_t length, int fam_thp_flag){
        if(0 != huge_page_select(addr, length, fam_thp_flag, THP_PROPERTY_ARRAY)){
            BOOST_LOG_TRIVIAL(fatal) << "madvise for PROPERTY array of FAM failed! " <<"strerror(errno):"<<strerror(errno);  
        }else {
            BOOST_LOG_TRIVIAL(info) << "madvise for PROPERTY array to use huge page success! ";
        }
    }
}

#endif
