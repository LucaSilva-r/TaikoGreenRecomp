#include "taiko_pc_mode.h"
#include "taiko_frontend.h"
#include "ppu_recomp.h"
#include "taiko_overlay.h"
#include "taiko_plus_runtime.h"
#include "taiko_host_audio.h"
#include "taiko_custom_songs.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>

extern "C" uint64_t ppu_guest_call_ct(uint32_t code, uint32_t toc,
                                        uint64_t a0, uint64_t a1,
                                        uint64_t a2, uint64_t a3);

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" void ppu_set_project_register_hooks(void (*register_hooks)(void));
extern "C" uint32_t taiko_animation_frame_ticks(void);

void taiko_custom_titles_select(const std::string&, const std::string&);

namespace {

constexpr uint32_t kSequencePushTaskAddr = 0x008DA500u;
constexpr uint32_t kSequenceUpdateAddr = 0x0026D530u;
constexpr uint32_t kEntryStateOffset = 0x14u;
constexpr uint32_t kEntryStateEnd = 39u;
constexpr uint32_t kEntryStateTerm = 40u;

std::atomic<bool> s_pc_mode_active{false};
std::atomic<bool> s_pending_pc_mode{false};
std::atomic<bool> s_entry_handoff_armed{false};
uint32_t s_lifetime_probe_manager = 0;
uint64_t s_lifetime_probe_ticks = 0;
uint32_t s_sequence_runtime = 0;
uint32_t s_setup_anchor = 0;
uint32_t s_custom_index = 0xffffffffu;
std::string s_custom_id;
bool s_custom_round = false;
constexpr uint32_t kCustomMetadata = 0xcffc1000u;
uint32_t s_custom_manager = 0;
uint32_t s_custom_source_index = 0xffffffffu;

bool lifetime_trace_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("TAIKO_PLUS_LIFETIME_TRACE");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

void trace_manager(const char* point)
{
    if (!lifetime_trace_enabled() || !s_lifetime_probe_manager) return;
    const uint32_t manager = s_lifetime_probe_manager;
    std::fprintf(stderr,
        "[taiko_plus_lifetime] %s manager=%08X head=%08X/%08X "
        "played=%u limit=%u songs=%08X..%08X%s\n",
        point, manager, vm_read32(manager), vm_read32(manager + 4u),
        vm_read32(manager + 0x408u), vm_read32(manager + 0x40cu),
        vm_read32(manager + 0x434u), vm_read32(manager + 0x438u),
        (vm_read32(manager) & 0xffffff00u) == 0xdddddd00u
            ? " POISONED" : "");
}

// POD call arguments only; native constructors own every player/scene object.
constexpr uint32_t kScratch = 0xcffc0000u;
constexpr uint32_t kToc = 0x01037a88u;
uint64_t s_launch_generation = 0;
unsigned s_preload_frames = 0;
unsigned s_ready_frames = 0;
bool s_transition_started = false;
bool s_preloading = false;
uint8_t s_costume_pending = 0;
thread_local bool s_score_building = false;
bool s_score_pending = false;
bool s_score_delivery = false;
uint8_t s_round_mask = 0, s_score_wanted = 0, s_score_queued = 0;
thread_local uint8_t s_score_current = 0;
uint64_t s_score_generation = 0;
std::chrono::steady_clock::time_point s_score_retry{};

uint32_t native(uint32_t code, uint32_t a = 0, uint32_t b = 0,
                uint32_t c = 0, uint32_t d = 0)
{
    return static_cast<uint32_t>(ppu_guest_call_ct(code, kToc, a, b, c, d));
}

uint32_t s_portrait_service = 0;
uint8_t s_portrait_pending = 0;
uint8_t s_portrait_joined = 0;
bool s_portrait_departing = false;
std::array<std::array<uint32_t, 10>, 2> s_portrait_costumes{};

void clear_portraits()
{
    s_portrait_service = 0;
    s_portrait_pending = 0;
    s_portrait_joined = 0;
    s_portrait_departing = false;
    for (unsigned p = 0; p < 2; ++p)
        taiko_overlay_set_browser_portrait(p, 0, 0, 0);
}

void service_portraits()
{
    const auto state = taiko_plus::runtime().state();
    const bool browsing = state == taiko_plus::State::Browser ||
                          state == taiko_plus::State::Error;
    const bool launching = state == taiko_plus::State::PreparingMatch ||
                           state == taiko_plus::State::LaunchingGameplay;
    if (!browsing && !launching) {
        // The outgoing host panels still sample the last Entry targets. Native
        // gameplay now owns the character service; do not hide its models.
        if (s_portrait_service && !taiko_overlay_browser_visible()) clear_portraits();
        return;
    }
    if (browsing && s_portrait_departing) clear_portraits();
    if (launching && !s_portrait_service) return;
    const uint32_t service = native(0x005c573c, s_lifetime_probe_manager);
    const uint32_t owner = service ? vm_read32(service) : 0;
    if (!owner || vm_read32(owner + 0x20) != 3) return;
    if (browsing && s_portrait_service != service) {
        s_portrait_service = service;
        s_portrait_pending = 3;
        // Player Entry's 00232E24 setup: clear final targets, camera preset 4,
        // animation 45. The persistent character service owns all resources
        // and its normal render traversal advances and draws both models.
        native(0x00298cd4, service);
        native(0x00298f9c, service, 1, 0);
        for (unsigned p = 0; p < 2; ++p) {
            const uint32_t model = owner + 0x3b0 + p * 0x580;
            native(0x002a2cb0, model, p);
            native(0x002a2d50, model, p ^ 1);
            native(0x002a2df0, model, 3);
            native(0x00298f18, service, p, 4);
            native(0x00298fd0, service, p, 0x2d, 0x2d);
        }
        std::fprintf(stderr, "[browser_portraits] initialized service=%08X\n", service);
    }
    const uint8_t joined = taiko_overlay_browser_joined();
    const uint32_t scratch = kScratch + 0x100;
    const uint32_t flags = vm_read32(0x01038e40u);
    for (unsigned p = 0; p < 2; ++p) {
        // Read the existing flat map without inserting players. Login can
        // relocate it, so never retain a guest profile pointer across ticks.
        uint32_t profile = 0;
        std::array<uint32_t, 10> costume{};
        if (browsing) {
            const uint32_t map = s_lifetime_probe_manager + 0x370;
            const uint32_t entries = vm_read32(map), count = vm_read32(map + 4);
            if (entries && count <= 2) for (uint32_t i = 0; i < count; ++i) {
                const uint32_t entry = entries + i * 0x7a8;
                if (vm_read32(entry) == p && vm_read8(entry + 0x395)) {
                    profile = entry + 8;
                    break;
                }
            }
            if (profile) {
                costume[0] = 1;
                // 007F9A9C reads three colors, five parts, and the special
                // whole-body costume variant at profile +320.
                for (unsigned i = 0; i < 8; ++i)
                    costume[i + 1] = vm_read32(profile + 0x40 + i * 4);
                costume[9] = vm_read32(profile + 0x320);
            }
            if (costume != s_portrait_costumes[p]) s_portrait_pending |= 1u << p;
        }
        if (browsing && (s_portrait_pending & (1u << p)) && flags &&
            vm_read8(flags + 8) && !vm_read8(flags + 9) &&
            static_cast<int32_t>(vm_read32(owner + 0x25c + p * 4)) <= 0) {
            // Minimal native course record: the helper only reads +28.
            // Null selects the slot's default guest colors and costume.
            vm_write32(scratch + 0x40, service);
            for (unsigned i = 0; i < 12; ++i) vm_write32(scratch + 0x50 + i * 4, 0);
            vm_write32(scratch + 0x78, profile);
            native(0x007f9a9c, scratch + 0x40, p, scratch + 0x50);
            s_portrait_costumes[p] = costume;
            s_portrait_pending &= ~(1u << p);
        }
        if (browsing && !(s_portrait_pending & (1u << p)) &&
            (joined & (1u << p)) && !(s_portrait_joined & (1u << p))) {
            // Native animation table 010F95E8: entry_in -> entry_loop.
            native(0x00298fd0, service, p, 0x2c, 0x2d);
            s_portrait_joined |= 1u << p;
        }
        // 00518768 finds a color buffer by its native render-target key.
        // Color buffer +f8 owns the sampled texture descriptor (0054CDDC).
        const uint32_t key = native(0x00298f34, service, p);
        vm_write32(scratch + 0x20, 0);
        if (!key || !native(0x00518768, scratch + 0x20, key)) continue;
        const uint32_t color = vm_read32(scratch + 0x20);
        const uint32_t texture = color ? vm_read32(color + 0xf8) : 0;
        if (!texture) continue;
        const uint32_t address = vm_read32(texture + 0x34);
        const uint32_t dimensions = vm_read32(texture + 0x24);
        const uint32_t width = dimensions >> 16;
        const uint32_t height = dimensions & 0xffff;
        if (address && width && height && width <= 1024 && height <= 1024)
            taiko_overlay_set_browser_portrait(p, address, width, height);
    }
    s_portrait_joined &= joined;
    // Loader requests temporarily hide a model; publish participation after
    // requesting, without restarting its animation on each frame.
    native(0x00298d24, service, joined & 1, (joined >> 1) & 1);
}

// This singleton owns serialized copies and persists them to playresultinfo.
uint32_t score_queue()
{
    return vm_read32(0x010399d0u);
}

void service_score_save()
{
    const uint32_t queue = score_queue();
    if (s_score_delivery && queue && vm_read32(queue + 0x18) == 0) {
        s_score_delivery = false;
        if (!s_score_pending)
            taiko_overlay_set_browser_save_status("Scores saved");
        std::fprintf(stderr, "[taiko_plus_save] native delivery queue drained\n");
    }
    if (!s_score_pending || s_score_building || !queue) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_score_retry) return;
    s_score_retry = now + std::chrono::seconds(1);
    const uint32_t begin = vm_read32(queue + 8), end = vm_read32(queue + 12);
    const uint32_t count = vm_read32(queue + 0x18);
    const unsigned missing = ((s_score_wanted & ~s_score_queued) & 1u) +
                             (((s_score_wanted & ~s_score_queued) >> 1) & 1u);
    if (!begin || end < begin || (end - begin) % 0x7fc ||
        count > (end - begin) / 0x7fc ||
        missing > (end - begin) / 0x7fc - count) {
        taiko_overlay_set_browser_save_status("Save queue full - waiting to send");
        return;
    }
    s_score_building = true;
    // Owns its stack objects, formats the current timestamp, serializes and
    // queues each player. No reward/shop/GameOver scene is constructed.
    ppu_guest_call_ct(0x0012ee34, 0x01027c58,
                      s_lifetime_probe_manager, 0, 0, 0);
    s_score_building = false;
    if (s_score_queued == s_score_wanted) {
        s_score_pending = false;
        s_score_delivery = true;
        taiko_overlay_set_browser_save_status("Saving scores...");
        std::fprintf(stderr, "[taiko_plus_save] round=%llu queued mask=%u\n",
            static_cast<unsigned long long>(s_score_generation), s_score_queued);
    } else {
        taiko_overlay_set_browser_save_status("Could not queue score - retrying");
    }
}

