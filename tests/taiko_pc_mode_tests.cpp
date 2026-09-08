#include "ppu_recomp.h"
#include "taiko_pc_mode.h"
#include "taiko_plus_runtime.h"
#include "taiko_browser_players.h"
#include <cstdlib>
#include <cstdio>
#include <map>
#include <cmath>
#include <cstring>
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
static uint8_t expected_mask=3;
static uint8_t expected_difficulties[2]={1,3};
static uint8_t present_players=1;
static unsigned failures=0, menus=0, commits=0, removals=0;
static std::vector<uint32_t> calls;
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*)) {}
extern "C" void ppu_set_project_register_hooks(void (*)(void)) {}
static uint32_t animation_ticks = 1;
extern "C" uint32_t taiko_animation_frame_ticks(void) { return animation_ticks; }
void func_008DA500(ppu_context*) {}
extern "C" void taiko_overlay_clear() {}
extern "C" void taiko_frontend_enter_song_select_shell() { ++menus; }
extern "C" void taiko_frontend_standalone_failure(const char*) { ++failures; }
extern "C" void taiko_frontend_standalone_session_begin() {}
extern "C" void taiko_frontend_standalone_gameplay() { gameplay=true; }
void taiko_host_audio_set_scene_active(bool) {}
static float group_gains[68]{};
void taiko_host_audio_set_group_gain(uint32_t g, float v) { group_gains[g]=v; }
void taiko_host_audio_reacquire_menu() {}
extern "C" uint64_t ppu_guest_call_ct(uint32_t code,uint32_t toc,uint64_t a,uint64_t b,uint64_t c,uint64_t) {
    calls.push_back(code);
    CHECK(toc == (code==0x717aec ? 0x1027c58u : 0x1037a88u));
    switch(code) {
    case 0x5c59bc: {
        CHECK(a==manager+0x370); const unsigned slot=vm_read32(b);
        CHECK(slot<2 && (expected_mask & (1u << slot)));
        present_players |= 1u << slot;
        vm_write32(a+4, present_players==3 ? 2 : 1);
        return slot ? p2 : p1;
    }
    case 0x717aec: CHECK(a==manager+0x430 && (b==p1 || b==p2)); return 0;
    case 0x7fce6c:
        CHECK(b==manager && vm_read32(a)==0);
        CHECK(vm_read32(manager+0x408)==0);
        CHECK(vm_read32(manager+0x40c)==1);
        for (unsigned slot=0; slot<2; ++slot) {
            const uint32_t course=vm_read32(a+4+slot*4);
            const bool enabled=(expected_mask & (1u << slot))!=0;
            CHECK(vm_read8(course)==enabled);
            CHECK(vm_read32(course+4)==expected_difficulties[slot]);
            CHECK(vm_read32(course+0x28)==(enabled ? (slot ? p2 : p1) : 0));
        }
        CHECK(vm_read32(manager+0x400)==expected_mask); ++commits; return 0;
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
    m.players[1].role=taiko_plus::PlayerRole::Local;
    for (unsigned slot=0;slot<2;++slot) {
        m.players[slot].enabled = (expected_mask & (1u << slot)) != 0;
        m.players[slot].difficulty = expected_difficulties[slot];
        m.players[slot].chart_hash=m.content.chart_hash;
    }
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
    vm_write32(manager+0x408,0); vm_write32(manager+0x40c,3);
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
    CHECK(vm_read32(manager+0x40c)==3); // Rejected launches leave session rules intact.
    CHECK(runtime.enqueue_launch(match()));
    for (unsigned i=0;i<150;++i) CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(commits==1 && !gameplay); // Cannot launch merely because 120 frames elapsed.
    ready=true;
    taiko_pc_mode_setup_tick(&ctx); // One-shot acceptance switches the service to busy.
    // At 240 Hz the 120 authored-frame wipe still takes two seconds.
    // Three render frames without an animation tick must not retire it.
    for (unsigned i=0;i<120;++i) {
        animation_ticks = 0;
        for (unsigned j=0;j<3;++j) {
            taiko_pc_mode_setup_tick(&ctx);
            CHECK(!gameplay);
        }
        animation_ticks = 1;
        taiko_pc_mode_setup_tick(&ctx);
    }
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
    vm_write32(manager+0x408,1); // Native Results has advanced the round counter.
    CHECK(runtime.enqueue_launch(match()));
    ready=true;
    for (unsigned i=0;i<122;++i) taiko_pc_mode_setup_tick(&ctx);
    CHECK(commits==2 && runtime.state()==taiko_plus::State::Gameplay);
    for (uint8_t mask : {uint8_t(1), uint8_t(2)}) {
        CHECK(taiko_pc_mode_results_return(0xb0000,owner)==1);
        expected_mask=mask;
        present_players=mask;
        vm_write32(manager+0x374,1);
        CHECK(runtime.enqueue_launch(match()));
        ready=true;
        for (unsigned i=0;i<122;++i) taiko_pc_mode_setup_tick(&ctx);
        CHECK(runtime.state()==taiko_plus::State::Gameplay);
    }
    // Native music group inherits service volume and mute from its parent.
    const auto write_float = [](uint32_t at, float value) {
        uint32_t bits; std::memcpy(&bits, &value, sizeof bits); vm_write32(at,bits);
    };
    vm_write32(0x1037a88+0x448c,0xd0000); vm_write32(0xd0000,0xe0000);
    vm_write32(0x1037a88+0x4508,0xf0000);
    write_float(0xe0000+0x1c1c,6.0f); write_float(0xe0000+0x1c20,-100.0f);
    const uint32_t music=0xf0000+11*0x40;
    vm_write8(music+0x14,1); vm_write32(music,0x100000); vm_write32(0x100000,0);
    vm_write8(0xf0014,1); vm_write32(0xf0000,0x100004); vm_write32(0x100004,0xffffffff);
    write_float(0xf0004,-12.0f);
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(std::abs(group_gains[11]-std::pow(10.0f,-18.0f/20.0f))<0.00001f);
    vm_write32(0xf003c,1);
    taiko_pc_mode_setup_tick(&ctx); CHECK(group_gains[11]==0.0f);
    vm_write32(0xf003c,0); vm_write32(0x100004,11); // Cyclic ancestry fails closed.
    taiko_pc_mode_setup_tick(&ctx); CHECK(group_gains[11]==0.0f);
    taiko_plus::BrowserPlayers browser;
    CHECK(browser.joined==0);
    browser.join(1); CHECK(browser.joined==2 && browser.focus==1);
    browser.change_difficulty(1,1,0x1f);
    CHECK(browser.difficulty[0]==3 && browser.difficulty[1]==4);
    CHECK(browser.confirm(1)); // P2-only can start.
    browser.join(0); CHECK(browser.joined==3 && browser.ready==0);
    CHECK(!browser.confirm(0)); CHECK(browser.confirm(1));
    browser.song_changed(0x05); CHECK(browser.ready==0);
    CHECK((0x05 & (1u << browser.difficulty[0]))!=0);
    CHECK(!browser.confirm(0));
    browser.change_difficulty(1,1,0x05);
    CHECK(browser.ready==1); CHECK(browser.confirm(1));

    browser.open(0x05);
    CHECK(browser.expanded && browser.ready == 0);
    CHECK((browser.cursors(browser.difficulty[0]) & 1) != 0);
    CHECK((browser.cursors(browser.difficulty[1]) & 2) != 0);
    const auto p2_course = browser.difficulty[1];
    CHECK(!browser.confirm(1));
    browser.change_difficulty(0, -1, 0x05);
    CHECK(browser.difficulty[1] == p2_course && browser.ready == 2);
    CHECK(browser.confirm(0));
    browser.collapse();
    CHECK(!browser.expanded && browser.ready == 0 && browser.joined == 3);
    CHECK(browser.cursors(browser.difficulty[0]) == 0);
    browser.open(0x08);
    CHECK(browser.cursors(3) == 3); // Both cursors remain visible on one row.
    browser.change_difficulty(1, 1, 0x08);
    CHECK(browser.cursors(3) == 3); // One-course songs cannot lose either cursor.
    browser.song_changed(0x02);
    CHECK(!browser.expanded && browser.ready == 0);
    CHECK(browser.difficulty[0] == 1 && browser.difficulty[1] == 1);
    browser.open(0);
    CHECK(!browser.expanded);

}
