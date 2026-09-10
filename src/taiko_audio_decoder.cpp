#include "taiko_audio_decoder.h"

#ifdef TAIKO_HAVE_FFMPEG
extern "C" {
#include <libvgmstream.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <fstream>
#include <mutex>
#include <unordered_map>

bool taiko_custom_preview(std::string_view, uint32_t, const std::atomic<bool>*,
                          TaikoDecodedAudio&, std::string&) __attribute__((weak));

namespace {

constexpr size_t kMaximumRiffBytes = 256u * 1024u * 1024u;
constexpr uint32_t kChannels = 2;
std::mutex g_proxy_mutex;
std::vector<uint8_t> g_proxy_riff;
TaikoDecodedAudio g_proxy_audio;
std::string g_proxy_source;

uint32_t read_le32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

bool cancelled(const std::atomic<bool>* flag)
{
    return flag && flag->load(std::memory_order_relaxed);
}

struct RiffLocation {
    std::string path;
    uint64_t offset = 0;
};

std::mutex g_riff_index_mutex;
std::unordered_map<uint64_t, RiffLocation> g_riff_index;
std::once_flag g_riff_index_once;

bool riff_declared_size(const uint8_t* data, size_t bytes, size_t& declared)
{
    if (bytes < 12 || std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0)
        return false;
    const uint64_t total = static_cast<uint64_t>(read_le32(data + 4)) + 8u;
    if (total < 12 || total > kMaximumRiffBytes)
        return false;
    declared = static_cast<size_t>(total);
    return true;
}

bool read_file_member(const RiffLocation& location,
                      const std::vector<uint8_t>* expected_prefix,
                      size_t declared, const std::atomic<bool>* stop,
                      std::vector<uint8_t>& riff)
{
    FILE* file = std::fopen(location.path.c_str(), "rb");
    if (!file) return false;
    bool ok = location.offset <= static_cast<uint64_t>(LONG_MAX) &&
              std::fseek(file, static_cast<long>(location.offset), SEEK_SET) == 0;
    if (ok && expected_prefix) {
        std::vector<uint8_t> check(expected_prefix->size());
        ok = std::fread(check.data(), 1, check.size(), file) == check.size() &&
             check == *expected_prefix;
        if (ok)
            ok = std::fseek(file, static_cast<long>(location.offset), SEEK_SET) == 0;
    }
    if (ok) {
        riff.resize(declared);
        size_t done = 0;
        while (done < declared && !cancelled(stop)) {
            const size_t step = std::min<size_t>(declared - done, 1024u * 1024u);
            const size_t got = std::fread(riff.data() + done, 1, step, file);
            done += got;
            if (got != step) break;
        }
        ok = done == declared && !cancelled(stop);
    }
    std::fclose(file);
    if (!ok) riff.clear();
    return ok;
}

void find_riff_members(const std::vector<uint8_t>& header,
                       std::vector<size_t>& offsets)
{
    auto marker = std::search(header.begin(), header.end(), "RIFF", "RIFF" + 4);
    while (marker != header.end()) {
        const size_t offset = static_cast<size_t>(marker - header.begin());
        if (offset + 12 <= header.size() &&
            std::memcmp(header.data() + offset + 8, "WAVE", 4) == 0)
            offsets.push_back(offset);
        marker = std::search(marker + 4, header.end(), "RIFF", "RIFF" + 4);
    }
}

bool read_nub_header(const std::string& path, std::vector<uint8_t>& header)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    header.resize(0x10000);
    header.resize(std::fread(header.data(), 1, header.size(), file));
    std::fclose(file);
    return !header.empty();
}

void build_riff_index()
{
    const char* root = std::getenv("PS3_VFS_ROOT");
    if (!root || !*root) return;
    const std::filesystem::path directory =
        std::filesystem::path(root) / "data" / "sound" / "bgm" / "nub";
    std::error_code error;
    std::filesystem::directory_iterator entries(directory, error);
    if (error) return;

    std::unordered_map<uint64_t, RiffLocation> built;
    for (const auto& entry : entries) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error ||
            entry.path().extension() != ".nub")
            continue;
        std::vector<uint8_t> header;
        const std::string path = entry.path().string();
        if (!read_nub_header(path, header)) continue;
        std::vector<size_t> offsets;
        find_riff_members(header, offsets);
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) continue;
        for (size_t offset : offsets) {
            if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
                continue;
            std::vector<uint8_t> signature(4096);
            if (std::fread(signature.data(), 1, signature.size(), file) ==
                signature.size())
                built[taiko_audio_hash_bytes(signature)] =
                    RiffLocation{path, offset};
        }
        std::fclose(file);
    }
    std::lock_guard<std::mutex> lock(g_riff_index_mutex);
    for (auto& [hash, location] : built)
        g_riff_index.emplace(hash, std::move(location));
}