void finish_score_round()
{
    if (s_custom_round) {
        taiko_overlay_set_browser_save_status("Custom song - local play");
        return;
    }
    if (s_score_generation == s_launch_generation) return;
    s_score_generation = s_launch_generation;
    s_score_wanted = s_score_queued = 0;
    const uint32_t map = s_lifetime_probe_manager + 0x370;
    const uint32_t entries = vm_read32(map), count = vm_read32(map + 4);
    if (!entries || count > 2) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = entries + i * 0x7a8;
        const uint32_t slot = vm_read32(entry);
        if (slot < 2 && (s_round_mask & (1u << slot)) &&
            vm_read8(entry + 0x395) && vm_read32(entry + 0x40) &&
            vm_read32(entry + 0x668) == 1)
            s_score_wanted |= 1u << slot;
    }
    s_score_pending = s_score_wanted != 0;
    s_score_retry = {};
    if (s_score_pending) {
        taiko_overlay_set_browser_save_status("Preparing score upload...");
        service_score_save();
    }
    else taiko_overlay_set_browser_save_status("No online score for this round");
}

// Mirror NuSound's group ancestry (003EEF00/003EFA0C) and gain reference
// (003FDCC4). This runs on the PPU thread; the host mixer only sees atomics.
void publish_audio_volume()
{
    const auto pointer = [](uint32_t p) { return p >= 0x10000 && p < 0xc0000000u; };
    const auto read_float = [](uint32_t p) {
        const uint32_t bits = vm_read32(p);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    };
    const uint32_t global = vm_read32(kToc + 0x448c);
    const uint32_t table = vm_read32(kToc + 0x4508);
    if (!pointer(global) || !pointer(table)) return;
    const uint32_t core = vm_read32(global);
    if (!pointer(core)) return;
    const float reference = read_float(core + 0x1c1c);
    const float floor = read_float(core + 0x1c20);
    if (!std::isfinite(reference) || !std::isfinite(floor)) return;
    for (uint32_t group = 0; group < 68; ++group) {
        float db = 0.0f;
        bool valid = false;
        int32_t ancestor = group;
        for (unsigned depth = 0; depth < 68; ++depth) {
            if (ancestor < 0) { valid = true; break; }
            if (ancestor >= 68) break;
            const uint32_t entry = table + uint32_t(ancestor) * 0x40;
            if (!vm_read8(entry + 0x14) || vm_read32(entry + 0x3c) == 1) break;
            db += read_float(entry + 4) + read_float(entry + 0x30) +
                  read_float(entry + 0x28);
            const uint32_t parent = vm_read32(entry);
            if (!pointer(parent)) break;
            ancestor = static_cast<int32_t>(vm_read32(parent));
        }
        const float gain = valid && std::isfinite(db) && db > floor &&
                           db - reference > floor
            ? std::pow(10.0f, (db - reference) / 20.0f) : 0.0f;
        taiko_host_audio_set_group_gain(group, gain);
        if (group == 11) {
            static float previous = -1.0f;
            if (gain != previous) {
                std::fprintf(stderr,
                    "[taiko_host_audio] native music group gain=%.4f db=%.2f reference=%.2f\n",
                    gain, db, reference);
                previous = gain;
            }
        }
    }
}

