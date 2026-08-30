/* Taiko System 357 USIO board emulation.
 *
 * Green talks to the Namco I/O board through libusbd's asynchronous ABI, not
 * the simplified synchronous cellUsbd API in ps3recomp.  This title-local HLE
 * registers context-aware handlers for the nine imports in Green's libusbd
 * cluster, injects a PS3A-USJ USB device, and implements the small register
 * protocol the game uses for cabinet switches, drum sensors, and backup SRAM.
 *
 * The wire values and descriptors mirror TaikoZucchini's proven virtual USIO
 * implementation.  All USB buffers and descriptors below are guest effective
 * addresses; callbacks are guest OPDs and are dispatched through ppu_guest_call.
 */

#include "taiko_card.h"  /* virtual BanaPassport on the reader */
#include "taiko_tls.h"   /* online redirect state */
#include "ppu_recomp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <ps3emu/host_platform.h>
#include "taiko_host_input.h"

#ifdef PS3RECOMP_INPUT_BACKEND_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#endif

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name,
                                      void (*handler)(ppu_context*));
extern "C" uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1,
                                   uint64_t a2, uint64_t a3);

namespace {

constexpr uint32_t kFakeDevice = 0x70000001u;
constexpr uint32_t kPipeControl = 0x70004001u;
constexpr uint32_t kPipeIn = 0x70004002u;
constexpr uint32_t kPipeOut = 0x70004003u;

/* Shared title-local guest scratch sits in the unused gap documented by
 * taiko_net.c (the image ends near 0x01500000; runtime arenas start at
 * 0x0D000000). */
constexpr uint32_t kDescriptorEa = 0x0C010000u;

constexpr std::array<uint8_t, 50> kDescriptors = {
    /* Device. */
    0x12, 0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x40,
    0x9A, 0x0B, 0x00, 0x09, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    /* Configuration. */
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    /* Interface. */
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00,
    /* Bulk IN 0x82, bulk OUT 0x01. */
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00,
};

constexpr std::array<uint8_t, 64> kKeepalive = {
    0x7E, 0xE4, 0x00, 0x00, 0x74, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::array<uint8_t, 16> kReaderStatus = {
    /* Byte 2 is replaced with the pending PN53x response length below. */
    0x02, 0x03, 0x06, 0x00, 0xFF, 0x0F, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x10, 0x00,
};

constexpr std::array<uint8_t, 64> kFpgaIdent = {
    0x8F, 0x2A, 0x49, 0x54, 0x41, 0x49, 0x4B, 0x4F,
    0x00, 0x11, 0x22, 0x33, 0xDE, 0xAD, 0xBE, 0xEF,
    0x7C, 0xA1, 0x4D, 0x93, 0x2B, 0xFE, 0x06, 0x88,
    0x55, 0x19, 0x6E, 0xBD, 0x3A, 0xC4, 0x12, 0x7F,
    0x90, 0x0D, 0xE2, 0x33, 0x51, 0x47, 0xA9, 0xBC,
    0x0F, 0xD1, 0x78, 0x24, 0x66, 0xAB, 0xC0, 0xD4,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
};

constexpr std::array<uint8_t, 0x180> kFirmwareInfo = {
    0x4E, 0x42, 0x47, 0x49, 0x2E, 0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30,
    0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20,
    0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x42, 0x47, 0x49, 0x31,
    0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E,
    0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20,
    0x50, 0x50, 0x47, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x03, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x75, 0x6C, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x42, 0x47, 0x49, 0x32, 0x3B, 0x55, 0x53, 0x49, 0x4F,
    0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74,
    0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x02, 0x00, 0x08, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x75, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

enum Action : uint32_t {
    kHitSl=TAIKO_ACTION_HIT_SL,kHitCl=TAIKO_ACTION_HIT_CL,
    kHitCr=TAIKO_ACTION_HIT_CR,kHitSr=TAIKO_ACTION_HIT_SR,
    kEnter=TAIKO_ACTION_ENTER,kService=TAIKO_ACTION_SERVICE,
    kTest=TAIKO_ACTION_TEST,kCoin=TAIKO_ACTION_COIN,
    kUp=TAIKO_ACTION_UP,kDown=TAIKO_ACTION_DOWN,
};

struct UsioState {
    bool initialized{};
    bool attached{};
    bool write_active{};
    bool idle_in_pending{};
    bool staged_zlp{};
    uint8_t write_channel{};
    uint16_t write_reg{};
    uint16_t write_remaining{};
    uint16_t write_total{};
    std::array<uint8_t, 0x2000> write_buffer{};
    std::array<uint8_t, 128> reader_rx{};
    uint16_t reader_rx_len{};
    std::array<uint8_t, 4352> reader_tx{};
    uint16_t reader_tx_len{};
    std::array<uint8_t, 0x2000> staged{};
    uint16_t staged_len{};
    uint16_t staged_pos{};
    std::array<std::array<uint8_t, 0x2000>, 2> sram{};
    std::array<uint8_t, 0x60> last_frame{};
    bool last_frame_valid{};
    uint16_t coin_counter{};
    bool test_on{};
    uint32_t previous_action[2]{};
    uint8_t hit_cooldown[2][4]{};
    unsigned trace_budget{96};
    unsigned input_trace_budget{};
    int64_t input_poll_window_qpc{};
    int64_t input_poll_last_qpc{};
    int64_t input_poll_min_qpc{INT64_MAX};
    int64_t input_poll_max_qpc{};
    unsigned input_poll_count{};
    unsigned online_trace_tick{};
    bool offline_state_applied{};
    bool chassis_flags_applied{};
};

UsioState g_usio;

/* Gameplay input must not depend on how often Wine dispatches window
 * messages, nor on whether a complete key-down/key-up falls between two
 * guest USIO reads.  A dedicated high-priority sampler publishes current
 * levels and latches every rising edge until build_input_frames consumes it.
 *
 * Keyboard is sampled at 1 kHz. XInput is relatively expensive under Wine,
 * so controllers are sampled at 250 Hz and combined into the same latch. The
 * guest still consumes virtual-board reports at its requested cadence (60 Hz
 * in the current title path); physical observation is no longer limited by
 * that cadence. */
#ifdef PS3RECOMP_INPUT_BACKEND_WIN32
HANDLE g_input_thread{};
HANDLE g_input_stop_event{};
#endif

constexpr uint16_t kReaderFrameWait = 0xFFFFu;
constexpr uint32_t kBackupMagic = 0x55534942u; /* 'USIB' */
constexpr uint32_t kBackupVersion = 1u;

uint32_t read_be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void write_be32(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

uint32_t backup_crc32(const uint8_t* p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u &
                                static_cast<uint32_t>(-
                                    static_cast<int32_t>(crc & 1u)));
    }
    return ~crc;
}

/* Green unconditionally deserializes two Boost binary archives from offset
 * 0x1000 of each backup-SRAM page during startup.  A real cabinet has already
 * provisioned SRAM, but a fresh recomp directory has no usiobackup.bin.  All
 * zeros therefore look like an archive with a zero-length signature and Boost
 * aborts while the preceding XML load happens to be the last visible action.
 *
 * Seed only the two empty archive records.  The rest of the page remains
 * erased/zeroed and the title can populate its ordinary cabinet state itself.
 * This is deliberately generated state rather than a bundled user backup. */
constexpr std::array<uint8_t, 0x60> kBlankArchiveState = {
    0x00, 0x00, 0x00, 0x16, 0x73, 0x65, 0x72, 0x69,
    0x61, 0x6c, 0x69, 0x7a, 0x61, 0x74, 0x69, 0x6f,
    0x6e, 0x3a, 0x3a, 0x61, 0x72, 0x63, 0x68, 0x69,
    0x76, 0x65, 0x00, 0x0a, 0x04, 0x04, 0x04, 0x08,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24,
    0x00, 0x00, 0x00, 0x16, 0x73, 0x65, 0x72, 0x69,
    0x61, 0x6c, 0x69, 0x7a, 0x61, 0x74, 0x69, 0x6f,
    0x6e, 0x3a, 0x3a, 0x61, 0x72, 0x63, 0x68, 0x69,
    0x76, 0x65, 0x00, 0x0a, 0x04, 0x04, 0x04, 0x08,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

bool backup_archives_valid()
{
    constexpr size_t kSignatureRecordSize = 4 + 22;
    for (const auto& page : g_usio.sram) {
        if (!std::equal(kBlankArchiveState.begin(),
                        kBlankArchiveState.begin() + kSignatureRecordSize,
                        page.begin() + 0x1000) ||
            !std::equal(kBlankArchiveState.begin() + 0x30,
                        kBlankArchiveState.begin() + 0x30 + kSignatureRecordSize,
                        page.begin() + 0x1030))
            return false;
    }
    return true;
}

void initialize_blank_backup()
{
    for (auto& page : g_usio.sram)
        std::copy(kBlankArchiveState.begin(), kBlankArchiveState.end(),
                  page.begin() + 0x1000);
    std::fprintf(stderr,
                 "[taiko_usio] initialized clean SRAM archive state\n");
}

std::string backup_path()
{
    const char* root = std::getenv("PS3_VFS_ROOT");
    if (!root || !*root)
        return {};
    std::string path(root);
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path.push_back('/');
    const char* layout = std::getenv("PS3_VFS_LAYOUT");
    if (layout && std::strcmp(layout, "usrdir") == 0)
        path += "usiobackup.bin";
    else
        path += "game/SCEEXE001/USRDIR/usiobackup.bin";
    return path;
}

bool load_backup()
{
    const std::string path = backup_path();
    if (path.empty())
        return false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        std::fprintf(stderr, "[taiko_usio] SRAM backup missing: %s\n",
                     path.c_str());
        return false;
    }

    std::array<uint8_t, 16> header{};
    const size_t bytes = sizeof(g_usio.sram);
    const bool header_ok = std::fread(header.data(), 1, header.size(), file) ==
                               header.size() &&
                           read_be32(header.data()) == kBackupMagic &&
                           read_be32(header.data() + 4) == kBackupVersion &&
                           read_be32(header.data() + 8) == bytes;
    const bool data_ok = header_ok &&
                         std::fread(g_usio.sram.data(), 1, bytes, file) == bytes;
    std::fclose(file);
    const uint32_t actual_crc = data_ok
        ? backup_crc32(reinterpret_cast<const uint8_t*>(g_usio.sram.data()), bytes)
        : 0;
    const bool ok = data_ok && actual_crc == read_be32(header.data() + 12);
    if (!ok) {
        for (auto& page : g_usio.sram)
            page.fill(0);
        std::fprintf(stderr, "[taiko_usio] SRAM backup invalid: %s\n",
                     path.c_str());
        return false;
    }
    std::fprintf(stderr, "[taiko_usio] loaded SRAM backup (%zu bytes, crc=%08X)\n",
                 bytes, actual_crc);
    return true;
}

void save_backup()
{
    const std::string path = backup_path();
    if (path.empty())
        return;
    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[taiko_usio] cannot save SRAM backup: %s\n",
                     temporary.c_str());
        return;
    }