struct PcmCacheEntry {
    uint64_t hash = 0;
    size_t riff_bytes = 0;
    uint32_t output_rate = 0;
    TaikoDecodedAudio decoded;
};

std::mutex g_pcm_cache_mutex;
std::list<PcmCacheEntry> g_pcm_cache;
size_t g_pcm_cache_bytes = 0;

size_t pcm_cache_limit_bytes()
{
    static const size_t limit = [] {
        const char* text = std::getenv("TAIKO_AUDIO_PCM_CACHE_MB");
        unsigned long mb = text ? std::strtoul(text, nullptr, 0) : 512ul;
        mb = std::clamp(mb, 64ul, 8192ul);
        return static_cast<size_t>(mb) * 1024u * 1024u;
    }();
    return limit;
}

bool cache_lookup(uint64_t hash, size_t bytes, uint32_t output_rate,
                  TaikoDecodedAudio& decoded)
{
    std::lock_guard<std::mutex> lock(g_pcm_cache_mutex);
    for (auto it = g_pcm_cache.begin(); it != g_pcm_cache.end(); ++it) {
        if (it->hash != hash || it->riff_bytes != bytes ||
            it->output_rate != output_rate)
            continue;
        decoded = it->decoded;
        decoded.cache_hit = true;
        g_pcm_cache.splice(g_pcm_cache.begin(), g_pcm_cache, it);
        return true;
    }
    return false;
}

void cache_insert(uint64_t hash, size_t riff_bytes, uint32_t output_rate,
                  const TaikoDecodedAudio& decoded)
{
    const size_t bytes = decoded.pcm->size() * sizeof(float);
    const size_t limit = pcm_cache_limit_bytes();
    if (bytes > limit) return;
    std::lock_guard<std::mutex> lock(g_pcm_cache_mutex);
    for (auto it = g_pcm_cache.begin(); it != g_pcm_cache.end(); ++it) {
        if (it->hash == hash && it->riff_bytes == riff_bytes &&
            it->output_rate == output_rate) {
            g_pcm_cache_bytes -= it->decoded.pcm->size() * sizeof(float);
            g_pcm_cache.erase(it);
            break;
        }
    }
    PcmCacheEntry entry{hash, riff_bytes, output_rate, decoded};
    entry.decoded.cache_hit = false;
    g_pcm_cache.push_front(std::move(entry));
    g_pcm_cache_bytes += bytes;
    while (g_pcm_cache_bytes > limit && g_pcm_cache.size() > 1) {
        g_pcm_cache_bytes -=
            g_pcm_cache.back().decoded.pcm->size() * sizeof(float);
        g_pcm_cache.pop_back();
    }
}

