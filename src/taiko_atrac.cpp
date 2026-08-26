/* Taiko-local cellAtrac compatibility shim.
 *
 * Green streams its BGM/jingles through cellAtrac.  ps3recomp does not yet
 * register that firmware module, so every import used to return CELL_OK while
 * leaving all output parameters untouched.  That left the decoder thread
 * stuck at its output-ring wait and JINGLE_ENTRY in state 1 forever, keeping
 * Player Entry non-interactive even though drum hits reached InputAnalog.
 *
 * A minimal, statically linked FFmpeg build decodes ATRAC3plus to source-rate
 * PCM. Playback is not delegated to FFmpeg: PCM is delivered only through
 * Taiko's own decoder ring and lifted bnusCore SPU mixer, so the game's voice
 * commands, reset positions, loop points, resampling, and audio clock remain
 * authoritative. The legacy silent path acknowledges three buffers only when
 * that mixer is disabled; with TAIKO_AUDIO_SPU the real consumer exclusively
 * owns the ring counter.
 */

#include "ppu_recomp.h"

#ifdef TAIKO_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ps3emu/host_platform.h>

extern "C" void ps3_frame_boot_fast_finish(void);
extern "C" void cellAudioGameplayDumpStart(void);
extern "C" uint32_t g_taiko_audio_ring_trace_ea;
extern "C" void spu_taiko_audio_ring_register(uint32_t ea);
extern "C" void spu_taiko_audio_ring_unregister(uint32_t ea);

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name,
                                      void (*handler)(ppu_context*));

