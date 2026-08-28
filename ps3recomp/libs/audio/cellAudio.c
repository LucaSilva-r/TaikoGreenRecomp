/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Guest ring handling, scheduling, diagnostics, and host output are kept
 * separate. The selected host sink only accepts complete stereo float blocks.
 */

#include "cellAudio.h"
#include "audio_sink.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <math.h>
#include <ps3emu/host_platform.h>

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern int      sys_event_queue_depth_by_id(uint32_t queue_id);
extern uint64_t ppu_timebase_usec_now(void);

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

/* Keep HLE-owned audio buffers out of the title's 256 MB main-memory image.
 * The old 0x01000000 bump pointer overlapped perfectly valid PT_LOAD data in
 * larger executables (Taiko's .opd reaches 0x0101FC58), so PortOpen erased
 * function descriptors before their worker threads used them. The runtime's
 * general sys_memory arena ends at 0x50000000; reserve the following 16 MB as
 * eight stable, reusable 2 MB audio-port slots. The flat VM commits it on
 * demand and the guest still receives ordinary 1 MB-aligned EAs. */
#define AUDIO_GUEST_BASE        0x50000000u
#define AUDIO_GUEST_PORT_STRIDE 0x00200000u
#define AUDIO_GUEST_RIDX_OFFSET 0x00100000u

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  write_index;    /* where the game is writing */
    u64                  timestamp_usec; /* time of the latest consumed tag */
    u64                  consumed_hash[CELL_AUDIO_BLOCK_32];
    u8                   consumed_valid[CELL_AUDIO_BLOCK_32];
    u32                  stale_blocks;
    u32                  raced_blocks;
    u32                  unfilled_blocks;
    /* Ring positions notified but not yet handed to the guest. Published from
     * sys_event_queue_receive so the guest reads the index belonging to its own
     * notification; see cellAudioNotifyDelivered. */
    u32                  pending_index[CELL_AUDIO_BLOCK_32];
    u32                  pending_head, pending_tail, pending_count;
    u32                  pending_dropped;
    u8                   ever_filled;
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Mixing thread */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
static int           s_mix_thread_started = 0;
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];
static CellAudioExternalMixer s_external_mixer = NULL;
static int s_null_audio_clock = 0;
/* How many blocks ahead of the block being played the guest is told to write.
 * cellAudio's contract publishes the block that is about to be consumed, which
 * leaves the producer exactly one 5.33 ms period; that is fine on a PS3 and far
 * too tight for a loaded host. The port ring has 8-32 blocks, so spending a few
 * of them as slack costs only output latency.
 *
 * Four measured clean where two did not: on a contended desktop, two left 2.1%
 * of blocks unfilled and several ATRAC slot discards per song, four left zero
 * of both. It also closes the mixer's slot-discard window -- the producer now
 * publishes far enough ahead that the SPU never advances onto a slot whose
 * consumed-counter has not been reset. Clamped to nBlock/2. */
static u32 s_read_index_lookahead = 4;
static int s_sink_initialized = 0;
static atomic_ullong s_notify_drops;
/* Notifications pushed while the previous one was still unserviced. Each of
 * those means the guest will drain a burst against a single readIndexAddr
 * value and overwrite blocks it was told to fill. */
static atomic_ullong s_notify_backlog_events;
static atomic_int    s_notify_backlog_max;
/* 0 = live/not started, 1 = shutdown owner active, 2 = stopped. */
static atomic_int s_host_output_shutdown = 0;

static u64 audio_guest_block_hash(u32 ea, u32 words, int* nonzero)
{
    u64 hash = 1469598103934665603ull;
    int any = 0;
    for (u32 i = 0; i < words; i++) {
        u32 value = vm_read32(ea + i * 4u);
        hash ^= value;
        hash *= 1099511628211ull;
        any |= (value & 0x7fffffffu) != 0;
    }
    if (nonzero) *nonzero = any;
    return hash;
}

/* RPCS3 tags six evenly distributed words with negative zero after consuming
 * a guest port block. Green's bnusCore producer copies every word of the next
 * 0x2000-byte block, so all six tags changing proves that the event associated
 * with readIndexAddr was handled before that mutable index advances again.
 * Surviving tags are therefore a direct, non-blocking count of missed producer
 * deadlines ([cellAudio-sink] UNFILLED).
 *
 * This is DIAGNOSTIC ONLY and off unless TAIKO_AUDIO_RING_TRACE=1. Detection
 * requires zeroing the retired block, which converts a missed refill from a
 * repeat of the previous revolution into digital silence -- two clicks instead
 * of a soft glitch. That is the right trade when measuring and the wrong one
 * when playing, so the default path leaves guest audio untouched. */
#define AUDIO_WRITE_TAG_COUNT 6u
#define AUDIO_WRITE_TAG       0x80000000u

