/* Virtual BanaPassport card. See taiko_card.h.
 *
 * Ported from TaikoZucchini's bpreader/bpreader_serial.c (MIT, (c) Luca Silva).
 * The differences are all environment: the NBGIC key tables are found by
 * scanning guest memory rather than the PS3 process, and the reader plumbing
 * (framing, the USIO registers) already exists in src/taiko_usio.cpp.
 */
#include "taiko_card.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern unsigned char* vm_base;

enum {
    CARD_BYTES = 10,             /* 20 decimal digits, BCD */
    MIFARE_BLOCK_SIZE = 16,
    MIFARE_BLOCK_COUNT = 64,
    MIFARE_CMD_AUTH_KEY_A = 0x60,
    MIFARE_CMD_AUTH_KEY_B = 0x61,
    MIFARE_CMD_READ = 0x30,
    CARD_HOLD_SECONDS = 10,
};

/* The pairing thread puts cards on the reader while a guest thread polls it,
 * so every entry point takes this. The encode is done under it too -- it is a
 * few milliseconds, once per card. */
static pthread_mutex_t g_card_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    uint8_t access_code[CARD_BYTES];
    uint8_t mifare_uid[4];
    uint8_t blocks[MIFARE_BLOCK_COUNT][MIFARE_BLOCK_SIZE];
    int     mifare_valid;
    int     card_present;
    long    presented_seconds;
    long    last_poll_seconds;
} g_card;

static long monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

/* ---------------------------------------------------------------------------
 * Byte helpers. Everything here is big-endian, guest-side and card-side alike.
 * -----------------------------------------------------------------------*/