namespace {

constexpr uint32_t kCellOk = 0;
constexpr uint32_t kWorkMemorySize = 0x1000;
constexpr uint32_t kChannels = 2;
/* ATRAC3plus decodes 2048 samples per frame, and RPCS3's cellAtrac reports
 * that.  It is not a free choice: the guest sizes its decoder ring from
 * cellAtracGetMaxSample.  Verified live -- with 2048 the guest rebuilt its ring
 * to 0x4000 bytes / 0x800 samples per slot, matching RPCS3 exactly, where 512
 * gave 0x1000 / 0x200. */
constexpr uint32_t kMaxSamples = 2048;
constexpr uint32_t kAllDataIsOnMemory = 0xFFFFFFFFu;
constexpr uint32_t kLoopDataIsOnMemory = 0xFFFFFFFDu;

constexpr uint32_t kSetSecondBuffer = 0x06DDB53Eu;
constexpr uint32_t kGetChannel = 0x0F9667B6u;
constexpr uint32_t kCreateDecoderExt = 0x2642D4CCu;
constexpr uint32_t kGetStreamDataInfo = 0x2BFFF084u;
constexpr uint32_t kAddStreamData = 0x46CFC013u;
constexpr uint32_t kGetMaxSample = 0x5F62D546u;
constexpr uint32_t kSetDataAndGetMemSize = 0x66AFC68Eu;
constexpr uint32_t kDeleteDecoder = 0x761CB9BEu;
constexpr uint32_t kResetPlayPosition = 0x7772EB2Bu;
constexpr uint32_t kSetLoopNum = 0x78BA5C41u;
constexpr uint32_t kDecode = 0x8EB0E65Fu;
constexpr uint32_t kIsSecondBufferNeeded = 0x99EFE171u;
constexpr uint32_t kGetBufferInfoForResetting = 0x99FB73D1u;
constexpr uint32_t kGetLoopInfo = 0xAB6B6DBFu;
constexpr uint32_t kGetInternalErrorInfo = 0xB5C11938u;
constexpr uint32_t kGetSecondBufferInfo = 0xBE07F05Eu;
constexpr uint32_t kGetVacantSize = 0xC9A95FCBu;

struct DecoderState {
    /* Decoded PCM is immutable after publication and may be shared by the
     * selection preview and gameplay decoder handles for the same asset. */
    std::shared_ptr<std::vector<float>> pcm =
        std::make_shared<std::vector<float>>();
    size_t decode_cursor = 0; // position reported through cellAtracDecode
    uint32_t sample_rate = 0;
    uint32_t ring_ea = 0;
    size_t gameplay_offset_frames = 0;
    bool gameplay_song = false;
    int32_t loop_num = 0;
    size_t loop_start = 0;
    size_t loop_end = 0;      // exclusive PCM frame
    bool has_loop = false;
    bool loop_num_set = false;
    /* The host decoder resolves a streamed RIFF prefix back to its complete
     * NUB source. The guest must still be allowed to drain its later file-read
     * callbacks, but those bytes do not form our decode input: cellAtrac's
     * input is a frame-aligned circular buffer, not a growing linear file. */
    uint32_t data_ea = 0;
    uint32_t buffer_bytes = 0;
    uint32_t stream_write_offset = 0;
    uint64_t stream_read_position = 0;
    uint64_t ready_host_ns = 0;
    uint64_t decode_work_ns = 0;
    uint64_t first_decode_host_ns = 0;
    size_t first_decode_cursor = 0;
    uint32_t decode_calls = 0;
    bool end_trace_written = false;
    bool first_decode_seen = false;
    bool pcm_cache_hit = false;
};

std::mutex g_decoder_mutex;
std::unordered_map<uint32_t, DecoderState> g_decoders;

uint64_t fnv1a64(uint32_t ea, uint32_t size)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < size; i++) {
        hash ^= vm_read8(ea + i);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint32_t read_le32(uint32_t ea)
{
    return static_cast<uint32_t>(vm_read8(ea + 0)) |
           (static_cast<uint32_t>(vm_read8(ea + 1)) << 8) |
           (static_cast<uint32_t>(vm_read8(ea + 2)) << 16) |
           (static_cast<uint32_t>(vm_read8(ea + 3)) << 24);
}

uint32_t read_le32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void read_riff_loop(uint32_t data, uint32_t bytes, DecoderState& state)
{
    /* ATRAC RIFFs carry sample-accurate loop points in a standard `smpl`
     * chunk. The first loop descriptor starts 36 bytes into its payload; its
     * end sample is inclusive. All looped Green jingles observed so far use a
     * play count of zero, meaning infinite. */
    uint32_t offset = 12;
    while (offset <= bytes && bytes - offset >= 8) {
        const uint32_t chunk = vm_read32(data + offset);
        const uint32_t size = read_le32(data + offset + 4);
        const uint64_t next = static_cast<uint64_t>(offset) + 8u + size + (size & 1u);
        if (chunk == 0x736D706Cu && size >= 60 && offset + 68u <= bytes) {
            const uint32_t loop_count = read_le32(data + offset + 8 + 28);
            const uint32_t start = read_le32(data + offset + 8 + 44);
            const uint32_t end = read_le32(data + offset + 8 + 48);
            const uint32_t play_count = read_le32(data + offset + 8 + 56);
            if (loop_count && end >= start) {
                state.has_loop = true;
                state.loop_start = start;
                state.loop_end = static_cast<size_t>(end) + 1;
                state.loop_num = play_count ? static_cast<int32_t>(play_count) : -1;
            }
            return;
        }
        if (chunk == 0x64617461u || next > bytes)
            return;
        offset = static_cast<uint32_t>(next);
    }
}

#ifdef TAIKO_HAVE_FFMPEG

constexpr size_t kMaxRiffBytes = 256u * 1024u * 1024u;

struct RiffLocation {
    std::string path;
    uint64_t offset = 0;
};

std::mutex g_riff_index_mutex;
std::unordered_map<uint64_t, RiffLocation> g_riff_index;

/* Full decoded songs are tens of MiB each, so an unbounded "cache every song"
 * policy would consume tens of GiB while browsing the catalog. Keep recently
 * used ATRAC assets in a byte-bounded LRU instead. Decoder handles share the
 * immutable vector, avoiding both another FFmpeg pass and a large PCM copy. */
struct PcmCacheEntry {
    uint64_t hash;
    size_t riff_bytes;
    uint32_t sample_rate;
    std::shared_ptr<std::vector<float>> pcm;
};

std::mutex g_pcm_cache_mutex;
std::list<PcmCacheEntry> g_pcm_cache;
size_t g_pcm_cache_bytes = 0;

size_t pcm_cache_limit_bytes()
{
    static const size_t limit = [] {
        const char* text = std::getenv("TAIKO_AUDIO_PCM_CACHE_MB");
        unsigned long mb = text ? std::strtoul(text, nullptr, 0) : 512ul;
        if (mb < 64ul) mb = 64ul;
        if (mb > 8192ul) mb = 8192ul;
        return static_cast<size_t>(mb) * 1024u * 1024u;
    }();
    return limit;
}

uint64_t host_fnv1a64(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool pcm_cache_lookup(uint64_t hash, size_t riff_bytes, DecoderState& state)
{
    std::lock_guard<std::mutex> lock(g_pcm_cache_mutex);
    for (auto it = g_pcm_cache.begin(); it != g_pcm_cache.end(); ++it) {
        if (it->hash != hash || it->riff_bytes != riff_bytes)
            continue;
        state.pcm = it->pcm;
        state.sample_rate = it->sample_rate;
        state.pcm_cache_hit = true;
        g_pcm_cache.splice(g_pcm_cache.begin(), g_pcm_cache, it);
        return true;
    }
    return false;
}

void pcm_cache_insert(uint64_t hash, size_t riff_bytes,
                      uint32_t sample_rate,
                      const std::shared_ptr<std::vector<float>>& pcm)
{
    const size_t bytes = pcm->size() * sizeof(float);
    const size_t limit = pcm_cache_limit_bytes();
    if (bytes > limit) return;

    std::lock_guard<std::mutex> lock(g_pcm_cache_mutex);
    for (auto it = g_pcm_cache.begin(); it != g_pcm_cache.end(); ++it) {
        if (it->hash == hash && it->riff_bytes == riff_bytes) {
            g_pcm_cache_bytes -= it->pcm->size() * sizeof(float);
            g_pcm_cache.erase(it);
            break;
        }
    }
    g_pcm_cache.push_front(PcmCacheEntry{hash, riff_bytes, sample_rate, pcm});
    g_pcm_cache_bytes += bytes;
    while (g_pcm_cache_bytes > limit && g_pcm_cache.size() > 1) {
        const auto& victim = g_pcm_cache.back();
        g_pcm_cache_bytes -= victim.pcm->size() * sizeof(float);
        g_pcm_cache.pop_back();
    }
}

bool read_file_riff(const RiffLocation& location,
                    const std::vector<uint8_t>& prefix,
                    size_t declared_bytes, std::vector<uint8_t>& riff)
{
    FILE* file = std::fopen(location.path.c_str(), "rb");
    if (!file) return false;
    /* NUB scanning only records RIFF offsets from the first 64 KiB, so the
     * portable C seek is sufficient even where long is 32-bit. */
    bool ok = std::fseek(file, static_cast<long>(location.offset), SEEK_SET) == 0;
    std::vector<uint8_t> check(prefix.size());
    if (ok)
        ok = std::fread(check.data(), 1, check.size(), file) == check.size() &&
             check == prefix;
    if (ok)
        ok = std::fseek(file, static_cast<long>(location.offset), SEEK_SET) == 0;
    if (ok) {
        riff.resize(declared_bytes);
        ok = std::fread(riff.data(), 1, riff.size(), file) == riff.size();
    }
    std::fclose(file);
    if (!ok) riff.clear();
    return ok;
}

bool resolve_complete_riff(uint64_t hash, const std::vector<uint8_t>& prefix,
                           std::vector<uint8_t>& riff, std::string& source)
{
    if (prefix.size() < 12 || std::memcmp(prefix.data(), "RIFF", 4) != 0 ||
        std::memcmp(prefix.data() + 8, "WAVE", 4) != 0)
        return false;
    const uint64_t declared64 = static_cast<uint64_t>(read_le32(prefix.data() + 4)) + 8;
    if (declared64 < 12 || declared64 > kMaxRiffBytes)
        return false;
    const size_t declared = static_cast<size_t>(declared64);
    if (declared <= prefix.size()) {
        riff.assign(prefix.begin(), prefix.begin() + declared);
        source = "guest-buffer";
        return true;
    }

    std::lock_guard<std::mutex> index_lock(g_riff_index_mutex);
    if (auto known = g_riff_index.find(hash); known != g_riff_index.end()) {
        if (read_file_riff(known->second, prefix, declared, riff)) {
            source = known->second.path;
            return true;
        }
        g_riff_index.erase(known);
    }

    const char* root = std::getenv("PS3_VFS_ROOT");
    if (!root || !*root) return false;
    const std::filesystem::path directory =
        std::filesystem::path(root) / "data" / "sound" / "bgm" / "nub";
    std::error_code directory_error;
    std::filesystem::directory_iterator entries(directory, directory_error);
    if (directory_error) return false;
    bool matched = false;
    for (const auto& entry : entries) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error ||
            entry.path().extension() != ".nub")
            continue;
        RiffLocation candidate{entry.path().string(), 0};
        FILE* file = std::fopen(candidate.path.c_str(), "rb");
        if (!file) continue;
        std::vector<uint8_t> header(0x10000);
        const size_t header_bytes = std::fread(header.data(), 1, header.size(), file);
        header.resize(header_bytes);
        std::fclose(file);
        auto marker = std::search(header.begin(), header.end(),
                                  prefix.begin(), prefix.begin() + 4);
        while (marker != header.end()) {
            candidate.offset = static_cast<uint64_t>(marker - header.begin());
            if (read_file_riff(candidate, prefix, declared, riff)) {
                g_riff_index[hash] = candidate;
                source = candidate.path;
                matched = true;
                break;
            }
            marker = std::search(marker + 1, header.end(),
                                 prefix.begin(), prefix.begin() + 4);
        }
        if (matched) break;
    }
    return matched;
}

