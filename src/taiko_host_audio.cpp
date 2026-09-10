#include "taiko_host_audio.h"

#include "cellAudio.h"
#include "taiko_audio_decoder.h"
#include "taiko_menu_samples.h"
#include <fstream>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

namespace {

/* The callback's mutable cursors are audio-thread-owned. Control and decode
 * threads publish immutable voices through one atomic exchange. Retired voices
 * go to a lock-free stack and are destroyed by the decoder worker. */
std::atomic<bool> g_installed{false};
std::atomic<float> g_guest_target{1.0f};
std::atomic<float> g_host_target{0.0f};
std::atomic<float> g_group_gain[68]{};
std::atomic<uint64_t> g_preview_generation{0};
std::mutex g_control_lock;
std::string g_preview_music_id;

constexpr uint32_t kOutputRate = 48000;
constexpr uint32_t kRampFrames = 4800; /* 100 ms at cellAudio's 48 kHz. */

struct MenuBank { TaikoMenuSample samples[2]; };
std::atomic<const MenuBank*> g_menu_bank{nullptr};
std::atomic<unsigned> g_sfx_hits[2]{};

void preload_menu_samples()
{
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            const char* root = std::getenv("PS3_VFS_ROOT");
            if (!root || !*root) return;
            std::ifstream file(std::string(root) + "/data/sound/se/SE_COM.nub",
                               std::ios::binary | std::ios::ate);
            if (!file || file.tellg() <= 0 || file.tellg() > 8 * 1024 * 1024) return;
            const size_t size = size_t(file.tellg());
            std::vector<uint8_t> bytes(size);
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return;
            auto bank = std::make_unique<MenuBank>();
            // SE_COM 0/3 are the default Don/Ka waveforms, byte-identical to
            // SE_GAME_NEIRO_000_C 0/1, with the menu volume group (5).
            if (!taiko_decode_menu_sample(bytes, 0, bank->samples[0]) ||
                !taiko_decode_menu_sample(bytes, 3, bank->samples[1])) {
                std::fprintf(stderr, "[taiko_host_audio] invalid SE_COM drum samples\n");
                return;
            }
            g_menu_bank.store(bank.release(), std::memory_order_release);
            std::fprintf(stderr, "[taiko_host_audio] original Don/Ka samples ready\n");
        }).detach();
    });
}

struct PreviewVoice {
    std::shared_ptr<std::vector<float>> pcm;
    size_t cursor = 0;
    float song_gain = 1.0f;
    float volume_gain = 0.0f;
    uint32_t volume_group = 11;
    size_t loop_start = 0;
    size_t loop_end = 0;
    bool has_loop = false;
    uint64_t generation = 0;
    PreviewVoice* retired_next = nullptr;
};

PreviewVoice* make_voice(TaikoDecodedAudio decoded, uint64_t generation)
{
    auto* voice = new PreviewVoice;
    voice->pcm = std::move(decoded.pcm);
    voice->cursor = decoded.preview_start;
    voice->song_gain = decoded.song_gain;
    voice->volume_group = decoded.volume_group;
    voice->loop_start = decoded.loop_start;
    voice->loop_end = decoded.loop_end;
    voice->has_loop = decoded.has_loop;
    voice->generation = generation;
    return voice;
}

std::atomic<PreviewVoice*> g_pending_voice{nullptr};
std::atomic<PreviewVoice*> g_retired_voices{nullptr};

void retire_voice(PreviewVoice* voice)
{
    if (!voice) return;
    PreviewVoice* head = g_retired_voices.load(std::memory_order_relaxed);
    do {
        voice->retired_next = head;
    } while (!g_retired_voices.compare_exchange_weak(
        head, voice, std::memory_order_release, std::memory_order_relaxed));
}

void publish_voice(PreviewVoice* voice)
{
    /* If the audio thread has not consumed the prior publication, ownership
     * returns here and destruction remains off the callback. */
    delete g_pending_voice.exchange(voice, std::memory_order_acq_rel);
}

