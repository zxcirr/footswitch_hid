/**
  * cmd.h - host-configurable pedal keymap command parser (frozen interface).
  *
  * This header is the complete contract between:
  *   - the firmware parser         (app/cmd.c)
  *   - the USB feature-report glue (app/usb_prop.c / src/main.c)
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
  *
  * Response (out[])
  *   - ASCII, terminated by '\n', NUL padded.  cmd_handle() must clear all
  *     CMD_FEATURE_LEN bytes before writing, so no stale report data leaks.
  *   - Responses always echo hex in canonical upper-case 2-digit form 0xNN.
  *
  * Commands and responses
  *   SET <n> <0xNN>   Bind pedal n (1..3) to HID usage code NN.
  *                    -> "OK SET <n>=0xNN\n"
  *   GET              -> "OK MAP 0xNN 0xNN 0xNN\n"
  *   SAVE             -> cmd_handle() returns 1 and out[] holds the
  *                       placeholder "OK saving\n".  main.c performs the
  *                       flash write, calls cmd_save_done() on success, and
  *                       publishes the final response itself (the final text
  *                       is owned by main.c, not by cmd_handle()).
  *   RESET            -> "OK RESET\n": restore defaults 0x1E/0x1F/0x20 and
  *                       mark the map dirty.
  *   INFO             -> "OK INFO fw=1.0 keys=0xNN,0xNN,0xNN dirty=<0|1>\n"
  *   anything else    -> "ERR unknown\n"  (also for an empty/blank request)
  *
  * Error responses
  *   "ERR syntax\n"   malformed command word, arguments or hex digits
  *   "ERR pedal\n"    pedal index outside 1..3
  *   "ERR range\n"    key code outside the accepted set (below)
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
  * ==========================================================================
  */
#ifndef CMD_H
#define CMD_H

#include <stdint.h>

/* Feature-report size in bytes and number of pedals. */
#define CMD_FEATURE_LEN 48
#define CMD_KEY_COUNT   3

/* Current binding per pedal: index 0..CMD_KEY_COUNT-1 = pedal 1..3.
 * volatile: written by the feature-report handler, read by the report
 * builder. */
extern volatile uint8_t  cmd_keymap[CMD_KEY_COUNT];

/* Number of feature reports handled since boot; incremented once per
 * cmd_handle() call, valid or not.  volatile for the same reason. */
extern volatile uint32_t cmd_count;

/* Reset RAM state to defaults: cmd_keymap = {0x1E, 0x1F, 0x20},
 * cmd_count = 0, dirty = 0.  Call once at boot, before any cmd_handle(). */
void    cmd_init(void);

/* Parse one CMD_FEATURE_LEN-byte request and build the reply in out[].
 * Always NUL-pads out[] to CMD_FEATURE_LEN.  Increments cmd_count.
 * Non-blocking; no dynamic memory.
 * Returns 0 = response ready in out[];
 *         1 = SAVE requested - out[] holds only the "OK saving\n" placeholder.
 *             The caller must write flash, call cmd_save_done(), then publish
 *             the final response. */
uint8_t cmd_handle(const uint8_t in[CMD_FEATURE_LEN], uint8_t out[CMD_FEATURE_LEN]);

/* Binding for index 0..CMD_KEY_COUNT-1 (= pedal 1..3); 0 if index is out of
 * range.  Note that a valid binding may itself be 0 (disabled), so a zero
 * return is only meaningful together with a valid index. */
uint8_t cmd_keycode(uint8_t index);

/* Raw binding write used by cfg_load() when restoring persisted state.
 * Out-of-range index is ignored.  No range validation of code, and does not
 * touch cmd_dirty(): a freshly loaded map equals the saved one. */
void    cmd_set_keymap(uint8_t index, uint8_t code);

/* 1 = RAM differs from the last successful flash save; 0 = clean. */
uint8_t cmd_dirty(void);

/* Called by main after a successful flash write of the current keymap:
 * clears the dirty flag. */
void    cmd_save_done(void);

#endif /* CMD_H */
