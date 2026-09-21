/**
  * cfg.h - persisted keymap configuration (flash layout contract).
  *
  * Backend: app/cfg_flash.c, register-level STM32F103 FPEC (no StdPeriph).
  *
  * On-flash image at CFG_FLASH_ADDR - fixed 16-byte layout, explicit byte
  * offsets, no struct/padding assumptions:
  *   [0..3]   magic, CFG_MAGIC little-endian
  *   [4]      version, CFG_VERSION
  *   [5..7]   keymap[0..2] (= pedal 1..3 HID usage codes)
  *   [8..9]   checksum, 16-bit sum of bytes 0..7, little-endian
  *   [10..15] 0xFF padding
  *
  * -I only pulls CMSIS; cmd.h is the frozen keymap API.  Keep this header
  * dependency-free apart from <stdint.h> so host tests may include it.
  */
#ifndef CFG_H
#define CFG_H

#include <stdint.h>

#define CFG_FLASH_ADDR  0x0800FC00u   /* last 1KB page of the 64KB flash */
#define CFG_MAGIC       0x31474643u   /* "CFG1" little-endian */
#define CFG_VERSION     1u

/* Read the stored config and apply it via cmd_set_keymap(i, code), i = 0..2.
 * Returns 0 = valid config loaded; 1 = no/invalid config, in which case the
 * caller keeps its current (default) keymap - invalid data never modifies it.
 * Main-loop only (see cfg_save()); never call from an ISR. */
uint8_t cfg_load(void);

/* Persist cmd_keycode(0..2): erase the page, program the 16-byte image, then
 * read it back and verify.  Returns 0 = OK (page erased, programmed and
 * verified); 1 = failure (page left erased/partially written).
 *
 * MUST be called from the main loop, never from an ISR: a page erase stalls
 * CPU flash fetch (~20-40 ms on STM32F103), so interrupts that run from flash
 * would either be delayed or fetch garbage.  `make flash -e sector` preserves
 * this page; `-e chip` (full chip erase) wipes it. */
uint8_t cfg_save(void);

#endif /* CFG_H */
