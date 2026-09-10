#include "ppu_recomp.h"
#include "taiko_chart_limits.h"
#include <map>
#include <cstdio>
#include <stdexcept>
uint32_t taiko_fumen_measure_address(uint32_t);
void taiko_fumen_release_pool(uint32_t);
static std::map<uint32_t,uint32_t> memory,allocations;
static uint32_t next=0x10000000;
uint32_t vm_read32(uint32_t a){return memory[a];}
void vm_write32(uint32_t a,uint32_t v){memory[a]=v;}
static void check(bool ok){if(!ok)throw std::runtime_error("fumen pool check failed");}
extern "C" uint64_t ppu_guest_call(uint32_t opd,uint64_t arg,uint64_t,uint64_t,uint64_t) {
    if(opd==0x0100f050) {check(allocations.erase(uint32_t(arg))==1);return 0;}
    check(opd==0x0100f048 && arg && !(arg%128));
    const auto base=next;next+=uint32_t(arg)+0x10000;
    allocations[base]=uint32_t(arg);return base;
}
int main() {
    // Simulate two simultaneous charts, reuse and destruction with different
    // lengths. All records retain unique stable addresses; inline metadata at
    // record 300's old address is never overwritten.
    for(unsigned pass=0;pass<3;++pass) {
        for(uint32_t lane:{0x10000,0x30000}) {
            const uint32_t header=lane+0xa000;
            const uint32_t count=pass==0?300:pass==1?5000:TAIKO_MAX_FUMEN_MEASURES;
            vm_write32(lane+0x9608,header);vm_write32(header+0x50,count);
            for(uint32_t i=0;i<count;++i) {
                vm_write32(lane+0x9604,i);auto address=taiko_fumen_measure_address(lane);
                if(i<300)check(address==lane+4+i*128);
                else {auto a=allocations.upper_bound(address);check(a!=allocations.begin());--a;check(address+128<=a->first+a->second);}
                check(address!=lane+0x9604 && address!=lane+0x9608);
                vm_write32(address,i);check(vm_read32(lane+0x9608)==header);
                check(taiko_fumen_measure_address(lane)==address);
            }
        }
        check(allocations.size()==(pass?2:0));
        taiko_fumen_release_pool(0x10000);taiko_fumen_release_pool(0x30000);
        check(allocations.empty());taiko_fumen_release_pool(0x10000);
    }
    std::puts("fumen overflow allocation/lifetime tests passed");
}