struct MemoryInput {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t position = 0;
};

int read_memory_packet(void* opaque, uint8_t* destination, int requested)
{
    auto& input = *static_cast<MemoryInput*>(opaque);
    const size_t available = input.size - std::min(input.position, input.size);
    const size_t count = std::min<size_t>(available, static_cast<size_t>(requested));
    if (!count) return AVERROR_EOF;
    std::memcpy(destination, input.data + input.position, count);
    input.position += count;
    return static_cast<int>(count);
}

int64_t seek_memory(void* opaque, int64_t offset, int whence)
{
    auto& input = *static_cast<MemoryInput*>(opaque);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(input.size);
    whence &= ~AVSEEK_FORCE;
    int64_t base = 0;
    if (whence == SEEK_CUR) base = static_cast<int64_t>(input.position);
    else if (whence == SEEK_END) base = static_cast<int64_t>(input.size);
    else if (whence != SEEK_SET) return AVERROR(EINVAL);
    if (offset < -base || offset > static_cast<int64_t>(input.size) - base)
        return AVERROR(EINVAL);
    input.position = static_cast<size_t>(base + offset);
    return static_cast<int64_t>(input.position);
}

uint32_t riff_fact_samples(const std::vector<uint8_t>& riff)
{
    size_t offset = 12;
    while (offset + 8 <= riff.size()) {
        const uint32_t size = read_le32(riff.data() + offset + 4);
        if (std::memcmp(riff.data() + offset, "fact", 4) == 0 &&
            size >= 4 && offset + 12 <= riff.size())
            return read_le32(riff.data() + offset + 8);
        if (std::memcmp(riff.data() + offset, "data", 4) == 0) break;
        const uint64_t next = static_cast<uint64_t>(offset) + 8 + size + (size & 1u);
        if (next > riff.size()) break;
        offset = static_cast<size_t>(next);
    }
    return 0;
}

std::string ffmpeg_error(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof text);
    return text;
}

bool decode_riff(const std::vector<uint8_t>& riff, DecoderState& state,
                 std::string& failure)
{
    static std::once_flag log_once;
    std::call_once(log_once, [] { av_log_set_level(AV_LOG_ERROR); });

    MemoryInput input{riff.data(), riff.size(), 0};
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int last_error = 0;

    bool ok = [&]() {
        constexpr int io_buffer_bytes = 32768;
        uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(io_buffer_bytes));
        if (!io_buffer) { failure = "out of memory allocating AVIO"; return false; }
        io = avio_alloc_context(io_buffer, io_buffer_bytes, 0, &input,
                                read_memory_packet, nullptr, seek_memory);
        if (!io) {
            av_free(io_buffer);
            failure = "could not create AVIO context";
            return false;
        }
        format = avformat_alloc_context();
        if (!format) { failure = "could not create format context"; return false; }
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        const AVInputFormat* wav = av_find_input_format("wav");
        last_error = avformat_open_input(&format, nullptr, wav, nullptr);
        if (last_error < 0) { failure = "open WAV: " + ffmpeg_error(last_error); return false; }
        last_error = avformat_find_stream_info(format, nullptr);
        if (last_error < 0) { failure = "read stream info: " + ffmpeg_error(last_error); return false; }
        const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO,
                                               -1, -1, nullptr, 0);
        if (stream < 0) { failure = "no audio stream: " + ffmpeg_error(stream); return false; }
        const AVCodecParameters* parameters = format->streams[stream]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) { failure = "ATRAC3plus decoder is unavailable"; return false; }
        decoder = avcodec_alloc_context3(codec);
        if (!decoder) { failure = "could not create decoder context"; return false; }
        last_error = avcodec_parameters_to_context(decoder, parameters);
        if (last_error < 0) { failure = "copy codec parameters: " + ffmpeg_error(last_error); return false; }
        last_error = avcodec_open2(decoder, codec, nullptr);
        if (last_error < 0) { failure = "open decoder: " + ffmpeg_error(last_error); return false; }
        if (decoder->sample_rate <= 0 || decoder->ch_layout.nb_channels <= 0) {
            failure = "decoder reported an invalid audio layout";
            return false;
        }
        state.sample_rate = static_cast<uint32_t>(decoder->sample_rate);
        last_error = swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT,
                                         decoder->sample_rate, &decoder->ch_layout,
                                         decoder->sample_fmt, decoder->sample_rate,
                                         0, nullptr);
        if (last_error < 0 || !resampler) {
            failure = "create PCM converter: " + ffmpeg_error(last_error);
            return false;
        }
        last_error = swr_init(resampler);
        if (last_error < 0) { failure = "initialize PCM converter: " + ffmpeg_error(last_error); return false; }

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!packet || !frame) { failure = "out of memory allocating decode frames"; return false; }

        auto receive_frames = [&]() {
            for (;;) {
                const int received = avcodec_receive_frame(decoder, frame);
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
                if (received < 0) {
                    failure = "receive ATRAC frame: " + ffmpeg_error(received);
                    return false;
                }
                const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
                if (capacity < 0) {
                    failure = "size converted PCM: " + ffmpeg_error(capacity);
                    return false;
                }
                const size_t old_size = state.pcm->size();
                state.pcm->resize(old_size + static_cast<size_t>(capacity) * kChannels);
                uint8_t* output[] = {
                    reinterpret_cast<uint8_t*>(state.pcm->data() + old_size)
                };
                const int converted = swr_convert(resampler, output, capacity,
                    const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                if (converted < 0) {
                    failure = "convert PCM: " + ffmpeg_error(converted);
                    return false;
                }
                state.pcm->resize(old_size + static_cast<size_t>(converted) * kChannels);
                av_frame_unref(frame);
            }
        };

        while ((last_error = av_read_frame(format, packet)) >= 0) {
            if (packet->stream_index == stream) {
                last_error = avcodec_send_packet(decoder, packet);
                if (last_error < 0) {
                    failure = "submit ATRAC packet: " + ffmpeg_error(last_error);
                    av_packet_unref(packet);
                    return false;
                }
                if (!receive_frames()) { av_packet_unref(packet); return false; }
            }
            av_packet_unref(packet);
        }
        if (last_error != AVERROR_EOF) {
            failure = "read WAV packets: " + ffmpeg_error(last_error);
            return false;
        }
        last_error = avcodec_send_packet(decoder, nullptr);
        if (last_error < 0) { failure = "flush ATRAC decoder: " + ffmpeg_error(last_error); return false; }
        if (!receive_frames()) return false;
        return !state.pcm->empty();
    }();

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resampler);
    avcodec_free_context(&decoder);
    if (format) avformat_close_input(&format);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }

    if (!ok) {
        state.pcm->clear();
        if (failure.empty()) failure = "decoder returned no PCM";
        return false;
    }
    for (float sample : *state.pcm) {
        if (!std::isfinite(sample) || std::abs(sample) > 4.0f) {
            failure = "decoder produced an unsafe PCM sample";
            state.pcm->clear();
            return false;
        }
    }
    const size_t frames = state.pcm->size() / kChannels;
    const uint32_t fact = riff_fact_samples(riff);
    if (fact && (frames < fact || frames > static_cast<size_t>(fact) + 8192)) {
        char detail[128];
        std::snprintf(detail, sizeof detail,
                      "decoded duration %zu does not match RIFF fact %u", frames, fact);
        failure = detail;
        state.pcm->clear();
        return false;
    }
    state.decode_cursor = 0;
    return true;
}

