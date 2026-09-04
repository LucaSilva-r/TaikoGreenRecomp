#ifndef TAIKO_PC_MODE_H
#define TAIKO_PC_MODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ppu_context;

/* Private Lumen-to-host value. The callback shim rewrites it to normal Play
 * before the original game code sees it, so it cannot collide with AI Battle. */
#define TAIKO_PC_MODE_SENTINEL 99u
#define TAIKO_PC_MODE_SAFE_GAME_MODE 1u

/* Returns 1 if Custom PC Mode is currently active, 0 otherwise. */
int taiko_pc_mode_is_active(void);

/* Called by the game-mode selection callback (func_002287BC). */
void taiko_pc_mode_on_game_mode_selected(uint32_t mode);

/* Observe the stock Player Entry dispatcher and arm the handoff only after
 * its authored fade/cleanup has reached the final state. */
void taiko_pc_mode_entry_tick(struct ppu_context* ctx);

/* Called to activate PC Mode frontend. */
void taiko_pc_mode_activate(uint32_t controller);

/* Called to deactivate PC Mode frontend. */
void taiko_pc_mode_deactivate(void);

/* Intercept SequenceController::push_task (0x008DA500). */
void taiko_pc_mode_push_task_hook(struct ppu_context* ctx);

/* Intercept SequenceController::update (0x008DA730). */
void taiko_pc_mode_update_hook(struct ppu_context* ctx);

/* Main PPU tick hook to handle song launch while PC mode is active. */
void taiko_pc_mode_tick(struct ppu_context* ctx);

/* Initialize PC Mode hooks at PPU startup. */
void taiko_pc_mode_init_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* TAIKO_PC_MODE_H */
