/**
  * cmd.h - host-configurable pedal binding command parser (frozen interface).
  *
  * This header is the complete contract between:
  *   - the firmware parser         (app/cmd.c)
  *   - the USB feature-report glue (app/usb_prop.c / src/main.c)
  *   - the macro playback engine   (app/macro.h, driven by src/main.c)
  *   - the flash persistence layer (app/cfg_flash.c)
  *   - the host configuration tool (host_test/)
  *
  * It intentionally includes <stdint.h> ONLY: no CMSIS, no ST USB headers.
  * app/cmd.c must therefore compile with a host compiler
  * (gcc -std=c11 -Wall -Wextra -Wshadow) for unit tests, as well as with
  * arm-none-eabi-gcc for the firmware.  Do not add other dependencies here.
  *
  * ============================== WIRE PROTOCOL ==============================
  *
  * Transport
  *   One HID feature report, CMD_FEATURE_LEN (48) bytes, in both directions.
  *
  * Request (in[])
  *   - A leading 0x00 byte (host report-ID artefact) must be tolerated and
  *     ignored; the remainder is parsed as the command line.
  *   - ASCII, case-insensitive command word, NUL padded, optional trailing
  *     '\n'.  Bytes after the first NUL are ignored.
  *   - Tokens are separated by spaces.
  *   - The request must fit the 48-byte report.  Six chord steps plus timing
  *     do NOT fit (6 + 35 + 10 = 51 > 47 usable characters); the host tool
  *     rejects oversized commands before sending.  Six steps alone (41 chars)
  *     or fewer steps with timing always fit.
  *
  * Response (out[])
  *   - ASCII, terminated by '\n', NUL padded.  cmd_handle() must clear all
  *     CMD_FEATURE_LEN bytes before writing, so no stale report data leaks.
  *   - Responses always echo hex in canonical upper-case 2-digit form 0xNN.
  *
  * Binding modes
  *   Each pedal is in exactly one mode:
  *   - CMD_MODE_SINGLE: the pedal is bound to ONE key code; hold = key down,
  *     release = key up (the historical v0.0.1 behaviour).
  *   - CMD_MODE_MACRO:  the pedal is bound to a step sequence.  While held,
  *     the engine (macro.h) cycles the steps endlessly: hold step i's chord
  *     for press_ms, release everything for gap_ms, next step, wrap around.
  *     Releasing the pedal immediately releases all keys.
  *   A step is one simultaneous chord of a modifier mask and at most one
  *   normal key: "shift, then z, then c as three independent taps" is the
  *   3-step sequence 02:00,1D,06; "Shift+z, then c" is 02:1D,06.
  *
  * Commands and responses
  *   SET <n> <0xNN>   Bind pedal n (1..3) to a single key code NN and leave
  *                    macro mode.  Stored steps/timing are KEPT but idle
  *                    (MGET still shows them); the next MAC re-uses the
  *                    stored timing unless new values are given.
  *                    -> "OK SET <n>=0xNN\n"
  *   MAC <n> <steps>  Bind pedal n (1..3) to a macro sequence (grammar below)
  *                    and switch it to macro mode; timing is left unchanged.
  *                    -> "OK MAC <n> <steps>\n" (canonical echo)
  *   MAC <n> <steps> <press> <gap>
  *                    Same, and also set the macro timing: decimal
  *                    milliseconds, 1..CMD_MACRO_TIME_MAX (9999) each.  The
  *                    echo covers the STEPS ONLY (echoing the timing too would
  *                    not fit the 48-byte report for long step lists); read
  *                    the timing back with MTIM.
  *   MGET <n>         Query pedal n's stored step list, whatever its active
  *                    mode is.
  *                    -> "OK MAC <n> <steps>\n", or "OK MAC <n> -\n" when no
  *                    steps are stored.
  *   MTIM <n>         Query pedal n's macro timing.
  *                    -> "OK MTIM <n> <press> <gap>\n" (defaults 30 30)
  *   MTIM <n> <press> <gap>
  *                    Set pedal n's macro timing (same ranges as MAC).
  *                    -> "OK MTIM <n> <press> <gap>\n" (echo)
  *   GET              -> "OK MAP <p> <p> <p>\n", each <p> = the single
  *                    binding "0xNN" or "M" (pedal is in macro mode)
  *   SAVE             -> cmd_handle() returns 1 and out[] holds the
  *                       placeholder "OK saving\n".  main.c performs the
  *                       flash write, calls cmd_save_done() on success, and
  *                       publishes the final response itself (the final text
  *                       is owned by main.c, not by cmd_handle()).
  *   RESET            -> "OK RESET\n": restore ALL defaults - single mode,
  *                    codes 0x1E/0x1F/0x20, no steps, timing 30/30 - and
  *                    mark the config dirty.
  *   INFO             -> "OK INFO fw=0.0.2 keys=<p>,<p>,<p> dirty=<0|1>\n"
  *   anything else    -> "ERR unknown\n"  (also for an empty/blank request)
  *
 * Step list grammar (the <steps> argument of MAC: one token, no spaces)
 *   steps      := step { "," step }          1..CMD_MACRO_MAX_STEPS (6)
 *   step       := "MM:KK" | "KK"             1-2 hex digits per field,
 *                                             optional "0x"/"0X" prefix
 *   MM         := modifier mask, 0x00..0xFF; bit b = HID modifier 0xE0+b
 *                (e.g. 0x02 = left shift).  "MM:00" = modifier-only step.
 *   KK         := 0x00 (only together with a non-zero mask) or 0x04..0x65
 *   - bare "KK" means mask 00 (one plain key tap)
 *   - modifier codes 0xE0..0xE7 are NOT accepted as KK: modifiers are
 *     expressed as mask bits
 *   - at least one of MM/KK must be non-zero
 *   canonical echo: "KK" when the mask is 00, else "MM:KK" (upper case)
 *
 * Error responses
 *   "ERR syntax\n"   malformed command word, arguments, step tokens or
 *                    timing digits, or a wrong argument count (e.g. MAC with
 *                    <press> but no <gap>).  The pedal index is a SINGLE
 *                    decimal digit, so a multi-digit argument (e.g.
 *                    "SET 10 0x04") is "ERR syntax", NOT "ERR pedal"
 *                    (the index range is checked after parsing).
 *   "ERR pedal\n"    pedal index outside 1..3
 *   "ERR range\n"    key code outside the accepted set; more than 6 macro
 *                    steps; a step whose mask and key are both zero; timing
 *                    0 or above 9999
 *
 * Key-code validation (SET)
 *   0x00        disabled - allowed; the pedal emits nothing
 *   0x04..0x65  normal key - goes into the 6-key array of the keyboard
 *               report (the descriptor declares Usage Maximum 0x65)
 *   0xE0..0xE7  modifier - OR-ed into report byte 0
 *   anything else (0x01..0x03, 0x66..0xDF, 0xE8..0xFF) -> "ERR range"
 *
 *   Note: 0x68..0x73 (F13-F24) are deliberately REJECTED even though they
 *   are valid HID usages: the descriptor's key array declares Usage
 *   Maximum 0x65, so such a key could never be transmitted.
 *
 * Hex format: NN is 1-2 hex digits, optionally prefixed by "0x"/"0X"
 *   ("0x1e", "0x1E" and "1E" all parse to 0x1E).
 *
 * ============================ PERSISTED STATE ==============================
 *
 * The flash image (app/cfg_flash.c, v2, 64 bytes) stores the whole
 * cmd_pedal_t of all three pedals.  A legacy v1 image (16 bytes, keymap
 * only) is migrated on load: the single bindings are kept, default timing
 * applies, and the config is marked dirty so the next SAVE rewrites it as
 * v2.  Invalid images are rejected as a whole and the defaults stay.
 *
 * ==========================================================================
 */