bool decode_guest_riff(uint32_t data, uint32_t bytes, uint64_t hash,
                       DecoderState& state, std::string& source,
                       std::string& failure)
{
    std::vector<uint8_t> prefix(bytes);
    for (uint32_t i = 0; i < bytes; ++i) prefix[i] = vm_read8(data + i);
    std::vector<uint8_t> riff;
    if (!resolve_complete_riff(hash, prefix, riff, source)) {
        failure = "could not resolve the complete RIFF in data/sound/bgm/nub";
        return false;
    }
    const uint64_t pcm_hash = host_fnv1a64(riff);
    if (pcm_cache_lookup(pcm_hash, riff.size(), state))
        return true;
    if (!decode_riff(riff, state, failure))
        return false;
    pcm_cache_insert(pcm_hash, riff.size(), state.sample_rate, state.pcm);
    return true;
}

#else

bool decode_guest_riff(uint32_t, uint32_t, uint64_t, DecoderState&,
                       std::string&, std::string& failure)
{
    failure = "this executable was built without in-process ATRAC support";
    return false;
}

#endif

uint32_t gameplay_audio_offset_ms()
{
    static const uint32_t offset = [] {
        const char* text = std::getenv("TAIKO_AUDIO_OFFSET_MS");
        if (!text || !*text) return 0u;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text, &end, 0);
        if (*text == '-' || end == text || *end != '\0') {
            std::fprintf(stderr,
                "[taiko_atrac-offset] invalid TAIKO_AUDIO_OFFSET_MS='%s'; using 0\n",
                text);
            return 0u;
        }
        return static_cast<uint32_t>(std::min(parsed, 1000ul));
    }();
    return offset;
}

bool is_gameplay_song(const std::string& source, uint32_t initial_bytes)
{
    if (initial_bytes <= 8192u || source == "guest-buffer") return false;
    const std::string filename = std::filesystem::path(source).filename().string();
    return filename.rfind("SONG_", 0) == 0;
}

void return_ok(ppu_context* ctx)
{
    ctx->gpr[3] = kCellOk;
}

