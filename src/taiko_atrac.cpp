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
#include "taiko_audio_decoder.h"
#include "taiko_audio_offset.h"
#include "taiko_sync_test.h"
#include "taiko_audio_clock.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
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
/* Live calibration must not jump the PCM cursor: that would audibly discard
 * or repeat the requested interval.  Instead, consume at most one percent
 * faster/slower until the saved offset is reached.  A 5 ms key step therefore
 * settles in about half a second without changing the device or guest clocks. */
constexpr double kMaximumOffsetSlew = 0.01;
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
    double decode_fraction = 0.0;
    uint32_t sample_rate = 0;
    uint32_t ring_ea = 0;
    double gameplay_offset_frames = 0.0;
    TaikoAudioClock recovery_clock;
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
    bool async_pending = false;
    bool discard_stream_reads = false;
    bool reset_requested = false;
    uint32_t pending_reset_sample = 0;
    uint64_t generation = 0;
};

std::mutex g_decoder_mutex;
std::unordered_map<uint32_t, DecoderState> g_decoders;
std::atomic<uint64_t> g_decoder_generation{1};

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

bool prepare_guest_riff(uint32_t data, uint32_t bytes, uint64_t hash,
                        DecoderState& state, std::string& source,
                        std::string& failure, std::vector<uint8_t>& riff,
                        uint64_t& pcm_hash)
{
    std::vector<uint8_t> prefix(bytes);
    for (uint32_t i = 0; i < bytes; ++i) prefix[i] = vm_read8(data + i);
    if (!taiko_audio_resolve_riff(hash, prefix, riff, source, failure)) {
        return false;
    }
    pcm_hash = taiko_audio_hash_bytes(riff);
    return true;
}

bool decode_riff(const std::vector<uint8_t>& riff, DecoderState& state,
                 std::string& failure,
                 const std::atomic<bool>* cancelled = nullptr)
{
    TaikoDecodedAudio decoded;
    if (!taiko_audio_decode_riff(riff, 0, cancelled, decoded, failure))
        return false;
    state.pcm = std::move(decoded.pcm);
    state.sample_rate = decoded.sample_rate;
    state.pcm_cache_hit = decoded.cache_hit;
    if (decoded.has_loop) {
        state.has_loop = true;
        state.loop_start = decoded.loop_start;
        state.loop_end = decoded.loop_end;
    }
    state.decode_cursor = 0;
    return true;
}

#ifdef TAIKO_HAVE_FFMPEG

struct PreviewDecodeJob {
    uint32_t handle = 0;
    uint32_t initial_bytes = 0;
    uint32_t buffer_bytes = 0;
    uint64_t generation = 0;
    uint64_t prefix_hash = 0;
    uint64_t pcm_hash = 0;
    uint64_t queued_host_ns = 0;
    std::string source;
    std::vector<uint8_t> riff;
    std::shared_ptr<std::atomic<bool>> cancelled =
        std::make_shared<std::atomic<bool>>(false);
};

class PreviewDecodeWorker {
public:
    PreviewDecodeWorker() : worker_([this] { run(); }) {}

    ~PreviewDecodeWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            if (active_cancelled_)
                active_cancelled_->store(true, std::memory_order_relaxed);
            if (pending_)
                pending_->cancelled->store(true, std::memory_order_relaxed);
        }
        ready_.notify_one();
        worker_.join();
    }

    void enqueue(PreviewDecodeJob job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            /* Song Select has one audible preview. Keep only its newest
             * request, and stop an obsolete full-song decode between packets
             * instead of turning rapid browsing into an unbounded work queue. */
            if (active_cancelled_)
                active_cancelled_->store(true, std::memory_order_relaxed);
            if (pending_)
                pending_->cancelled->store(true, std::memory_order_relaxed);
            pending_ = std::make_unique<PreviewDecodeJob>(std::move(job));
        }
        ready_.notify_one();
    }