static void audio_tag_consumed_block(const AudioPortSlot* port, u32 block_idx)
{
    const u32 nch = (u32)port->param.nChannel;
    const u32 words = CELL_AUDIO_BLOCK_SAMPLES * nch;
    const u32 last = words - 1u;
    const u32 delta = last / (AUDIO_WRITE_TAG_COUNT - 1u);
    u32 pos = last % (AUDIO_WRITE_TAG_COUNT - 1u);
    const u32 ea = (u32)port->port_addr + block_idx * words * 4u;

    /* Match cellAudio's consumed-buffer reset. This also makes a missed guest
     * fill become silence rather than replaying unrelated PCM from one ring
     * revolution ago. */
    memset(vm_base + ea, 0, words * 4u);
    for (u32 i = 0; i < AUDIO_WRITE_TAG_COUNT; ++i, pos += delta)
        vm_write32(ea + pos * 4u, AUDIO_WRITE_TAG);
}

static int audio_guest_block_written(const AudioPortSlot* port, u32 block_idx)
{
    const u32 nch = (u32)port->param.nChannel;
    const u32 words = CELL_AUDIO_BLOCK_SAMPLES * nch;
    const u32 last = words - 1u;
    const u32 delta = last / (AUDIO_WRITE_TAG_COUNT - 1u);
    u32 pos = last % (AUDIO_WRITE_TAG_COUNT - 1u);
    const u32 ea = (u32)port->port_addr + block_idx * words * 4u;

    for (u32 i = 0; i < AUDIO_WRITE_TAG_COUNT; ++i, pos += delta) {
        if (vm_read32(ea + pos * 4u) == AUDIO_WRITE_TAG)
            return 0;
    }
    return 1;
}

/* Called from sys_event_queue_receive as an event is delivered to the guest.
 * If it is one of our notifications, publish the ring position that belongs to
 * it. The guest mixer reads readIndexAddr immediately after this returns, so it
 * observes exactly its own block and can never skip one. */
void cellAudioNotifyDelivered(u64 source)
{
    int match = 0;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++)
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == source) {
            match = 1; break;
        }
    if (!match) return;

    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->read_idx_addr) continue;
        if (port->pending_count == 0) continue;
        const u32 nblock = (u32)port->param.nBlock;
        const u32 idx = port->pending_index[port->pending_head];
        port->pending_head = (port->pending_head + 1u) % nblock;
        port->pending_count--;
        vm_write64((u32)port->read_idx_addr, idx);
    }
    mutex_unlock(&s_audio_mutex);
}

void cellAudioSetExternalMixer(CellAudioExternalMixer mixer)
{
    s_external_mixer = mixer;
}

/* Backend-neutral capture of exactly the complete blocks accepted by a sink. */
static FILE* s_audio_dump = NULL;
static uint32_t s_audio_dump_frames = 0;
static uint32_t s_audio_dump_limit = 0;
static int s_audio_dump_state = 0; /* 0 armed, 1 writing, 2 done */
static atomic_int s_audio_gameplay_dump_started;
/* Host time at which the title's gameplay song became decodable, and whether
 * the first audible block after that has been reported. The gap between the
 * two is this stack's audio start latency: everything between the guest being
 * able to fetch PCM and that PCM leaving for the device. The chart runs on a
 * free-running wall clock with no feedback from audio, so this offset is
 * applied once at song start and never corrected. */
static atomic_ullong s_gameplay_arm_ns;
static atomic_int s_gameplay_audible_reported;
/* Blocks submitted since the gameplay capture armed. The WAV dump starts at
 * the same instant, so this is the exact position in that file: every log
 * event can be placed on the waveform rather than merely correlated with it. */
static atomic_ullong s_blocks_since_arm;

/* Seconds into the gameplay WAV capture, or -1 while nothing is being written.
 * The dump does not open until the first non-silent block, so this must count
 * frames actually in the file -- counting from the arm instant would place
 * every event earlier than its true position on the waveform. */
static double audio_capture_seconds(void)
{
    if (s_audio_dump_state != 1) return -1.0;
    return (double)s_audio_dump_frames / (double)CELL_AUDIO_SAMPLE_RATE;
}


void cellAudioGameplayDumpStart(void)
{
    if (!atomic_exchange_explicit(&s_audio_gameplay_dump_started, 1,
                                  memory_order_release)) {
        atomic_store_explicit(&s_gameplay_arm_ns, ps3_host_monotonic_ns(),
                              memory_order_release);
        atomic_store_explicit(&s_gameplay_audible_reported, 0,
                              memory_order_release);
        atomic_store_explicit(&s_blocks_since_arm, 0, memory_order_release);
        fprintf(stderr, "[cellAudio-start] gameplay audio armed\n");
    }
}

/* Report the first non-silent block submitted after arming. */
static void audio_report_first_audible(const float* samples, uint32_t frames)
{
    if (atomic_load_explicit(&s_gameplay_audible_reported,
                             memory_order_acquire)) return;
    const unsigned long long arm =
        atomic_load_explicit(&s_gameplay_arm_ns, memory_order_acquire);
    if (!arm) return;

    float peak = 0.0f;
    for (uint32_t i = 0; i < frames * 2u; ++i) {
        const float a = fabsf(samples[i]);
        if (a > peak) peak = a;
    }
    if (peak < 0.0005f) return;

    atomic_store_explicit(&s_gameplay_audible_reported, 1,
                          memory_order_release);
    const uint32_t queued = s_null_audio_clock ? 0 : audio_sink_queued_frames();
    const uint32_t device = s_null_audio_clock
        ? 0 : audio_sink_device_buffer_frames();
    fprintf(stderr,
            "[cellAudio-start] first audible block %.2f ms after arm "
            "(peak=%g queued=%u frames device=%u frames, +%.2f ms still to "
            "play out)\n",
            (double)(ps3_host_monotonic_ns() - arm) / 1000000.0, peak,
            queued, device,
            1000.0 * (double)(queued + device) / CELL_AUDIO_SAMPLE_RATE);
}