bool live_song(uint32_t manager, const std::string& id, uint32_t& index)
{
    const uint32_t begin = vm_read32(manager + 0x434);
    const uint32_t end = vm_read32(manager + 0x438);
    if (!begin || end < begin || (end - begin) % 0x90 ||
        (end - begin) / 0x90 > 2048) return false;
    for (index = 0; index < (end - begin) / 0x90; ++index) {
        const uint32_t str = begin + index * 0x90 + 4;
        if (vm_read32(str + 0x10) != id.size() || id.size() > 64) continue;
        const uint32_t data = vm_read32(str + 0x14) <= 15 ? str : vm_read32(str);
        if (!data) continue;
        bool same = true;
        for (unsigned i = 0; i < id.size(); ++i)
            if (vm_read8(data + i) != static_cast<uint8_t>(id[i])) same = false;
        if (same) return true;
    }
    return false;
}

bool custom_slot(uint32_t manager, const TaikoCatalogSong& song, uint32_t& index)
{
    if (s_custom_manager != manager) {
        s_custom_index = s_custom_source_index = 0xffffffffu;
        s_custom_id.clear();
        s_custom_manager = manager;
    }
    const uint32_t begin = vm_read32(manager + 0x434), end = vm_read32(manager + 0x438);
    if (!begin || end <= begin || (end-begin)%0x90 || (end-begin)/0x90 > 2048) return false;
    // Native copy construction owns string allocations and vector relocation.
    // One reusable slot keeps library size independent of the guest catalog.
    if (s_custom_index == 0xffffffffu) {
        uint32_t donor = 0;
        for (uint32_t p=begin; p<end; p+=0x90)
            if (vm_read32(p+0x18) == 15 && vm_read32(p+0x14) > 0) {donor=p;break;}
        if (!donor) return false;
        ppu_guest_call_ct(0x00632b5c, 0x01027c58, kCustomMetadata + 0x200, manager + 0x464, donor, 0);
        const uint32_t meta = vm_read32(kCustomMetadata + 0x200);
        if (!meta) return false;
        for(unsigned off=0;off<0x110;off+=4) vm_write32(kCustomMetadata+off,vm_read32(meta+off));
        ppu_guest_call_ct(0x00716850, 0x01027c58, manager+0x430, end, 1, donor);
        const uint32_t next_begin=vm_read32(manager+0x434), next_end=vm_read32(manager+0x438);
        if (!next_begin || next_end-next_begin != end-begin+0x90) return false;
        s_custom_index=(end-begin)/0x90;
    }
    if (s_custom_source_index == 0xffffffffu) {
        const uint32_t source_begin = vm_read32(manager + 0xd04);
        const uint32_t source_end = vm_read32(manager + 0xd08);
        if (!source_begin || source_end <= source_begin || (source_end-source_begin)%0x90) return false;
        const uint32_t donor = vm_read32(manager+0x434) + s_custom_index*0x90;
        ppu_guest_call_ct(0x00716850, 0x01027c58, manager+0xd00, source_end, 1, donor);
        const uint32_t next_begin=vm_read32(manager+0xd04), next_end=vm_read32(manager+0xd08);
        if (!next_begin || next_end-next_begin != source_end-source_begin+0x90) return false;
        s_custom_source_index=(source_end-source_begin)/0x90;
    }
    index=s_custom_index;
    const auto inline_id=[&](uint32_t str) {
        for(unsigned i=0;i<16;++i) vm_write8(str+i,i<song.music_id.size()?song.music_id[i]:0);
        vm_write32(str+0x10,song.music_id.size()); vm_write32(str+0x14,15);
    };
    for (const auto [offset, ordinal] : {std::pair{0x434u,s_custom_index},
                                        std::pair{0xd04u,s_custom_source_index}}) {
        const uint32_t rec=vm_read32(manager+offset)+ordinal*0x90;
        if (rec+0x90 > vm_read32(manager+offset+4) || vm_read32(rec+0x18)!=15) return false;
        inline_id(rec+4);
        vm_write32(rec+0x1c,6000);
        vm_write8(rec+0x23,(song.difficulty_mask>>4)&1);
    }
    inline_id(kCustomMetadata+4);
    for(unsigned d=0;d<5;++d) vm_write32(kCustomMetadata+0x1c+d*4,song.stars[d]);
    for(unsigned d=0;d<4;++d) vm_write32(kCustomMetadata+0x30+d*4,song.stars[d]);
    vm_write32(kCustomMetadata+0x54,(song.difficulty_mask>>4)&1);
    s_custom_id=song.music_id;
    return true;
}