private:
    bool current(const PreviewDecodeJob& job)
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        const auto it = g_decoders.find(job.handle);
        return it != g_decoders.end() &&
            it->second.generation == job.generation &&
            it->second.async_pending;
    }

    void run()
    {
        ps3_host_apply_thread_affinity(
            "TAIKO_CPU_PREVIEW_AFFINITY", "ATRAC preview decode");
        for (;;) {
            std::unique_ptr<PreviewDecodeJob> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || pending_; });
                if (stopping_) return;
                job = std::move(pending_);
                active_cancelled_ = job->cancelled;
            }

            if (!current(*job)) {
                finish(job->cancelled);
                continue;
            }

            DecoderState decoded;
            std::string failure;
            const uint64_t decode_start_ns = ps3_host_monotonic_ns();
            const bool ready = decode_riff(job->riff, decoded, failure,
                                           job->cancelled.get());
            const uint64_t ready_host_ns = ps3_host_monotonic_ns();
            if (ready &&
                !job->cancelled->load(std::memory_order_relaxed)) {
                size_t frames = 0;
                const bool cache_hit = decoded.pcm_cache_hit;
                bool published = false;
                {
                    std::lock_guard<std::mutex> lock(g_decoder_mutex);
                    auto it = g_decoders.find(job->handle);
                    if (it != g_decoders.end() &&
                        it->second.generation == job->generation &&
                        it->second.async_pending) {
                        DecoderState& state = it->second;
                        state.pcm = std::move(decoded.pcm);
                        state.sample_rate = decoded.sample_rate;
                        state.pcm_cache_hit = decoded.pcm_cache_hit;
                        if (decoded.has_loop) {
                            state.has_loop = true;
                            state.loop_start = decoded.loop_start;
                            state.loop_end = decoded.loop_end;
                        }
                        frames = state.pcm->size() / kChannels;
                        if (state.reset_requested)
                            state.decode_cursor = std::min<size_t>(
                                state.pending_reset_sample, frames);
                        state.decode_fraction = 0.0;
                        state.async_pending = false;
                        state.ready_host_ns = ready_host_ns;
                        state.decode_work_ns = ready_host_ns - decode_start_ns;
                        published = true;
                    }
                }
                if (published) {
                    std::fprintf(stderr,
                        "[taiko_atrac] async decoded handle=%08X hash=%016llX "
                        "frames=%zu decode=%.2fms cache=%s read=%u buffer=%u source=%s\n",
                        job->handle,
                        static_cast<unsigned long long>(job->prefix_hash),
                        frames,
                        static_cast<double>(ready_host_ns - decode_start_ns) /
                            1000000.0,
                        cache_hit ? "hit" : "miss",
                        job->initial_bytes, job->buffer_bytes,
                        job->source.c_str());
                }
            } else if (failure != "cancelled" && current(*job)) {
                std::fprintf(stderr,
                    "[taiko_atrac] async decode failed handle=%08X hash=%016llX: %s\n",
                    job->handle,
                    static_cast<unsigned long long>(job->prefix_hash),
                    failure.c_str());
            }
            finish(job->cancelled);
        }
    }

    void finish(const std::shared_ptr<std::atomic<bool>>& cancelled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_cancelled_ == cancelled)
            active_cancelled_.reset();
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::unique_ptr<PreviewDecodeJob> pending_;
    std::shared_ptr<std::atomic<bool>> active_cancelled_;
    bool stopping_ = false;
    std::thread worker_;
};

PreviewDecodeWorker& preview_decode_worker()
{
    static PreviewDecodeWorker worker;
    return worker;
}

#else

#endif

uint32_t gameplay_audio_offset_ms()
{
    return static_cast<uint32_t>(taiko_audio_offset_get_ms());
}

bool is_gameplay_song(const std::string& source, uint32_t initial_bytes)
{
    if (initial_bytes <= 8192u || source == "guest-buffer") return false;
    const std::string filename = std::filesystem::path(source).filename().string();
    return filename.rfind("SONG_", 0) == 0;
}

