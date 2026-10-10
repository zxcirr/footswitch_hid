/**
  * cfg.h - persisted pedal configuration (flash layout contract).
  *
  * Backend: app/cfg_flash.c, register-level STM32F103 FPEC (no StdPeriph).
  *
  * On-flash image v2 at CFG_FLASH_ADDR - fixed CFG_IMAGE_LEN (64) byte
  * layout, explicit byte offsets, little-endian, no struct/padding
  * assumptions:
  *   [0..3]    magic, CFG_MAGIC little-endian
  *   [4]       version, CFG_VERSION (2)
  *   [5..61]   three 19-byte pedal blocks; block i starts at 5 + 19*i:
  *               +0     mode       (0 = single, 1 = macro)
  *               +1     code       (single-mode binding)
  *               +2..3  press_ms   (u16 LE, macro step press duration)
  *               +4..5  gap_ms     (u16 LE, macro inter-step gap)
  *               +6     step_count (0..CMD_MACRO_MAX_STEPS)
  *               +7..18 steps: 6 x {mod_mask, keycode}; slots at or above
  *                      step_count stay 0xFF (erased state)
  *   [62..63]  checksum, 16-bit sum of bytes 0..61, little-endian
  *
  * A legacy v1 image (16 bytes: magic, version 1, keymap[3], checksum over
  * bytes 0..7) is MIGRATED on load: the three single bindings are kept,
  * default timing applies, and the RAM config is marked dirty so the next
  * SAVE rewrites the page as v2.
  *
  * -I only pulls CMSIS; cmd.h provides cmd_pedal_t and the validation
  * predicates.  Keep this header dependency-free apart from <stdint.h> and
  * "cmd.h" so host tests may include it.
  */
#ifndef CFG_H
#define CFG_H

#include <stdint.h>

#include "cmd.h"   /* cmd_pedal_t, CMD_KEY_COUNT, CMD_MACRO_* */

#define CFG_FLASH_ADDR  0x0800FC00u   /* last 1KB page of the 64KB flash */
#define CFG_MAGIC       0x31474643u   /* "CFG1" little-endian */
#define CFG_VERSION     2u
#define CFG_IMAGE_LEN   64u

/* Build the on-flash image (magic/version/pedal blocks/checksum) from a
 * snapshot of the three pedal configs.  Pure: no flash access, no locking -
 * the caller takes the snapshot (e.g. under a critical section) and hands
 * the finished image to cfg_save(). */
void cfg_image_build(uint8_t image[CFG_IMAGE_LEN], const cmd_pedal_t ped[CMD_KEY_COUNT]);

/* Erase the config page, program the prebuilt CFG_IMAGE_LEN-byte image, then
 * read it back and verify every byte.  Returns 0 = OK; 1 = failure (page
 * left erased/partially written).
 *
 * MUST be called from the main loop, never from an ISR: a page erase stalls
 * CPU flash fetch (~20-40 ms on STM32F103), so interrupts that run from flash
 * would either be delayed or fetch garbage.  `make flash -e sector` preserves
 * this page; `-e chip` (full chip erase) wipes it. */
uint8_t cfg_save(const uint8_t image[CFG_IMAGE_LEN]);

/* Read the stored config, validate it and apply it via cmd_set_pedal().
 * Returns 0 = valid config loaded (a legacy v1 image counts as loaded: it is
 * migrated to single-mode bindings with default timing and the config is
 * marked dirty); 1 = no/invalid config, in which case the caller keeps its
 * current (default) state - invalid data never modifies it.
 * Main-loop only (the call site is shared with cfg_save()); never call from
 * an ISR. */
uint8_t cfg_load(void);

#endif /* CFG_H */