    const size_t bytes = sizeof(g_usio.sram);
    const uint32_t crc = backup_crc32(
        reinterpret_cast<const uint8_t*>(g_usio.sram.data()), bytes);
    std::array<uint8_t, 16> header{};
    write_be32(header.data(), kBackupMagic);
    write_be32(header.data() + 4, kBackupVersion);
    write_be32(header.data() + 8, static_cast<uint32_t>(bytes));
    write_be32(header.data() + 12, crc);
    bool ok = std::fwrite(header.data(), 1, header.size(), file) == header.size();
    ok = std::fwrite(g_usio.sram.data(), 1, bytes, file) == bytes && ok;
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        std::remove(temporary.c_str());
        std::fprintf(stderr, "[taiko_usio] failed writing SRAM backup\n");
        return;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(path.c_str());
        if (std::rename(temporary.c_str(), path.c_str()) != 0) {
            std::remove(temporary.c_str());
            std::fprintf(stderr, "[taiko_usio] failed replacing SRAM backup\n");
            return;
        }
    }
    std::fprintf(stderr, "[taiko_usio] saved SRAM backup (crc=%08X)\n", crc);
}

uint16_t reader_frame_length(const uint8_t* data, uint16_t length)
{
    if (!length) return 0;
    if (data[0] == 0x55) return 1;
    if (data[0] != 0x00) return 0;
    if (length == 1) return kReaderFrameWait;
    if (data[1] != 0x00) return 0;
    if (length == 2) return kReaderFrameWait;
    if (data[2] != 0xFF) return 0;
    if (length < 5) return kReaderFrameWait;
    if (data[3] == 0x00 && data[4] == 0xFF) {
        if (length < 6) return kReaderFrameWait;
        return data[5] == 0x00 ? 6 : 0;
    }
    return static_cast<uint16_t>(data[3] + 7);
}

size_t build_reader_response(uint8_t response_command, const uint8_t* data,
                             size_t data_length, uint8_t* out, size_t capacity)
{
    const size_t frame_length = data_length + 9;
    if (frame_length > capacity || data_length > 0xFD) return 0;

    const uint8_t payload_length = static_cast<uint8_t>(data_length + 2);
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0xFF;
    out[3] = payload_length;
    out[4] = static_cast<uint8_t>(0u - payload_length);
    out[5] = 0xD5;
    out[6] = response_command;
    if (data_length) std::memcpy(out + 7, data, data_length);

    uint8_t sum = 0;
    for (size_t i = 0; i < data_length + 7; ++i)
        sum = static_cast<uint8_t>(sum + out[i]);
    out[data_length + 7] = static_cast<uint8_t>(0xFFu - sum);
    out[data_length + 8] = 0x00;
    return frame_length;
}