void set_data_and_get_mem_size(ppu_context* ctx)
{
    const uint64_t setdata_start_ns = ps3_host_monotonic_ns();
    if (std::getenv("TAIKO_ATRAC_TRACE")) {
        const uint32_t data = static_cast<uint32_t>(ctx->gpr[4]);
        static unsigned calls = 0;
        if (calls++ < 32)
            std::fprintf(stderr,
                "[taiko_atrac] SetData handle=%08X data=%08X read=%u buffer=%u work_out=%08X magic=%08X/%08X\n",
                static_cast<uint32_t>(ctx->gpr[3]), data,
                static_cast<uint32_t>(ctx->gpr[5]),
                static_cast<uint32_t>(ctx->gpr[6]),
                static_cast<uint32_t>(ctx->gpr[7]), data ? vm_read32(data) : 0,
                data ? vm_read32(data + 8) : 0);
    }
    const uint32_t work_size = static_cast<uint32_t>(ctx->gpr[7]);
    if (work_size)
        vm_write32(work_size, kWorkMemorySize);

    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t data = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t bytes = static_cast<uint32_t>(ctx->gpr[5]);
    if (std::getenv("TAIKO_AUDIO_DECODE") && handle && data &&
        bytes >= 12 && bytes < 64u * 1024u * 1024u &&
        vm_read32(data) == 0x52494646u && vm_read32(data + 8) == 0x57415645u) {
        const uint32_t buffer = static_cast<uint32_t>(ctx->gpr[6]);
        /* Hash exactly the bytes known valid at SetData. For streamed songs the
         * in-process decoder uses this prefix to locate the complete RIFF inside
         * the title's NUB directory. Never concatenate later ring-buffer writes:
         * live comparison against RPCS3 proved they are circular and the old
         * linear concatenation skipped 1.59 MiB on SONG_MIKUGV. */
        const uint64_t hash = fnv1a64(data, bytes);
        /* SetData may supply 8 KiB for a preview and hundreds of KiB for the
         * same gameplay RIFF. Key the source-location index by the stable
         * first 4 KiB so the second handle can reuse the known NUB path. */
        const uint64_t source_hash = fnv1a64(data, std::min(bytes, 4096u));
        DecoderState state;
        read_riff_loop(data, bytes, state);
        std::string source;
        std::string failure;
        /* Decode before publishing the handle. This may add setup latency for
         * an uncached song, but it never advances the game with fabricated
         * silence. Once SetData returns, every sample is consumed exclusively
         * on the game's decoder/SPU timeline. */
        const bool ready = decode_guest_riff(data, bytes, source_hash, state,
                                             source, failure);
        if (ready && is_gameplay_song(source, bytes)) {
            state.gameplay_song = true;
        }
        if (ready && state.gameplay_song) {
            cellAudioGameplayDumpStart();
            const uint64_t offset_frames =
                static_cast<uint64_t>(gameplay_audio_offset_ms()) *
                state.sample_rate / 1000u;
            const size_t total_frames = state.pcm->size() / kChannels;
            state.gameplay_offset_frames = static_cast<size_t>(
                std::min<uint64_t>(offset_frames, total_frames));
            state.decode_cursor = state.gameplay_offset_frames;
            std::fprintf(stderr,
                "[taiko_atrac-offset] gameplay source=%s offset=%ums "
                "cursor=%zu rate=%u\n",
                std::filesystem::path(source).filename().string().c_str(),
                gameplay_audio_offset_ms(), state.decode_cursor,
                state.sample_rate);
        }
        const uint64_t ready_host_ns = ps3_host_monotonic_ns();
        state.ready_host_ns = ready_host_ns;
        state.decode_work_ns = ready_host_ns - setdata_start_ns;
        const size_t frames = state.pcm->size() / kChannels;
        const size_t loop_start = state.loop_start;
        const size_t loop_end = state.loop_end;
        const bool cache_hit = state.pcm_cache_hit;
        int32_t loop_num = state.loop_num;
        {
            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            auto previous = g_decoders.find(handle);
            if (previous != g_decoders.end()) {
                if (previous->second.loop_num_set) {
                    state.loop_num = previous->second.loop_num;
                    state.loop_num_set = true;
                }
                spu_taiko_audio_ring_unregister(previous->second.ring_ea);
            }
            state.data_ea = data;
            state.buffer_bytes = buffer;
            state.stream_write_offset = 0;
            state.stream_read_position = bytes;
            loop_num = state.loop_num;
            g_decoders[handle] = std::move(state);
        }
        if (ready) {
            /* First decode is the attract/logo BGM, i.e. the boot state machine
             * is done -- drop the frame driver back to the 60 Hz play rate. */
            ps3_frame_boot_fast_finish();
            std::fprintf(stderr,
                "[taiko_atrac] decoded handle=%08X hash=%016llX frames=%zu "
                "decode=%.2fms cache=%s read=%u buffer=%u loop=%zu..%zu count=%d source=%s\n",
                handle, static_cast<unsigned long long>(hash), frames,
                static_cast<double>(ready_host_ns - setdata_start_ns) / 1000000.0,
                cache_hit ? "hit" : "miss", bytes, buffer,
                loop_start, loop_end, loop_num, source.c_str());
        } else {
            std::fprintf(stderr,
                "[taiko_atrac] decode failed handle=%08X hash=%016llX: %s\n",
                handle, static_cast<unsigned long long>(hash), failure.c_str());
        }
    }
    return_ok(ctx);
}

void get_channel(ppu_context* ctx)
{
    const uint32_t channel = static_cast<uint32_t>(ctx->gpr[4]);
    if (channel)
        vm_write32(channel, kChannels);
    return_ok(ctx);
}

void get_max_sample(ppu_context* ctx)
{
    const uint32_t samples = static_cast<uint32_t>(ctx->gpr[4]);
    if (samples)
        vm_write32(samples, kMaxSamples);
    return_ok(ctx);
}

void get_stream_data_info(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t write_pointer = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t writable_bytes = static_cast<uint32_t>(ctx->gpr[5]);
    const uint32_t read_position = static_cast<uint32_t>(ctx->gpr[6]);

    /* Drain the title's streaming callbacks through its original circular
     * input buffer. The complete source is decoded in-process, so these
     * writes are bookkeeping only; returning one contiguous tail at a time
     * prevents any write from crossing the guest allocation. */
    uint32_t write_ea = 0, writable = 0, read_pos = 0;
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        auto it = g_decoders.find(handle);
        if (it != g_decoders.end() && it->second.buffer_bytes) {
            const DecoderState& s = it->second;
            write_ea = s.data_ea + s.stream_write_offset;
            writable = s.buffer_bytes - s.stream_write_offset;
            read_pos = static_cast<uint32_t>(s.stream_read_position);
        }
    }
    { static std::unordered_map<uint32_t, unsigned> n;
      if (std::getenv("TAIKO_ATRAC_TRACE") && n[handle]++ < 4) {
          std::fprintf(stderr, "[taiko_atrac] GetStreamInfo handle=%08X -> write=%08X writable=%u readpos=%u\n",
                       handle, write_ea, writable, read_pos); } }
    if (write_pointer)
        vm_write32(write_pointer, write_ea ? write_ea : handle);
    if (writable_bytes)
        vm_write32(writable_bytes, writable);
    if (read_position)
        vm_write32(read_position, read_pos);
    return_ok(ctx);
}

/* cellAtracAddStreamData(handle, uiAddByte): acknowledge bytes written to the
 * circular drain buffer. They must not be appended to the decode prefix: the
 * write pointer wraps and the logical file position is not the buffer offset. */
void add_stream_data(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t added = static_cast<uint32_t>(ctx->gpr[4]);
    { static std::unordered_map<uint32_t, unsigned> n;
      if (std::getenv("TAIKO_ATRAC_TRACE") && n[handle]++ < 4) {
          std::fprintf(stderr, "[taiko_atrac] AddStreamData handle=%08X added=%u\n",
                       handle, added); } }
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        auto it = g_decoders.find(handle);
        if (it != g_decoders.end() && added) {
            DecoderState& s = it->second;
            if (s.buffer_bytes) {
                const uint32_t tail = s.buffer_bytes - s.stream_write_offset;
                const uint32_t accepted = std::min(added, tail);
                s.stream_write_offset += accepted;
                if (s.stream_write_offset == s.buffer_bytes)
                    s.stream_write_offset = 0;
                s.stream_read_position += accepted;
            }
        }
    }
    return_ok(ctx);
}

