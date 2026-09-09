// Native title injection adapted from Zucchini's songselect_natives.c.
// Only the selected custom song needs HUD/transition resources: the host owns
// the browser. Private upload owners and persistent draw copies outlive scenes.
#include "ppu_recomp.h"
extern "C" {
#include "taiko_title_render.h"
int32_t cellGcmMapMainMemory(uint32_t ea, uint32_t size, uint32_t* offset);
int64_t sys_memory_allocate(ppu_context* ctx);
uint64_t ppu_guest_call_ct8(uint32_t, uint32_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t ppu_guest_call_ct(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
}
#include <array>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>

namespace {
std::mutex mutex;
std::string selected_title, selected_subtitle;
uint64_t generation = 0;
constexpr uint32_t scratch = 0xcffc2000u;
constexpr uint32_t slab_size = 0x100000u;
uint32_t slab = 0, io = 0, allocation = 0;
thread_local bool fallback_lookup = false;
struct Slot {
    uint32_t resource = 0;
    uint64_t generation = 0;
};
std::array<Slot, 2> slots;
bool sane(uint32_t p) { return p >= 0x10000u && p < 0xd0000000u && !(p & 3u); }

bool memory_ready()
{
    if (slab) return true;
    if (!allocation) {
        ppu_context ctx{};
        ctx.gpr[3] = slab_size;
        ctx.gpr[4] = 0x400u; // SYS_MEMORY_PAGE_SIZE_1M
        ctx.gpr[5] = scratch;
        if (sys_memory_allocate(&ctx)) return false;
        allocation = vm_read32(scratch);
    }
    if (cellGcmMapMainMemory(allocation, slab_size,
                           reinterpret_cast<uint32_t*>(uintptr_t(scratch)))) return false;
    io = vm_read32(scratch);
    slab = allocation;
    return true;
}

uint32_t resource(unsigned type, uint32_t key)
{
    if (!memory_ready()) return 0;
    const unsigned index = type - 11;
    auto& slot = slots[index];
    const uint32_t descriptor = slab + 0xf0000 + index * 0x40;
    const uint32_t pixels = slab + index * 0x30000;
    const uint32_t staging = slab + 0x80000;
    if (slot.generation == generation) {
        vm_write32(descriptor + 8, key);
        return descriptor;
    }
    const uint32_t private_key = 0x7e100000u + index;
    // A private key prevents song teardown from removing the upload owner.
    // Still validate its liveness before calling a virtual method.
    if (slot.resource && (!sane(vm_read32(slot.resource)) ||
                         vm_read32(slot.resource + 8) != private_key))
        slot.resource = 0;
    uint32_t w = 0, h = 0;
    if (!title_tex_dims(type, &w, &h)) return 0;
    if (!slot.resource) {
        const uint32_t manager = vm_read32(0x0103f224u);
        if (!sane(manager)) return 0;
        vm_write32(scratch, 0);
        if (ppu_guest_call_ct8(0x00538fa4, 0x01037a88, manager,
                private_key, 0x82, w, h, 1, 0, scratch)) return 0;
        slot.resource = vm_read32(scratch);
        if (!sane(slot.resource) || vm_read32(slot.resource + 8) != private_key) {
            slot.resource = 0;
            return 0;
        }
    }
    std::vector<uint32_t> rendered(w * h);
    if (!title_tex_render_ex(type, selected_title.c_str(), selected_subtitle.c_str(), rendered.data(), w, h, 0)) {
        std::fprintf(stderr, "[custom_titles] cannot rasterize title (font unavailable)\n");
        return 0;
    }
    for (size_t i = 0; i < rendered.size(); ++i)
        vm_write32(staging + uint32_t(i * 4), rendered[i]);
    const uint32_t vtable = vm_read32(slot.resource);
    if (!sane(vtable)) return 0;
    const uint32_t upload = vm_read32(vtable + 0x20);
    if (!sane(upload)) return 0;
    if (ppu_guest_call(upload, slot.resource, staging, 1, 0)) return 0;
    const uint32_t source = vm_read32(slot.resource + 0x34);
    if (!sane(source)) return 0;
    for (uint32_t i = 0; i < w * h * 4; i += 4)
        vm_write32(pixels + i, vm_read32(source + i));
    for (uint32_t i = 0; i < 0x40; i += 4)
        vm_write32(descriptor + i, vm_read32(slot.resource + i));
    vm_write32(descriptor + 0x30, io + pixels - slab);
    vm_write32(descriptor + 0x34, pixels);
    vm_write32(descriptor + 0x20, 0x0000aae4u); // ARGB remap, including alpha
    vm_write32(descriptor + 0x18, 1); // main-memory texture
    vm_write32(descriptor + 8, key);
    PPU_SYNC();
    slot.generation = generation;
    std::fprintf(stderr, "[custom_titles] native type=%u key=%08X %ux%u title=%s\n",
                 type, key, w, h, selected_title.c_str());
    return descriptor;
}
}

void taiko_custom_titles_select(const std::string& title, const std::string& subtitle)
{
    std::lock_guard<std::mutex> lock(mutex);
    selected_title = title;
    selected_subtitle = subtitle;
    ++generation;
}

int taiko_custom_title_lookup_bridge(ppu_context* ctx)
{
    if (fallback_lookup) return 0;
    const uint32_t key = uint32_t(ctx->gpr[4]);
    const unsigned type = key >> 16;
    if ((type != 11 && type != 12) || ((key & 0xffff) != 0 &&
                                      (key & 0xffff) != 6000)) return 0;
    std::lock_guard<std::mutex> lock(mutex);
    if (selected_title.empty()) return 0;
    uint32_t result = resource(type, key);
    if (!result) {
        // As in Zucchini, an allocation/font failure must not feed Green's
        // unconditional material dereference a null pointer. Stock lookups
        // remain untouched, including misses used to trigger streaming.
        fallback_lookup = true;
        const auto lookup = [&](uint32_t stock_key) {
            return uint32_t(ppu_guest_call_ct(0x0054a988, uint32_t(ctx->gpr[2]),
                ctx->gpr[3], stock_key, 0, 0));
        };
        result = lookup(type << 16);
        for (unsigned t = 9; !result && t <= 10; ++t)
            for (unsigned uid = 100; !result && uid < 116; ++uid)
                result = lookup((t << 16) | uid);
        fallback_lookup = false;
        static unsigned failures = 0;
        if (failures++ < 4)
            std::fprintf(stderr, "[custom_titles] native upload failed key=%08X fallback=%08X\n", key, result);
        if (!result) return 0;
    }
    ctx->gpr[3] = result;
    return 1;
}