void reclaim_retired_voices()
{
    PreviewVoice* voice =
        g_retired_voices.exchange(nullptr, std::memory_order_acq_rel);
    while (voice) {
        PreviewVoice* next = voice->retired_next;
        delete voice;
        voice = next;
    }
}

struct PreviewRequest {
    std::string music_id;
    uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> cancelled =
        std::make_shared<std::atomic<bool>>(false);
};

class HostPreviewWorker {
public:
    HostPreviewWorker() : thread_([this] { run(); }) {}

    void enqueue(std::string music_id, uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_cancelled_)
            active_cancelled_->store(true, std::memory_order_relaxed);
        if (pending_)
            pending_->cancelled->store(true, std::memory_order_relaxed);
        pending_.emplace();
        pending_->music_id = std::move(music_id);
        pending_->generation = generation;
        ready_.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            reclaim_retired_voices();
            PreviewRequest request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!pending_)
                    ready_.wait_for(lock, std::chrono::seconds(1),
                                    [this] { return pending_.has_value(); });
                if (!pending_) continue;
                const uint64_t generation = pending_->generation;
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(150);
                if (ready_.wait_until(lock, deadline, [this, generation] {
                        return !pending_ || pending_->generation != generation;
                    }))
                    continue;
                request = std::move(*pending_);
                pending_.reset();
                active_cancelled_ = request.cancelled;
            }

            PreviewVoice* voice = nullptr;
            std::string failure;
            TaikoDecodedAudio decoded;
            bool decoded_ok = request.music_id.empty();
            if (!request.music_id.empty())
                decoded_ok = taiko_audio_decode_song(
                    request.music_id, kOutputRate, request.cancelled.get(),
                    decoded, failure);
            if (decoded_ok &&
                !request.cancelled->load(std::memory_order_relaxed) &&
                request.generation ==
                    g_preview_generation.load(std::memory_order_acquire)) {
                std::fprintf(stderr,
                    "[taiko_host_audio] preview %s cue=%.3fs song_gain=%.4f group=%u cache=%u\n",
                    request.music_id.c_str(), double(decoded.preview_start) / kOutputRate,
                    decoded.song_gain, decoded.volume_group, unsigned(decoded.cache_hit));
                voice = make_voice(std::move(decoded), request.generation);
                publish_voice(voice);
            } else if (!failure.empty() && failure != "cancelled" &&
                       request.generation ==
                           g_preview_generation.load(std::memory_order_acquire)) {
                if (reported_failures_.insert(request.music_id).second)
                    std::fprintf(stderr,
                        "[taiko_host_audio] preview %s unavailable: %s\n",
                        request.music_id.c_str(), failure.c_str());
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (active_cancelled_ == request.cancelled)
                    active_cancelled_.reset();
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<PreviewRequest> pending_;
    std::shared_ptr<std::atomic<bool>> active_cancelled_;
    std::unordered_set<std::string> reported_failures_;
    std::thread thread_;
};

HostPreviewWorker& preview_worker()
{
    /* Audio owns callback-local voice pointers until process shutdown. Avoid a
     * static-destruction race by giving this process-lifetime worker the same
     * lifetime as cellAudio's external mixer registration. */
    static HostPreviewWorker* worker = new HostPreviewWorker;
    return *worker;
}

void ramp(float& value, float target)
{
    const float step = 1.0f / static_cast<float>(kRampFrames);
    if (value < target) value = std::min(target, value + step);
    else if (value > target) value = std::max(target, value - step);
}