void decode(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t pcm = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t samples = static_cast<uint32_t>(ctx->gpr[5]);
    const uint32_t finished = static_cast<uint32_t>(ctx->gpr[6]);
    const uint32_t remaining_frames = static_cast<uint32_t>(ctx->gpr[7]);
    if (std::getenv("TAIKO_ATRAC_TRACE")) {
        /* Count per handle, not globally: BGM decodes constantly and a shared
         * budget is spent long before a preview handle ever appears. */
        static std::unordered_map<uint32_t, unsigned> calls;
        if (calls[handle]++ < 4)
            std::fprintf(stderr,
                "[taiko_atrac] Decode lr=%08X handle=%08X pcm=%08X samples=%08X finished=%08X remaining=%08X\n",
                static_cast<uint32_t>(ctx->lr), handle, pcm, samples,
                finished, remaining_frames);
    }

    /* bnusCore's decoder owner is immediately before CellAtracHandle and its
     * shared three-slot output ring is owner->decoder at -4.  Decode's caller
     * increments decoder+0x18 after this return, then waits for decoder+0x00
     * to catch up.  The silent compatibility path has no consumer, so it
     * acknowledges that pending buffer.  When the raw SPU mixer is enabled,
     * only the real mixer may advance the consumer counter.  The vtable guard
     * identifies the observed Taiko decoder owner. */
    const char* audio_spu = std::getenv("TAIKO_AUDIO_SPU");
    const bool has_spu_consumer = audio_spu && audio_spu[0] != '0';
    uint32_t ring = 0;
    uint32_t consumer = 0;
    uint32_t produced = 3;
    if (handle >= 8 && vm_read32(handle - 8) == 0x00F9F520u) {
        ring = vm_read32(handle - 4);
        if (ring && ring < 0xF0000000u) {
            consumer = vm_read32(ring);
            produced = vm_read32(ring + 0x18);
            if (!has_spu_consumer)
                vm_write32(ring, produced + 1);
        }
    }

    bool decoded = false;
    uint32_t decoded_samples = 0;
    bool end_of_stream = false;
    bool streaming = false;      // guest has not yet supplied the whole file
    bool loop_data = false;
    uint32_t remain = kAllDataIsOnMemory;
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        auto it = g_decoders.find(handle);
        if (it != g_decoders.end() && ring && it->second.ring_ea != ring) {
            spu_taiko_audio_ring_unregister(it->second.ring_ea);
            it->second.ring_ea = ring;
            spu_taiko_audio_ring_register(ring);
        }
        if (it != g_decoders.end() && it->second.pcm->empty()) {
            /* Decoder failure compatibility path. This is never used while a
             * successful in-process decode is pending: SetData does not return
             * until its PCM is ready, so playback cannot run ahead in silence. */
            if (pcm)
                for (uint32_t i = 0; i < kMaxSamples * kChannels; i++)
                    vm_write32(pcm + i * 4u, 0);
            decoded_samples = kMaxSamples;
            decoded = true;
            end_of_stream = false;
            /* Keep the decoder thread and its three-slot output ring alive. */
            streaming = true;
            remain = 2;
        } else if (it != g_decoders.end()) {
            DecoderState& state = it->second;
            if (!state.first_decode_seen) {
                state.first_decode_seen = true;
                const uint64_t now_ns = ps3_host_monotonic_ns();
                state.first_decode_host_ns = now_ns;
                state.first_decode_cursor = state.decode_cursor;
                if (std::getenv("TAIKO_AUDIO_LATENCY_TRACE"))
                    std::fprintf(stderr,
                        "[taiko_atrac-latency] first-decode handle=%08X "
                        "ready_to_request=%.2fms decode_work=%.2fms cursor=%zu\n",
                        handle,
                        state.ready_host_ns && now_ns >= state.ready_host_ns
                            ? static_cast<double>(now_ns - state.ready_host_ns) / 1000000.0
                            : 0.0,
                        static_cast<double>(state.decode_work_ns) / 1000000.0,
                        state.decode_cursor);
            }
            state.decode_calls++;
            loop_data = state.has_loop;
            const size_t total_frames = state.pcm->size() / kChannels;
            streaming = false;
            const size_t loop_end = state.has_loop
                ? std::min(state.loop_end, total_frames) : total_frames;
            const bool can_loop = state.has_loop && state.loop_num != 0 &&
                state.loop_start < loop_end;
            /* Honor the sample-accurate loop region from the RIFF `smpl`
             * chunk. Looping at total_frames is audibly wrong because FFmpeg's
             * decoded PCM includes codec tail padding after the authored end. */
            if (can_loop && state.decode_cursor >= loop_end) {
                state.decode_cursor = state.loop_start;
                if (state.loop_num > 0) state.loop_num--;
            }
            const size_t decode_end = can_loop ? loop_end : total_frames;
            const size_t available = decode_end -
                std::min(state.decode_cursor, decode_end);
            decoded_samples = static_cast<uint32_t>(
                std::min<size_t>(available, kMaxSamples));
            if (pcm) {
                for (uint32_t i = 0; i < decoded_samples * kChannels; i++) {
                    uint32_t bits;
                    std::memcpy(&bits,
                        &(*state.pcm)[state.decode_cursor * kChannels + i], sizeof(bits));
                    vm_write32(pcm + i * 4u, bits);
                }
                for (uint32_t i = decoded_samples * kChannels;
                     i < kMaxSamples * kChannels; i++)
                    vm_write32(pcm + i * 4u, 0);
            }
            const size_t cursor_before = state.decode_cursor;
            state.decode_cursor += decoded_samples;
            end_of_stream = !can_loop && state.decode_cursor >= total_frames;
            decoded = true;
            if (state.gameplay_song) {
                /* The SPU DMA publisher uses this exact address to distinguish
                 * the gameplay three-slot descriptor from short-effect headers
                 * emitted by the same raw-SPU call site.  Keep it available in
                 * normal runs; the environment variable controls logging only. */
                __atomic_store_n(&g_taiko_audio_ring_trace_ea, ring,
                                 __ATOMIC_RELEASE);
                if (std::getenv("TAIKO_AUDIO_RING_TRACE")) {
                    const uint32_t slot = produced % 3u;
                    std::fprintf(stderr,
                        "[taiko_atrac-ring] t=%llu call=%u handle=%08X "
                        "ring=%08X consumer=%u produced=%u depth=%u slot=%u "
                        "pcm=%08X source=%zu samples=%u\n",
                        static_cast<unsigned long long>(ps3_host_monotonic_ns()),
                        state.decode_calls, handle, ring, consumer, produced,
                        produced - consumer, slot, pcm, cursor_before,
                        decoded_samples);
                }
            }
            if (state.gameplay_song &&
                std::getenv("TAIKO_AUDIO_LATENCY_TRACE")) {
                const uint64_t now_ns = ps3_host_monotonic_ns();
                const uint64_t elapsed_ns = state.first_decode_host_ns &&
                    now_ns >= state.first_decode_host_ns
                    ? now_ns - state.first_decode_host_ns : 0;
                const size_t source_frames = state.decode_cursor -
                    std::min(state.first_decode_cursor, state.decode_cursor);
                if ((state.decode_calls & 255u) == 0 ||
                    (end_of_stream && !state.end_trace_written)) {
                    const double elapsed_s =
                        static_cast<double>(elapsed_ns) / 1000000000.0;
                    std::fprintf(stderr,
                        "[taiko_atrac-clock] handle=%08X calls=%u "
                        "source=%zu/%zu elapsed=%.3fs effective=%.2fHz "
                        "nominal=%.3fs eos=%u\n",
                        handle, state.decode_calls, source_frames,
                        total_frames - state.first_decode_cursor, elapsed_s,
                        elapsed_s > 0.0 ? source_frames / elapsed_s : 0.0,
                        static_cast<double>(source_frames) /
                            std::max(state.sample_rate, 1u),
                        end_of_stream ? 1u : 0u);
                    if (end_of_stream)
                        state.end_trace_written = true;
                }
            }
            if (std::getenv("TAIKO_AUDIO_TRACE")) {
                static unsigned blocks = 0;
                float peak = 0.0f;
                for (uint32_t i = 0; i < decoded_samples * kChannels; i++)
                    peak = std::max(peak, std::abs(
                        (*state.pcm)[(state.decode_cursor - decoded_samples) * kChannels + i]));
                blocks++;
                if (blocks <= 32 || (blocks & 255u) == 0)
                    std::fprintf(stderr,
                        "[taiko_atrac-pcm] handle=%08X samples=%u peak=%g cursor=%zu/%zu\n",
                        handle, decoded_samples, peak, state.decode_cursor, total_frames);
            }
        }
    }

    const bool priming = produced < 3;
    if (!decoded && priming && pcm) {
        /* One maximum-size stereo block. Silence is bitwise zero in IEEE-754. */
        for (uint32_t offset = 0; offset < kMaxSamples * kChannels * 4;
             offset += 4)
            vm_write32(pcm + offset, 0);
    }
    if (samples)
        vm_write32(samples, decoded ? decoded_samples : (priming ? kMaxSamples : 0));
    if (finished)
        vm_write32(finished, decoded ? (end_of_stream ? 1u : 0u)
                                    : (priming ? 0u : 1u));
    /* PCM represents the complete NUB source even if the guest is still
     * draining compressed-data callbacks. */
    if (remaining_frames)
        vm_write32(remaining_frames, streaming ? remain
            : (loop_data ? kLoopDataIsOnMemory : kAllDataIsOnMemory));

    { static std::unordered_map<uint32_t, unsigned> n;
      if (std::getenv("TAIKO_ATRAC_TRACE") && n[handle]++ < 4) {
          std::fprintf(stderr,
              "[taiko_atrac] Decode-> handle=%08X state=%d samples=%u finished=%u remain=%d\n",
              handle, decoded ? 1 : 0,
              decoded ? decoded_samples : (priming ? kMaxSamples : 0u),
              decoded ? (end_of_stream ? 1u : 0u) : (priming ? 0u : 1u),
              streaming ? static_cast<int>(remain) : -1); } }

    return_ok(ctx);
}