void read_loop(const std::vector<uint8_t>& riff, uint32_t source_rate,
               uint32_t output_rate, TaikoDecodedAudio& decoded)
{
    size_t offset = 12;
    while (offset + 8 <= riff.size()) {
        const uint32_t size = read_le32(riff.data() + offset + 4);
        const uint64_t next = static_cast<uint64_t>(offset) + 8u + size + (size & 1u);
        if (std::memcmp(riff.data() + offset, "smpl", 4) == 0 &&
            size >= 60 && offset + 68u <= riff.size()) {
            const uint32_t count = read_le32(riff.data() + offset + 8 + 28);
            const uint32_t start = read_le32(riff.data() + offset + 8 + 44);
            const uint32_t end = read_le32(riff.data() + offset + 8 + 48);
            if (count && end >= start && source_rate && output_rate) {
                decoded.loop_start = static_cast<size_t>(
                    (static_cast<uint64_t>(start) * output_rate + source_rate / 2u) /
                    source_rate);
                decoded.loop_end = static_cast<size_t>(
                    (static_cast<uint64_t>(end + 1ull) * output_rate +
                     source_rate / 2u) / source_rate);
                const size_t frames = decoded.pcm->size() / kChannels;
                decoded.loop_start = std::min(decoded.loop_start, frames);
                decoded.loop_end = std::min(decoded.loop_end, frames);
                decoded.has_loop = decoded.loop_end > decoded.loop_start;
            }
            return;
        }
        if (std::memcmp(riff.data() + offset, "data", 4) == 0 ||
            next > riff.size())
            return;
        offset = static_cast<size_t>(next);
    }
}

uint32_t fact_samples(const std::vector<uint8_t>& riff)
{
    size_t offset = 12;
    while (offset + 8 <= riff.size()) {
        const uint32_t size = read_le32(riff.data() + offset + 4);
        if (std::memcmp(riff.data() + offset, "fact", 4) == 0 &&
            size >= 4 && offset + 12 <= riff.size())
            return read_le32(riff.data() + offset + 8);
        if (std::memcmp(riff.data() + offset, "data", 4) == 0) break;
        const uint64_t next = static_cast<uint64_t>(offset) + 8u + size + (size & 1u);
        if (next > riff.size()) break;
        offset = static_cast<size_t>(next);
    }
    return 0;
}

#ifdef TAIKO_HAVE_FFMPEG

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

std::string ffmpeg_error(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, text, sizeof text);
    return text;
}

// Restrict vgmstream to the already-read bank. No companion-file reads and no
// second open of a potentially changed source between hashing and decoding.
libstreamfile_t* bank_stream(const std::vector<uint8_t>* bytes)
{
    auto sf = new (std::nothrow) libstreamfile_t{};
    if (!sf) return nullptr;
    sf->user_data = const_cast<std::vector<uint8_t>*>(bytes);
    sf->read = [](void* data, uint8_t* dst, int64_t at, int count) -> int {
        const auto& b = *static_cast<const std::vector<uint8_t>*>(data);
        if (at < 0 || uint64_t(at) >= b.size() || count <= 0) return 0;
        const size_t n = std::min(size_t(count), b.size()-size_t(at));
        std::memcpy(dst,b.data()+at,n); return int(n);
    };
    sf->get_size = [](void* data) -> int64_t {return static_cast<const std::vector<uint8_t>*>(data)->size();};
    sf->get_name = [](void*) -> const char* {return "song.nus3bank";};
    sf->open = [](void* data, const char* name) -> libstreamfile_t* {
        return std::strcmp(name,"song.nus3bank") == 0 ? bank_stream(static_cast<const std::vector<uint8_t>*>(data)) : nullptr;
    };
    sf->close = [](libstreamfile_t* s) {delete s;};
    return sf;
}

