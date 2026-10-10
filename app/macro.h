/**
  * macro.h - pedal macro playback engine (pure logic, host-testable).
  *
  * One macro_state_t per pedal, driven by src/main.c once per main-loop pass
  * (1 ms timestamp granularity).  The engine owns ONLY playback progress;
  * the step list and timing are passed in on every tick as plain arguments
  * (the caller snapshots them under a critical section via cmd_get_pedal()),
  * so a MAC/MTIM command landing in the USB ISR takes effect from the next
  * tick without corrupting engine state.
  *
  * Playback while the pedal is held: hold step i's chord for press_ms,
  * release everything for gap_ms, advance to step (i+1) % count, repeat
  * forever.  Releasing the pedal zeroes the contribution on the very next
  * tick - no key can stick, even mid-press.  After a stall (e.g. the
  * ~20-40 ms flash save) at most ONE phase advance happens per tick, so the
  * cycle resumes smoothly instead of fast-forwarding.
  *
  * Like cmd.c: pure C11, <stdint.h> only - no CMSIS, no libc - so the host
  * unit test (host_test/test_macro.c) compiles app/macro.c directly.
  */
#ifndef MACRO_H
#define MACRO_H

#include <stdint.h>

/* Playback progress of one pedal.  Zero-initialised (or macro_reset()) is a
 * valid idle state. */
typedef struct
{
    uint8_t  active;    /* 1 = pedal held and a cycle is running */
    uint8_t  pressing;  /* 1 = a step's chord is held, 0 = inter-step gap */
    uint8_t  step;      /* index of the step being pressed */
    uint8_t  reserved;
    uint32_t deadline;  /* ms timestamp of the next phase switch (wrap-safe) */
} macro_state_t;

/* Clear playback state; call at boot and whenever a pedal's mode changes
 * (single <-> macro), so the next hold always starts at step 0. */
void macro_reset(macro_state_t *st);

/* Advance one pedal's engine and output its current chord.
 *   st             playback state (one per pedal, owned by the caller)
 *   mod_mask/keycode/count
 *                  step list snapshot; mod_mask bit b = HID modifier 0xE0+b,
 *                  keycode 0x00 = modifier-only step; count 0 disables the
 *                  engine (outputs stay zero)
 *   press_ms/gap_ms
 *                  timing snapshot; 0 is clamped to 1 ms defensively
 *   held           debounced pedal level, 1 = down
 *   now_ms         current millisecond timestamp (may wrap; comparisons use
 *                  signed differences)
 *   out_mask/out_key
 *                  chord to merge into the keyboard report this pass; both
 *                  zero during a gap, on release, or when count == 0.
 * Non-blocking; touches only *st and the two outputs. */
void macro_tick(macro_state_t *st,
                const uint8_t *mod_mask, const uint8_t *keycode, uint8_t count,
                uint16_t press_ms, uint16_t gap_ms,
                uint8_t held, uint32_t now_ms,
                uint8_t *out_mask, uint8_t *out_key);

#endif /* MACRO_H */