void prepare_match(const taiko_plus::MatchConfig& match)
{
    auto& runtime = taiko_plus::runtime();
    if (s_score_pending) {
        runtime.fail(match.generation, taiko_plus::GuestErrorCode::PlayerUnavailable,
                     "previous score is waiting for space in the save queue");
        return;
    }
    const uint32_t manager = s_lifetime_probe_manager;
    uint32_t index = 0;
    const auto* custom = taiko_custom_find(match.content.music_id);
    if (!manager || manager != s_sequence_runtime + 0xd8 ||
        !(custom ? custom_slot(manager, *custom, index) : live_song(manager, match.content.music_id, index))) {
        runtime.fail(match.generation, taiko_plus::GuestErrorCode::InvalidContent,
                     "selected song is absent from the native session catalog");
        return;
    }
    const uint32_t map = manager + 0x370;
    const uint32_t old_count = vm_read32(map + 4);
    if (old_count > 2 || (old_count && !vm_read32(map))) {
        runtime.fail(match.generation, taiko_plus::GuestErrorCode::PlayerUnavailable,
                     "native player map is invalid");
        return;
    }
    uint8_t mask = 0;
    uint8_t inserted = 0;
    std::array<uint32_t, 2> players{};
    // Only participating slots are constructed. Insertions can relocate the
    // whole flat map, so reacquire all pointers after the final insertion.
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!match.players[slot].enabled) continue;
        mask |= 1u << slot;
        const uint32_t before = vm_read32(map + 4);
        vm_write32(kScratch, slot);
        native(0x005c59bc, map, kScratch);
        if (vm_read32(map + 4) != before) inserted |= 1u << slot;
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!(mask & (1u << slot))) continue;
        vm_write32(kScratch, slot);
        players[slot] = native(0x005c59bc, map, kScratch);
        if (!players[slot]) {
            runtime.fail(match.generation, taiko_plus::GuestErrorCode::PlayerUnavailable,
                         "native player construction failed");
            return;
        }
        if (inserted & (1u << slot))
            ppu_guest_call_ct(0x00717aec, 0x01027c58, manager + 0x430,
                              players[slot], 0, 0);
    }
    vm_write32(manager + 0x400, mask); // 1=P1, 2=P2-only, 3=both.
    // The map entry contains two separately constructed objects: persistent
    // profile at +8 and round data at +0x4d8. Reconstruct only the latter,
    // including for unjoined slots, before gameplay obtains any stage pointers.
    const uint32_t entries = vm_read32(map), count = vm_read32(map + 4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t round = entries + i * 0x7a8 + 0x4d8;
        ppu_guest_call_ct(0x00621784, 0x01027c58, round, 0, 0, 0);
        ppu_guest_call_ct(0x0062a318, 0x01027c58, round, 0, 0, 0);
    }
    s_round_mask = mask;
    s_custom_round = custom != nullptr;
    taiko_custom_titles_select(custom ? custom->title : std::string{},
                              custom ? custom->custom_subtitle : std::string{});
    for (uint32_t offset = 0; offset < 0x90; offset += 4)
        vm_write32(kScratch + offset, 0);
    for (unsigned slot = 0; slot < 2; ++slot) {
        const uint32_t course = kScratch + 0x10 + slot * 0x30;
        vm_write8(course, match.players[slot].enabled);
        vm_write32(course + 4, match.players[slot].difficulty);
        vm_write32(course + 0x28, players[slot]);
        vm_write32(course + 0x2c, players[slot] ? players[slot] + 0x4d0 : 0);
    }
    vm_write32(kScratch, index);
    vm_write32(kScratch + 4, kScratch + 0x10);
    vm_write32(kScratch + 8, kScratch + 0x40);
    // Each browser launch is a fresh one-song round. Keeping the cabinet's
    // multi-song limit enables the failed-song revival drum challenge before
    // Results, where our return-to-browser hook cannot intercept it. Reset the
    // round counter too: this manager survives every Results/browser cycle.
    vm_write32(manager + 0x408, 0);
    vm_write32(manager + 0x40c, 1);
    native(0x007fce6c, kScratch, manager);
    s_costume_pending = mask;
    s_launch_generation = match.generation;
    s_preload_frames = 0;
    s_ready_frames = 0;
    s_transition_started = false;
    s_preloading = runtime.transition(match.generation,
        taiko_plus::State::PreparingMatch, taiko_plus::State::LaunchingGameplay,
        taiko_plus::EventKind::MatchAccepted);
    std::fprintf(stderr, "[taiko_plus] native selection committed id=%s index=%u "
        "difficulty=%u/%u mask=%u players=%08X/%08X generation=%llu\n",
        match.content.music_id.c_str(), index, match.players[0].difficulty,
        match.players[1].difficulty, mask, players[0], players[1],
        static_cast<unsigned long long>(match.generation));
}