void external_mix(float* stereo, u32 frames)
{
    static float guest_gain = 1.0f;
    static float host_gain = 0.0f;
    static PreviewVoice* current = nullptr;
    static PreviewVoice* previous = nullptr;
    static uint32_t crossfade_left = 0;
    if (!stereo) return;
    struct HitVoice { const TaikoMenuSample* sample = nullptr; size_t cursor = 0; };
    static HitVoice hits[16];
    static unsigned next_hit = 0;
    const MenuBank* bank = g_menu_bank.load(std::memory_order_acquire);
    for (unsigned sound = 0; sound < 2; ++sound) {
        unsigned count = g_sfx_hits[sound].exchange(0, std::memory_order_acq_rel);
        if (bank && g_host_target.load(std::memory_order_relaxed) > 0.0f)
            for (unsigned n = 0; n < std::min(count, 8u); ++n)
                hits[next_hit++ % 16] = {&bank->samples[sound], 0};
    }
    const float guest_target =
        g_guest_target.load(std::memory_order_relaxed);
    const float host_target =
        g_host_target.load(std::memory_order_relaxed);
    for (u32 frame = 0; frame < frames; ++frame) {
        if (PreviewVoice* incoming =
                g_pending_voice.exchange(nullptr, std::memory_order_acq_rel)) {
            if (incoming->generation <
                g_preview_generation.load(std::memory_order_acquire)) {
                retire_voice(incoming);
            } else {
                retire_voice(previous);
                previous = current;
                current = incoming;
                crossfade_left = kRampFrames;
            }
        }
        ramp(guest_gain, guest_target);
        ramp(host_gain, host_target);
        float host_left = 0.0f;
        float host_right = 0.0f;
        const auto mix_voice = [](PreviewVoice*& voice, float gain,
                                  float& left, float& right) {
            if (!voice || !voice->pcm || voice->pcm->empty()) return;
            const size_t frame_count = voice->pcm->size() / 2u;
            if (voice->cursor >= frame_count) {
                if (voice->has_loop && voice->loop_end > voice->loop_start &&
                    voice->loop_end <= frame_count)
                    voice->cursor = voice->loop_start;
                else {
                    retire_voice(voice);
                    voice = nullptr;
                    return;
                }
            }
            ramp(voice->volume_gain, std::min(1.0f, voice->song_gain *
                g_group_gain[voice->volume_group].load(std::memory_order_relaxed)));
            gain *= voice->volume_gain;
            left += (*voice->pcm)[voice->cursor * 2u] * gain;
            right += (*voice->pcm)[voice->cursor * 2u + 1u] * gain;
            ++voice->cursor;
            if (voice->has_loop && voice->cursor >= voice->loop_end)
                voice->cursor = voice->loop_start;
        };
        if (crossfade_left) {
            const float new_gain = 1.0f -
                static_cast<float>(crossfade_left) /
                    static_cast<float>(kRampFrames);
            mix_voice(previous, 1.0f - new_gain, host_left, host_right);
            mix_voice(current, new_gain, host_left, host_right);
            if (!--crossfade_left) {
                retire_voice(previous);
                previous = nullptr;
            }
        } else {
            mix_voice(current, 1.0f, host_left, host_right);
        }
        float drum = 0.0f;
        for (auto& hit : hits) {
            if (!hit.sample) continue;
            drum += hit.sample->pcm[hit.cursor++] * hit.sample->gain *
                g_group_gain[hit.sample->group].load(std::memory_order_relaxed);
            if (hit.cursor == hit.sample->pcm.size()) hit.sample = nullptr;
        }
        host_left += drum;
        host_right += drum;
        stereo[frame * 2u] = std::clamp(
            stereo[frame * 2u] * guest_gain + host_left * host_gain,
            -1.0f, 1.0f);
        stereo[frame * 2u + 1u] = std::clamp(
            stereo[frame * 2u + 1u] * guest_gain + host_right * host_gain,
            -1.0f, 1.0f);
    }
    if (host_target == 0.0f && host_gain == 0.0f) {
        retire_voice(previous);
        retire_voice(current);
        previous = nullptr;
        current = nullptr;
        crossfade_left = 0;
        for (auto& hit : hits) hit.sample = nullptr;
    }
}

} // namespace