void delete_decoder(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    std::lock_guard<std::mutex> lock(g_decoder_mutex);
    auto it = g_decoders.find(handle);
    if (it != g_decoders.end()) {
        spu_taiko_audio_ring_unregister(it->second.ring_ea);
        g_decoders.erase(it);
    }
    return_ok(ctx);
}

void reset_play_position(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t sample = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t write_bytes = static_cast<uint32_t>(ctx->gpr[5]);
    std::lock_guard<std::mutex> lock(g_decoder_mutex);
    auto it = g_decoders.find(handle);
    if (it != g_decoders.end()) {
        /* Green obtains the preview cue from the companion NSH and passes it
        * here as an absolute PCM sample. The old shim discarded uiSample,
         * which made every catalog preview begin at the start of the song. */
        const size_t total_frames = it->second.pcm->size() / kChannels;
        const uint64_t adjusted = static_cast<uint64_t>(sample) +
            (it->second.gameplay_song ? it->second.gameplay_offset_frames : 0u);
        it->second.decode_cursor = static_cast<size_t>(
            std::min<uint64_t>(adjusted, total_frames));
    }
    if (std::getenv("TAIKO_ATRAC_TRACE"))
        std::fprintf(stderr,
            "[taiko_atrac] ResetPlayPosition handle=%08X sample=%u write=%u\n",
            handle, sample, write_bytes);
    return_ok(ctx);
}

void set_loop_num(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const int32_t loops = static_cast<int32_t>(ctx->gpr[4]);
    {
        static unsigned calls = 0;
        if (calls++ < 16 && std::getenv("TAIKO_AUDIO_TRACE"))
            std::fprintf(stderr, "[taiko_atrac] SetLoopNum handle=%08X loops=%d\n",
                         handle, loops);
    }
    std::lock_guard<std::mutex> lock(g_decoder_mutex);
    DecoderState& state = g_decoders[handle];
    state.loop_num = loops;
    state.loop_num_set = true;
    return_ok(ctx);
}