bool is_selection_preview(const std::string& source, uint32_t initial_bytes)
{
    if (initial_bytes > 8192u || source == "guest-buffer") return false;
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
    const char* audio_decode = std::getenv("TAIKO_AUDIO_DECODE");
    if (audio_decode && audio_decode[0] != '0' && handle && data &&
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
        std::vector<uint8_t> riff;
        uint64_t pcm_hash = 0;
        const bool prepared = prepare_guest_riff(
            data, bytes, source_hash, state, source, failure, riff, pcm_hash);
        state.gameplay_song = prepared && is_gameplay_song(source, bytes);
        bool ready = false;
        bool queued = false;
#ifdef TAIKO_HAVE_FFMPEG
        const char* async_previews = std::getenv("TAIKO_AUDIO_ASYNC_PREVIEWS");
        state.discard_stream_reads = prepared && async_previews &&
            async_previews[0] != '0' && is_selection_preview(source, bytes);
        const bool use_async_preview = prepared && !ready &&
            state.discard_stream_reads;
        if (use_async_preview) {
            /* The empty PCM state below intentionally produces temporary
             * silence while the latest preview is decoded. This keeps the
             * guest's decoder/ring alive without blocking its render thread. */
            state.async_pending = true;
            queued = true;
        } else if (prepared && !ready) {
            /* Gameplay and short in-memory jingles remain synchronous: their
             * start time and authored loop state must be ready on return. */
            ready = decode_riff(riff, state, failure);
        }
#endif
        if (ready && state.gameplay_song) {
            taiko_sync_test_start(handle, source.c_str());
            cellAudioGameplayDumpStart();
            const double offset_frames =
                static_cast<double>(gameplay_audio_offset_ms()) *
                state.sample_rate / 1000.0;
            const size_t total_frames = state.pcm->size() / kChannels;
            state.gameplay_offset_frames = std::min(
                offset_frames, static_cast<double>(total_frames));
            state.decode_cursor = static_cast<size_t>(
                state.gameplay_offset_frames);
            state.decode_fraction = state.gameplay_offset_frames -
                static_cast<double>(state.decode_cursor);
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
        const uint64_t generation =
            g_decoder_generation.fetch_add(1, std::memory_order_relaxed);
        state.generation = generation;
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
#ifdef TAIKO_HAVE_FFMPEG
        if (queued) {
            PreviewDecodeJob job;
            job.handle = handle;
            job.initial_bytes = bytes;
            job.buffer_bytes = buffer;
            job.generation = generation;
            job.prefix_hash = hash;
            job.pcm_hash = pcm_hash;
            job.queued_host_ns = ready_host_ns;
            job.source = source;
            job.riff = std::move(riff);
            preview_decode_worker().enqueue(std::move(job));
            std::fprintf(stderr,
                "[taiko_atrac] async queued handle=%08X hash=%016llX "
                "prepare=%.2fms read=%u buffer=%u source=%s\n",
                handle, static_cast<unsigned long long>(hash),
                static_cast<double>(ready_host_ns - setdata_start_ns) /
                    1000000.0,
                bytes, buffer, source.c_str());
        }
#endif
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
        } else if (!queued) {
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
            /* Keep a failed or asynchronously pending decoder alive with
             * silence. For an async preview the host already copied the whole
             * RIFF, so report all data in memory: asking for two more frames
             * made Green redundantly stream the complete NUB while browsing. */
            if (pcm)
                for (uint32_t i = 0; i < kMaxSamples * kChannels; i++)
                    vm_write32(pcm + i * 4u, 0);
            decoded_samples = kMaxSamples;
            decoded = true;
            end_of_stream = false;
            /* Keep the decoder thread and its three-slot output ring alive. */
            streaming = !it->second.async_pending;
            remain = streaming ? 2u : kAllDataIsOnMemory;
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
                state.decode_fraction = 0.0;
                if (state.loop_num > 0) state.loop_num--;
            }
            const size_t decode_end = can_loop ? loop_end : total_frames;
            double position = std::min<double>(
                static_cast<double>(state.decode_cursor) +
                    state.decode_fraction,
                static_cast<double>(decode_end));
            const double uncorrected_position = position;
            static const bool recover_clock = [] {
                const char* value = std::getenv("TAIKO_AUDIO_CLOCK_RECOVERY");
                return value && std::strcmp(value, "1") == 0;
            }();
            if (recover_clock && state.gameplay_song && !can_loop) {
                const bool was_anchored = state.recovery_clock.anchored;
                const double skip = state.recovery_clock.correction(
                    ps3_host_monotonic_ns(), position,
                    state.gameplay_offset_frames, state.sample_rate);
                if (!was_anchored && state.recovery_clock.anchored)
                    std::fprintf(stderr, "[audio-clock] anchored handle=%08X rate=%u\n",
                                 handle, state.sample_rate);
                if (skip > 0) {
                    const double previous = position;
                    position = std::min(position + skip, double(decode_end));
                    std::fprintf(stderr, "[audio-clock] recover handle=%08X skip_ms=%.3f "
                                 "source=%.0f->%.0f offset_ms=%u\n", handle,
                                 (position - previous) * 1000 / state.sample_rate,
                                 previous, position, gameplay_audio_offset_ms());
                }
            }
            double source_step = 1.0;
            if (state.gameplay_song && state.sample_rate) {
                const double target_offset_frames =
                    static_cast<double>(gameplay_audio_offset_ms()) *
                    state.sample_rate / 1000.0;
                const double remaining =
                    target_offset_frames - state.gameplay_offset_frames;
                source_step += std::clamp(
                    remaining / static_cast<double>(kMaxSamples),
                    -kMaximumOffsetSlew, kMaximumOffsetSlew);
            }
            const double available = static_cast<double>(decode_end) - position;
            decoded_samples = static_cast<uint32_t>(std::min<double>(
                std::ceil(available / source_step), kMaxSamples));
            float output_peak = 0.0f;
            if (pcm) {
                for (uint32_t frame = 0; frame < decoded_samples; frame++) {
                    const double source = std::min(
                        position + static_cast<double>(frame) * source_step,
                        static_cast<double>(decode_end - 1u));
                    const size_t first = static_cast<size_t>(source);
                    const size_t second = std::min(first + 1u, decode_end - 1u);
                    const float fraction = static_cast<float>(
                        source - static_cast<double>(first));
                    for (uint32_t channel = 0; channel < kChannels; channel++) {
                        const float a = (*state.pcm)[first * kChannels + channel];
                        const float b = (*state.pcm)[second * kChannels + channel];
                        float sample = a + (b - a) * fraction;
                        // Short crossfade at a recovery seek avoids a hard
                        // waveform discontinuity; never extends the timeline.
                        const unsigned fade_frames = state.sample_rate / 200u;
                        if (position > uncorrected_position && frame < fade_frames) {
                            const size_t old = std::min<size_t>(
                                static_cast<size_t>(uncorrected_position) + frame,
                                decode_end - 1u);
                            const float weight = float(frame + 1u) / fade_frames;
                            sample = (*state.pcm)[old * kChannels + channel] * (1 - weight)
                                   + sample * weight;
                        }
                        uint32_t bits;
                        std::memcpy(&bits, &sample, sizeof(bits));
                        vm_write32(pcm +
                            (frame * kChannels + channel) * 4u, bits);
                        output_peak = std::max(output_peak, std::abs(sample));
                    }
                }
                for (uint32_t i = decoded_samples * kChannels;
                     i < kMaxSamples * kChannels; i++)
                    vm_write32(pcm + i * 4u, 0);
            }
            const size_t cursor_before = state.decode_cursor;
            const double advanced = std::min(
                static_cast<double>(decoded_samples) * source_step, available);
            const double next_position = position + advanced;
            state.decode_cursor = static_cast<size_t>(next_position);
            state.decode_fraction = next_position -
                static_cast<double>(state.decode_cursor);
            if (state.gameplay_song) {
                state.gameplay_offset_frames += advanced - decoded_samples;
            }
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
                blocks++;
                if (blocks <= 32 || (blocks & 255u) == 0)
                    std::fprintf(stderr,
                        "[taiko_atrac-pcm] handle=%08X samples=%u peak=%g cursor=%zu/%zu\n",
                        handle, decoded_samples, output_peak, state.decode_cursor,
                        total_frames);
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
    taiko_sync_test_stop(handle);
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
        if (it->second.gameplay_song) {
            it->second.gameplay_offset_frames =
                static_cast<double>(gameplay_audio_offset_ms()) *
                it->second.sample_rate / 1000.0;
        }
        it->second.pending_reset_sample = sample;
        it->second.reset_requested = true;
        const uint64_t adjusted = static_cast<uint64_t>(sample) +
            static_cast<uint64_t>(it->second.gameplay_song
                ? it->second.gameplay_offset_frames : 0.0);
        if (!it->second.async_pending)
            it->second.decode_cursor = static_cast<size_t>(
                std::min<uint64_t>(adjusted, total_frames));
        it->second.decode_fraction = 0.0;
        it->second.recovery_clock = {};
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

bool discard_preview_stream_read(uint32_t buffer, uint64_t bytes)
{
    if (!buffer || !bytes) return false;
    std::lock_guard<std::mutex> lock(g_decoder_mutex);
    for (const auto& [handle, state] : g_decoders) {
        (void)handle;
        if (!state.discard_stream_reads || !state.data_ea ||
            !state.buffer_bytes)
            continue;
        const uint64_t begin = state.data_ea;
        const uint64_t end = begin + state.buffer_bytes;
        if (buffer >= begin && static_cast<uint64_t>(buffer) + bytes <= end)
            return true;
    }
    return false;
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

extern "C" int taiko_atrac_discard_stream_read(uint32_t buffer, uint64_t bytes)
{
    return discard_preview_stream_read(buffer, bytes) ? 1 : 0;
}
