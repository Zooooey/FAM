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
    inline int huge_page_select(void *addr, size_t length, FAM_THP_FLAG user_select, FAM_THP_FLAG current_select){
        int mad_ret;
        if(user_select & current_select){
            mad_ret = madvice(addr, length, MADV_HUGEPAGE);
        }else {
            mad_ret = madvice(addr, length, MADV_NOHUGEPAGE);
        }
        return mad_ret;
    }

    int advice_edge_thp(void *addr, size_t length, FAM_THP_FLAG fam_thp_flag){
        if(0 != huge_page_select(addr, length, fam_thp_flag, THP_EDGE_ARRAY)){
            BOOST_LOG_TRIVIAL(fatal) << "madvice for EDGE array of FAM failed! " <<"strerror(errno):"<<strerror(errno);  
        }
    }
    int advice_vertex_thp(void *addr, size_t length, FAM_THP_FLAG fam_thp_flag){
        if(0 != huge_page_select(addr, length, fam_thp_flag, THP_VERTEX_ARRAY)){
            BOOST_LOG_TRIVIAL(fatal) << "madvice for VERTEX array of FAM failed! " <<"strerror(errno):"<<strerror(errno);  
        }
    }

    void advice_prop_thp(void *addr, size_t length, FAM_THP_FLAG fam_thp_flag){
        if(0 != huge_page_select(addr, length, fam_thp_flag, THP_PROPERTY_ARRAY)){
            BOOST_LOG_TRIVIAL(fatal) << "madvice for PROPERTY array of FAM failed! " <<"strerror(errno):"<<strerror(errno);  
        }
    }
}

#endif