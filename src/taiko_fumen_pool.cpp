#include "ppu_recomp.h"
#include "taiko_chart_limits.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
namespace {
constexpr uint32_t inline_count=300, stride=128;
constexpr uint32_t allocate_opd=0x0100f048, release_opd=0x0100f050;
struct Pool {uint32_t address=0, count=0;};
std::mutex pool_mutex;
std::map<uint32_t,Pool> pools;
[[noreturn]] void fail(const char* message,uint32_t lane,uint32_t count) {
    std::fprintf(stderr,"[fumen-pool] %s lane=%08x count=%u\n",message,lane,count);
    std::abort();
}
}

// Called at the record-address calculation in Green's 0040BB54. Its pointer
// vector already grows with the chart's header count. Only the embedded record
// array needed extending; the rest of the loader and all consumers use pointers.
uint32_t taiko_fumen_measure_address(uint32_t lane)
{
    const uint32_t index=vm_read32(lane+0x9604);
    if(index<inline_count)return lane+4+index*stride;
    const uint32_t header=vm_read32(lane+0x9608);
    const uint32_t count=header ? vm_read32(header+0x50) : 0;
    if(count<=index || count>TAIKO_MAX_FUMEN_MEASURES)fail("invalid measure count",lane,count);
    std::lock_guard lock(pool_mutex);
    auto& pool=pools[lane];
    if(!pool.address) {
        const uint32_t bytes=(count-inline_count)*stride;
        // Use the same reusable guest heap as the chart pointer vector. The
        // runtime's lv2 sys_memory allocator is a bump allocator, so allocating
        // and freeing pages per song would still exhaust its address range.
        pool={uint32_t(ppu_guest_call(allocate_opd,bytes,0,0,0)),count};
        if(!pool.address)fail("empty guest allocation",lane,count);
        std::fprintf(stderr,"[fumen-pool] lane=%08x measures=%u overflow=%u bytes\n",lane,count,bytes);
    }
    if(index>=pool.count)fail("overflow pool count changed",lane,count);
    return pool.address+(index-inline_count)*stride;
}

// Constructor reuse and all three lane destructor variants. The guest destroys
// individual records via its pointer list before destroying the lane; these
// destructors themselves only release that list, never dereference its records.
void taiko_fumen_release_pool(uint32_t lane)
{
    std::lock_guard lock(pool_mutex);
    const auto it=pools.find(lane);if(it==pools.end())return;
    ppu_guest_call(release_opd,it->second.address,0,0,0);
    pools.erase(it);
}