static void audio_dump_u16(uint16_t value)
{
    fwrite(&value, sizeof(value), 1, s_audio_dump);
}

static void audio_dump_u32(uint32_t value)
{
    fwrite(&value, sizeof(value), 1, s_audio_dump);
}

static void audio_dump_update_header(void)
{
    if (!s_audio_dump) return;
    const uint32_t data_bytes = s_audio_dump_frames * 2u * sizeof(float);
    fseek(s_audio_dump, 4, SEEK_SET);
    audio_dump_u32(36u + data_bytes);
    fseek(s_audio_dump, 40, SEEK_SET);
    audio_dump_u32(data_bytes);
    fseek(s_audio_dump, 0, SEEK_END);
    fflush(s_audio_dump);
}

static void audio_dump_begin(const char* path)
{
    s_audio_dump = fopen(path, "wb");
    if (!s_audio_dump) {
        fprintf(stderr, "[cellAudio-dump] failed to open '%s'\n", path);
        s_audio_dump_state = 2;
        return;
    }
    fwrite("RIFF", 1, 4, s_audio_dump); audio_dump_u32(36);
    fwrite("WAVEfmt ", 1, 8, s_audio_dump); audio_dump_u32(16);
    audio_dump_u16(3); /* WAVE_FORMAT_IEEE_FLOAT */
    audio_dump_u16(2);
    audio_dump_u32(CELL_AUDIO_SAMPLE_RATE);
    audio_dump_u32(CELL_AUDIO_SAMPLE_RATE * 2u * sizeof(float));
    audio_dump_u16(2u * sizeof(float));
    audio_dump_u16(32);
    fwrite("data", 1, 4, s_audio_dump); audio_dump_u32(0);
    s_audio_dump_state = 1;
    fprintf(stderr, "[cellAudio-dump] recording exact %s submissions to '%s'\n",
            audio_sink_name(), path);
}

static void audio_dump_submitted(const float* samples, uint32_t frames)
{
    const char* path = getenv("TAIKO_AUDIO_DUMP");
    if (!path || !*path) {
        path = getenv("TAIKO_AUDIO_GAMEPLAY_DUMP");
        if (!path || !*path ||
            !atomic_load_explicit(&s_audio_gameplay_dump_started,
                                  memory_order_acquire))
            return;
    }
    if (!path || !*path || s_audio_dump_state == 2) return;

    if (s_audio_dump_limit == 0) {
        const char* seconds_text = getenv("TAIKO_AUDIO_DUMP_SECONDS");
        uint32_t seconds = seconds_text ? (uint32_t)strtoul(seconds_text, NULL, 0) : 10u;
        if (seconds == 0 || seconds > 600u) seconds = 10u;
        s_audio_dump_limit = seconds * CELL_AUDIO_SAMPLE_RATE;
    }
    if (s_audio_dump_state == 0) {
        float peak = 0.0f;
        for (uint32_t i = 0; i < frames * 2u; i++) {
            float value = fabsf(samples[i]);
            if (value > peak) peak = value;
        }
        if (peak < 0.0001f) return;
        audio_dump_begin(path);
    }
    if (s_audio_dump_state != 1) return;

    uint32_t remaining = s_audio_dump_limit - s_audio_dump_frames;
    if (frames > remaining) frames = remaining;
    fwrite(samples, 2u * sizeof(float), frames, s_audio_dump);
    s_audio_dump_frames += frames;
    audio_dump_update_header();
    if (s_audio_dump_frames >= s_audio_dump_limit) {
        fclose(s_audio_dump);
        s_audio_dump = NULL;
        s_audio_dump_state = 2;
        fprintf(stderr, "[cellAudio-dump] complete: %u frames\n",
                s_audio_dump_frames);
    }
}

