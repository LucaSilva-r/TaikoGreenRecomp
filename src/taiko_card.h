/* Virtual BanaPassport card for the emulated USIO reader.
 *
 * The card reader lives inside the USIO bulk stream (src/taiko_usio.cpp): the
 * game writes PN53x frames to channel 0 register 0x7000 and reads replies
 * back. That responder already answers the reader's housekeeping commands and
 * reports "no card"; this module owns the card itself -- the MIFARE image an
 * access code encodes to, and the two commands that read it.
 *
 * Ported from TaikoZucchini's bpreader_serial.c (MIT, same author), whose
 * NBGIC encoder is the part that matters: block 1 of a BanaPassport carries
 * the card id encrypted with a Blowfish variant whose key material lives in
 * the game's own image, so a card is only accepted if it is encoded with the
 * tables this build carries.
 */
#ifndef TAIKO_CARD_H
#define TAIKO_CARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TAIKO_CARD_OK = 0,
    TAIKO_CARD_BUSY = -1,          /* a card is already on the reader */
    TAIKO_CARD_INVALID = -2,       /* not 20 decimal digits */
    TAIKO_CARD_NOT_ENCODABLE = -3, /* no NBGIC profile accepts this code */
};

/* Put a card on the reader. `access_code` is 20 decimal digits. */
int  taiko_card_present(const char* access_code);
int  taiko_card_is_present(void);

/* Whether the game polled the reader recently. Pairing uses this the way
 * Zucchini uses the reader's own "waiting for a card" signal: no point asking
 * the server for a card while nothing is looking for one. */
int  taiko_card_reader_active(void);

/* PN53x commands that depend on card state (0x4A poll, 0x40 MIFARE). Returns
 * the response length, or 0 when the command is not one of those. */
size_t taiko_card_process(const uint8_t* rx, size_t rx_length,
                          uint8_t* tx, size_t tx_capacity);

#ifdef __cplusplus
}
#endif

#endif /* TAIKO_CARD_H */
