#ifndef FAM_COMMON_H
#define FAM_COMMON_H
#include <sys/mman.h>
#include <boost/log/trivial.hpp>
#include <string.h>
#define ENABLE_MADVISE_THP true
enum FAM_THP_FLAG{
    THP_VERTEX_ARRAY = (1 << 1),
    THP_EDGE_ARRAY = (1 << 2),
    THP_PROPERTY_ARRAY = (1 << 3),
    THP_CACHE_REGION = (1 << 4),
    THP_ALL = THP_VERTEX_ARRAY|THP_EDGE_ARRAY|THP_PROPERTY_ARRAY|THP_CACHE_REGION
};

#define TEST_NZ(x) do { if ( (x)) rc_die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) rc_die("error: " #x " failed (returned zero/null)."); } while (0)

inline void rc_die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

namespace fam_thp{

    /**
     * @return 0: Not to use huge page
     * @return 1: To use huge page
     * @return -1: Something went wrong!
    */
    inline int huge_page_select(void *addr, size_t length, int user_select, FAM_THP_FLAG current_select){
        int mad_ret;
        if(user_select & current_select){
            mad_ret = madvise(addr, length, MADV_HUGEPAGE);
        }else {
            mad_ret = madvise(addr, length, MADV_NOHUGEPAGE);
        }
        return mad_ret;
    }

    inline void advice_edge_thp(void *addr, size_t length, int fam_thp_flag){
        if(!ENABLE_MADVISE_THP){
            return;
        }
        int mad_ret;
        if(fam_thp_flag & THP_EDGE_ARRAY){
            mad_ret = madvise(addr, length, MADV_HUGEPAGE);
            BOOST_LOG_TRIVIAL(info) << "madvise EDGE array to USE huge page";
        }else {
            mad_ret = madvise(addr, length, MADV_NOHUGEPAGE);
            BOOST_LOG_TRIVIAL(info) << "madvise EDGE array NOT to use huge page";
        }
        if( 0 != mad_ret ){
            BOOST_LOG_TRIVIAL(fatal) << "madvise for EDGE array of FAM failed! " <<"strerror(errno):"<<strerror(errno);
        }
    }
    inline int advice_vertex_thp(void *addr, size_t length, int fam_thp_flag){
        if(!ENABLE_MADVISE_THP){
            return 0;
        }
        int mad_ret;
        if(fam_thp_flag & THP_VERTEX_ARRAY){
            mad_ret = madvise(addr, length, MADV_HUGEPAGE);
            BOOST_LOG_TRIVIAL(info) << "madvise VERTEX array to USE huge page";
        }else {
            mad_ret = madvise(addr, length, MADV_NOHUGEPAGE);
            BOOST_LOG_TRIVIAL(info) << "madvise VERTEX array NOT to use huge page";
        }
        if( 0 != mad_ret ){
            BOOST_LOG_TRIVIAL(fatal) << "madvise for VERTEX array of FAM failed! " <<"strerror(errno):"<<strerror(errno);
        }
    }

    inline void advice_prop_thp(void *addr, size_t length, int fam_thp_flag){
        if(!ENABLE_MADVISE_THP){
            return;
        }
        int mad_ret;
        if(fam_thp_flag & THP_PROPERTY_ARRAY){
            mad_ret = madvise(addr, length, MADV_HUGEPAGE);
            BOOST_LOG_TRIVIAL(info) << "madvise PROPERTY array to USE huge page";
        }else {
            mad_ret = madvise(addr, length, MADV_NOHUGEPAGE);
            BOOST_LOG_TRIVIAL(info) << "madvise PROPERTY array NOT to use huge page";
        }
        if( 0 != mad_ret ){
            BOOST_LOG_TRIVIAL(fatal) << "madvise for PROPERTY array of FAM failed! " <<"strerror(errno):"<<strerror(errno);
        }
    }
}

#endif