void is_second_buffer_needed(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

void get_second_buffer_info(ppu_context* ctx)
{
    const uint32_t read_position = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t data_bytes = static_cast<uint32_t>(ctx->gpr[5]);
    if (read_position)
        vm_write32(read_position, 0);
    if (data_bytes)
        vm_write32(data_bytes, 0);
    return_ok(ctx);
}

void get_loop_info(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t loop_count = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t loop_status = static_cast<uint32_t>(ctx->gpr[5]);
    int32_t count = 0;
    uint32_t status = 0;
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        auto it = g_decoders.find(handle);
        if (it != g_decoders.end()) {
            count = it->second.loop_num;
            status = it->second.has_loop ? 1u : 0u;
        }
    }
    if (loop_count)
        vm_write32(loop_count, static_cast<uint32_t>(count));
    if (loop_status)
        vm_write32(loop_status, status);
    return_ok(ctx);
}

void get_buffer_info_for_resetting(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t info = static_cast<uint32_t>(ctx->gpr[5]);
    if (info) {
        vm_write32(info + 0x00, handle);
        vm_write32(info + 0x04, 0);
        vm_write32(info + 0x08, 0);
        vm_write32(info + 0x0C, 0);
    }
    return_ok(ctx);
}

void get_internal_error_info(ppu_context* ctx)
{
    const uint32_t result = static_cast<uint32_t>(ctx->gpr[4]);
    if (result)
        vm_write32(result, 0);
    return_ok(ctx);
}

/* The host already owns complete PCM, so the guest compressed-data path is a
 * drain. Keep it writable so CnuSound2 can release every asynchronous read. */
void get_vacant_size(ppu_context* ctx)
{
    const uint32_t handle = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t vacant_size = static_cast<uint32_t>(ctx->gpr[4]);
    uint32_t vacant = 0;
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        auto it = g_decoders.find(handle);
        if (it != g_decoders.end())
            vacant = it->second.buffer_bytes;
    }
    { static std::unordered_map<uint32_t, unsigned> n;
      if (std::getenv("TAIKO_ATRAC_TRACE") && n[handle]++ < 4) {
          std::fprintf(stderr, "[taiko_atrac] GetVacantSize handle=%08X -> %u\n",
                       handle, vacant); } }
    if (vacant_size)
        vm_write32(vacant_size, vacant);
    return_ok(ctx);
}

/* TAIKO_VOICE_WATCH=1: log every state change of an AT3P bnusCore voice.
 * Previews configure a voice and then hand it back, so a static dump only ever
 * catches one edge; this prints the timeline instead. */
void voice_watch_thread()
{
    const uint32_t kMixerCtx = 0x1394140;
    struct Snap { uint32_t f140, f1a8, f1ac, f1b0, f1b4; };
    std::vector<Snap> prev;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        const uint32_t table = vm_read32(kMixerCtx + 0xB0);
        const uint32_t count = vm_read32(kMixerCtx + 0x70);
        if (!table || count == 0 || count > 512)
            continue;
        if (prev.size() != count)
            prev.assign(count, Snap{});
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t v = table + i * 0x310;
            const Snap s{vm_read32(v + 0x140), vm_read32(v + 0x1A8),
                         vm_read32(v + 0x1AC), vm_read32(v + 0x1B0),
                         vm_read32(v + 0x1B4)};
            const bool at3p = s.f1b4 == 0x41543350u || prev[i].f1b4 == 0x41543350u;
            if (at3p && std::memcmp(&s, &prev[i], sizeof s) != 0) {
                std::fprintf(stderr,
                             "[voice] %3u fmt=%c%c%c%c size=%08X 140=%08X 1A8=%08X "
                             "1AC=%08X cmdn=%u\n",
                             i, (char)(s.f1b4 >> 24), (char)(s.f1b4 >> 16),
                             (char)(s.f1b4 >> 8), (char)s.f1b4, s.f1b0, s.f140,
                             s.f1a8, s.f1ac, vm_read32(kMixerCtx + 0xC4));
            }
            prev[i] = s;
        }
    }
}

__attribute__((constructor)) void register_taiko_atrac()
{
    if (std::getenv("TAIKO_VOICE_WATCH"))
        std::thread(voice_watch_thread).detach();
    std::fprintf(stderr,
                 "[taiko_atrac] registering in-process ATRAC3plus decoder\n");
    ps3_hle_register_ctx(kSetSecondBuffer, "cellAtracSetSecondBuffer", return_ok);
    ps3_hle_register_ctx(kGetChannel, "cellAtracGetChannel", get_channel);
    ps3_hle_register_ctx(kCreateDecoderExt, "cellAtracCreateDecoderExt", return_ok);
    ps3_hle_register_ctx(kGetStreamDataInfo, "cellAtracGetStreamDataInfo",
                         get_stream_data_info);
    ps3_hle_register_ctx(kAddStreamData, "cellAtracAddStreamData", add_stream_data);
    ps3_hle_register_ctx(kGetMaxSample, "cellAtracGetMaxSample", get_max_sample);
    ps3_hle_register_ctx(kSetDataAndGetMemSize, "cellAtracSetDataAndGetMemSize",
                         set_data_and_get_mem_size);
    ps3_hle_register_ctx(kDeleteDecoder, "cellAtracDeleteDecoder", delete_decoder);
    ps3_hle_register_ctx(kResetPlayPosition, "cellAtracResetPlayPosition", reset_play_position);
    ps3_hle_register_ctx(kSetLoopNum, "cellAtracSetLoopNum", set_loop_num);
    ps3_hle_register_ctx(kDecode, "cellAtracDecode", decode);
    ps3_hle_register_ctx(kIsSecondBufferNeeded, "cellAtracIsSecondBufferNeeded",
                         is_second_buffer_needed);
    ps3_hle_register_ctx(kGetBufferInfoForResetting,
                         "cellAtracGetBufferInfoForResetting",
                         get_buffer_info_for_resetting);
    ps3_hle_register_ctx(kGetLoopInfo, "cellAtracGetLoopInfo", get_loop_info);
    ps3_hle_register_ctx(kGetInternalErrorInfo, "cellAtracGetInternalErrorInfo",
                         get_internal_error_info);
    ps3_hle_register_ctx(kGetSecondBufferInfo, "cellAtracGetSecondBufferInfo",
                         get_second_buffer_info);
    ps3_hle_register_ctx(kGetVacantSize, "cellAtracGetVacantSize", get_vacant_size);
}

} // namespace