#ifndef CMD_H
#define CMD_H

#include <stdint.h>

/* Feature-report size in bytes and number of pedals. */
#define CMD_FEATURE_LEN 48
#define CMD_KEY_COUNT   3

/* Macro limits and defaults (timing in milliseconds). */
#define CMD_MACRO_MAX_STEPS     6
#define CMD_MACRO_PRESS_DEFAULT 30u
#define CMD_MACRO_GAP_DEFAULT   30u
#define CMD_MACRO_TIME_MAX      9999u

/* Binding modes (cmd_pedal_t.mode). */
#define CMD_MODE_SINGLE 0u
#define CMD_MODE_MACRO  1u

/* Whole per-pedal configuration.  Written by cmd_handle() in USB ISR
 * context; the main loop reads it under a short critical section via
 * cmd_get_pedal() and hands the snapshot to the report builder, the macro
 * engine and the flash save path. */
typedef struct
{
    uint8_t  mode;        /* CMD_MODE_SINGLE / CMD_MODE_MACRO */
    uint8_t  code;        /* single-mode binding (HID usage or modifier) */
    uint16_t press_ms;    /* macro: how long each step's chord is held */
    uint16_t gap_ms;      /* macro: release gap between two steps */
    uint8_t  step_count;  /* 0..CMD_MACRO_MAX_STEPS; 0 = no steps stored */
    uint8_t  mod_mask[CMD_MACRO_MAX_STEPS];  /* per-step modifier mask */
    uint8_t  keycode[CMD_MACRO_MAX_STEPS];   /* per-step key, 0 = modifier-only */
} cmd_pedal_t;