size_t process_reader_request(const uint8_t* rx, size_t rx_length,
                              uint8_t* tx, size_t tx_capacity)
{
    if (rx_length == 1 && rx[0] == 0x55) return 0;
    if (rx_length < 7 || rx[0] != 0x00 || rx[1] != 0x00 ||
        rx[2] != 0xFF || rx[5] != 0xD4)
        return 0;

    /* Card-dependent commands first: with a card on the reader they answer
     * with it, and without one they fall through to the "empty field"
     * replies below. */
    if (const size_t card_length =
            taiko_card_process(rx, rx_length, tx, tx_capacity))
        return card_length;

    const uint8_t command = rx[6];
    switch (command) {
    case 0x06: {
        static constexpr uint8_t a[8] =
            {0xFF, 0x3F, 0x0E, 0xF1, 0xFF, 0x3F, 0x0E, 0xF1};
        static constexpr uint8_t b[11] =
            {0xDC, 0xF4, 0x3F, 0x11, 0x4D, 0x85, 0x61, 0xF1, 0x26, 0x6A, 0x87};
        return rx_length > 8 && rx[8] == 0x1C
            ? build_reader_response(0x07, a, sizeof(a), tx, tx_capacity)
            : build_reader_response(0x07, b, sizeof(b), tx, tx_capacity);
    }
    case 0x08: {
        static constexpr uint8_t data[1] = {0x00};
        return build_reader_response(0x09, data, sizeof(data), tx, tx_capacity);
    }
    case 0x0C: {
        static constexpr uint8_t data[3] = {0x00, 0x06, 0x00};
        return build_reader_response(0x0D, data, sizeof(data), tx, tx_capacity);
    }
    case 0x0E: return build_reader_response(0x0F, nullptr, 0, tx, tx_capacity);
    case 0x12: return build_reader_response(0x13, nullptr, 0, tx, tx_capacity);
    case 0x18: return build_reader_response(0x19, nullptr, 0, tx, tx_capacity);
    case 0x32: return build_reader_response(0x33, nullptr, 0, tx, tx_capacity);
    case 0x40: {
        /* Startup has no card inserted.  Report a normal PN53x target error. */
        static constexpr uint8_t data[1] = {0x01};
        return build_reader_response(0x41, data, sizeof(data), tx, tx_capacity);
    }
    case 0x44: {
        static constexpr uint8_t data[2] = {0x01, 0x00};
        return build_reader_response(0x45, data, sizeof(data), tx, tx_capacity);
    }
    case 0x4A: {
        /* Reader is healthy, but there is currently no card in its field. */
        static constexpr uint8_t data[3] = {0x00, 0x00, 0x00};
        return build_reader_response(0x4B, data, sizeof(data), tx, tx_capacity);
    }
    case 0x52: {
        static constexpr uint8_t data[2] = {0x01, 0x00};
        return build_reader_response(0x53, data, sizeof(data), tx, tx_capacity);
    }
    case 0x54: {
        static constexpr uint8_t data[1] = {0x00};
        return build_reader_response(0x55, data, sizeof(data), tx, tx_capacity);
    }
    case 0xA0: {
        static constexpr uint8_t data[1] = {0x01};
        return build_reader_response(0xA1, data, sizeof(data), tx, tx_capacity);
    }
    default:
        std::fprintf(stderr, "[taiko_card] unhandled PN53x command %02X\n", command);
        return 0;
    }
}

void queue_reader_response(const uint8_t* data, size_t length)
{
    if (!length) return;
    if (length > g_usio.reader_tx.size() - g_usio.reader_tx_len)
        g_usio.reader_tx_len = 0;
    if (length > g_usio.reader_tx.size()) return;
    std::copy_n(data, length, g_usio.reader_tx.begin() + g_usio.reader_tx_len);
    g_usio.reader_tx_len += static_cast<uint16_t>(length);
}

void feed_reader(const uint8_t* data, uint16_t length)
{
    if (!length) return;
    if (length > g_usio.reader_rx.size() - g_usio.reader_rx_len)
        g_usio.reader_rx_len = 0;
    if (length > g_usio.reader_rx.size()) return;
    std::copy_n(data, length, g_usio.reader_rx.begin() + g_usio.reader_rx_len);
    g_usio.reader_rx_len += length;

    while (g_usio.reader_rx_len) {
        const uint16_t frame_length = reader_frame_length(
            g_usio.reader_rx.data(), g_usio.reader_rx_len);
        if (frame_length == kReaderFrameWait || frame_length > g_usio.reader_rx_len)
            return;
        if (!frame_length) {
            std::move(g_usio.reader_rx.begin() + 1,
                      g_usio.reader_rx.begin() + g_usio.reader_rx_len,
                      g_usio.reader_rx.begin());
            --g_usio.reader_rx_len;
            continue;
        }

        uint8_t response[128]{};
        const size_t response_length = process_reader_request(
            g_usio.reader_rx.data(), frame_length, response, sizeof(response));
        const uint8_t command = frame_length >= 7 ? g_usio.reader_rx[6] : 0xFF;
        queue_reader_response(response, response_length);
        /* The game polls the idle reader more than a hundred times per second.
         * Logging every healthy request contends with the render/main threads
         * on stderr and produced tens of thousands of lines per boot. */
        static const bool trace_reader = std::getenv("TAIKO_CARD_TRACE") != nullptr;
        if (trace_reader)
            std::fprintf(stderr,
                         "[taiko_card] PN53x command=%02X request=%u response=%zu pending=%u\n",
                         command, frame_length, response_length, g_usio.reader_tx_len);

        g_usio.reader_rx_len -= frame_length;
        if (g_usio.reader_rx_len)
            std::move(g_usio.reader_rx.begin() + frame_length,
                      g_usio.reader_rx.begin() + frame_length + g_usio.reader_rx_len,
                      g_usio.reader_rx.begin());
    }
}

void callback(uint32_t opd, int32_t result, int32_t count, uint32_t arg)
{
    if (opd)
        ppu_guest_call(opd, static_cast<uint32_t>(result),
                       static_cast<uint32_t>(count), arg, 0);
}

#ifdef PS3RECOMP_INPUT_BACKEND_WIN32
bool key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

uint32_t keyboard_actions(unsigned player)
{
    uint32_t a = 0;
    if (player == 0) {
        if (key_down('D')) a |= kHitSl;
        if (key_down('F')) a |= kHitCl;
        if (key_down('J')) a |= kHitCr;
        if (key_down('K')) a |= kHitSr;
    } else {
        if (key_down('Z')) a |= kHitSl;
        if (key_down('X')) a |= kHitCl;
        if (key_down('C')) a |= kHitCr;
        if (key_down('V')) a |= kHitSr;
    }
    if (player != 0) return a;
    if (key_down(VK_RETURN)) a |= kEnter;
    if (key_down(VK_F2)) a |= kCoin;
    if (key_down(VK_F6)) a |= kService;
    if (key_down(VK_F1)) a |= kTest;
    if (key_down(VK_UP)) a |= kUp;
    if (key_down(VK_DOWN)) a |= kDown;
    return a;
}

uint32_t controller_actions(unsigned player)
{
    XINPUT_STATE state{};
    if (XInputGetState(player, &state) != ERROR_SUCCESS)
        return 0;
    const auto b = state.Gamepad.wButtons;
    uint32_t a = 0;
    if (b & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP |
             XINPUT_GAMEPAD_LEFT_SHOULDER)) a |= kHitSl;
    if (state.Gamepad.bLeftTrigger > 30) a |= kHitSl;
    if (b & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN)) a |= kHitCl;
    if (b & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_A)) a |= kHitCr;
    if (b & (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B |
             XINPUT_GAMEPAD_RIGHT_SHOULDER)) a |= kHitSr;
    if (state.Gamepad.bRightTrigger > 30) a |= kHitSr;
    if (b & XINPUT_GAMEPAD_START) a |= kEnter;
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB) a |= kService;
    if (b & XINPUT_GAMEPAD_BACK) a |= kTest;
    if (b & XINPUT_GAMEPAD_LEFT_THUMB) a |= kCoin;
    if (b & XINPUT_GAMEPAD_DPAD_UP) a |= kUp;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) a |= kDown;
    return a;
}
#endif