void advance_launch(uint32_t owner)
{
    if (!s_preloading) return;
    auto& runtime = taiko_plus::runtime();
    if (runtime.generation() != s_launch_generation ||
        runtime.state() != taiko_plus::State::LaunchingGameplay) {
        s_preloading = false;
        return;
    }
    const uint32_t manager = s_lifetime_probe_manager;
    ++s_preload_frames;
    if (s_costume_pending) {
        // Stock Song Select (001f76ec) passes a pointer to the shared
        // character service and a course record to apply profile colors and
        // costume parts. Copying the profile alone leaves the previous model.
        const uint32_t characters = native(0x005c573c, manager);
        if (!characters) return;
        vm_write32(kScratch + 0x80, characters);
        for (unsigned slot = 0; slot < 2; ++slot) {
            if (!(s_costume_pending & (1u << slot))) continue;
            // Stock 001f5f6c -> 001f7538 maps P2-only play to the first
            // displayed character, while retaining P2's course/profile.
            const unsigned character_slot = s_round_mask == 2 ? 0 : slot;
            // 0029d474's availability gates distinguish busy from unchanged:
            // once available, 0029cc34 returns zero only for a model that is
            // already loaded with the requested parts. That is also success.
            const uint32_t character_owner = vm_read32(characters);
            const uint32_t loader_flags = vm_read32(0x01038e40u);
            if (!character_owner || !loader_flags ||
                !vm_read8(loader_flags + 8) || vm_read8(loader_flags + 9) ||
                static_cast<int32_t>(vm_read32(character_owner + 0x25c +
                                               character_slot * 4)) > 0)
                continue;
            native(0x007f9a9c, kScratch + 0x80, character_slot,
                   kScratch + 0x10 + slot * 0x30);
            s_costume_pending &= ~(1u << slot);
        }
        if (s_costume_pending) return;
        // GameEnso's native state 1 waits for asset loading to complete.
        std::fprintf(stderr, "[taiko_plus] native player costumes accepted\n");
    }
    if (!s_transition_started) {
        if (s_portrait_service && !s_portrait_departing) {
            s_portrait_departing = true;
            for (unsigned p = 0; p < 2; ++p) {
                if (!(s_round_mask & (1u << p))) continue;
                // Native Song Select departure, after costume acceptance so
                // model loading cannot overwrite the requested motion.
                const uint32_t motion = p ? 0x4c : 0x4a;
                // 0029BD88 treats follow-up -1 as hold at the final frame.
                native(0x00298fd0, s_portrait_service, p, motion, UINT32_MAX);
            }
        }
        const uint32_t service = native(0x005c5c1c, manager);
        vm_write32(kScratch, 0);
        // This starts the shared Lumen transition; it is not a pure readiness
        // predicate. Once accepted, native Song Select changes state and never
        // calls it again during the following 120-frame interval.
        s_transition_started = service && static_cast<uint8_t>(
            native(0x005c583c, service, kScratch));
        if (!s_transition_started) {
            if (s_preload_frames % 300 == 0)
                std::fprintf(stderr, "[taiko_plus] waiting for native transition "
                    "frames=%u service=%08X state=%d\n", s_preload_frames,
                    service, service ? static_cast<int32_t>(vm_read32(service + 8)) : -1);
            return;
        }
        std::fprintf(stderr, "[taiko_plus] native transition accepted; waiting 120 animation ticks\n");
        return;
    }
    s_ready_frames += taiko_animation_frame_ticks();
    if (s_ready_frames <= 120) return;
    s_preloading = false;
    const uint32_t sound = native(0x005c544c, manager);
    if (sound) native(0x005c535c, sound, 0);
    const uint32_t scene = native(0x0035d1a0, 0x178);
    if (!scene) {
        runtime.fail(s_launch_generation,
            taiko_plus::GuestErrorCode::PartialConstructionFailed,
            "native GameEnso allocation failed");
        return;
    }
    native(0x001e1c04, scene, manager);
    // Match the native factory: queue before configuring; the current frame
    // transaction invokes initialization only after this hook returns.
    native(0x008da500, owner, scene, 0);
    vm_write32(kScratch, manager);
    vm_write32(kScratch + 0x10, 0);
    for (unsigned off = 4; off <= 0x24; off += 4)
        vm_write32(kScratch + 0x10 + off, 0xffffffff);
    vm_write32(kScratch + 0x38, 0);
    native(0x00251c08, kScratch, kScratch + 0x10);
    native(0x001de520, scene, kScratch + 0x10);
    runtime.transition(s_launch_generation, taiko_plus::State::LaunchingGameplay,
                       taiko_plus::State::Gameplay, taiko_plus::EventKind::GameplayStarted);
    taiko_frontend_standalone_gameplay();
    std::fprintf(stderr, "[taiko_plus] GameEnso queued scene=%08X state=%u owner=%08X\n",
                 scene, vm_read32(scene + 8), owner);
}

} // namespace