void taiko_host_audio_set_group_gain(uint32_t group, float gain)
{
    if (group < 68)
        g_group_gain[group].store(std::isfinite(gain) && gain >= 0.0f ? gain : 0.0f,
                                 std::memory_order_relaxed);
}

void taiko_host_audio_install()
{
    if (!g_installed.exchange(true, std::memory_order_acq_rel)) {
        cellAudioSetExternalMixer(external_mix);
        std::fprintf(stderr,
                     "[taiko_host_audio] process-lifetime mixer installed\n");
    }
}

/* Install before guest or SDL audio threads can start. Scene changes only
 * publish atomic gain targets; they never replace the callback. */
__attribute__((constructor))
static void taiko_host_audio_register_process_mixer()
{
    taiko_host_audio_install();
}

void taiko_host_audio_set_scene_active(bool active)
{
    taiko_host_audio_install();
    if (active) preload_menu_samples();
    g_guest_target.store(active ? 0.0f : 1.0f,
                         std::memory_order_release);
    g_host_target.store(active ? 1.0f : 0.0f,
                        std::memory_order_release);
}

void taiko_host_audio_select_preview(std::string_view music_id,
                                     uint64_t generation)
{
    std::lock_guard<std::mutex> guard(g_control_lock);
    if (generation < g_preview_generation.load(std::memory_order_acquire))
        return;
    g_preview_music_id.assign(music_id);
    g_preview_generation.store(generation, std::memory_order_release);
    preview_worker().enqueue(g_preview_music_id, generation);
}

void taiko_host_audio_play_sfx(TaikoPlusSfx sound)
{
    if (g_host_target.load(std::memory_order_relaxed) == 0.0f ||
        !g_menu_bank.load(std::memory_order_acquire)) return;
    const unsigned index = (sound == TaikoPlusSfx::Move ||
                            sound == TaikoPlusSfx::Difficulty) ? 1 : 0;
    // Bounded pending hits; multiple input producers never touch voice cursors.
    unsigned count = g_sfx_hits[index].load(std::memory_order_relaxed);
    while (count < 8 && !g_sfx_hits[index].compare_exchange_weak(
        count, count + 1, std::memory_order_release, std::memory_order_relaxed)) {}

}

void taiko_host_audio_begin_gameplay_handoff()
{
    g_host_target.store(0.0f, std::memory_order_release);
    g_guest_target.store(1.0f, std::memory_order_release);
}

void taiko_host_audio_reacquire_menu()
{
    taiko_host_audio_set_scene_active(true);
    std::lock_guard<std::mutex> guard(g_control_lock);
    preview_worker().enqueue(
        g_preview_music_id,
        g_preview_generation.load(std::memory_order_acquire));
}

#ifdef TAIKO_HOST_AUDIO_TESTING
void taiko_host_audio_test_install_drums(const float* mono, size_t frames)
{
    auto* bank = new MenuBank;
    for (auto& sample : bank->samples)
        sample.pcm.assign(mono, mono + frames);
    g_menu_bank.store(bank, std::memory_order_release);
}

void taiko_host_audio_test_publish_pcm(const float* stereo, size_t frames,
                                       bool loop, uint64_t generation,
                                       size_t preview_start, float song_gain)
{
    TaikoDecodedAudio decoded;
    decoded.pcm = std::make_shared<std::vector<float>>(
        stereo, stereo + frames * 2u);
    decoded.preview_start = preview_start;
    decoded.song_gain = song_gain;
    decoded.loop_end = frames;
    decoded.has_loop = loop && frames != 0;
    auto* voice = make_voice(std::move(decoded), generation);
    uint64_t current = g_preview_generation.load(std::memory_order_relaxed);
    while (generation > current &&
           !g_preview_generation.compare_exchange_weak(
               current, generation, std::memory_order_release,
               std::memory_order_relaxed)) {}
    publish_voice(voice);
}
#endif