constexpr uint32_t kHitBits[4] = {kHitSl, kHitCl, kHitCr, kHitSr};

bool input_trace_enabled()
{
    static const bool enabled = std::getenv("TAIKO_INPUT_TRACE") != nullptr;
    return enabled;
}

#ifdef PS3RECOMP_INPUT_BACKEND_WIN32
DWORD WINAPI input_sampler_main(void*)
{
    timeBeginPeriod(1);

    HANDLE timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (timer) {
        LARGE_INTEGER first{};
        first.QuadPart = -10000; /* Relative 1 ms, in 100 ns units. */
        if (!SetWaitableTimer(timer, &first, 1, nullptr, nullptr, FALSE)) {
            CloseHandle(timer);
            timer = nullptr;
        }
    }

    uint32_t previous[2]{};
    uint32_t controllers[2]{};
    unsigned controller_tick = 0;
    LARGE_INTEGER frequency{};
    LARGE_INTEGER trace_start{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&trace_start);
    unsigned samples = 0;

    taiko_host_input_set_active(1);
    for (;;) {
        if ((controller_tick++ & 3u) == 0) {
            controllers[0] = controller_actions(0);
            controllers[1] = controller_actions(1);
        }

        const uint32_t actions[2] = {
            keyboard_actions(0) | controllers[0],
            keyboard_actions(1) | controllers[1]};
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const uint64_t now_ns = ps3_host_monotonic_ns();
        for (unsigned player = 0; player < 2; ++player) {
            taiko_host_input_update_levels(player, actions[player], now_ns);
            previous[player] = actions[player];
        }

        ++samples;
        if (input_trace_enabled() &&
            now.QuadPart - trace_start.QuadPart >= frequency.QuadPart) {
            const double seconds = static_cast<double>(
                now.QuadPart - trace_start.QuadPart) / frequency.QuadPart;
            std::fprintf(stderr, "[taiko_input] sampler %.1f Hz\n",
                         samples / seconds);
            samples = 0;
            trace_start = now;
        }

        DWORD wait_result;
        if (timer) {
            HANDLE waits[2] = {g_input_stop_event, timer};
            wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        } else {
            wait_result = WaitForSingleObject(g_input_stop_event, 1);
        }
        if (wait_result == WAIT_OBJECT_0)
            break;
    }

    taiko_host_input_set_active(0);
    if (timer) {
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }
    timeEndPeriod(1);
    return 0;
}

void stop_input_sampler()
{
    if (g_input_stop_event)
        SetEvent(g_input_stop_event);
    if (g_input_thread) {
        WaitForSingleObject(g_input_thread, INFINITE);
        CloseHandle(g_input_thread);
        g_input_thread = nullptr;
    }
    if (g_input_stop_event) {
        CloseHandle(g_input_stop_event);
        g_input_stop_event = nullptr;
    }
    taiko_host_input_set_active(0);
}

void start_input_sampler()
{
    stop_input_sampler();
    taiko_host_input_reset();

    g_input_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_input_stop_event) {
        std::fprintf(stderr,
                     "[taiko_input] cannot create sampler stop event (%lu); "
                     "using guest-poll fallback\n",
                     GetLastError());
        return;
    }
    g_input_thread = CreateThread(nullptr, 0, input_sampler_main, nullptr, 0,
                                  nullptr);
    if (!g_input_thread) {
        std::fprintf(stderr,
                     "[taiko_input] cannot create sampler thread (%lu); "
                     "using guest-poll fallback\n",
                     GetLastError());
        CloseHandle(g_input_stop_event);
        g_input_stop_event = nullptr;
        return;
    }
    SetThreadPriority(g_input_thread, THREAD_PRIORITY_HIGHEST);
    std::fprintf(stderr,
                 "[taiko_input] direct sampler started "
                 "(keyboard=1000 Hz, XInput=250 Hz)\n");
}
#elif defined(PS3RECOMP_INPUT_BACKEND_SDL3)
void stop_input_sampler() { taiko_host_input_set_active(0); }
void start_input_sampler()
{
    taiko_host_input_reset();
    taiko_host_input_set_active(1);
    std::fprintf(stderr, "[taiko_input] SDL3 event input provider\n");
}
#else
void stop_input_sampler() {}
void start_input_sampler()
{
    taiko_host_input_reset();
    std::fprintf(stderr, "[taiko_input] native headless zero-input provider\n");
}
#endif

void trace_online_state()
{
    /* Green S11113 network readiness globals, confirmed against the matching
     * Zucchini online diagnostic.  Sampling here is safe because register 1080
     * is polled continuously after the game's managers have been constructed. */
    constexpr uint32_t kAllnetPtr = 0x0102D9A4u;
    constexpr uint32_t kOnlineCheckPtr = 0x01028F1Cu;
    constexpr uint32_t kServiceStatePtr = 0x0102B72Cu;
    constexpr uint32_t kIndicatorPtr = 0x0102B5C4u;
    constexpr uint32_t kNetContextPtr = 0x01039280u;

    if (g_usio.online_trace_tick++ % 60 != 0)
        return;

    const uint32_t allnet = vm_read32(kAllnetPtr);
    const uint32_t online = vm_read32(kOnlineCheckPtr);
    const uint32_t service = vm_read32(kServiceStatePtr);
    const uint32_t indicator = vm_read32(kIndicatorPtr);
    const uint32_t context = vm_read32(kNetContextPtr);

    const uint32_t auth = allnet ? vm_read32(allnet + 0x3C) : 0;
    const uint32_t online_state = online ? vm_read32(online) : 0;
    const uint32_t service_state = service ? vm_read32(service) : 0;
    const uint8_t ready = online ? vm_read8(online + 7) : 0;
    const uint8_t block = online ? vm_read8(online + 5) : 0;
    const uint8_t indicator_online = indicator ? vm_read8(indicator + 2) : 0;
    const uint8_t context_ready = context ? vm_read8(context) : 0;

    std::fprintf(stderr,
                 "[taiko_netstate] ptr=%08X/%08X/%08X/%08X/%08X "
                 "auth=%08X online_state=%08X service=%08X ready=%u block=%u indicator=%u ctx=%u\n",
                 allnet, online, service, indicator, context, auth,
                 online_state, service_state, ready, block, indicator_online,
                 context_ready);
}

extern "C" uint32_t g_main_toc;   /* set by the loader from the ELF's TOC */

/* The chassis operator flags, as the game holds them at runtime.
 *
 * `data/config/S11100-1/chassisinfo.xml` is a list of <Info> records keyed by
 * a numeric dongle serial; the loader (FUN_001ABF4C) copies the record that
 * matches this cabinet into an 18-byte flag block and sets byte 0 to 1. The
 * block's address is a TOC global, and the byte order is the XML element
 * order, which the game's own [ChassisInfo] dump confirms.
 *
 * `ignore_mucha_invalid_enforced` (byte 12) is the one that matters here: with
 * it clear, the boot network check enforces a valid MUCHA licence and fails on
 * a private server that only answers boardauth. Zucchini defaults the same
 * flag on, and does it by synthesising the XML; we have the parsed block. */
