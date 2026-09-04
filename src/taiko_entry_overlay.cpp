/* Runtime VFS overlay for the Player Entry TAIKO+ Lumen patch.
 *
 * The user's packeddata.ddp is never modified.  On the exact supported Green
 * archive, apply the embedded source-copy/data patch into host memory and hand
 * cellFs a delete-on-close temporary stream containing the result.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

extern "C" const unsigned char taiko_entry_overlay_patch_data[];
extern "C" const unsigned taiko_entry_overlay_patch_size;

namespace {

constexpr char kMagic[] = "TKOVLY01";
constexpr uint8_t kCopy = 0;
constexpr uint8_t kData = 1;
constexpr uint32_t kCellFsAccessMask = 3;
constexpr uint32_t kCellFsReadOnly = 0;

std::once_flag g_prepare_once;
std::vector<uint8_t> g_overlay;

uint32_t crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool read_u32(const uint8_t*& cursor, const uint8_t* end, uint32_t& value)
{
    if (static_cast<size_t>(end - cursor) < 4) return false;
    value = static_cast<uint32_t>(cursor[0]) |
            (static_cast<uint32_t>(cursor[1]) << 8) |
            (static_cast<uint32_t>(cursor[2]) << 16) |
            (static_cast<uint32_t>(cursor[3]) << 24);
    cursor += 4;
    return true;
}

bool read_u64(const uint8_t*& cursor, const uint8_t* end, uint64_t& value)
{
    if (static_cast<size_t>(end - cursor) < 8) return false;
    value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(cursor[i]) << (i * 8);
    cursor += 8;
    return true;
}

bool load_file(const char* path, std::vector<uint8_t>& output)
{
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    output.resize(static_cast<size_t>(length));
    const bool ok = output.empty() ||
        std::fread(output.data(), 1, output.size(), file) == output.size();
    std::fclose(file);
    if (!ok) output.clear();
    return ok;
}

bool apply_patch(const std::vector<uint8_t>& source)
{
    const uint8_t* cursor = taiko_entry_overlay_patch_data;
    const uint8_t* end = cursor + taiko_entry_overlay_patch_size;
    if (static_cast<size_t>(end - cursor) < sizeof(kMagic) - 1 ||
        std::memcmp(cursor, kMagic, sizeof(kMagic) - 1) != 0)
        return false;
    cursor += sizeof(kMagic) - 1;

    uint64_t source_size = 0;
    uint64_t target_size = 0;
    uint32_t source_crc = 0;
    uint32_t target_crc = 0;
    uint32_t command_count = 0;
    if (!read_u64(cursor, end, source_size) ||
        !read_u64(cursor, end, target_size) ||
        !read_u32(cursor, end, source_crc) ||
        !read_u32(cursor, end, target_crc) ||
        !read_u32(cursor, end, command_count))
        return false;

    const uint32_t actual_crc = crc32(source.data(), source.size());
    if (source.size() == target_size && actual_crc == target_crc) {
        std::fprintf(stderr,
                     "[taiko_entry_overlay] archive is already patched; "
                     "using it unchanged\n");
        return false;
    }
    if (source.size() != source_size || actual_crc != source_crc) {
        std::fprintf(stderr,
                     "[taiko_entry_overlay] unsupported entry archive "
                     "size=%zu crc=%08X (expected size=%llu crc=%08X); "
                     "using original\n",
                     source.size(), actual_crc,
                     static_cast<unsigned long long>(source_size), source_crc);
        return false;
    }

    std::vector<uint8_t> target;
    target.reserve(static_cast<size_t>(target_size));
    for (uint32_t command = 0; command < command_count; ++command) {
        if (cursor == end) return false;
        const uint8_t kind = *cursor++;
        uint64_t length = 0;
        if (!read_u64(cursor, end, length)) return false;
        if (target.size() > target_size ||
            length > target_size - target.size())
            return false;
        if (kind == kCopy) {
            uint64_t offset = 0;
            if (!read_u64(cursor, end, offset) ||
                offset > source.size() || length > source.size() - offset)
                return false;
            target.insert(target.end(), source.begin() + offset,
                          source.begin() + offset + length);
        } else if (kind == kData) {
            if (length > static_cast<uint64_t>(end - cursor)) return false;
            target.insert(target.end(), cursor, cursor + length);
            cursor += length;
        } else {
            return false;
        }
    }
    if (cursor != end || target.size() != target_size ||
        crc32(target.data(), target.size()) != target_crc)
        return false;

    g_overlay = std::move(target);
    std::fprintf(stderr,
                 "[taiko_entry_overlay] activated in-memory Player Entry "
                 "overlay (%zu -> %zu bytes, source crc=%08X)\n",
                 source.size(), g_overlay.size(), source_crc);
    return true;
}

bool is_entry_archive(const char* path)
{
    static constexpr char suffix[] =
        "/data/lumendata/packed/entry/packeddata.ddp";
    const size_t path_length = std::strlen(path);
    const size_t suffix_length = sizeof(suffix) - 1;
    if (path_length < suffix_length) return false;
    const char* tail = path + path_length - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        const char actual = tail[i] == '\\' ? '/' : tail[i];
        if (actual != suffix[i]) return false;
    }
    return true;
}

bool overlay_enabled()
{
    const char* setting = std::getenv("TAIKO_ENTRY_PC_MODE_OVERLAY");
    return !setting || std::strcmp(setting, "0") != 0;
}

void prepare_overlay(const char* host_path)
{
    std::call_once(g_prepare_once, [host_path] {
        std::vector<uint8_t> source;
        if (!load_file(host_path, source)) {
            std::fprintf(stderr,
                         "[taiko_entry_overlay] could not read '%s'; using "
                         "original cellFs path\n", host_path);
            return;
        }
        if (!apply_patch(source) && !g_overlay.empty()) g_overlay.clear();
    });
}

} // namespace

extern "C" int taiko_fs_stat_overlay(const char* guest_path,
                                      const char* host_path,
                                      uint64_t* size)
{
    if (!guest_path || !host_path || !size ||
        !is_entry_archive(guest_path) || !overlay_enabled())
        return 0;
    prepare_overlay(host_path);
    if (g_overlay.empty()) return 0;
    *size = g_overlay.size();
    return 1;
}

extern "C" FILE* taiko_fs_open_overlay(const char* guest_path,
                                        const char* host_path,
                                        uint32_t flags)
{
    if (!guest_path || !host_path || !is_entry_archive(guest_path) ||
        (flags & kCellFsAccessMask) != kCellFsReadOnly)
        return nullptr;
    if (!overlay_enabled()) return nullptr;
    prepare_overlay(host_path);
    if (g_overlay.empty()) return nullptr;

    FILE* stream = std::tmpfile();
    if (!stream || std::fwrite(g_overlay.data(), 1, g_overlay.size(), stream) !=
                       g_overlay.size() ||
        std::fflush(stream) != 0 || std::fseek(stream, 0, SEEK_SET) != 0) {
        if (stream) std::fclose(stream);
        std::fprintf(stderr,
                     "[taiko_entry_overlay] temporary stream creation failed; "
                     "using original archive\n");
        return nullptr;
    }
    return stream;
}
