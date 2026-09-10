#include "ppu_recomp.h"
#include "taiko_pc_mode.h"
#include "taiko_plus_runtime.h"
#include "taiko_browser_players.h"
#include "taiko_custom_songs.h"
#include <cstdlib>
#include <cstdio>
#include <map>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>
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
static bool costume_busy=true;
static unsigned costume_applied[2]{};
static unsigned portrait_costumes[2]{};
static uint32_t portrait_profiles[2]{};
static uint8_t expected_mask=3;
static uint8_t expected_difficulties[2]={1,3};
static uint8_t present_players=1;
static unsigned failures=0, menus=0, commits=0, removals=0;
static std::vector<uint32_t> calls;
static std::string save_status;
static unsigned save_builds=0, queued_scores=0;
static bool reject_enqueue=false;
static int reject_slot=-1;
extern "C" void taiko_overlay_set_browser_save_status(const char* text) { save_status=text; }
static uint32_t portrait_addresses[2]{};
static uint8_t browser_joined = 3;
static bool browser_visible = true;
extern "C" uint8_t taiko_overlay_browser_joined(void) { return browser_joined; }
extern "C" int taiko_overlay_browser_visible(void) { return browser_visible; }
static unsigned join_animations[2]{}, departure_animations[2]{};
extern "C" void taiko_overlay_set_browser_portrait(unsigned p, uint32_t address,
                                                   uint32_t w, uint32_t h) {
    CHECK(p < 2);
    CHECK(!address || (w == 600 && h == 600));
    portrait_addresses[p] = address;
}
const TaikoCatalogSong* taiko_custom_find(std::string_view) { return nullptr; }
void taiko_custom_titles_select(const std::string&, const std::string&) {}
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*)) {}
extern "C" void ppu_set_project_register_hooks(void (*)(void)) {}
static uint32_t animation_ticks = 1;
extern "C" uint32_t taiko_animation_frame_ticks(void) { return animation_ticks; }
void func_008DA500(ppu_context*) {}
extern "C" void taiko_overlay_clear() {}
extern "C" void taiko_frontend_enter_song_select_shell() { ++menus; }
extern "C" void taiko_frontend_standalone_failure(const char*) { ++failures; }
extern "C" void taiko_frontend_standalone_session_begin() {}
extern "C" void taiko_frontend_browser_account(unsigned, const char*, int) {}
extern "C" void taiko_frontend_browser_login_tick(uint32_t, int) {}
extern "C" void taiko_frontend_standalone_gameplay() { gameplay=true; }
void taiko_host_audio_set_scene_active(bool) {}
static float group_gains[68]{};
void taiko_host_audio_set_group_gain(uint32_t g, float v) { group_gains[g]=v; }
void taiko_host_audio_reacquire_menu() {}
extern "C" uint64_t ppu_guest_call_ct(uint32_t code,uint32_t toc,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    calls.push_back(code);
    CHECK(toc == ((code==0x717aec || code==0x621784 || code==0x62a318 ||
                   code==0x12ee34) ? 0x1027c58u : 0x1037a88u));
    switch(code) {
    case 0x298fd0:
        CHECK(b < 2);
        if (c == 0x2c) { CHECK(d == 0x2d); ++join_animations[b]; }
        if (c == 0x4a || c == 0x4c) {
            CHECK(c == (b ? 0x4c : 0x4a) && d == UINT32_MAX);
            ++departure_animations[b];
        }
        return 0;
    case 0x298cd4: case 0x298f9c: case 0x298f18:
    case 0x298d24: case 0x2a2cb0: case 0x2a2d50: case 0x2a2df0:
        return 0;
    case 0x29d474:
        CHECK(!costume_busy && b < 2);
        for (unsigned i=0;i<5;++i) CHECK(vm_read32(c+i*4)==0);
        return 0; // Already loaded is a successful request, too.
    case 0x298f34: CHECK(b<2); return 0x50000011 + b*0x100;
    case 0x518768: {
        const unsigned slot = (b - 0x50000011)/0x100;
        CHECK(slot < 2);
        const uint32_t color = 0x140000 + slot*0x1000;
        const uint32_t texture = color + 0x200;
        vm_write32(a,color); vm_write32(color+0xf8,texture);
        vm_write32(texture+0x34,0xc1000000+slot*0x200000);
        vm_write32(texture+0x24,(600u<<16)|600u);
        return 1;
    }
    case 0x5c573c:
        CHECK(a==manager);
        vm_write32(0x130000,0x131000);
        vm_write32(0x01038e40,0x132000);
        vm_write8(0x132008,1);
        vm_write8(0x132009,costume_busy);
        return 0x130000;
    case 0x7f9a9c:
        CHECK(vm_read32(a)==0x130000 && b<2);
        if (c == 0xcffc0150u) {
            CHECK(!costume_busy);
            ++portrait_costumes[b];
            portrait_profiles[b] = vm_read32(c+0x28);
            return 0;
        }
        if (expected_mask==2) {
            CHECK(b==0 && vm_read32(c+0x28)==p2);
        } else {
            CHECK(expected_mask & (1u << b));
            CHECK(vm_read32(c+0x28)==(b ? p2 : p1));
        }
        CHECK(!costume_busy);
        ++costume_applied[b];
        // P1 already has these parts; native reports no new load required.
        // P2 starts a fresh load. Both must allow the transition to proceed.
        return b==0 ? 0 : 1;
    case 0x621784: return 0;
    case 0x62a318:
        for (unsigned i=0;i<0x2d0;++i) vm_write8(a+i,0);
        return 0;
    case 0x12ee34: {
        CHECK(a==manager); ++save_builds;
        const uint32_t entries=vm_read32(manager+0x370);
        for (uint32_t i=0;i<vm_read32(manager+0x374);++i) {
            const uint32_t entry=entries+i*0x7a8;
            if (!taiko_pc_mode_score_player(entry+8)) continue;
            CHECK(vm_read32(entry+0x668)==1);
            const bool rejected=reject_enqueue || int(vm_read32(entry))==reject_slot;
            taiko_pc_mode_score_enqueued(rejected ? 0 : 1);
            if (rejected) return 0;
            ++queued_scores;
            vm_write32(0x120018,vm_read32(0x120018)+1);
        }
        return 1;
    }
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
    // An unready service must not be touched. Once ready, targets belong to
    // distinct players, initialization happens once, and busy model loading
    // is retried without reconstructing any scene or player map.
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(!portrait_addresses[0] && !portrait_addresses[1]);
    vm_write32(0x131020,3);
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(portrait_addresses[0]==0xc1000000 && portrait_addresses[1]==0xc1200000);
    CHECK(portrait_costumes[0]==0 && portrait_costumes[1]==0);
    costume_busy=false;
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(std::count(calls.begin(),calls.end(),0x298cd4)==1);
    CHECK(portrait_costumes[0]==1 && portrait_costumes[1]==1);
    CHECK(join_animations[0]==1 && join_animations[1]==1);
    browser_joined = 1;
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    browser_joined = 3;
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(join_animations[0]==1 && join_animations[1]==2);
    // Login while browsing updates the matching slot, waits for the loader,
    // and detects color/variant changes without reloading every frame.
    vm_write32(manager+0x374,2);
    vm_write32(0x80000,0); vm_write32(0x807a8,1);
    vm_write8(0x80395,1); vm_write8(0x80b3d,1);
    vm_write32(0x80048,7); vm_write32(0x80800,42);
    costume_busy=true;
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(portrait_costumes[0]==1 && portrait_costumes[1]==1);
    costume_busy=false;
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(portrait_profiles[0]==0x80008 && portrait_profiles[1]==0x807b0);
    CHECK(portrait_costumes[0]==2 && portrait_costumes[1]==2);
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(portrait_costumes[0]==2 && portrait_costumes[1]==2);
    vm_write32(0x80ad0,3); // P2 special whole-body variant.
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(portrait_costumes[0]==2 && portrait_costumes[1]==3);
    vm_write8(0x80395,0); // Account removed: restore P1 guest defaults.
    CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(portrait_profiles[0]==0 && portrait_costumes[0]==3);
    vm_write32(manager+0x374,1);
    costume_busy=true;
    auto bad=match(); bad.content.music_id="missing";
    CHECK(runtime.enqueue_launch(bad)); CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(failures==1 && commits==0 && runtime.state()==taiko_plus::State::Browser);
    CHECK(vm_read32(manager+0x40c)==3); // Rejected launches leave session rules intact.
    CHECK(runtime.enqueue_launch(match()));
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(costume_applied[0]==0 && costume_applied[1]==0 && !gameplay);
    CHECK(portrait_addresses[0] && portrait_addresses[1]); // Still on the host panels.
    CHECK(std::find(calls.begin(),calls.end(),0x5c583c)==calls.end());
    costume_busy=false;
    for (unsigned i=0;i<150;++i) CHECK(taiko_pc_mode_setup_tick(&ctx));
    CHECK(costume_applied[0]==1 && costume_applied[1]==1);
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
    CHECK(departure_animations[0]==1 && departure_animations[1]==1);
    CHECK(portrait_addresses[0] && portrait_addresses[1]);
    browser_visible = false;
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(!portrait_addresses[0] && !portrait_addresses[1]);
    browser_visible = true;
    CHECK(std::count(calls.begin(),calls.end(),0x35d1a0)==1);
    vm_write32(0xb0000,0xf92c98); vm_write32(0xb000c,manager);
    vm_write32(root+0x68,0xc0000); vm_write32(root+0x6c,1);
    vm_write32(0xc0000,0xb0000);
    ctx.gpr[3]=root; taiko_pc_mode_frame_begin(&ctx);
    CHECK(runtime.state()==taiko_plus::State::Results);
    // Two authenticated profiles, one completed stage each. Native enqueue
    // takes owning copies; duplicate Results destinations must not resend.
    vm_write32(0x10399d0,0x120000);
    vm_write32(0x120008,0x130000);
    vm_write32(0x12000c,0x130000+32*0x7fc);
    for (unsigned slot=0;slot<2;++slot) {
        const uint32_t entry=0x80000+slot*0x7a8;
        vm_write32(entry,slot); vm_write32(entry+0x40,100+slot);
        vm_write8(entry+0x395,1); vm_write32(entry+0x668,1);
    }
    ctx.gpr[3]=setup;
    CHECK(taiko_pc_mode_results_return(0xb0000,0)==0);
    CHECK(taiko_pc_mode_results_return(0xb0000,owner)==1);
    CHECK(removals==1 && menus==2 && runtime.state()==taiko_plus::State::Browser);
    CHECK(taiko_pc_mode_results_return(0xb0000,owner)==1);
    CHECK(removals==1 && menus==2);
    CHECK(save_builds==1 && queued_scores==2 && save_status=="Saving scores...");
    CHECK(vm_read32(manager+0x438)==0x90090);
    // A second match reuses the same native session beyond the first Results.
    vm_write32(manager+0x408,1); // Native Results has advanced the round counter.
    CHECK(runtime.enqueue_launch(match()));
    ready=true;
    for (unsigned i=0;i<122;++i) taiko_pc_mode_setup_tick(&ctx);
    CHECK(commits==2 && runtime.state()==taiko_plus::State::Gameplay);
    CHECK(vm_read32(0x80668)==0 && vm_read32(0x80040)==100);
    CHECK(vm_read32(0x80000+0x7a8+0x668)==0);
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
    // P2-only: a retained logged-in P1 with no participation must not submit.
    vm_write32(manager+0x374,2);
    for (unsigned slot=0;slot<2;++slot) {
        const uint32_t entry=0x80000+slot*0x7a8;
        vm_write32(entry,slot); vm_write32(entry+0x40,100+slot);
        vm_write8(entry+0x395,1); vm_write32(entry+0x668,1);
    }
    vm_write32(0x120018,32); // Full queue: preserve the result and block overwrite.
    CHECK(taiko_pc_mode_results_return(0xb0000,owner)==1);
    CHECK(queued_scores==2 && save_status.find("queue full")!=std::string::npos);
    const unsigned previous_commits=commits;
    CHECK(runtime.enqueue_launch(match()));
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(commits==previous_commits && runtime.state()==taiko_plus::State::Browser);
    CHECK(vm_read32(0x80000+0x7a8+0x668)==1);
    vm_write32(0x120018,0);
    reject_enqueue=true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(queued_scores==2 && save_status=="Could not queue score - retrying");
    CHECK(vm_read32(0x80000+0x7a8+0x668)==1);
    reject_enqueue=false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(queued_scores==3); // P2 only, despite both profiles being authenticated.
    CHECK(vm_read32(0x120018)==1 && save_status=="Saving scores...");
    vm_write32(0x120018,0); // Native acknowledgement removes the queued record.
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(save_status=="Scores saved");
    CHECK(taiko_pc_mode_score_player(0)==1); // Arcade builder is untouched.
    // A partial two-player enqueue must retry only the unaccepted player.
    expected_mask=3;
    CHECK(runtime.enqueue_launch(match()));
    taiko_pc_mode_setup_tick(&ctx);
    ready=true;
    for (unsigned i=0;i<122;++i) taiko_pc_mode_setup_tick(&ctx);
    CHECK(runtime.state()==taiko_plus::State::Gameplay);
    vm_write32(0x80668,1);
    vm_write32(0x80000+0x7a8+0x668,1);
    reject_slot=1;
    CHECK(taiko_pc_mode_results_return(0xb0000,owner)==1);
    CHECK(queued_scores==4 && save_status=="Could not queue score - retrying");
    reject_slot=-1;
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    taiko_pc_mode_setup_tick(&ctx);
    CHECK(queued_scores==5 && vm_read32(0x120018)==2);
    CHECK(save_status=="Saving scores...");
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