int taiko_pc_mode_is_active(void)
{
    return s_pc_mode_active.load(std::memory_order_acquire) ? 1 : 0;
}

int taiko_pc_mode_is_standalone(void)
{
    return taiko_plus::standalone_enabled() ? 1 : 0;
}

void taiko_pc_mode_on_game_mode_selected(uint32_t mode)
{
    if (mode == TAIKO_PC_MODE_SENTINEL) {
        std::fprintf(stderr,
                     "[taiko_pc_mode] PC Mode selected in the stock carousel\n");
        s_entry_handoff_armed.store(false, std::memory_order_release);
        s_pending_pc_mode.store(true, std::memory_order_release);
    } else {
        s_pending_pc_mode.store(false, std::memory_order_release);
        s_entry_handoff_armed.store(false, std::memory_order_release);
        std::fprintf(stderr,
                     "[taiko_pc_mode] stock arcade mode %u selected\n", mode);
    }
}

void taiko_pc_mode_entry_tick(ppu_context* ctx)
{
    if (!ctx || !s_pending_pc_mode.load(std::memory_order_acquire) ||
        s_entry_handoff_armed.load(std::memory_order_acquire))
        return;

    const uint32_t entry = static_cast<uint32_t>(ctx->gpr[3]);
    if (!entry) return;
    const uint32_t state = vm_read32(entry + kEntryStateOffset);
    if (state != kEntryStateEnd && state != kEntryStateTerm) return;

    s_lifetime_probe_manager = vm_read32(entry + 0x0cu);
    s_lifetime_probe_ticks = 0;
    trace_manager("entry-final-state");
    s_entry_handoff_armed.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "[taiko_pc_mode] Player Entry reached final state %u; "
                 "handoff armed\n", state);
}

void taiko_pc_mode_activate(uint32_t controller)
{
    s_score_pending = s_score_building = false;
    s_score_generation = 0;
    taiko_overlay_set_browser_save_status("");
    s_pc_mode_active.store(true, std::memory_order_release);
    taiko_plus::runtime().activate();
    taiko_frontend_standalone_session_begin();
    if (s_lifetime_probe_manager) {
        const uint32_t map = s_lifetime_probe_manager + 0x370;
        const uint32_t begin = vm_read32(map), count = vm_read32(map + 4);
        if (begin && count <= 2) for (unsigned i = 0; i < count; ++i) {
            const uint32_t record = begin + i * 0x7a8;
            const unsigned slot = vm_read32(record);
            if (slot > 1) continue;
            const bool authenticated = vm_read8(record + 0x395) != 0;
            const uint32_t profile = record + 8;
            const uint32_t length = vm_read32(profile + 0x14);
            const uint32_t capacity = vm_read32(profile + 0x18);
            const uint32_t data = capacity <= 15 ? profile + 4 : vm_read32(profile + 4);
            char name[128] = {};
            if (authenticated && data && length < sizeof name && length <= capacity)
                for (unsigned n = 0; n < length; ++n) name[n] = vm_read8(data + n);
            taiko_frontend_browser_account(slot, name, authenticated);
        }
    }
    if (taiko_pc_mode_is_standalone())
        taiko_host_audio_set_scene_active(true);
    std::fprintf(stderr,
                 "[taiko_pc_mode] host PC Mode activated (%s); "
                 "SequenceController=%08X\n",
                 taiko_pc_mode_is_standalone() ? "standalone" : "legacy",
                 controller);

    taiko_overlay_clear();
    taiko_frontend_enter_song_select_shell();
}

int taiko_pc_mode_destination_override(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_standalone() ||
        !s_pending_pc_mode.load(std::memory_order_acquire) ||
        !s_entry_handoff_armed.load(std::memory_order_acquire))
        return 0;

    const uint32_t outgoing = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t scene_owner = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t vtable = scene_owner ? vm_read32(scene_owner) : 0;
    if (!outgoing || !scene_owner || !vtable) return 0;

    /* This is diagnostic observation, not ownership. The manager remains
     * unusable by standalone bootstrap until its native retain/release
     * operation has been proved. */
    s_lifetime_probe_manager = vm_read32(outgoing + 0x0cu);
    s_lifetime_probe_ticks = 0;
    trace_manager("before-entry-removal");

    const uint32_t contains_opd = vm_read32(vtable + 0x14u);
    if (!contains_opd || !vm_read32(contains_opd) ||
        !vm_read32(contains_opd + 4u))
        return 0;

    /* Mirror the exact pre-allocation prefix of func_001FE470. The +0x14
     * membership predicate returns a byte; when it is nonzero the native
     * factory invokes the +0x0c removal operation once, then proceeds to
     * construct the destination. */
    const bool contains_outgoing = static_cast<uint8_t>(ppu_guest_call_ct(
        vm_read32(contains_opd), vm_read32(contains_opd + 4u),
        scene_owner, outgoing, 0, 0)) != 0;
    if (contains_outgoing) {
        const uint32_t remove_opd = vm_read32(vtable + 0x0cu);
        if (!remove_opd || !vm_read32(remove_opd) ||
            !vm_read32(remove_opd + 4u))
            return 0;
        ppu_guest_call_ct(vm_read32(remove_opd), vm_read32(remove_opd + 4u),
                          scene_owner, outgoing, 0, 0);
        trace_manager("after-entry-removal");
    }

    s_entry_handoff_armed.store(false, std::memory_order_release);
    s_pending_pc_mode.store(false, std::memory_order_release);
    std::fprintf(stderr,
                 "[taiko_pc_mode] suppressed Song Select destination before "
                 "allocation owner=%08X outgoing=%08X\n",
                 scene_owner, outgoing);
    taiko_pc_mode_activate(scene_owner);
    return 1;
}