constexpr uint32_t kChassisFlagsTocOffset = 0x5F1Cu;
constexpr int kChassisFlagCount = 18;
constexpr int kChassisForceOffline = 2;

const char* const kChassisFlagNames[kChassisFlagCount] = {
    "is_registered", "is_promotion", "force_offline", "force_freeplay",
    "force_autoplay", "force_serious", "force_musicinfo_allrelease",
    "force_burst_mode", "ignore_network_authentication",
    "ignore_network_connection", "ignore_closetime", "ignore_nblinepoint",
    "ignore_mucha_invalid_enforced", "disable_countdowntimer",
    "anytime_tokkun", "anytime_dani", "force_dani", "anytime_ghostbattle",
};

uint32_t chassis_flags_ea()
{
    if (!g_main_toc) return 0;
    return vm_read32(g_main_toc + kChassisFlagsTocOffset);
}

void settle_chassis_flags()
{
    if (g_usio.chassis_flags_applied)
        return;

    const uint32_t flags = chassis_flags_ea();
    if (!flags || vm_read8(flags) == 0)       /* byte 0: no record loaded yet */
        return;

    /* A cabinet that is talking to a server is not an offline cabinet, and the
     * dump's record for this dongle sets force_offline. Everything else is
     * left as the operator's XML has it. TAIKO_CHASSIS_FLAGS overrides any of
     * them by name, e.g. "force_offline=0,ignore_network_connection=1". */
    uint8_t wanted[kChassisFlagCount];
    for (int i = 0; i < kChassisFlagCount; ++i)
        wanted[i] = static_cast<uint8_t>(vm_read8(flags + i) & 1);
    if (taiko_online_enabled())
        wanted[kChassisForceOffline] = 0;

    if (const char* overrides = std::getenv("TAIKO_CHASSIS_FLAGS")) {
        std::string spec(overrides);
        size_t pos = 0;
        while (pos < spec.size()) {
            size_t comma = spec.find(',', pos);
            if (comma == std::string::npos) comma = spec.size();
            const std::string item = spec.substr(pos, comma - pos);
            pos = comma + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            const std::string name = item.substr(0, eq);
            const uint8_t value = item.substr(eq + 1) != "0" ? 1 : 0;
            bool matched = false;
            for (int i = 0; i < kChassisFlagCount; ++i) {
                if (name == kChassisFlagNames[i]) { wanted[i] = value; matched = true; break; }
            }
            if (!matched)
                std::fprintf(stderr, "[taiko_chassis] unknown flag '%s'\n", name.c_str());
        }
    }

    std::string dump;
    for (int i = 0; i < kChassisFlagCount; ++i) {
        const uint8_t was = static_cast<uint8_t>(vm_read8(flags + i) & 1);
        if (was != wanted[i])
            vm_write8(flags + i, wanted[i]);
        dump += kChassisFlagNames[i];
        dump += '=';
        dump += static_cast<char>('0' + wanted[i]);
        if (was != wanted[i]) {
            dump += "(was ";
            dump += static_cast<char>('0' + was);
            dump += ')';
        }
        dump += ' ';
    }
    std::fprintf(stderr, "[taiko_chassis] block=%08X %s\n", flags, dump.c_str());
    g_usio.chassis_flags_applied = true;
}

void settle_offline_network_state()
{
    /* The real initial-data callback leaves OnlineCheck in state 2 on success
     * or state 3 on failure.  With no arcade service connected the callback
     * never arrives, so Green remains in state 0 forever.  Reproduce the
     * callback's short failure branch: clear its result flags and raise the
     * global "initial data unavailable" bit as well as completing the state.
     * Downstream scene code polls that global bit, not just OnlineCheck::state. */
    static const char* setting = std::getenv("TAIKO_OFFLINE_COMPLETE");
    static const bool enabled = setting && std::strcmp(setting, "0") != 0;
    /* With a server configured the real callback can arrive, so forcing the
     * failure branch would cut the online path off before it starts. */
    if (!enabled || taiko_online_enabled() || g_usio.offline_state_applied)
        return;

    constexpr uint32_t kOnlineCheckPtr = 0x01028F1Cu;
    constexpr uint32_t kNetworkFlagsPtr = 0x010290A4u;
    constexpr uint32_t kNetContextPtr = 0x01039280u;
    const uint32_t online = vm_read32(kOnlineCheckPtr);
    const uint32_t network_flags = vm_read32(kNetworkFlagsPtr);
    const uint32_t context = vm_read32(kNetContextPtr);
    if (!online || !network_flags || !context || vm_read8(context) == 0)
        return;

    const uint32_t state = vm_read32(online);
    if (state == 0) {
        vm_write32(online, 3);
        vm_write8(online + 4, 0);
        vm_write8(online + 5, 0);
        vm_write8(online + 6, 0);
        vm_write8(online + 7, 0);
        vm_write8(online + 8, 0);
        vm_write64(network_flags, vm_read64(network_flags) | 0x10000000ull);
        std::fprintf(stderr,
                     "[taiko_netstate] completed unavailable initial-data check: "
                     "state 0 -> 3, unavailable bit set\n");
    }
    g_usio.offline_state_applied = true;
}