bool decode_bank(const std::vector<uint8_t>& bytes, uint32_t requested_rate,
                 const std::atomic<bool>* stop, TaikoDecodedAudio& decoded,
                 std::string& failure)
{
    if (bytes.size()<24 || std::memcmp(bytes.data()+8,"BANKTOC ",8)) {
        failure="invalid NUS3BANK header";return false;
    }
    libvgmstream_config_t config{};
    config.ignore_loop=true; config.disable_config_override=true;
    config.force_sfmt=LIBVGMSTREAM_SFMT_FLOAT;
    auto sf=bank_stream(&bytes);
    if (!sf) {failure="cannot allocate bank reader";return false;}
    auto vgm=libvgmstream_create(sf,0,&config);
    libstreamfile_close(sf);
    if (!vgm) {failure="unsupported or damaged NUS3BANK audio";return false;}
    struct Release {libvgmstream_t* p;~Release(){libvgmstream_free(p);}} release{vgm};
    const auto& format=*vgm->format;
    const uint32_t rate=requested_rate ? requested_rate : format.sample_rate;
    if (format.channels<1 || format.channels>2 || format.sample_rate<8000 || format.sample_rate>192000 ||
        rate<8000 || rate>192000 || format.play_samples<=0 || format.subsong_count!=1 ||
        uint64_t(format.play_samples)*rate/format.sample_rate > kMaximumRiffBytes/sizeof(float)/2) {
        failure="NUS3BANK layout or duration exceeds supported limits";return false;
    }
    AVChannelLayout input{}, stereo=AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_default(&input,format.channels);
    SwrContext* swr=nullptr;
    int err=swr_alloc_set_opts2(&swr,&stereo,AV_SAMPLE_FMT_FLT,rate,
        &input,AV_SAMPLE_FMT_FLT,format.sample_rate,0,nullptr);
    av_channel_layout_uninit(&input);
    struct Resample {SwrContext** p;~Resample(){swr_free(p);}} resample{&swr};
    if(err<0 || !swr || swr_init(swr)<0) {failure="cannot initialize bank resampler";return false;}
    decoded=TaikoDecodedAudio{};decoded.sample_rate=rate;
    decoded.pcm=std::make_shared<std::vector<float>>();
    auto append=[&](const uint8_t* data,int samples) -> int {
        int capacity=swr_get_out_samples(swr,samples);
        const size_t old=decoded.pcm->size();
        if(capacity<0 || uint64_t(old)+uint64_t(capacity)*2>kMaximumRiffBytes/sizeof(float))return -1;
        decoded.pcm->resize(old+size_t(capacity)*2);
        uint8_t* output[]={reinterpret_cast<uint8_t*>(decoded.pcm->data()+old)};
        int count=swr_convert(swr,output,capacity,data?&data:nullptr,samples);
        if(count<0)return -1;
        decoded.pcm->resize(old+size_t(count)*2);return count;
    };
    int64_t source_samples=0;
    while(!vgm->decoder->done) {
        if(cancelled(stop)) {failure="cancelled";return false;}
        if(libvgmstream_render(vgm)<0) {failure="NUS3BANK decode failed";return false;}
        const auto& chunk=*vgm->decoder;
        if(chunk.buf_samples<0 || (!chunk.done && !chunk.buf_samples) ||
            source_samples+chunk.buf_samples>format.play_samples ||
            append(static_cast<const uint8_t*>(chunk.buf),chunk.buf_samples)<0) {
            failure="invalid or oversized decoded bank audio";return false;
        }
        source_samples+=chunk.buf_samples;
    }
    if(source_samples!=format.play_samples) {failure="truncated bank audio";return false;}
    for (;;) {int n=append(nullptr,0);if(n<0){failure="bank resample flush failed";return false;}if(!n)break;}
    return !decoded.pcm->empty();
}