void taiko_pc_mode_deactivate(void)
{
    clear_portraits();
    taiko_plus::runtime().deactivate();
    taiko_host_audio_set_scene_active(false);
    s_pc_mode_active.store(false, std::memory_order_release);
    s_pending_pc_mode.store(false, std::memory_order_release);
    s_entry_handoff_armed.store(false, std::memory_order_release);
    s_lifetime_probe_manager = 0;
    s_lifetime_probe_ticks = 0;
    s_setup_anchor = 0;
    s_preloading = false;
    std::fprintf(stderr, "[taiko_pc_mode] PC Mode deactivated\n");
}

int taiko_pc_mode_setup_complete(uint32_t setup, uint32_t owner)
{
    if (!taiko_pc_mode_is_standalone() ||
        !s_pending_pc_mode.load(std::memory_order_acquire) ||
        !s_entry_handoff_armed.load(std::memory_order_acquire) ||
        !setup || vm_read32(setup) != 0x00f8bae8u ||
        !owner || vm_read32(owner) != 0x00f9ae70u)
        return 0;
    s_setup_anchor = setup;
    s_lifetime_probe_manager = vm_read32(setup + 4u);
    trace_manager("song-setup-complete");
    s_entry_handoff_armed.store(false, std::memory_order_release);
    s_pending_pc_mode.store(false, std::memory_order_release);
    std::fprintf(stderr,
        "[taiko_pc_mode] native GameSongSetup complete; "
        "suppressed Song Select before allocation manager=%08X\n",
        s_lifetime_probe_manager);
    taiko_pc_mode_activate(owner);
    return 1;
}

void taiko_pc_mode_push_task_hook(ppu_context* ctx)
{
    if (!ctx) return;
    const uint32_t controller = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t task = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t task_vtable = task ? vm_read32(task) : 0;

    /* Entry first queues GameSongSetup (f8bae8), which owns the session
     * population work. It is not a Song Select variant. Keep the marker armed
     * through setup and unrelated task pushes until the destination factory. */
    if (s_entry_handoff_armed.load(std::memory_order_acquire) &&
        task_vtable != 0x00f92fc0u) {
        if (task_vtable == 0x00f8bae8u)
            std::fprintf(stderr,
                "[taiko_pc_mode] retaining native GameSongSetup task=%08X\n",
                task);
        func_008DA500(ctx);
        return;
    }

    if (s_entry_handoff_armed.exchange(false, std::memory_order_acq_rel)) {
        s_pending_pc_mode.store(false, std::memory_order_release);
        if (taiko_pc_mode_is_standalone()) {
            std::fprintf(stderr,
                         "[taiko_pc_mode] suppressing post-Entry arcade task "
                         "controller=%08X task=%08X vtable=%08X\n",
                         controller, task, task_vtable);
            taiko_pc_mode_activate(controller);
            ctx->gpr[3] = 1;
            return;
        }

        std::fprintf(stderr,
                     "[taiko_pc_mode] legacy path retains post-Entry task "
                     "controller=%08X task=%08X vtable=%08X\n",
                     controller, task, task_vtable);
        func_008DA500(ctx);
        taiko_pc_mode_activate(controller);
        return;
    }

    func_008DA500(ctx);
}

void taiko_pc_mode_frame_begin(ppu_context* ctx)
{
    if (!ctx) return;
    s_sequence_runtime = static_cast<uint32_t>(ctx->gpr[3]);
    if (!taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone()) return;
    if (taiko_plus::runtime().state() == taiko_plus::State::Gameplay) {
        const uint32_t tasks = vm_read32(s_sequence_runtime + 0x68);
        const uint32_t count = vm_read32(s_sequence_runtime + 0x6c);
        if (tasks && count <= 256) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t task = vm_read32(tasks + i * 4);
                if (task && vm_read32(task) == 0x00f92c98 &&
                    vm_read32(task + 0xc) == s_lifetime_probe_manager) {
                    taiko_plus::runtime().transition(s_launch_generation,
                        taiko_plus::State::Gameplay, taiko_plus::State::Results,
                        taiko_plus::EventKind::ResultsStarted);
                    std::fprintf(stderr, "[taiko_plus] native Results active task=%08X\n", task);
                    break;
                }
            }
        }
    }
    if (s_lifetime_probe_ticks == 0 && lifetime_trace_enabled())
        std::fprintf(stderr,
            "[taiko_plus_lifetime] frame runtime=%08X session=%08X "
            "entry_manager=%08X active_tasks=%u pending_tasks=%u tid=%u\n",
            s_sequence_runtime, s_sequence_runtime + 0xd8u,
            s_lifetime_probe_manager, vm_read32(s_sequence_runtime + 0x6cu),
            vm_read32(s_sequence_runtime + 0x78u), ctx->thread_id);
    /* This routine also completes deferred native scene destruction. Never
     * freeze it: the scene-owner facade exists only inside its stack frame. */
}

