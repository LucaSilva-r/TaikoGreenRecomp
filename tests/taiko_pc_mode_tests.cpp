#include "ppu_recomp.h"
#include "taiko_pc_mode.h"
#include "taiko_plus_runtime.h"
#include <cstdlib>
#include <cstdio>
#include <map>
#include <vector>
#include <algorithm>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed at %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
static std::map<uint32_t, uint8_t> memory;
uint8_t vm_read8(uint32_t a) { return memory[a]; }
uint32_t vm_read32(uint32_t a) {
    return uint32_t(vm_read8(a)) << 24 | uint32_t(vm_read8(a+1)) << 16 |
           uint32_t(vm_read8(a+2)) << 8 | vm_read8(a+3);
}
void vm_write8(uint32_t a, uint8_t v) { memory[a] = v; }
void vm_write32(uint32_t a, uint32_t v) {
    for (unsigned i=0;i<4;++i) vm_write8(a+i, v >> (24-i*8));
}
constexpr uint32_t root=0x10000, manager=root+0xd8, owner=0x20000;
constexpr uint32_t setup=0x30000, p1=0x40000, p2=0x50000, scene=0x60000;
static bool ready=false, gameplay=false;
static unsigned failures=0, menus=0, commits=0, removals=0;
static std::vector<uint32_t> calls;
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*)) {}
extern "C" void ppu_set_project_register_hooks(void (*)(void)) {}
void func_008DA500(ppu_context*) {}
extern "C" void taiko_overlay_clear() {}
extern "C" void taiko_frontend_enter_song_select_shell() { ++menus; }
extern "C" void taiko_frontend_standalone_failure(const char*) { ++failures; }
extern "C" void taiko_frontend_standalone_gameplay() { gameplay=true; }
void taiko_host_audio_set_scene_active(bool) {}
void taiko_host_audio_reacquire_menu() {}
extern "C" uint64_t ppu_guest_call_ct(uint32_t code,uint32_t toc,uint64_t a,uint64_t b,uint64_t c,uint64_t) {
    calls.push_back(code);
    CHECK(toc == (code==0x717aec ? 0x1027c58u : 0x1037a88u));
    switch(code) {
    case 0x5c59bc: CHECK(a==manager+0x370); vm_write32(a+4,2); return vm_read32(b) ? p2 : p1;
    case 0x717aec: CHECK(a==manager+0x430 && b==p2); return 0;
    case 0x7fce6c:
        CHECK(b==manager && vm_read32(a)==0);
        CHECK(vm_read32(vm_read32(a+4)+0x28)==p1);
        CHECK(vm_read32(vm_read32(a+8)+0x28)==p2);
        CHECK(vm_read32(manager+0x400)==3); ++commits; return 0;
    case 0x5c5c1c: CHECK(a==manager); return 0x70000;
    case 0x5c583c: { CHECK(a==0x70000 && vm_read32(b)==0);
        bool accepted=ready; ready=false; return accepted; }
    case 0x5c544c: return 0;
    case 0x35d1a0: CHECK(a==0x178); return scene;
    case 0x1e1c04: CHECK(a==scene && b==manager); vm_write32(scene,0xf92c08); return scene;
    case 0x8da500: CHECK(a==owner && b==scene && c==0); return 1;
    case 0x251c08: CHECK(vm_read32(a)==manager && vm_read32(b)==0 && vm_read32(b+4)==0xffffffff); return 0;
    case 0x1de520: CHECK(a==scene); vm_write32(scene+8,1); return 1;
    case 0x8d427c: CHECK(a==owner); return 1;
    case 0x8ddd30: CHECK(a==owner); ++removals; return 0;
    default: std::fprintf(stderr,"unexpected native call %08x\n",code); std::abort();
    }
}
static taiko_plus::MatchConfig match() {
    taiko_plus::MatchConfig m;
    m.content.game_revision="Green"; m.content.music_id="test";
    m.content.chart_hash.bytes[0]=1; m.content.audio_hash.bytes[0]=2;
    m.players[1].slot=taiko_plus::PlayerSlot::P2;
    m.players[1].role=taiko_plus::PlayerRole::SyntheticRemote;
    return m;
}
int main() {
#ifdef _WIN32
    _putenv_s("TAIKO_PLUS_STANDALONE", "1");
#else
    setenv("TAIKO_PLUS_STANDALONE", "1", 1);
#endif
    vm_write32(owner,0xf9ae70); vm_write32(setup,0xf8bae8);
    vm_write32(setup+4,manager);
    vm_write32(manager+0x370,0x80000); vm_write32(manager+0x374,1);
    vm_write32(manager+0x434,0x90000); vm_write32(manager+0x438,0x90090);
    vm_write32(0x90014,4); vm_write32(0x90018,15);
    for (unsigned i=0;i<4;++i) vm_write8(0x90004+i,"test"[i]);
    ppu_context ctx; ctx.gpr[3]=root; taiko_pc_mode_frame_begin(&ctx);
    taiko_pc_mode_on_game_mode_selected(TAIKO_PC_MODE_SENTINEL);
    ctx.gpr[3]=0xa0000; vm_write32(0xa0014,39); vm_write32(0xa000c,manager);
    taiko_pc_mode_entry_tick(&ctx);
    CHECK(taiko_pc_mode_setup_complete(setup,owner)==1);
    CHECK(removals==0 && menus==1); // Setup stays alive; no native selector.
    ctx.gpr[3]=setup; ctx.gpr[4]=owner;
    auto& runtime=taiko_plus::runtime();
    auto bad=match(); bad.content.music_id="missing";
    CHECK(runtime.enqueue_launch(bad)); CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(failures==1 && commits==0 && runtime.state()==taiko_plus::State::Browser);
    CHECK(runtime.enqueue_launch(match()));
    for (unsigned i=0;i<150;++i) CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(commits==1 && !gameplay); // Cannot launch merely because 120 frames elapsed.
    ready=true;
    taiko_pc_mode_setup_tick(&ctx); // One-shot acceptance switches the service to busy.
    for (unsigned i=0;i<120;++i) taiko_pc_mode_setup_tick(&ctx);
    CHECK(!gameplay);
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(gameplay && runtime.state()==taiko_plus::State::Gameplay);
    CHECK(std::count(calls.begin(),calls.end(),0x35d1a0)==1);
    vm_write32(0xb0000,0xf92c98); vm_write32(0xb000c,manager);
    vm_write32(root+0x68,0xc0000); vm_write32(root+0x6c,1);
    vm_write32(0xc0000,0xb0000);
    ctx.gpr[3]=root; taiko_pc_mode_frame_begin(&ctx);
    CHECK(runtime.state()==taiko_plus::State::Results);
    ctx.gpr[3]=setup;
    CHECK(taiko_pc_mode_results_return(0xb0000,0)==0);
    CHECK(taiko_pc_mode_results_return(0xb0000,owner)==1);
    CHECK(removals==1 && menus==2 && runtime.state()==taiko_plus::State::Browser);
    CHECK(taiko_pc_mode_results_return(0xb0000,owner)==1);
    CHECK(removals==1 && menus==2);
    CHECK(vm_read32(manager+0x438)==0x90090);
    // A second match reuses the same native session beyond the first Results.
    CHECK(runtime.enqueue_launch(match()));
    ready=true;
    for (unsigned i=0;i<122;++i) taiko_pc_mode_setup_tick(&ctx);
    CHECK(commits==2 && runtime.state()==taiko_plus::State::Gameplay);
}