void write_le16(std::array<uint8_t, 0x60>& out, size_t offset, uint16_t value)
{
    out[offset] = static_cast<uint8_t>(value);
    out[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void trace_input_poll_rate()
{
#ifdef PS3RECOMP_INPUT_BACKEND_NULL
    return;
#else
    if (!input_trace_enabled())
        return;

    const int64_t now = static_cast<int64_t>(ps3_host_monotonic_ns());
    if (!g_usio.input_poll_window_qpc) {
        g_usio.input_poll_window_qpc = now;
        g_usio.input_poll_last_qpc = now;
        return;
    }

    const int64_t delta = now - g_usio.input_poll_last_qpc;
    g_usio.input_poll_last_qpc = now;
    g_usio.input_poll_min_qpc = std::min(g_usio.input_poll_min_qpc, delta);
    g_usio.input_poll_max_qpc = std::max(g_usio.input_poll_max_qpc, delta);
    ++g_usio.input_poll_count;

    const int64_t elapsed = now - g_usio.input_poll_window_qpc;
    if (elapsed < 1000000000ll)
        return;

    const double seconds = static_cast<double>(elapsed) / 1000000000.0;
    const double qpc_to_ms = 1.0 / 1000000.0;
    std::fprintf(stderr,
                 "[taiko_input] usio %.2f Hz interval=%.3f..%.3fms\n",
                 g_usio.input_poll_count / seconds,
                 g_usio.input_poll_min_qpc * qpc_to_ms,
                 g_usio.input_poll_max_qpc * qpc_to_ms);
    g_usio.input_poll_window_qpc = now;
    g_usio.input_poll_min_qpc = INT64_MAX;
    g_usio.input_poll_max_qpc = 0;
    g_usio.input_poll_count = 0;
#endif
}

void build_input_frames()
{
    uint32_t actions[2]{};
    uint32_t rising[2]{};
#ifndef PS3RECOMP_INPUT_BACKEND_NULL
    taiko_host_input_snapshot input{};
    taiko_host_input_consume(&input);
    if (input.active) {
        for (unsigned player = 0; player < 2; ++player) {
            actions[player] = input.levels[player];
            rising[player] = input.rising[player];
        }
#ifdef PS3RECOMP_INPUT_BACKEND_WIN32
    } else {
        /* Initialization/thread-creation fallback. It retains the old
         * behaviour, but normal play always uses the sampler above. */
        actions[0] = controller_actions(0) | keyboard_actions(0);
        actions[1] = controller_actions(1) | keyboard_actions(1);
        rising[0] = actions[0] & ~g_usio.previous_action[0];
        rising[1] = actions[1] & ~g_usio.previous_action[1];
#endif
    }
#endif

    settle_offline_network_state();
    settle_chassis_flags();
    trace_online_state();
    trace_input_poll_rate();
    g_usio.previous_action[0] = actions[0];
    g_usio.previous_action[1] = actions[1];
    if ((rising[0] | rising[1]) & kCoin)
        ++g_usio.coin_counter;
    if ((rising[0] | rising[1]) & kTest)
        g_usio.test_on = !g_usio.test_on;
#ifndef PS3RECOMP_INPUT_BACKEND_NULL
    if (std::getenv("TAIKO_INPUT_E2E_TRACE") &&
        ((rising[0] | rising[1]) & (kHitSl | kHitCl | kHitCr | kHitSr))) {
        uint64_t oldest = UINT64_MAX;
        for (unsigned player = 0; player < 2; ++player)
            for (unsigned hit = 0; hit < 4; ++hit)
                if ((rising[player] & kHitBits[hit]) &&
                    input.hit_timestamp_ns[player][hit])
                    oldest = std::min(oldest,
                                      input.hit_timestamp_ns[player][hit]);
        if (oldest != UINT64_MAX)
            taiko_host_input_trace_consumed(oldest, ps3_host_monotonic_ns());
    }
    if (input_trace_enabled() &&
        (g_usio.input_trace_budget || rising[0] || rising[1])) {
        if (g_usio.input_trace_budget) --g_usio.input_trace_budget;
        double oldest_hit_ms = 0.0;
        const uint64_t now = ps3_host_monotonic_ns();
        for (unsigned player = 0; player < 2; ++player) {
            for (unsigned hit = 0; hit < 4; ++hit) {
                if (!(rising[player] & kHitBits[hit]))
                    continue;
                const uint64_t sampled = input.hit_timestamp_ns[player][hit];
                if (sampled) {
                    const double age = (now - sampled) / 1000000.0;
                    oldest_hit_ms = std::max(oldest_hit_ms, age);
                }
            }
        }
        std::fprintf(stderr,
                     "[taiko_usio] input p1=%03X p2=%03X rise=%03X/%03X "
                     "coin=%u test=%u sample_age=%.3fms\n",
                     actions[0], actions[1], rising[0], rising[1],
                     g_usio.coin_counter, g_usio.test_on ? 1u : 0u,
                         oldest_hit_ms);
    }
#endif

    /* A drum sensor is an analog piezo reading, not a switch: the board
     * reports a short pulse and the game peak-detects it. Tune without a
     * rebuild via TAIKO_HIT_VALUE (hex or decimal) and TAIKO_HIT_HOLD
     * (polls). */
    static uint16_t hit_value = 0;
    static uint8_t  hit_hold  = 0;
    if (!hit_hold) {
        const char* v = std::getenv("TAIKO_HIT_VALUE");
        const char* h = std::getenv("TAIKO_HIT_HOLD");
        hit_value = v ? (uint16_t)std::strtoul(v, nullptr, 0) : 0x0FFF;
        hit_hold  = h ? (uint8_t)std::strtoul(h, nullptr, 0) : 3;
        if (!hit_hold) hit_hold = 1;
    }

    const uint32_t level = actions[0] | actions[1];
    uint16_t digital = 0;
    if (g_usio.test_on) digital |= 0x0080;
    if (level & kEnter) digital |= 0x0200;
    if (level & kDown) digital |= 0x1000;
    if (level & kUp) digital |= 0x2000;
    if (level & kService) digital |= 0x4000;

    auto& out = g_usio.last_frame;
    out.fill(0);
    write_le16(out, 0, digital);
    write_le16(out, 16, g_usio.coin_counter);
    for (unsigned p = 0; p < 2; ++p) {
        for (unsigned i = 0; i < 4; ++i) {
            if (rising[p] & kHitBits[i])
                g_usio.hit_cooldown[p][i] = hit_hold;
            uint16_t value = 0;
            if (g_usio.hit_cooldown[p][i]) {
                /* Linear decay across the hold window: full amplitude on
                 * the first poll, tapering to zero. */
                value = (uint16_t)(hit_value * g_usio.hit_cooldown[p][i] / hit_hold);
                --g_usio.hit_cooldown[p][i];
            }
            /* Taiko packs both drums into the same 0x60-byte report: P1 uses
             * bytes 32..39 and P2 bytes 40..47. 0x1100 mirrors the last
             * 0x1080 report; it is not a separate player-local frame. */
            write_le16(out, 32 + p * 8 + i * 2, value);
        }
    }
    g_usio.last_frame_valid = true;
}

std::array<uint8_t, 0x60> build_input_frame(bool advance)
{
    if (advance || !g_usio.last_frame_valid)
        build_input_frames();
    return g_usio.last_frame;
}

void stage(const uint8_t* source, size_t source_len, uint16_t requested)
{
    const uint16_t cap = std::min<uint16_t>(requested, g_usio.staged.size());
    std::fill_n(g_usio.staged.begin(), cap, 0);
    if (source) {
        const size_t n = std::min<size_t>(source_len, cap);
        std::copy_n(source, n, g_usio.staged.begin());
    }
    g_usio.staged_len = cap;
    g_usio.staged_pos = 0;
    g_usio.staged_zlp = cap && (cap % 64 == 0);
}

void complete_write()
{
    if (g_usio.write_channel == 0 &&
        (g_usio.write_reg == 0x7000 || g_usio.write_reg == 0x7400)) {
        feed_reader(g_usio.write_buffer.data(), g_usio.write_total);
    } else if (g_usio.write_channel >= 2) {
        const unsigned page = g_usio.write_channel - 2;
        const size_t end = static_cast<size_t>(g_usio.write_reg) + g_usio.write_total;
        if (page < g_usio.sram.size() && end <= g_usio.sram[page].size()) {
            uint8_t* destination = g_usio.sram[page].data() + g_usio.write_reg;
            const bool changed = std::memcmp(destination, g_usio.write_buffer.data(),
                                             g_usio.write_total) != 0;
            std::fprintf(stderr,
                         "[taiko_usio] SRAM write page=%u reg=%04X len=%u changed=%u\n",
                         page, g_usio.write_reg, g_usio.write_total,
                         changed ? 1u : 0u);
            if (changed) {
                std::copy_n(g_usio.write_buffer.begin(), g_usio.write_total,
                            destination);
                save_backup();
            }
        } else {
            std::fprintf(stderr,
                         "[taiko_usio] invalid SRAM write ch=%u reg=%04X len=%u\n",
                         g_usio.write_channel, g_usio.write_reg,
                         g_usio.write_total);
        }
    }
    g_usio.idle_in_pending = true;
}

void handle_out(uint32_t buffer, int32_t length)
{
    if (g_usio.write_active) {
        const uint16_t take = std::min<uint16_t>(
            std::max<int32_t>(length, 0), g_usio.write_remaining);
        const uint16_t room = static_cast<uint16_t>(g_usio.write_buffer.size() - g_usio.write_total);
        const uint16_t copied = std::min(take, room);
        for (uint16_t i = 0; i < copied; ++i)
            g_usio.write_buffer[g_usio.write_total + i] = vm_read8(buffer + i);
        g_usio.write_total += copied;
        g_usio.write_remaining -= take;
        if (!g_usio.write_remaining) {
            g_usio.write_active = false;
            complete_write();
        }
        return;
    }

    if (length != 6)
        return;
    uint8_t command[6];
    for (unsigned i = 0; i < 6; ++i)
        command[i] = vm_read8(buffer + i);
    const uint8_t channel = command[0] & 0x0F;
    const uint16_t reg = command[2] | (static_cast<uint16_t>(command[3]) << 8);
    const uint16_t requested = command[4] | (static_cast<uint16_t>(command[5]) << 8);

    if (g_usio.trace_budget) {
        --g_usio.trace_budget;
        std::fprintf(stderr, "[taiko_usio] cmd=%02X ch=%u reg=%04X len=%u\n",
                     command[0], channel, reg, requested);
    }

    if ((command[0] & 0x90) == 0x90) {
        g_usio.write_channel = channel;
        g_usio.write_reg = reg;
        g_usio.write_remaining = requested;
        g_usio.write_total = 0;
        g_usio.write_active = requested != 0;
        if (!requested)
            complete_write();
        return;
    }

    if ((command[0] & 0x10) == 0x10) {
        if (channel == 0) {
            if (reg == 0x0000) {
                stage(kKeepalive.data(), kKeepalive.size(), requested);
            } else if (reg == 0x0080) {
                auto status = kReaderStatus;
                status[2] = static_cast<uint8_t>(
                    std::min<uint16_t>(g_usio.reader_tx_len, 0xFF));
                stage(status.data(), status.size(), requested);
            } else if (reg == 0x7000) {
                const uint16_t take = std::min<uint16_t>(requested,
                                                        g_usio.reader_tx_len);
                stage(g_usio.reader_tx.data(), take, requested);
                g_usio.reader_tx_len -= take;
                if (g_usio.reader_tx_len)
                    std::move(g_usio.reader_tx.begin() + take,
                              g_usio.reader_tx.begin() + take + g_usio.reader_tx_len,
                              g_usio.reader_tx.begin());
            } else if (reg == 0x4954) {
                stage(kFpgaIdent.data(), kFpgaIdent.size(), requested);
            } else if (reg == 0x1000) {
                /* Cabinet-level snapshot for product 0x0900. Non-advancing:
                 * player register 0x1080 owns edge detection. */
                const auto frame = build_input_frame(false);
                stage(frame.data(), frame.size(), requested);
            } else if (reg == 0x1080 || reg == 0x1100) {
                const auto frame = build_input_frame(reg == 0x1080);
                /* Log both packed player payloads whenever a hit is present
                 * so register mirroring and P2 routing can be verified. */
                {
                    bool any_hit = false;
                    for (size_t i = 32; i < 48; ++i)
                        if (frame[i]) { any_hit = true; break; }
                    static unsigned budget = 40;
                    if (input_trace_enabled() && any_hit && budget) {
                        --budget;
                        std::fprintf(stderr,
                                     "[usio_hit] reg=%04X requested=%u frame=0x%zX "
                                     "p1=%02X%02X %02X%02X %02X%02X %02X%02X "
                                     "p2=%02X%02X %02X%02X %02X%02X %02X%02X\n",
                                     reg, requested, frame.size(),
                                     frame[33], frame[32], frame[35], frame[34],
                                     frame[37], frame[36], frame[39], frame[38],
                                     frame[41], frame[40], frame[43], frame[42],
                                     frame[45], frame[44], frame[47], frame[46]);
                    }
                }
                stage(frame.data(), frame.size(), requested);
            } else if (reg >= 0x1800 &&
                       static_cast<size_t>(reg - 0x1800) < kFirmwareInfo.size()) {
                const size_t offset = reg - 0x1800;
                stage(kFirmwareInfo.data() + offset, kFirmwareInfo.size() - offset,
                      requested);
            } else {
                /* Count zero-filled register reads and report periodically so
                 * a continuous poll is distinguishable from a one-off probe. */
                {
                    static uint16_t seen_reg[16];
                    static unsigned seen_cnt[16];
                    static unsigned seen_n;
                    static unsigned since_report;
                    unsigned k = 0;
                    for (; k < seen_n; ++k)
                        if (seen_reg[k] == reg) break;
                    if (k == seen_n && seen_n < 16) {
                        seen_reg[seen_n] = reg;
                        seen_cnt[seen_n] = 0;
                        ++seen_n;
                    }
                    if (k < 16) ++seen_cnt[k];
                    if (++since_report >= 600) {
                        since_report = 0;
                        std::fprintf(stderr, "[usio_unhandled]");
                        for (unsigned i = 0; i < seen_n; ++i)
                            std::fprintf(stderr, " %04X:%u", seen_reg[i], seen_cnt[i]);
                        std::fprintf(stderr, "\n");
                    }
                }
                stage(nullptr, 0, requested);
            }
        } else if (channel >= 2) {
            const unsigned page = channel - 2;
            const size_t end = static_cast<size_t>(reg) + requested;
            std::fprintf(stderr,
                         "[taiko_usio] SRAM read page=%u reg=%04X len=%u\n",
                         page, reg, requested);
            if (page < g_usio.sram.size() && end <= g_usio.sram[page].size())
                stage(g_usio.sram[page].data() + reg, requested, requested);
            else
                stage(nullptr, 0, requested);
        } else {
            stage(nullptr, 0, requested);
        }
        return;
    }

    if ((command[0] & 0xA0) == 0xA0) {
        if (channel == 0 && reg == 0x000A) {
            std::fprintf(stderr, "[taiko_usio] SRAM initialize/erase command\n");
            for (auto& page : g_usio.sram) page.fill(0);
            save_backup();
        }
        g_usio.idle_in_pending = true;
    }
}

void hle_init(ppu_context* ctx)
{
    stop_input_sampler();
    g_usio = {};
    g_usio.input_trace_budget = input_trace_enabled() ? 8 : 0;
    g_usio.initialized = true;
    const bool backup_loaded = load_backup();
    if (!backup_loaded || !backup_archives_valid()) {
        initialize_blank_backup();
        save_backup();
    }
    start_input_sampler();
    std::fprintf(stderr, "[taiko_usio] initialized (virtual PS3A-USJ)\n");
    ctx->gpr[3] = 0;
}

void hle_end(ppu_context* ctx)
{
    stop_input_sampler();
    g_usio.initialized = false;
    ctx->gpr[3] = 0;
}

void hle_set_thread_priority(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

void hle_register_ldd(ppu_context* ctx)
{
    const uint32_t ldd = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t name_ea = ldd ? vm_read32(ldd) : 0;
    char name[32]{};
    for (size_t i = 0; name_ea && i + 1 < sizeof(name); ++i) {
        name[i] = static_cast<char>(vm_read8(name_ea + i));
        if (!name[i]) break;
    }
    std::fprintf(stderr, "[taiko_usio] RegisterLdd '%s' @%08X\n", name, ldd);
    ctx->gpr[3] = 0;
    if (!g_usio.attached && std::strncmp(name, "PS3A-USJ", 8) == 0) {
        const uint32_t attach_opd = vm_read32(ldd + 8);
        g_usio.attached = true;
        std::fprintf(stderr, "[taiko_usio] injecting device %08X via attach OPD %08X\n",
                     kFakeDevice, attach_opd);
        ppu_guest_call(attach_opd, kFakeDevice, 0, 0, 0);
    }
}

void hle_scan_descriptor(ppu_context* ctx)
{
    const uint32_t device = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t previous = static_cast<uint32_t>(ctx->gpr[4]);
    const int type = static_cast<int>(ctx->gpr[5]);
    if (device != kFakeDevice) {
        ctx->gpr[3] = 0;
        return;
    }
    for (size_t i = 0; i < kDescriptors.size(); ++i)
        vm_write8(kDescriptorEa + i, kDescriptors[i]);
    size_t offset = 0;
    if (previous >= kDescriptorEa && previous < kDescriptorEa + kDescriptors.size())
        offset = previous - kDescriptorEa + vm_read8(previous);
    while (offset + 2 <= kDescriptors.size()) {
        const uint8_t length = kDescriptors[offset];
        if (!length || offset + length > kDescriptors.size()) break;
        if (!type || kDescriptors[offset + 1] == type) {
            ctx->gpr[3] = kDescriptorEa + offset;
            return;
        }
        offset += length;
    }
    ctx->gpr[3] = 0;
}

void hle_open_pipe(ppu_context* ctx)
{
    const uint32_t device = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t descriptor = static_cast<uint32_t>(ctx->gpr[4]);
    if (device != kFakeDevice) {
        ctx->gpr[3] = static_cast<uint32_t>(-1);
        return;
    }
    const uint8_t endpoint = descriptor ? vm_read8(descriptor + 2) : 0;
    ctx->gpr[3] = descriptor ? ((endpoint & 0x80) ? kPipeIn : kPipeOut)
                             : kPipeControl;
    std::fprintf(stderr, "[taiko_usio] OpenPipe ep=%02X -> %08X\n", endpoint,
                 static_cast<uint32_t>(ctx->gpr[3]));
}

void hle_close_pipe(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

void hle_control_transfer(ppu_context* ctx)
{
    const uint32_t callback_opd = static_cast<uint32_t>(ctx->gpr[6]);
    const uint32_t argument = static_cast<uint32_t>(ctx->gpr[7]);
    callback(callback_opd, 0, 0, argument);
    ctx->gpr[3] = 0;
}

void hle_bulk_transfer(ppu_context* ctx)
{
    const uint32_t pipe = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t buffer = static_cast<uint32_t>(ctx->gpr[4]);
    const int32_t length = static_cast<int32_t>(ctx->gpr[5]);
    const uint32_t callback_opd = static_cast<uint32_t>(ctx->gpr[6]);
    const uint32_t argument = static_cast<uint32_t>(ctx->gpr[7]);
    int32_t count = 0;

    if (pipe == kPipeOut) {
        handle_out(buffer, length);
        count = std::max(length, 0);
    } else if (pipe == kPipeIn) {
        if (length == 0) {
            g_usio.idle_in_pending = false;
            count = 0;
        } else if (g_usio.staged_pos < g_usio.staged_len) {
            const uint16_t remaining = g_usio.staged_len - g_usio.staged_pos;
            count = std::min<int32_t>(remaining, length);
            for (int32_t i = 0; i < count; ++i)
                vm_write8(buffer + i, g_usio.staged[g_usio.staged_pos + i]);
            for (int32_t i = count; i < length; ++i)
                vm_write8(buffer + i, 0);
            g_usio.staged_pos += count;
            if (g_usio.staged_pos == g_usio.staged_len) {
                g_usio.staged_len = 0;
                g_usio.staged_pos = 0;
            }
        } else if (g_usio.staged_zlp || g_usio.idle_in_pending) {
            g_usio.staged_zlp = false;
            g_usio.idle_in_pending = false;
            count = 0;
        }
    }
    if (pipe == kPipeIn && count >= 7 &&
        std::getenv("TAIKO_ENTRY_TRACE")) {
        const uint8_t response_command = vm_read8(buffer + 6);
        /* A no-card InListPassiveTarget poll is the very common 12-byte 4B
         * response.  Keep the entry trace focused on an actual target and the
         * following InDataExchange replies.  The active lifted caller is
         * UsbConnectionCell::bulk_receive; its saved LR is still in this
         * frame, and identifies the command/parser call site above it. */
        if ((response_command == 0x4B && count > 12) ||
            response_command == 0x41) {
            const uint32_t submitter_return =
                static_cast<uint32_t>(vm_read64(ctx->gpr[1] + 0xC0));
            std::fprintf(stderr,
                         "[entry-usio] response=%02X count=%d buffer=%08X "
                         "callback-opd=%08X argument=%08X hle-lr=%08X "
                         "submitter-return=%08X data=",
                         response_command, count, buffer, callback_opd,
                         argument, static_cast<uint32_t>(ctx->lr),
                         submitter_return);
            for (int32_t i = 0; i < count; ++i) {
                std::fprintf(stderr, "%02X", vm_read8(buffer +
                                                       static_cast<uint32_t>(i)));
            }
            std::fputc('\n', stderr);
        }
    }
    callback(callback_opd, 0, count, argument);
    ctx->gpr[3] = 0;
}

extern "C" int ppu_gcm_pump_after_hle(uint32_t nid, ppu_context* ctx)
{
    static const bool enabled = [] {
        const char* value = std::getenv("TAIKO_INPUT_PUMP_AFTER_USIO");
        return value && value[0] && value[0] != '0';
    }();
    if (!enabled || nid != 0xAC77EB78u || !ctx) return 0;
    const uint32_t pipe = static_cast<uint32_t>(ctx->gpr[3]);
    return pipe == kPipeIn || pipe == kPipeOut;
}

__attribute__((constructor)) void register_taiko_usio()
{
    ps3_hle_register_ctx(0x254289ACu, "cellUsbdOpenPipe", hle_open_pipe);
    ps3_hle_register_ctx(0x2FB08E1Eu, "cellUsbdScanStaticDescriptor", hle_scan_descriptor);
    ps3_hle_register_ctx(0x359BEFBAu, "cellUsbdRegisterLdd", hle_register_ldd);
    ps3_hle_register_ctx(0x35F22AC3u, "cellUsbdEnd", hle_end);
    ps3_hle_register_ctx(0x5C832BD7u, "cellUsbdSetThreadPriority2", hle_set_thread_priority);
    ps3_hle_register_ctx(0x9763E962u, "cellUsbdClosePipe", hle_close_pipe);
    ps3_hle_register_ctx(0x97CF128Eu, "cellUsbdControlTransfer", hle_control_transfer);
    ps3_hle_register_ctx(0xAC77EB78u, "cellUsbdBulkTransfer", hle_bulk_transfer);
    ps3_hle_register_ctx(0xD0E766FEu, "cellUsbdInit", hle_init);
}

} // namespace