static void audio_dump_shutdown(void)
{
    if (s_audio_dump) {
        audio_dump_update_header();
        fclose(s_audio_dump);
        s_audio_dump = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        u32 src_ea = (u32)port->port_addr + block_offset * (u32)sizeof(float);
        static int producer_trace = -1;
        if (producer_trace < 0)
            producer_trace = getenv("TAIKO_AUDIO_RING_TRACE") ? 1 : 0;
        u64 hash_before = 0;
        if (producer_trace)
            hash_before = audio_guest_block_hash(
                src_ea, CELL_AUDIO_BLOCK_SAMPLES * nch, NULL);

        /* If the current consumer slot is silent, occasionally scan the whole
         * guest ring. This distinguishes a producer/consumer index mismatch
         * from a producer that never copied the SPU output into cellAudio. */
        { static int ring_trace = -1; static u32 ring_blocks = 0;
          if (ring_trace < 0) ring_trace = getenv("TAIKO_AUDIO_RING_TRACE") ? 1 : 0;
          if (ring_trace && (++ring_blocks <= 16u || (ring_blocks & 255u) == 0)) {
              float current_peak = 0.0f, ring_peak = 0.0f;
              u32 peak_block = 0;
              for (u32 b = 0; b < nblock; b++) {
                  float block_peak = 0.0f;
                  u32 block_ea = (u32)port->port_addr +
                      b * CELL_AUDIO_BLOCK_SAMPLES * nch * 4u;
                  for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i++) {
                      float a = fabsf(vm_read_f32(block_ea + i * 4u));
                      if (a > block_peak) block_peak = a;
                  }
                  if (b == block_idx) current_peak = block_peak;
                  if (block_peak > ring_peak) {
                      ring_peak = block_peak;
                      peak_block = b;
                  }
              }
              fprintf(stderr,
                      "[cellAudio-ring] port=%d addr=%08X read=%llu slot=%u current=%g ring=%g peakslot=%u guestread=%u\n",
                      p, (u32)port->port_addr,
                      (unsigned long long)port->read_index, block_idx,
                      current_peak, ring_peak, peak_block,
                      port->read_idx_addr ?
                          (u32)vm_read64((u32)port->read_idx_addr) : 0u);
          }
        }

        /* Only meaningful once every block has been tagged at least once,
         * and only for a port the guest actually feeds: Green opens two and
         * fills one, and an untouched port is idle, not a missed deadline. */
        if (producer_trace && port->read_index >= nblock) {
            if (audio_guest_block_written(port, block_idx)) {
                port->ever_filled = 1;
            } else if (port->ever_filled) {
                port->unfilled_blocks++;
                if (port->unfilled_blocks <= 512 ||
                    (port->unfilled_blocks & 15u) == 0) {
                    /* Which blocks does the guest actually hold written right
                     * now? If it wrote some other slot twice instead of this
                     * one, that slot shows written while this one still has
                     * its tags -- that is a duplicate readIndexAddr read, not
                     * a missed deadline. 'w' written, '.' still tagged, and
                     * the block we told it to fill is bracketed. */
                    char map[CELL_AUDIO_BLOCK_32 * 3 + 1];
                    const u32 told = (u32)((port->read_index +
                        (s_read_index_lookahead > nblock - 2u
                             ? nblock - 2u : s_read_index_lookahead) - 1u)
                        % nblock);
                    unsigned o = 0;
                    for (u32 b = 0; b < nblock && o + 3 < sizeof(map); ++b) {
                        const char c = audio_guest_block_written(port, b)
                            ? 'w' : '.';
                        if (b == told) { map[o++] = '['; map[o++] = c;
                                         map[o++] = ']'; }
                        else map[o++] = c;
                    }
                    map[o] = '\0';
                    fprintf(stderr,
                            "[cellAudio-producer] UNFILLED port=%d slot=%u "
                            "read=%llu count=%u told=%u map=%s\n",
                            p, block_idx,
                            (unsigned long long)port->read_index,
                            port->unfilled_blocks, told, map);
                }
            }
        }

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                /* cellAudio port samples live in big-endian guest memory.
                 * Reading the mapped bytes through a host float* on x86
                 * byte-swaps every sample and turns valid PCM into garbage. */
                left  = vm_read_f32(src_ea + (s * nch + 0) * 4u) * level;
                right = vm_read_f32(src_ea + (s * nch + 1) * 4u) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = vm_read_f32(src_ea + s * 4u) * level;
            }

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                u32 sample_ea = src_ea + s * 8u * 4u;
                float center = vm_read_f32(sample_ea + 2u * 4u) * level * 0.707f;
                float lfe    = vm_read_f32(sample_ea + 3u * 4u) * level * 0.5f;
                float rl     = vm_read_f32(sample_ea + 4u * 4u) * level * 0.5f;
                float rr     = vm_read_f32(sample_ea + 5u * 4u) * level * 0.5f;
                float sl     = vm_read_f32(sample_ea + 6u * 4u) * level * 0.3f;
                float sr     = vm_read_f32(sample_ea + 7u * 4u) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        if (producer_trace) {
            int after_nonzero = 0;
            u64 hash_after = audio_guest_block_hash(
                src_ea, CELL_AUDIO_BLOCK_SAMPLES * nch, &after_nonzero);
            if (hash_after != hash_before) {
                port->raced_blocks++;
                if (port->raced_blocks <= 16 ||
                    (port->raced_blocks & 63u) == 0)
                    fprintf(stderr,
                            "[cellAudio-producer] RACE port=%d slot=%u count=%u before=%016llX after=%016llX\n",
                            p, block_idx, port->raced_blocks,
                            (unsigned long long)hash_before,
                            (unsigned long long)hash_after);
            }
            if (port->consumed_valid[block_idx] && after_nonzero &&
                port->consumed_hash[block_idx] == hash_after) {
                port->stale_blocks++;
                if (port->stale_blocks <= 16 ||
                    (port->stale_blocks & 63u) == 0)
                    fprintf(stderr,
                            "[cellAudio-producer] STALE port=%d slot=%u read=%llu count=%u hash=%016llX\n",
                            p, block_idx, (unsigned long long)port->read_index,
                            port->stale_blocks, (unsigned long long)hash_after);
            }
            port->consumed_hash[block_idx] = hash_after;
            port->consumed_valid[block_idx] = 1;
        }

        /* Retire this slot for the detector. It is not handed back to the
         * guest until s_read_index_lookahead revolutions from now, so the tags
         * survive until then and prove whether the guest refilled it. */
        if (producer_trace)
            audio_tag_consumed_block(port, block_idx);

        /* Advance the host-side counter, but publish the current RING POSITION
         * to the guest.  CellAudio's readIndexAddr is not a monotonically
         * increasing block count: RPCS3 (matching the PS3 contract) stores
         * port.position(1), which is modulo nBlock.  bnusCore uses the value
         * directly as `portAddr + (readIndex << 13)` for its 8-channel,
         * 256-frame blocks, so publishing the unbounded counter eventually
         * made every game-produced mix land far beyond the audio port. */
        port->read_index++;
        port->timestamp_usec = ppu_timebase_usec_now();
        /* Bound: the guest writes `lookahead` periods ahead of the block being
         * played, so the ring must still hold a block we have not reached yet.
         * nblock-2 leaves one spare slot; nblock/2 was an arbitrary early
         * guess and caps an 8-block port at 21 ms of producer slack. */
        u32 lookahead = s_read_index_lookahead;
        const u32 lookahead_max = nblock > 3u ? nblock - 2u : 1u;
        if (lookahead > lookahead_max) lookahead = lookahead_max;
        if (lookahead < 1u) lookahead = 1u;
        const u32 next_block =
            (u32)((port->read_index + lookahead - 1u) % nblock);
        /* Queue it; sys_event_queue_receive publishes it as the guest picks up
         * the matching notification. If the guest is far enough behind that the
         * queue fills, drop the oldest -- those blocks are already played. */
        if (port->pending_count == nblock) {
            port->pending_head = (port->pending_head + 1u) % nblock;
            port->pending_count--;
            port->pending_dropped++;
        }
        port->pending_index[port->pending_tail] = next_block;
        port->pending_tail = (port->pending_tail + 1u) % nblock;
        port->pending_count++;
    }

    mutex_unlock(&s_audio_mutex);

    if (s_external_mixer)
        s_external_mixer(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);

    /* Never pass non-finite guest samples to the host audio stack.  A broken
     * guest decoder/mixer can produce NaN or infinity; ordinary comparisons
     * do not clamp NaNs, so they previously reached WASAPI unchanged and
     * could poison the shared endpoint stream.  Treat invalid samples as
     * silence, then apply the normal full-scale clamp. */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (!isfinite(s_mix_buffer[i]))
            s_mix_buffer[i] = 0.0f;
        else if (s_mix_buffer[i] > 1.0f)
            s_mix_buffer[i] = 1.0f;
        else if (s_mix_buffer[i] < -1.0f)
            s_mix_buffer[i] = -1.0f;
    }

    /* Opt-in proof of whether the guest mixer is producing PCM. */
    { static int trace = -1; static u32 blocks = 0, nonzero_blocks = 0;
      static float max_peak = 0.0f;
      if (trace < 0) trace = getenv("TAIKO_AUDIO_TRACE") ? 1 : 0;
      if (trace) {
          float peak = 0.0f;
          for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
              float a = fabsf(s_mix_buffer[i]);
              if (a > peak) peak = a;
          }
          blocks++;
          if (peak > 0.0f) nonzero_blocks++;
          if (peak > max_peak) max_peak = peak;
          if ((peak > 0.0f && nonzero_blocks <= 32u) || (blocks & 255u) == 0)
              fprintf(stderr, "[cellAudio-pcm] block=%u peak=%g nonzero=%u max=%g\n",
                      blocks, peak, nonzero_blocks, max_peak);
      }
    }
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid) {
            const int depth = sys_event_queue_depth_by_id(qid);
            if (depth > 0) {
                atomic_fetch_add_explicit(&s_notify_backlog_events, 1,
                                          memory_order_relaxed);
                int previous = atomic_load_explicit(&s_notify_backlog_max,
                                                    memory_order_relaxed);
                while (depth > previous &&
                       !atomic_compare_exchange_weak_explicit(
                           &s_notify_backlog_max, &previous, depth,
                           memory_order_relaxed, memory_order_relaxed)) { }
            }
        }
        if (qid && sys_event_queue_push_by_id(
                qid, s_notify_queues[i].key,
                0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0) != 0) {
            const unsigned long long drops = atomic_fetch_add_explicit(
                &s_notify_drops, 1, memory_order_relaxed) + 1;
            if (drops <= 16 || (drops & 63u) == 0)
                fprintf(stderr,
                        "[cellAudio-notify] DROP q=%u key=%016llX count=%llu\n",
                        qid, (unsigned long long)s_notify_queues[i].key,
                        drops);
        }
    }
}