/* Number of feature reports handled since boot; incremented once per
 * cmd_handle() call, valid or not.  volatile: written in the USB ISR. */
extern volatile uint32_t cmd_count;

/* Reset RAM state to defaults: every pedal single-mode with codes
 * {0x1E, 0x1F, 0x20}, no steps, timing 30/30 ms; cmd_count = 0, dirty = 0.
 * Call once at boot, before any cmd_handle(). */
void    cmd_init(void);

/* Parse one CMD_FEATURE_LEN-byte request and build the reply in out[].
 * Always NUL-pads out[] to CMD_FEATURE_LEN.  Increments cmd_count.
 * Non-blocking; no dynamic memory.
 * Returns 0 = response ready in out[];
 *         1 = SAVE requested - out[] holds only the "OK saving\n" placeholder.
 *             The caller must write flash, call cmd_save_done(), then publish
 *             the final response. */
uint8_t cmd_handle(const uint8_t in[CMD_FEATURE_LEN], uint8_t out[CMD_FEATURE_LEN]);

/* Copy pedal `index` (0..CMD_KEY_COUNT-1 = pedal 1..3) into *out.  An
 * out-of-range index yields a zeroed struct.  Callers outside the USB ISR
 * (main loop) must hold a critical section around the call: cmd_handle()
 * mutates the same state from ISR context. */
void    cmd_get_pedal(uint8_t index, cmd_pedal_t *out);

/* Raw whole-pedal restore used by cfg_load() after validating a flash
 * image.  Out-of-range index is ignored.  NO validation of the contents and
 * does not touch the dirty flag: a freshly loaded config equals the saved
 * one. */
void    cmd_set_pedal(uint8_t index, const cmd_pedal_t *in);

/* Single-mode binding of pedal `index` (0..CMD_KEY_COUNT-1); 0 if the index
 * is out of range.  Note that a valid binding may itself be 0 (disabled), so
 * a zero return is only meaningful together with a valid index. */
uint8_t cmd_keycode(uint8_t index);

/* 1 = code is accepted by SET: 0x00 (disabled) | 0x04..0x65 (key) |
 * 0xE0..0xE7 (modifier); 0 = out of range.  Shared by the parser and by
 * cfg_load() when validating persisted bytes. */
uint8_t cmd_code_ok(uint8_t code);

/* 1 = (mask, key) is a valid macro step: mask any 0x00..0xFF, key 0x00 or
 * 0x04..0x65, and not both zero.  Shared by the MAC parser and by cfg_load()
 * when validating persisted steps. */
uint8_t cmd_step_ok(uint8_t mask, uint8_t key);

/* 1 = RAM differs from the last successful flash save; 0 = clean. */
uint8_t cmd_dirty(void);

/* Called by main after a successful flash write of the current config:
 * clears the dirty flag. */
void    cmd_save_done(void);

/* Force the dirty flag.  Used by cfg_load() after migrating a legacy v1
 * flash image: the RAM view then differs from what the next v2 save would
 * write, so INFO should nudge the user to re-SAVE. */
void    cmd_mark_dirty(void);

#endif /* CMD_H */