bool decode_uncached(const std::vector<uint8_t>& riff, uint32_t requested_rate,
                     const std::atomic<bool>* stop, TaikoDecodedAudio& decoded,
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
    uint32_t source_rate = 0;

    bool ok = [&]() {
        constexpr int io_buffer_bytes = 32768;
        uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(io_buffer_bytes));
        if (!io_buffer) { failure = "out of memory allocating AVIO"; return false; }
        io = avio_alloc_context(io_buffer, io_buffer_bytes, 0, &input,
                                read_memory_packet, nullptr, seek_memory);
        if (!io) { av_free(io_buffer); failure = "could not create AVIO context"; return false; }
        format = avformat_alloc_context();
        if (!format) { failure = "could not create format context"; return false; }
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        last_error = avformat_open_input(&format, nullptr,
                                         nullptr, nullptr);
        if (last_error < 0) { failure = "open WAV: " + ffmpeg_error(last_error); return false; }
        last_error = avformat_find_stream_info(format, nullptr);
        if (last_error < 0) { failure = "read stream info: " + ffmpeg_error(last_error); return false; }
        const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO,
                                               -1, -1, nullptr, 0);
        if (stream < 0) { failure = "no audio stream: " + ffmpeg_error(stream); return false; }
        const AVCodecParameters* parameters = format->streams[stream]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) { failure = "ATRAC decoder is unavailable"; return false; }
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
        source_rate = static_cast<uint32_t>(decoder->sample_rate);
        decoded.sample_rate = requested_rate ? requested_rate : source_rate;
        last_error = swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT,
                                         decoded.sample_rate, &decoder->ch_layout,
                                         decoder->sample_fmt, source_rate, 0, nullptr);
        if (last_error < 0 || !resampler) {
            failure = "create PCM converter: " + ffmpeg_error(last_error);
            return false;
        }
        last_error = swr_init(resampler);
        if (last_error < 0) { failure = "initialize PCM converter: " + ffmpeg_error(last_error); return false; }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!packet || !frame) { failure = "out of memory allocating decode frames"; return false; }
        decoded.pcm = std::make_shared<std::vector<float>>();

        auto receive = [&]() {
            for (;;) {
                const int received = avcodec_receive_frame(decoder, frame);
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
                if (received < 0) { failure = "receive ATRAC frame: " + ffmpeg_error(received); return false; }
                const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
                if (capacity < 0) { failure = "size converted PCM: " + ffmpeg_error(capacity); return false; }
                const size_t old = decoded.pcm->size();
                if (static_cast<size_t>(capacity) > kMaximumRiffBytes / sizeof(float) / kChannels ||
                    old > kMaximumRiffBytes / sizeof(float) - static_cast<size_t>(capacity) * kChannels) {
                    failure = "decoded audio exceeds 256 MiB";
                    return false;
                }
                decoded.pcm->resize(old + static_cast<size_t>(capacity) * kChannels);
                uint8_t* output[] = {reinterpret_cast<uint8_t*>(decoded.pcm->data() + old)};
                const int converted = swr_convert(resampler, output, capacity,
                    const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                if (converted < 0) { failure = "convert PCM: " + ffmpeg_error(converted); return false; }
                decoded.pcm->resize(old + static_cast<size_t>(converted) * kChannels);
                av_frame_unref(frame);
            }
        };

        while ((last_error = av_read_frame(format, packet)) >= 0) {
            if (cancelled(stop)) { failure = "cancelled"; av_packet_unref(packet); return false; }
            if (packet->stream_index == stream) {
                last_error = avcodec_send_packet(decoder, packet);
                if (last_error < 0) { failure = "submit ATRAC packet: " + ffmpeg_error(last_error); av_packet_unref(packet); return false; }
                if (!receive()) { av_packet_unref(packet); return false; }
            }
            av_packet_unref(packet);
        }
        if (last_error != AVERROR_EOF) { failure = "read WAV packets: " + ffmpeg_error(last_error); return false; }
        last_error = avcodec_send_packet(decoder, nullptr);
        if (last_error < 0 || !receive()) {
            if (failure.empty()) failure = "flush ATRAC decoder: " + ffmpeg_error(last_error);
            return false;
        }
        for (;;) {
            const int capacity = swr_get_out_samples(resampler, 0);
            if (capacity <= 0) break;
            const size_t old = decoded.pcm->size();
            decoded.pcm->resize(old + static_cast<size_t>(capacity) * kChannels);
            uint8_t* output[] = {
                reinterpret_cast<uint8_t*>(decoded.pcm->data() + old)
            };
            const int converted =
                swr_convert(resampler, output, capacity, nullptr, 0);
            if (converted < 0) {
                failure = "flush PCM converter: " + ffmpeg_error(converted);
                return false;
            }
            decoded.pcm->resize(old +
                static_cast<size_t>(converted) * kChannels);
            if (!converted) break;
        }
        return !decoded.pcm->empty();
    }();

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resampler);
    avcodec_free_context(&decoder);
    if (format) avformat_close_input(&format);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    if (!ok) {
        decoded = {};
        if (failure.empty()) failure = "decoder returned no PCM";
        return false;
    }
    for (size_t i = 0; i < decoded.pcm->size(); ++i) {
        if ((i & 0x3ffffu) == 0 && cancelled(stop)) {
            failure = "cancelled";
            decoded = {};
            return false;
        }
        const float sample = (*decoded.pcm)[i];
        if (!std::isfinite(sample) || std::abs(sample) > 4.0f) {
            failure = "decoder produced an unsafe PCM sample";
            decoded = {};
            return false;
        }
    }
    const uint32_t fact = fact_samples(riff);
    const size_t frames = decoded.pcm->size() / kChannels;
    const uint64_t expected = fact ?
        (static_cast<uint64_t>(fact) * decoded.sample_rate + source_rate / 2u) /
            source_rate : 0;
    const uint64_t tolerance =
        (8192ull * decoded.sample_rate + source_rate - 1u) / source_rate;
    if (expected && (frames < expected || frames > expected + tolerance)) {
        char detail[128];
        std::snprintf(detail, sizeof detail,
                      "decoded duration %zu does not match RIFF fact %llu",
                      frames, static_cast<unsigned long long>(expected));
        failure = detail;
        decoded = {};
        return false;
    }
    read_loop(riff, source_rate, decoded.sample_rate, decoded);
    return true;
}