static void audio_trace_sink(uint64_t* interval_start, uint32_t* interval_blocks,
                             uint64_t total_blocks, uint64_t failed_blocks)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TAIKO_AUDIO_SINK_TRACE") ? 1 : 0;
    if (!enabled) return;
    uint64_t now = ps3_host_monotonic_ns();
    if (!*interval_start) *interval_start = now;
    ++*interval_blocks;
    if (now - *interval_start < 1000000000u) return;

    uint32_t races = 0, stale = 0, unfilled = 0;
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; ++p) {
        races += s_ports[p].raced_blocks;
        stale += s_ports[p].stale_blocks;
        unfilled += s_ports[p].unfilled_blocks;
    }
    /* RACE/STALE/UNFILLED all require TAIKO_AUDIO_RING_TRACE. Printing 0 when
     * the detector is off reads as "no misses" and is how a blind trace got
     * mistaken for a clean one; say "off" instead. */
    char unfilled_text[24];
    if (getenv("TAIKO_AUDIO_RING_TRACE"))
        snprintf(unfilled_text, sizeof(unfilled_text), "%u", unfilled);
    else
        snprintf(unfilled_text, sizeof(unfilled_text), "off");

    double seconds = (double)(now - *interval_start) / 1000000000.0;
    uint32_t queued = s_null_audio_clock ? 0 : audio_sink_queued_frames();
    uint32_t device = s_null_audio_clock ? 0 : audio_sink_device_buffer_frames();
    fprintf(stderr,
            "[cellAudio-sink] backend=%s clock=%s rate=%.2f blocks/s "
            "queued=%u frames (%.2f blocks/%.2f ms) "
            "device=%u frames (%.2f ms) visible_bound=%.2f ms "
            "total=%llu failed=%llu notify_drop=%llu "
            "sink_starve=%llu/%llu_frames "
            "RACE=%u STALE=%u UNFILLED=%s "
            "notify_backlog=%llu max_depth=%d\n",
            audio_sink_name(), s_null_audio_clock ? "null" : "device",
            (double)*interval_blocks / seconds, queued,
            (double)queued / CELL_AUDIO_BLOCK_SAMPLES,
            1000.0 * queued / CELL_AUDIO_SAMPLE_RATE,
            device, 1000.0 * device / CELL_AUDIO_SAMPLE_RATE,
            1000.0 * (queued + device) / CELL_AUDIO_SAMPLE_RATE,
            (unsigned long long)total_blocks,
            (unsigned long long)failed_blocks,
            atomic_load_explicit(&s_notify_drops, memory_order_relaxed),
            (unsigned long long)audio_sink_starvation_events(),
            (unsigned long long)audio_sink_starvation_frames(),
            races, stale, unfilled_text,
            atomic_load_explicit(&s_notify_backlog_events,
                                 memory_order_relaxed),
            atomic_load_explicit(&s_notify_backlog_max,
                                 memory_order_relaxed));
    *interval_start = now;
    *interval_blocks = 0;
}

