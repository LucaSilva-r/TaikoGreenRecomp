#undef NDEBUG
#include <assert.h>
unsigned char* vm_base;
// Include the implementation to inject an already encoded card without a game dump.
#include "../src/taiko_card.c"
int main(void) {
    char code[21] = {0}; uint8_t uid[4] = {0};
    const uint64_t first = taiko_card_browser_begin();
    assert(first && taiko_card_reader_active());
    assert(!taiko_card_browser_begin());
    taiko_card_browser_end(first);
    const uint64_t second = taiko_card_browser_begin();
    assert(second && second != first);
    taiko_card_browser_end(first);
    assert(taiko_card_reader_active());
    /* A successfully encoded card arriving from the pairing worker. */
    memset(g_card.access_code, 0x12, CARD_BYTES);
    g_card.mifare_uid[0] = 42; g_card.card_present = 1;
    assert(!taiko_card_browser_take(first, code, uid));
    uint8_t rx[7] = {0}, tx[32] = {0}; rx[6] = 0x4a;
    assert(!taiko_card_process(rx, sizeof rx, tx, sizeof tx));
    assert(taiko_card_is_present());
    assert(taiko_card_browser_take(second, code, uid));
    assert(!strcmp(code, "12121212121212121212") && uid[0] == 42);
    assert(!taiko_card_is_present() && !taiko_card_reader_active());
    assert(!taiko_card_browser_take(second, code, uid));
    return 0;
}