#endif

bool valid_music_id(std::string_view id)
{
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char value : id) {
        if (!(value >= 'A' && value <= 'Z') &&
            !(value >= 'a' && value <= 'z') &&
            !(value >= '0' && value <= '9') && value != '_' && value != '-')
            return false;
    }
    return true;
}

} // namespace

uint64_t taiko_audio_hash_bytes(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool taiko_audio_resolve_riff(uint64_t prefix_hash,
                              const std::vector<uint8_t>& prefix,
                              std::vector<uint8_t>& riff,
                              std::string& source,
                              std::string& failure)
{
    {
        std::lock_guard<std::mutex> lock(g_proxy_mutex);
        if (prefix.size() >= 128 && prefix.size() <= g_proxy_riff.size() &&
            std::equal(prefix.begin(), prefix.end(), g_proxy_riff.begin())) {
            riff = g_proxy_riff;
            source = g_proxy_source;
            return true;
        }
    }
    size_t declared = 0;
    if (!riff_declared_size(prefix.data(), prefix.size(), declared)) {
        failure = "invalid RIFF prefix";
        return false;
    }
    if (declared <= prefix.size()) {
        riff.assign(prefix.begin(), prefix.begin() + declared);
        source = "guest-buffer";
        return true;
    }
    if (prefix.size() >= 4096) std::call_once(g_riff_index_once, build_riff_index);
    std::lock_guard<std::mutex> lock(g_riff_index_mutex);
    auto known = g_riff_index.find(prefix_hash);
    if (known != g_riff_index.end() &&
        read_file_member(known->second, &prefix, declared, nullptr, riff)) {
        source = known->second.path;
        return true;
    }
    if (known != g_riff_index.end()) g_riff_index.erase(known);
    const char* root = std::getenv("PS3_VFS_ROOT");
    if (root && *root) {
        const std::filesystem::path directory =
            std::filesystem::path(root) / "data" / "sound" / "bgm" / "nub";
        std::error_code error;
        std::filesystem::directory_iterator entries(directory, error);
        if (!error) {
            for (const auto& entry : entries) {
                std::error_code entry_error;
                if (!entry.is_regular_file(entry_error) || entry_error ||
                    entry.path().extension() != ".nub")
                    continue;
                std::vector<uint8_t> header;
                if (!read_nub_header(entry.path().string(), header)) continue;
                std::vector<size_t> offsets;
                find_riff_members(header, offsets);
                for (size_t offset : offsets) {
                    RiffLocation candidate{entry.path().string(), offset};
                    if (read_file_member(candidate, &prefix, declared, nullptr,
                                         riff)) {
                        g_riff_index[prefix_hash] = candidate;
                        source = candidate.path;
                        return true;
                    }
                }
            }
        }
    }
    failure = "could not resolve the complete RIFF in data/sound/bgm/nub";
    return false;
}

bool taiko_audio_decode_riff(const std::vector<uint8_t>& riff,
                             uint32_t output_rate,
                             const std::atomic<bool>* stop,
                             TaikoDecodedAudio& decoded,
                             std::string& failure)
{
    {
        std::lock_guard<std::mutex> lock(g_proxy_mutex);
        if (!g_proxy_riff.empty() && riff == g_proxy_riff &&
            (!output_rate || output_rate == g_proxy_audio.sample_rate)) {
            decoded = g_proxy_audio;
            return true;
        }
    }
    size_t declared = 0;
    if (!riff_declared_size(riff.data(), riff.size(), declared) ||
        declared != riff.size()) {
        failure = "invalid or incomplete RIFF";
        return false;
    }
    const uint64_t hash = taiko_audio_hash_bytes(riff);
    if (cache_lookup(hash, riff.size(), output_rate, decoded)) return true;
    if (cancelled(stop)) { failure = "cancelled"; return false; }
#ifdef TAIKO_HAVE_FFMPEG
    if (!decode_uncached(riff, output_rate, stop, decoded, failure)) return false;
    decoded.asset_hash = hash;
    cache_insert(hash, riff.size(), output_rate, decoded);
    return true;
#else
    (void)output_rate;
    failure = "this executable was built without in-process ATRAC support";
    return false;
#endif
}

bool taiko_audio_apply_nsh(const std::vector<uint8_t>& nsh,
                            TaikoDecodedAudio& decoded)
{
    decoded.preview_start = 0;
    decoded.song_gain = 1.0f;
    decoded.volume_group = 11;
    const auto be32 = [&nsh](size_t at) {
        return (uint32_t(nsh[at]) << 24) | (uint32_t(nsh[at + 1]) << 16) |
               (uint32_t(nsh[at + 2]) << 8) | nsh[at + 3];
    };
    if (nsh.size() < 0x24 || be32(0) != 0x00020100 || be32(0xc) != 1)
        return false;
    const size_t table = be32(0x18);
    if (table > nsh.size() - 4) return false;
    const size_t entry = be32(table);
    if (entry > nsh.size() || nsh.size() - entry < 0xb4 ||
        be32(entry) != 0x61743300 || be32(entry + 0x90) != 20)
        return false;
    // CnuSound2 (003FE8E0): entry+34 is dB, entry+60 is the group.
    const uint32_t bits = be32(entry + 0x34);
    float db;
    std::memcpy(&db, &bits, sizeof db);
    const uint32_t group = be32(entry + 0x60);
    if (!std::isfinite(db) || db < -100.0f || db > 24.0f || group >= 68)
        return false;
    decoded.song_gain = db <= -100.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
    decoded.volume_group = group;
    // Green's 20-byte user data ends with the preview cue in milliseconds.
    const uint64_t cue = uint64_t(be32(entry + 0xb0)) * decoded.sample_rate / 1000;
    const size_t frames = decoded.pcm ? decoded.pcm->size() / 2 : 0;
    if (cue < frames && (!decoded.has_loop || cue < decoded.loop_end))
        decoded.preview_start = static_cast<size_t>(cue);
    return true;
}

bool taiko_audio_decode_song(std::string_view music_id,
                             uint32_t output_rate,
                             const std::atomic<bool>* stop,
                             TaikoDecodedAudio& decoded,
                             std::string& failure)
{
    if ((music_id.substr(0, 2) == "tc" || music_id.substr(0, 2) == "nj") && taiko_custom_preview)
        return taiko_custom_preview(music_id, output_rate, stop, decoded, failure);
    if (!valid_music_id(music_id)) {
        failure = "invalid music_id";
        return false;
    }
    const char* root = std::getenv("PS3_VFS_ROOT");
    if (!root || !*root) {
        failure = "PS3_VFS_ROOT is not configured";
        return false;
    }
    // Catalog IDs are lowercase; Green's installed song banks are uppercase.
    // Normalize ASCII after validating the ID, including on case-sensitive VFS.
    std::string bank_id(music_id);
    for (char& c : bank_id)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const std::filesystem::path path = std::filesystem::path(root) / "data" /
        "sound" / "bgm" / "nub" / ("SONG_" + bank_id + ".nub");
    std::vector<uint8_t> header;
    if (!read_nub_header(path.string(), header)) {
        failure = "song NUB is unavailable";
        return false;
    }
    std::vector<size_t> offsets;
    find_riff_members(header, offsets);
    if (offsets.empty()) {
        failure = "song NUB has no RIFF member";
        return false;
    }
    size_t declared = 0;
    if (!riff_declared_size(header.data() + offsets.front(),
                            header.size() - offsets.front(), declared)) {
        failure = "song NUB has an invalid RIFF member";
        return false;
    }
    std::vector<uint8_t> riff;
    if (!read_file_member(RiffLocation{path.string(), offsets.front()}, nullptr,
                          declared, stop, riff)) {
        failure = cancelled(stop) ? "cancelled" : "could not read song RIFF";
        return false;
    }
    if (!taiko_audio_decode_riff(riff, output_rate, stop, decoded, failure))
        return false;
    std::vector<uint8_t> nsh;
    const auto nsh_path = path.parent_path().parent_path() / "nsh" /
                          ("SONG_" + bank_id + ".nsh");
    read_nub_header(nsh_path.string(), nsh);
    taiko_audio_apply_nsh(nsh, decoded);
    return true;
}

void taiko_audio_register_custom_proxy(std::vector<uint8_t> riff,
                                       TaikoDecodedAudio decoded, std::string source)
{
    std::lock_guard<std::mutex> lock(g_proxy_mutex);
    g_proxy_riff = std::move(riff);
    g_proxy_audio = std::move(decoded);
    g_proxy_source = std::move(source);
}

bool taiko_audio_decode_file(const std::string& path, uint32_t output_rate,
                            const std::atomic<bool>* stop,
                            TaikoDecodedAudio& decoded, std::string& failure)
{
    const std::filesystem::path native_path(std::u8string(
        reinterpret_cast<const char8_t*>(path.data()), path.size()));
    std::ifstream file(native_path, std::ios::binary | std::ios::ate);
    const auto length = file ? file.tellg() : std::streampos(-1);
    if (length <= 0 || length > std::streampos(kMaximumRiffBytes)) {
        failure = "audio missing, empty, or larger than 256 MiB: " + path;
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        failure = "cannot read audio: " + path;
        return false;
    }
    const auto hash = taiko_audio_hash_bytes(bytes);
    if (cache_lookup(hash, bytes.size(), output_rate, decoded)) {
        decoded.has_loop = false;
        decoded.loop_start = decoded.loop_end = 0;
        return true;
    }
    if (cancelled(stop)) { failure = "cancelled"; return false; }
#ifdef TAIKO_HAVE_FFMPEG
    const bool bank = bytes.size() >= 4 && std::memcmp(bytes.data(), "NUS3", 4) == 0;
    if (!(bank ? decode_bank(bytes, output_rate, stop, decoded, failure) :
                 decode_uncached(bytes, output_rate, stop, decoded, failure))) return false;
    decoded.has_loop = false;
    decoded.loop_start = decoded.loop_end = 0;
    decoded.asset_hash = hash;
    cache_insert(hash, bytes.size(), output_rate, decoded);
    return true;
#else
    failure = "custom audio requires an FFmpeg-enabled build";
    return false;
#endif
}