static void audio_mix_thread_run(void)
{
    ps3_host_apply_thread_affinity("TAIKO_CPU_AUDIO_AFFINITY", "audio mixer");
    printf("[cellAudio] Mixing thread started (%s sink, %s clock)\n",
           audio_sink_name(), s_null_audio_clock ? "null" : "device");

    const uint64_t period_ns =
        (uint64_t)CELL_AUDIO_BLOCK_SAMPLES * 1000000000u /
        CELL_AUDIO_SAMPLE_RATE;
    uint64_t next_null_period = ps3_host_monotonic_ns();
    uint64_t trace_start = 0, total_blocks = 0, failed_blocks = 0;
    uint32_t trace_blocks = 0;
    uint64_t next_period = ps3_host_monotonic_ns();

    while (s_mix_thread_running) {
        if (!s_null_audio_clock &&
            !audio_sink_wait_for_block(CELL_AUDIO_BLOCK_SAMPLES,
                                       &s_mix_thread_running)) {
            if (!s_mix_thread_running) break;
            fprintf(stderr,
                    "[cellAudio] device clock failed; entering null-clock mode\n");
            audio_sink_shutdown();
            s_sink_initialized = 0;
            s_null_audio_clock = 1;
            next_null_period = ps3_host_monotonic_ns();
        }

        audio_mix_one_block();
        int submitted = s_null_audio_clock ||
            audio_sink_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        if (!submitted) {
            ++failed_blocks;
            fprintf(stderr,
                    "[cellAudio] complete block submission failed; entering null-clock mode\n");
            audio_sink_shutdown();
            s_sink_initialized = 0;
            s_null_audio_clock = 1;
            next_null_period = ps3_host_monotonic_ns();
        } else {
            audio_dump_submitted(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
            audio_report_first_audible(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        }
        ++total_blocks;
        atomic_fetch_add_explicit(&s_blocks_since_arm, 1, memory_order_relaxed);
        audio_notify_event_queues();
        audio_trace_sink(&trace_start, &trace_blocks, total_blocks, failed_blocks);

        if (s_null_audio_clock) {
            /* Explicit fallback clock: absolute deadlines avoid accumulating
             * scheduler and mixing latency. */
            next_null_period += period_ns;
            ps3_host_sleep_until_ns(next_null_period);
        } else {
            /* Release exactly one notification per 48 kHz block period.
             * The device pulls a whole ALSA period (four blocks here) at once,
             * so pure queue backpressure hands the guest four notifications
             * back to back. bnusCore reads readIndexAddr once per
             * notification, mixes, and copies 0x2000 bytes to that block --
             * given four in a burst it copies into the same block several
             * times, and the song audio for the other blocks is destroyed.
             * That is the drift: every burst silently discards a few
             * milliseconds of song while the beatmap keeps real time.
             * Absolute deadlines so scheduler latency cannot accumulate; if
             * the device queue really is short, skip the wait and let queue
             * backpressure take the clock back. */
            const uint32_t queued = audio_sink_queued_frames();
            uint64_t period = period_ns;
            /* Pacing alone cannot restore the queue after a starvation dip,
             * and skipping the wait outright would reintroduce the burst.
             * Pull the period by 12.5% instead: notifications stay at least
             * 4.6 ms apart while the level walks back into the deadband.
             * audio_sink_wait_for_block remains the hard ceiling. */
            uint32_t low_watermark = 2u * CELL_AUDIO_BLOCK_SAMPLES;
            uint32_t high_watermark = 6u * CELL_AUDIO_BLOCK_SAMPLES;
            const uint32_t prebuffer = audio_sink_prebuffer_frames();
            const uint32_t device_period = audio_sink_device_buffer_frames();
            if (prebuffer) {
                /* The SDL sink already blocks submission at its prebuffer
                 * ceiling. Applying the slow clock pull as well double-
                 * throttles the producer whenever SDL reports conversion
                 * residue above the nominal target; the Q6A settled at only
                 * ~180 blocks/s instead of 187.5. Keep the low-water recovery,
                 * but let hard queue backpressure govern the upper bound. */
                high_watermark = UINT32_MAX;
                if (prebuffer > device_period) {
                    const uint32_t after_device_pull =
                        ((prebuffer - device_period) /
                         CELL_AUDIO_BLOCK_SAMPLES) *
                        CELL_AUDIO_BLOCK_SAMPLES;
                    if (after_device_pull > low_watermark)
                        low_watermark = after_device_pull;
                }
            }
            if (queued < low_watermark)
                period -= period_ns / 8u;
            else if (queued > high_watermark)
                period += period_ns / 8u;
            next_period += period;
            const uint64_t now = ps3_host_monotonic_ns();
            if (next_period > now)
                ps3_host_sleep_until_ns(next_period);
            else {
                /* Preserve ordinary scheduler overshoot so the absolute
                 * clock actually averages 48 kHz. Resetting the deadline to
                 * `now` here adds every wake-up delay to the next 5.33 ms
                 * period; on the Q6A that slowed a 44.1 kHz source to about
                 * 42.5 kHz. Bound the retained lateness so even after a long
                 * stall the next notification remains at least 7/8 of a
                 * block period away instead of being released in a burst. */
                const uint64_t minimum_period =
                    period_ns - period_ns / 8u;
                const uint64_t maximum_catchup =
                    period > minimum_period ? period - minimum_period : 0;
                const uint64_t lateness = now - next_period;
                if (lateness > maximum_catchup)
                    next_period = now - maximum_catchup;
            }
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
}

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    audio_mix_thread_run();
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    audio_mix_thread_run();
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u * 1024 * 1024);
    int rc = pthread_create(&s_mix_thread, &attr, audio_mix_thread_func, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
#endif
    s_mix_thread_started = 1;
    return 0;
}

static void audio_stop_mix_thread(void)
{
    if (!s_mix_thread_started) return;
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    pthread_join(s_mix_thread, NULL);
#endif
    s_mix_thread_started = 0;
}

static void audio_stop_host_output(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &s_host_output_shutdown, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        while (atomic_load_explicit(&s_host_output_shutdown,
                                    memory_order_acquire) == 1)
            ps3_host_sleep_ms(1);
        return;
    }
    audio_stop_mix_thread();
    if (s_sink_initialized) audio_sink_shutdown();
    s_sink_initialized = 0;
    audio_dump_shutdown();
    atomic_store_explicit(&s_host_output_shutdown, 2, memory_order_release);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);
    s_audio_dump_frames = 0;
    s_audio_dump_limit = 0;
    s_audio_dump_state = 0;
    atomic_store_explicit(&s_audio_gameplay_dump_started, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&s_gameplay_arm_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s_gameplay_audible_reported, 0,
                          memory_order_relaxed);
    s_null_audio_clock = 0;
    s_read_index_lookahead = 4;
    { const char* text = getenv("TAIKO_AUDIO_LOOKAHEAD_BLOCKS");
      if (text && *text) {
          const unsigned long parsed = strtoul(text, NULL, 0);
          if (parsed >= 1 && parsed <= 30) s_read_index_lookahead = (u32)parsed;
      } }
    fprintf(stderr, "[cellAudio] guest write lookahead=%u blocks (%.2f ms)\n",
            s_read_index_lookahead,
            1000.0 * (double)(s_read_index_lookahead - 1) *
                CELL_AUDIO_BLOCK_SAMPLES / CELL_AUDIO_SAMPLE_RATE);
    atomic_store_explicit(&s_notify_drops, 0, memory_order_relaxed);
    atomic_store_explicit(&s_notify_backlog_events, 0, memory_order_relaxed);
    atomic_store_explicit(&s_notify_backlog_max, 0, memory_order_relaxed);
    atomic_store_explicit(&s_host_output_shutdown, 0, memory_order_release);
    int sink_result = audio_sink_init();
    if (sink_result == AUDIO_SINK_INIT_OK) {
        s_sink_initialized = 1;
    } else {
        audio_sink_shutdown();
        s_sink_initialized = 0;
        s_null_audio_clock = 1;
        fprintf(stderr,
                "[cellAudio] %s host sink %s; explicit null clock enabled\n",
                audio_sink_name(), sink_result == AUDIO_SINK_INIT_NULL_CLOCK
                    ? "disabled" : "initialization failed");
    }

    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_host_output();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

void cellAudioHostShutdown(void)
{
    if (!s_audio_initialized) return;
    fprintf(stderr, "[cellAudio] host shutdown\n");
    audio_stop_host_output();
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->read_index  = 0;
    port->write_index = 0;
    port->timestamp_usec = ppu_timebase_usec_now();
    memset(port->consumed_hash, 0, sizeof(port->consumed_hash));
    memset(port->consumed_valid, 0, sizeof(port->consumed_valid));
    port->stale_blocks = 0;
    port->raced_blocks = 0;
    port->unfilled_blocks = 0;
    port->ever_filled = 0;
    port->pending_head = port->pending_tail = port->pending_count = 0;
    port->pending_dropped = 0;

    /* Place the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Each port owns a fixed slot so closing
     * and reopening ports cannot exhaust or alias the HLE arena. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    u32 guest_buf = AUDIO_GUEST_BASE + (u32)found * AUDIO_GUEST_PORT_STRIDE;
    u32 guest_ridx = guest_buf + AUDIO_GUEST_RIDX_OFFSET;

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* u64 read index counter */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (bump-allocated in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address. CellAudioPortConfig uses 32-bit guest
     * pointers, not host-sized/u64 pointers. Its PS3 ABI layout is:
     *   +0 readIndexAddr (u32), +4 status (u32), +8 nChannel (u64),
     *   +16 nBlock (u64), +24 portSize (u32), +28 portAddr (u32).
     * The old 0/8/16/24/32/40 layout left the fields the game actually read
     * unset, so bnusCore never copied its SPU mix into this port ring. */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                              /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                                /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                    /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                              /* portAddr */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;

    const u32 tag_ea = (u32)(uintptr_t)tag;
    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    if (!port->in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }
    const u64 nblock = port->param.nBlock;
    if (blockNo >= nblock) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }

    /* Match the PS3/RPCS3 contract: identify this physical ring slot in the
     * current monotonically increasing tag epoch. */
    const u64 current_position = port->read_index % nblock;
    const u64 block_tag = port->read_index + blockNo - current_position;
    if (tag_ea)
        vm_write64(tag_ea, block_tag);
    { static unsigned traces;
      if (traces++ < 12)
          fprintf(stderr,
                  "[cellAudio-clock] block-tag port=%u block=%llu read=%llu pos=%llu tag=%llu\n",
                  portNum, (unsigned long long)blockNo,
                  (unsigned long long)port->read_index,
                  (unsigned long long)current_position,
                  (unsigned long long)block_tag); }
    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;

    const u32 stamp_ea = (u32)(uintptr_t)stamp;
    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    if (!port->in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }
    if (port->read_index < tag) {
        { static unsigned pending_traces;
          if (pending_traces++ < 12)
              fprintf(stderr,
                      "[cellAudio-clock] timestamp pending port=%u tag=%llu read=%llu\n",
                      portNum, (unsigned long long)tag,
                      (unsigned long long)port->read_index); }
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;
    }

    const u64 delta = port->read_index - tag;
    const u64 delta_usec = delta * (u64)CELL_AUDIO_PERIOD_US;
    const u64 timestamp = port->timestamp_usec > delta_usec
        ? port->timestamp_usec - delta_usec : 0;
    if (stamp_ea)
        vm_write64(stamp_ea, timestamp);
    { static unsigned traces;
      static int latency_trace = -1;
      if (latency_trace < 0)
          latency_trace = getenv("TAIKO_AUDIO_LATENCY_TRACE") ? 1 : 0;
      const unsigned trace_no = traces++;
      if (trace_no < 12 || (latency_trace && (trace_no & 127u) == 0)) {
          const u64 now = ppu_timebase_usec_now();
          const u32 queued = s_null_audio_clock ? 0 : audio_sink_queued_frames();
          const u32 device = s_null_audio_clock ? 0 : audio_sink_device_buffer_frames();
          fprintf(stderr,
                  "[cellAudio-clock] timestamp port=%u tag=%llu read=%llu "
                  "delta=%llu stamp=%llu age=%.2fms queued=%.2fms "
                  "device=%.2fms visible_bound=%.2fms\n",
                  portNum, (unsigned long long)tag,
                  (unsigned long long)port->read_index,
                  (unsigned long long)delta,
                  (unsigned long long)timestamp,
                  now >= timestamp ? (double)(now - timestamp) / 1000.0 : 0.0,
                  1000.0 * queued / CELL_AUDIO_SAMPLE_RATE,
                  1000.0 * device / CELL_AUDIO_SAMPLE_RATE,
                  1000.0 * (queued + device) / CELL_AUDIO_SAMPLE_RATE);
      } }
    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        *status = CELL_AUDIO_STATUS_CLOSE;
    } else if (s_ports[portNum].running) {
        *status = CELL_AUDIO_STATUS_RUN;
    } else {
        *status = CELL_AUDIO_STATUS_READY;
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