void taiko_pc_mode_frame_dispatch(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone())
        return;
    /* Called after native traversal, before the facade is destroyed and
     * queued scene initialization/retirement is committed. */
    const uint32_t owner = static_cast<uint32_t>(ctx->gpr[1]) + 0x15cu;
    if (vm_read32(owner) != 0x00f9ae70u) return;
    // Runtime commands are serviced by the retained setup task, whose facade
    // has the correct native parent for queuing gameplay siblings.
}

void taiko_pc_mode_tick(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_active()) return;
    service_score_save();
    taiko_frontend_browser_login_tick(s_lifetime_probe_manager, s_score_pending);
    publish_audio_volume();
    ++s_lifetime_probe_ticks;
    if (s_lifetime_probe_manager &&
        (s_lifetime_probe_ticks == 1u ||
         s_lifetime_probe_ticks % 600u == 0u))
        trace_manager("session-tick");
    taiko_plus::HostGuestCommand command;
    if (taiko_plus::runtime().try_pop_command(command)) {
        if (command.kind == taiko_plus::CommandKind::Abort) {
            taiko_plus::runtime().return_to_browser(
                command.match.generation);
        } else {
            prepare_match(command.match);
        }
    }
    service_portraits();
    advance_launch(static_cast<uint32_t>(ctx->gpr[4]));
    taiko_plus::HostGuestEvent event;
    while (taiko_plus::runtime().try_pop_event(event)) {
        if (event.kind == taiko_plus::EventKind::Failed)
            taiko_frontend_standalone_failure(event.error.detail.c_str());
    }
}

void taiko_pc_mode_init_hooks(void)
{
    ppu_register_function(kSequencePushTaskAddr, taiko_pc_mode_push_task_hook);
    std::fprintf(stderr,
                 "[taiko_pc_mode] Registered SequenceController hooks: push_task=0x%08X, update=0x%08X\n",
                 kSequencePushTaskAddr, kSequenceUpdateAddr);
}

namespace {
struct PcModeHookRegistrar {
    PcModeHookRegistrar() {
        ppu_set_project_register_hooks(taiko_pc_mode_init_hooks);
    }
} s_pc_mode_hook_registrar;
} // namespace

int taiko_pc_mode_results_return(uint32_t results, uint32_t owner)
{
    if (!taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone() ||
        !results || vm_read32(results) != 0x00f92c98 ||
        !owner || vm_read32(owner) != 0x00f9ae70 ||
        vm_read32(results + 0xc) != s_lifetime_probe_manager) return 0;
    const auto state = taiko_plus::runtime().state();
    if (state != taiko_plus::State::Gameplay && state != taiko_plus::State::Results)
        return 1; // A duplicate destination callback must not retire twice.
    finish_score_round();
    if (static_cast<uint8_t>(native(0x008d427c, owner, results)))
        native(0x008ddd30, owner, results);
    taiko_plus::runtime().return_to_browser(taiko_plus::runtime().generation());
    taiko_host_audio_reacquire_menu();
    taiko_frontend_enter_song_select_shell();
    std::fprintf(stderr, "[taiko_plus] Results retired without Song Select allocation "
                 "results=%08X owner=%08X\n", results, owner);
    return 1;
}

int taiko_pc_mode_score_player(uint32_t player)
{
    if (!s_score_building) return 1;
    s_score_current = 0;
    if (player < 8) return 0;
    const uint32_t entry = player - 8;
    const uint32_t slot = vm_read32(entry);
    if (slot >= 2 || !(s_score_wanted & (1u << slot)) ||
        (s_score_queued & (1u << slot))) return 0;
    s_score_current = 1u << slot;
    return 1;
}

void taiko_pc_mode_score_enqueued(uint32_t success)
{
    if (!s_score_building) return;
    if (success & 0xff) s_score_queued |= s_score_current;
    s_score_current = 0;
}

int taiko_pc_mode_setup_tick(ppu_context* ctx)
{
    if (!ctx || !taiko_pc_mode_is_active() || !taiko_pc_mode_is_standalone() ||
        static_cast<uint32_t>(ctx->gpr[3]) != s_setup_anchor) return 0;
    // Keep this already initialized native task as the session's browser
    // anchor. Its original update would rerun setup and enter Song Select.
    // No Lumen or audio object is created by this idle task.
    taiko_pc_mode_tick(ctx);
    return 1;
}

/* Same BasicSong metadata contract used by Zucchini's native injector. Only
 * the reusable custom record is intercepted; stock lookups retain guest code. */
extern "C" int taiko_custom_basic_lookup(ppu_context* ctx)
{
    if (!ctx || s_custom_id.empty()) return 0;
    const uint32_t out=uint32_t(ctx->gpr[3]), rec=uint32_t(ctx->gpr[5]);
    if (!out || !rec || vm_read32(rec+0x1c)!=6000 ||
        vm_read32(rec+0x14)!=s_custom_id.size() || vm_read32(rec+0x18)!=15) return 0;
    for(unsigned i=0;i<s_custom_id.size();++i)
        if(vm_read8(rec+4+i)!=uint8_t(s_custom_id[i])) return 0;
    vm_write32(out,kCustomMetadata);
    ctx->gpr[3]=out;
    return 1;
}
int taiko_custom_basic_lookup_bridge(ppu_context* ctx)
{
    return taiko_custom_basic_lookup(ctx);
}