static uint32_t rd_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t bswap32(uint32_t v)
{
    return (v << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | (v >> 24);
}

static uint64_t rd_be64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void wr_be64(uint8_t* p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static int parse_access_code(const char* code, uint8_t out[CARD_BYTES])
{
    if (!code) return 0;
    for (size_t i = 0; i < CARD_BYTES; i++) {
        const char hi = code[i * 2];
        const char lo = code[i * 2 + 1];
        if (hi < '0' || hi > '9' || lo < '0' || lo > '9') return 0;
        out[i] = (uint8_t)(((hi - '0') << 4) | (lo - '0'));
    }
    return code[20] == '\0';
}

/* ---------------------------------------------------------------------------
 * NBGIC: the key material lives in the game image
 *
 * A BanaPassport's block 1 is "\x00\x02NBGIC<profile>" followed by the card id
 * encrypted with a Blowfish variant. Both the per-profile keys and the cipher
 * seeds are constants inside the EBOOT, found by their own tags, so the
 * encoder is always in step with whatever build is loaded.
 * -----------------------------------------------------------------------*/

enum {
    NBGIC_SCAN_START = 0x00010000u,   /* the guest image's load address */
    NBGIC_SCAN_END = 0x00E00000u,
    NBGIC_TAG_STRIDE = 0x88u,
    NBGIC_PROFILE_COUNT = 8,
    NBGIC_PROFILE_STRIDE = 0x48u,
};

static struct {
    int ready;
    const uint8_t* base;
    const uint8_t* profiles;
    const uint8_t* s_seed;
    const uint8_t* p_seed;
} g_nbgic;

static int nbgic_tables_find(void)
{
    static const char* prefixes[NBGIC_PROFILE_COUNT] = {
        "300", "302", "303", "304", "305", "306", "307", "308",
    };
    static const uint8_t tag[8] = {'N', 'B', 'G', 'I', 'C', '0', 0x00, 0x00};

    if (g_nbgic.ready) return 1;
    if (!vm_base) return 0;

    for (uint32_t p = NBGIC_SCAN_START; p + 8u <= NBGIC_SCAN_END; p++) {
        const uint8_t* b = vm_base + p;
        if (memcmp(b, tag, sizeof(tag)) != 0) continue;

        int tags_ok = 1;
        for (int i = 1; i < NBGIC_PROFILE_COUNT; i++) {
            const uint8_t* t = b + (size_t)i * NBGIC_TAG_STRIDE;
            if (p + (uint32_t)i * NBGIC_TAG_STRIDE + 8u > NBGIC_SCAN_END ||
                memcmp(t, "NBGIC", 5) != 0 || t[5] != (uint8_t)('0' + i)) {
                tags_ok = 0;
                break;
            }
        }
        if (!tags_ok) continue;

        const uint8_t* profiles = NULL;
        for (uint32_t q = p + 0x400u;
             q + 8u * NBGIC_PROFILE_STRIDE <= NBGIC_SCAN_END && q < p + 0x1200u;
             q += 4u) {
            const uint8_t* rec = vm_base + q;
            int ok = 1;
            for (int i = 0; i < NBGIC_PROFILE_COUNT; i++) {
                if (memcmp(rec + (size_t)i * NBGIC_PROFILE_STRIDE + 4,
                           prefixes[i], 3) != 0) { ok = 0; break; }
            }
            if (ok) { profiles = rec; break; }
        }
        if (!profiles) continue;

        /* The Blowfish S-box seed, by its standard first words. */
        const uint8_t* s_seed = NULL;
        for (uint32_t q = p + 0x600u;
             q + 16u <= NBGIC_SCAN_END && q < p + 0x3000u; q += 4u) {
            const uint8_t* s = vm_base + q;
            if (rd_be32(s) == 0xD1310BA6u && rd_be32(s + 4) == 0x98DFB5ACu &&
                rd_be32(s + 8) == 0x2FFD72DBu && rd_be32(s + 12) == 0xD01ADFB7u) {
                s_seed = s;
                break;
            }
        }
        if (!s_seed) continue;

        /* The P array sits either just before the S boxes or ~0x1000 after. */
        const uint8_t* p_seed = NULL;
        const uint32_t s_addr = (uint32_t)(s_seed - vm_base);
        if (s_addr >= NBGIC_SCAN_START + 18u * 4u) {
            const uint8_t* pp = s_seed - 18u * 4u;
            if (rd_be32(pp) == 0x243F6A88u && rd_be32(pp + 4) == 0x85A308D3u &&
                rd_be32(pp + 8) == 0x13198A2Eu && rd_be32(pp + 12) == 0x03707344u)
                p_seed = pp;
        }
        for (uint32_t q = s_addr + 0x1000u;
             !p_seed && q + 16u <= NBGIC_SCAN_END && q < s_addr + 0x1800u; q += 4u) {
            const uint8_t* pp = vm_base + q;
            if (rd_be32(pp) == 0x243F6A88u && rd_be32(pp + 4) == 0x85A308D3u &&
                rd_be32(pp + 8) == 0x13198A2Eu && rd_be32(pp + 12) == 0x03707344u) {
                p_seed = pp;
                break;
            }
        }
        if (!p_seed) continue;

        g_nbgic.base = b;
        g_nbgic.profiles = profiles;
        g_nbgic.s_seed = s_seed;
        g_nbgic.p_seed = p_seed;
        g_nbgic.ready = 1;
        fprintf(stderr, "[taiko_card] NBGIC tables at %08X (profiles %08X)\n",
                p, (uint32_t)(profiles - vm_base));
        return 1;
    }

    fprintf(stderr, "[taiko_card] NBGIC tables not found in the guest image\n");
    return 0;
}

static const uint8_t* nbgic_profile(int profile)
{
    return g_nbgic.profiles + (size_t)profile * NBGIC_PROFILE_STRIDE;
}

static uint32_t nbgic_f(uint32_t s[4][256], uint32_t x)
{
    return ((s[0][(x >> 24) & 0xFF] + s[1][(x >> 16) & 0xFF]) ^
            s[2][(x >> 8) & 0xFF]) + s[3][x & 0xFF];
}

static void nbgic_encrypt_words(uint32_t p[18], uint32_t s[4][256],
                                uint32_t* left, uint32_t* right)
{
    uint32_t l = *left, r = *right;
    for (int i = 0; i < 16; i++) {
        l ^= p[i];
        r = nbgic_f(s, l) ^ r;
        const uint32_t t = l; l = r; r = t;
    }
    const uint32_t t = l; l = r; r = t;
    r ^= p[16];
    l ^= p[17];
    *left = l;
    *right = r;
}

static void nbgic_init_cipher(int profile, uint32_t p[18], uint32_t s[4][256])
{
    const uint8_t* key = g_nbgic.base + (size_t)profile * NBGIC_TAG_STRIDE + 8;

    for (int i = 0; i < 18; i++)
        p[i] = rd_be32(g_nbgic.p_seed + (size_t)i * 4);
    for (int box = 0; box < 4; box++)
        for (int i = 0; i < 256; i++)
            s[box][i] = rd_be32(g_nbgic.s_seed + (size_t)box * 0x400 + (size_t)i * 4);

    int key_pos = 0;
    for (int i = 0; i < 18; i++) {
        uint32_t word = 0;
        for (int j = 0; j < 4; j++) {
            word = (word << 8) | key[key_pos++];
            if (key_pos >= 0x38) key_pos = 0;
        }
        p[i] ^= word;
    }

    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
        nbgic_encrypt_words(p, s, &l, &r);
        p[i] = l;
        p[i + 1] = r;
    }
    for (int box = 0; box < 4; box++) {
        for (int i = 0; i < 256; i += 2) {
            nbgic_encrypt_words(p, s, &l, &r);
            s[box][i] = l;
            s[box][i + 1] = r;
        }
    }
}

static void nbgic_encrypt_payload(int profile, const uint8_t plain[8],
                                  uint8_t cipher[8])
{
    static uint32_t s[4][256];    /* 4 KiB; not on a guest thread's stack */
    uint32_t p[18];
    nbgic_init_cipher(profile, p, s);

    uint32_t left = bswap32(rd_be32(plain));
    uint32_t right = bswap32(rd_be32(plain + 4));
    nbgic_encrypt_words(p, s, &left, &right);
    wr_be32(cipher, bswap32(left));
    wr_be32(cipher + 4, bswap32(right));
}

static uint8_t nbgic_get_bit(const uint8_t bits[7], int pos)
{
    return (uint8_t)((bits[pos >> 3] >> (7 - (pos & 7))) & 1);
}

static void nbgic_set_bit(uint8_t bits[7], int pos)
{
    bits[pos >> 3] |= (uint8_t)(1u << (7 - (pos & 7)));
}

static uint32_t nbgic_get_bits(const uint8_t bits[7], int off, int width)
{
    uint32_t v = 0;
    for (int i = 0; i < width; i++) v = (v << 1) | nbgic_get_bit(bits, off + i);
    return v;
}

static uint8_t nbgic_xor8(uint32_t profile_id, uint32_t card_id)
{
    uint8_t x = 0;
    for (int i = 0; i < 4; i++) {
        x ^= (uint8_t)(profile_id >> (24 - i * 8));
        x ^= (uint8_t)(card_id >> (24 - i * 8));
    }
    return x;
}

/* The printed access code is a permuted, offset decimal encoding of the card
 * id. Recover the id (and which profile issued it) by undoing that. */
static int nbgic_invert_access_code(uint32_t* out_card_id, int* out_profile)
{
    char code[21];
    for (size_t i = 0; i < CARD_BYTES; i++) {
        code[i * 2] = (char)('0' + ((g_card.access_code[i] >> 4) & 0x0F));
        code[i * 2 + 1] = (char)('0' + (g_card.access_code[i] & 0x0F));
    }
    code[20] = '\0';

    for (int profile = 0; profile < NBGIC_PROFILE_COUNT; profile++) {
        const uint8_t* rec = nbgic_profile(profile);
        if (memcmp(code, rec + 4, 3) != 0) continue;

        uint64_t decimal = 0;
        for (int i = 3; i < 20; i++)
            decimal = decimal * 10u + (uint64_t)(code[i] - '0');

        const uint64_t permuted = decimal - rd_be64(rec + 0x40);
        uint8_t tmp[8];
        uint8_t permuted_bits[7];
        uint8_t packed[7] = {0};
        wr_be64(tmp, permuted);
        memcpy(permuted_bits, tmp + 1, sizeof(permuted_bits));

        const uint8_t* perm = rec + 8;
        for (int src = 0; src < 56; src++) {
            if (nbgic_get_bit(permuted_bits, perm[src] % 56))
                nbgic_set_bit(packed, src);
        }

        const uint32_t profile_id = rd_be32(rec);
        const uint32_t pid_field = nbgic_get_bits(packed, 2, 10);
        const uint32_t expected_pid = ((profile_id >> 2) & 0xFF) |
                                      ((profile_id & 3) << 8);
        const uint32_t card_id = bswap32(nbgic_get_bits(packed, 16, 32));
        const uint8_t xor_byte = (uint8_t)nbgic_get_bits(packed, 48, 8);
        const uint8_t expected_xor = nbgic_xor8(profile_id, card_id);
        const uint8_t check2 = (uint8_t)((expected_xor - 3u * (expected_xor / 11u)) & 3u);

        if (pid_field != expected_pid || nbgic_get_bits(packed, 12, 4) != 0 ||
            xor_byte != expected_xor || nbgic_get_bits(packed, 0, 2) != check2)
            continue;

        *out_card_id = card_id;
        *out_profile = profile;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * The card image
 * -----------------------------------------------------------------------*/

static void access_code_to_mifare_uid(uint8_t uid[4])
{
    uint32_t v = 0;
    for (size_t i = 0; i < CARD_BYTES; i++) {
        v = v * 10u + ((g_card.access_code[i] >> 4) & 0x0Fu);
        v = v * 10u + (g_card.access_code[i] & 0x0Fu);
    }
    uid[0] = (uint8_t)v;
    uid[1] = (uint8_t)(v >> 8);
    uid[2] = (uint8_t)(v >> 16);
    uid[3] = (uint8_t)(v >> 24);
}

static int populate_card(void)
{
    uint32_t card_id = 0;
    int profile = 0;

    memset(g_card.blocks, 0, sizeof(g_card.blocks));
    access_code_to_mifare_uid(g_card.mifare_uid);

    memcpy(&g_card.blocks[0][0], g_card.mifare_uid, sizeof(g_card.mifare_uid));
    g_card.blocks[0][4] = g_card.mifare_uid[0] ^ g_card.mifare_uid[1] ^
                          g_card.mifare_uid[2] ^ g_card.mifare_uid[3];
    g_card.blocks[0][5] = 0x08;
    g_card.blocks[0][6] = 0x04;
    memcpy(&g_card.blocks[2][6], g_card.access_code, CARD_BYTES);

    g_card.mifare_valid = 0;
    if (!nbgic_tables_find() || !nbgic_invert_access_code(&card_id, &profile)) {
        fprintf(stderr, "[taiko_card] access code is not encodable by this build\n");
        return 0;
    }

    uint8_t plain[8];
    wr_be32(plain, bswap32(card_id));
    plain[4] = plain[5] = plain[6] = 0;
    plain[7] = plain[0] ^ plain[1] ^ plain[2] ^ plain[3];

    uint8_t cipher[8];
    nbgic_encrypt_payload(profile, plain, cipher);

    uint8_t* block = g_card.blocks[1];
    block[0] = 0x00;
    block[1] = 0x02;
    memcpy(&block[2], "NBGIC", 5);
    block[7] = (uint8_t)('0' + profile);
    memcpy(&block[8], cipher, sizeof(cipher));
    g_card.mifare_valid = 1;
    return 1;
}

int taiko_card_present(const char* access_code)
{
    uint8_t parsed[CARD_BYTES];

    if (!parse_access_code(access_code, parsed)) return TAIKO_CARD_INVALID;

    pthread_mutex_lock(&g_card_lock);
    if (g_card.card_present) {
        pthread_mutex_unlock(&g_card_lock);
        return TAIKO_CARD_BUSY;
    }
    memcpy(g_card.access_code, parsed, sizeof(g_card.access_code));
    if (!populate_card()) {
        pthread_mutex_unlock(&g_card_lock);
        return TAIKO_CARD_NOT_ENCODABLE;
    }
    g_card.card_present = 1;
    g_card.presented_seconds = monotonic_seconds();
    fprintf(stderr, "[taiko_card] card %s on the reader (uid %02X%02X%02X%02X)\n",
            access_code, g_card.mifare_uid[0], g_card.mifare_uid[1],
            g_card.mifare_uid[2], g_card.mifare_uid[3]);
    pthread_mutex_unlock(&g_card_lock);
    return TAIKO_CARD_OK;
}

int taiko_card_is_present(void)
{
    pthread_mutex_lock(&g_card_lock);
    const int present = g_card.card_present;
    pthread_mutex_unlock(&g_card_lock);
    return present;
}

int taiko_card_reader_active(void)
{
    pthread_mutex_lock(&g_card_lock);
    const long last = g_card.last_poll_seconds;
    pthread_mutex_unlock(&g_card_lock);
    return last != 0 && monotonic_seconds() - last <= 2;
}

/* ---------------------------------------------------------------------------
 * The two PN53x commands that depend on the card
 * -----------------------------------------------------------------------*/

static size_t build_response(uint8_t response_cmd, const uint8_t* data,
                             size_t data_length, uint8_t* out, size_t out_capacity)
{
    const size_t frame_length = data_length + 9;
    if (out_capacity < frame_length || data_length > 0xFD) return 0;

    const uint8_t len = (uint8_t)(data_length + 2);
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0xFF;
    out[3] = len;
    out[4] = (uint8_t)(0x100u - len);
    out[5] = 0xD5;
    out[6] = response_cmd;
    if (data_length && data) memcpy(&out[7], data, data_length);

    uint8_t sum = 0;
    for (size_t i = 0; i < data_length + 7; i++) sum = (uint8_t)(sum + out[i]);
    out[data_length + 7] = (uint8_t)(0xFFu - sum);
    out[data_length + 8] = 0x00;
    return frame_length;
}

static size_t handle_poll(uint8_t* tx, size_t tx_capacity)
{
    uint8_t data[10] = { 0x01, 0x01, 0x00, 0x04, 0x08, 0x04, 0, 0, 0, 0 };
    memcpy(&data[6], g_card.mifare_uid, sizeof(g_card.mifare_uid));
    return build_response(0x4B, data, sizeof(data), tx, tx_capacity);
}

static size_t handle_mifare(const uint8_t* rx, size_t rx_length,
                            uint8_t* tx, size_t tx_capacity)
{
    if (rx_length < 10) return 0;

    const uint8_t subcmd = rx[8];
    const uint8_t block = rx[9];

    if (subcmd == MIFARE_CMD_AUTH_KEY_A || subcmd == MIFARE_CMD_AUTH_KEY_B) {
        const uint8_t ok[1] = {0x00};
        return build_response(0x41, ok, sizeof(ok), tx, tx_capacity);
    }
    if (subcmd != MIFARE_CMD_READ || block >= MIFARE_BLOCK_COUNT) {
        const uint8_t error[1] = {0x14};
        return build_response(0x41, error, sizeof(error), tx, tx_capacity);
    }

    uint8_t data[1 + MIFARE_BLOCK_SIZE] = {0};
    memcpy(&data[1], g_card.blocks[block], MIFARE_BLOCK_SIZE);
    return build_response(0x41, data, sizeof(data), tx, tx_capacity);
}

/* TAIKO_CARD_CODE puts one access code on the reader the first time the game
 * polls it -- a login with no server and no pairing, for a code you already
 * know. Deferred to the first poll because encoding it needs the guest image
 * loaded. */
static void present_configured_card_once(void)
{
    static int done;
    if (done) return;
    done = 1;

    uint8_t parsed[CARD_BYTES];
    const char* code = getenv("TAIKO_CARD_CODE");
    if (!code || !code[0]) return;

    /* Called with g_card_lock held, so this is the locked body of
     * taiko_card_present rather than a call back into it. */
    if (!parse_access_code(code, parsed)) {
        fprintf(stderr, "[taiko_card] TAIKO_CARD_CODE is not 20 digits\n");
        return;
    }
    memcpy(g_card.access_code, parsed, sizeof(g_card.access_code));
    if (!populate_card()) return;
    g_card.card_present = 1;
    g_card.presented_seconds = monotonic_seconds();
    fprintf(stderr, "[taiko_card] card %s on the reader (uid %02X%02X%02X%02X)\n",
            code, g_card.mifare_uid[0], g_card.mifare_uid[1],
            g_card.mifare_uid[2], g_card.mifare_uid[3]);
}

size_t taiko_card_process(const uint8_t* rx, size_t rx_length,
                          uint8_t* tx, size_t tx_capacity)
{
    if (rx_length < 7) return 0;
    const uint8_t command = rx[6];
    if (command != 0x4A && command != 0x40) return 0;

    pthread_mutex_lock(&g_card_lock);
    size_t length = 0;
    if (command == 0x4A) {
        /* A tap is short: the card leaves the field once the game has had its
         * look, and a gap in the polling means the scene moved on without
         * taking it. Neither is a "consumed" signal from the reader -- there
         * is none -- so this is modelled on how a player actually holds a
         * card. */
        const long now = monotonic_seconds();
        if (g_card.card_present &&
            (now - g_card.presented_seconds > CARD_HOLD_SECONDS ||
             (g_card.last_poll_seconds && now - g_card.last_poll_seconds > 2))) {
            g_card.card_present = 0;
            fprintf(stderr, "[taiko_card] card left the reader\n");
        }
        g_card.last_poll_seconds = now;
        present_configured_card_once();
        if (g_card.card_present) length = handle_poll(tx, tx_capacity);
    } else if (g_card.card_present) {
        length = handle_mifare(rx, rx_length, tx, tx_capacity);
    }
    pthread_mutex_unlock(&g_card_lock);
    return length;
}
