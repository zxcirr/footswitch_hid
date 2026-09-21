/**
  ******************************************************************************
  * @file    cfg_flash.c
  * @brief   Register-level FPEC persistence for the 3-pedal keymap, stored in
  *          the LAST 1 KB page of the 64 KB flash (CFG_FLASH_ADDR).
  *
  * Only CMSIS register definitions are used (via platform_config.h) - no
  * StdPeriph and no libc string functions.  The on-flash image is a fixed
  * 16-byte layout with explicit byte offsets, no struct/padding assumptions:
  *
  *   [0..3]   magic, CFG_MAGIC little-endian
  *   [4]      version, CFG_VERSION
  *   [5..7]   keymap[0..2] from cmd_keycode()
  *   [8..9]   checksum, 16-bit sum of bytes 0..7, little-endian
  *   [10..15] 0xFF padding
  *
  * FPEC sequence per RM0008: unlock (KEYR magic keys) -> page erase (PER/AR/
  * STRT, poll BSY) -> half-word program (PG, 16-bit store per half-word, poll
  * BSY) -> lock.  Any timeout or PGERR/WRPRTERR aborts: error flags cleared,
  * FPEC re-locked, failure returned.
  *
  * !! cfg_save() MUST be called from the main loop, NEVER from an ISR:
  *    page erase stalls CPU instruction fetch from flash for ~20-40 ms.
  *
  * Flash-image guard: the firmware is ~6 KB, far below 0xFC00, so nothing
  * here collides with the linker script.  `make flash -e sector` preserves
  * this page while `-e chip` wipes it.
  ******************************************************************************
  */

#include "cfg.h"
#include "cmd.h"
#include "platform_config.h"

/* ---- On-flash image offsets (16 bytes, half-word aligned payload) -------- */
#define CFG_OFF_MAGIC     0u    /* [0..3]  */
#define CFG_OFF_VERSION   4u    /* [4]     */
#define CFG_OFF_KEYMAP    5u    /* [5..7]  */
#define CFG_OFF_CHECKSUM  8u    /* [8..9]  */
#define CFG_IMAGE_LEN     16u   /* 8 half-words */
#define CFG_PAD_BYTE      0xFFu /* erased-state filler */

/* FPEC unlock keys (RM0008, fixed constants). */
#define FLASH_KEY1        0x45670123u
#define FLASH_KEY2        0xCDEF89ABu

/* Bounded busy-wait budget per erase/program operation.  Page erase takes
 * ~20-40 ms at 72 MHz; the poll loop is a few cycles per iteration, so the
 * budget is well above 100 ms - generous margin without hanging forever on a
 * stuck BSY. */
#define FLASH_TIMEOUT     2000000u

/*******************************************************************************
* load_u32_le: little-endian u32 assembled from bytes (no alignment or
* endianness assumption).
*******************************************************************************/
static uint32_t load_u32_le(const uint8_t *p)
{
  return ((uint32_t)p[0]) |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/*******************************************************************************
* store_u32_le: write v as four little-endian bytes.
*******************************************************************************/
static void store_u32_le(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 0);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/*******************************************************************************
* cfg_checksum: 16-bit sum of image bytes [0..CFG_OFF_CHECKSUM) (magic,
* version, keymap); wraps modulo 2^16.
*******************************************************************************/
static uint16_t cfg_checksum(const uint8_t *image)
{
  uint16_t sum = 0u;

  for (uint8_t i = 0u; i < CFG_OFF_CHECKSUM; i++)
  {
    sum = (uint16_t)(sum + image[i]);
  }
  return sum;
}

/*******************************************************************************
* flash_wait_bsy: poll FLASH_SR_BSY.  Returns 0 = operation finished,
* 1 = timeout (BSY still set).
*******************************************************************************/
static uint8_t flash_wait_bsy(void)
{
  uint32_t timeout = FLASH_TIMEOUT;

  while (((FLASH->SR & FLASH_SR_BSY) != 0u) && (timeout != 0u))
  {
    timeout--;
  }

  return ((FLASH->SR & FLASH_SR_BSY) != 0u) ? 1u : 0u;
}

/*******************************************************************************
* flash_clear_errors: clear EOP/PGERR/WRPRTERR (RM0008: these are rc_w1, i.e.
* writing 1 clears them).
*******************************************************************************/
static void flash_clear_errors(void)
{
  FLASH->SR = (uint32_t)FLASH_SR_EOP |
              (uint32_t)FLASH_SR_PGERR |
              (uint32_t)FLASH_SR_WRPRTERR;
}

/*******************************************************************************
* flash_unlock: write the two FPEC keys, then verify CR.LOCK actually cleared.
* Returns 0 = unlocked, 1 = still locked.
*******************************************************************************/
static uint8_t flash_unlock(void)
{
  FLASH->KEYR = FLASH_KEY1;
  FLASH->KEYR = FLASH_KEY2;

  return ((FLASH->CR & FLASH_CR_LOCK) != 0u) ? 1u : 0u;
}

/*******************************************************************************
* flash_lock: re-lock the FPEC so stray stores cannot corrupt flash.
*******************************************************************************/
static void flash_lock(void)
{
  FLASH->CR |= (uint32_t)FLASH_CR_LOCK;
}

/*******************************************************************************
* flash_erase_page: erase the 1 KB page containing addr (PER + AR + STRT, wait
* for BSY, clear PER).  Returns 0 = OK, 1 = timeout or erase error.  Caller
* holds the FPEC unlocked and locks on failure.
*******************************************************************************/
static uint8_t flash_erase_page(uint32_t addr)
{
  FLASH->CR |= (uint32_t)FLASH_CR_PER;
  FLASH->AR = addr;
  FLASH->CR |= (uint32_t)FLASH_CR_STRT;

  if (flash_wait_bsy() != 0u)
  {
    FLASH->CR &= ~(uint32_t)FLASH_CR_PER;
    return 1u;
  }

  FLASH->CR &= ~(uint32_t)FLASH_CR_PER;

  if ((FLASH->SR & ((uint32_t)FLASH_SR_PGERR | (uint32_t)FLASH_SR_WRPRTERR)) != 0u)
  {
    return 1u;
  }

  return 0u;
}

/*******************************************************************************
* flash_program_halfwords: program len bytes of src (len even, half-word
* aligned) at addr with the FPEC PG sequence.  Each 16-bit store is followed
* by a BSY poll and an error-flag check; PG is cleared on every exit path.
* Returns 0 = OK, 1 = timeout or programming/write-protection error.
*******************************************************************************/
static uint8_t flash_program_halfwords(uint32_t addr, const uint8_t *src, uint32_t len)
{
  FLASH->CR |= (uint32_t)FLASH_CR_PG;

  for (uint32_t i = 0u; i < len; i += 2u)
  {
    uint16_t halfword = (uint16_t)((uint16_t)src[i] | ((uint16_t)src[i + 1u] << 8));

    *(volatile uint16_t *)(addr + i) = halfword;

    if ((flash_wait_bsy() != 0u) ||
        ((FLASH->SR & ((uint32_t)FLASH_SR_PGERR | (uint32_t)FLASH_SR_WRPRTERR)) != 0u))
    {
      FLASH->CR &= ~(uint32_t)FLASH_CR_PG;
      return 1u;
    }
  }

  FLASH->CR &= ~(uint32_t)FLASH_CR_PG;
  return 0u;
}

/*******************************************************************************
* cfg_save: build the 16-byte image from cmd_keycode(0..2), erase the config
* page, program 8 half-words, then read the page back and verify every byte
* (magic/version/keymap/checksum and the 0xFF padding in one pass).  Returns 0
* only if the readback matches exactly.
*
* Main-loop only - never call from an ISR: page erase stalls CPU flash fetch
* for ~20-40 ms.  `make flash -e sector` preserves this page, `-e chip` wipes
* it.
*******************************************************************************/
uint8_t cfg_save(void)
{
  uint8_t  image[CFG_IMAGE_LEN];
  uint16_t sum;
  uint8_t  i;

  /* Fixed layout; the whole image starts as erased-state padding. */
  for (i = 0u; i < CFG_IMAGE_LEN; i++)
  {
    image[i] = CFG_PAD_BYTE;
  }
  store_u32_le(&image[CFG_OFF_MAGIC], CFG_MAGIC);
  image[CFG_OFF_VERSION] = (uint8_t)CFG_VERSION;
  for (i = 0u; i < CMD_KEY_COUNT; i++)
  {
    image[CFG_OFF_KEYMAP + i] = cmd_keycode(i);
  }
  sum = cfg_checksum(image);
  image[CFG_OFF_CHECKSUM] = (uint8_t)(sum & 0xFFu);
  image[CFG_OFF_CHECKSUM + 1u] = (uint8_t)(sum >> 8);

  flash_clear_errors();

  if (flash_unlock() != 0u)
  {
    flash_lock();
    return 1u;
  }

  if (flash_erase_page(CFG_FLASH_ADDR) != 0u)
  {
    flash_clear_errors();
    flash_lock();
    return 1u;
  }

  flash_clear_errors();   /* drop EOP from the erase before programming */

  if (flash_program_halfwords(CFG_FLASH_ADDR, image, CFG_IMAGE_LEN) != 0u)
  {
    flash_clear_errors();
    flash_lock();
    return 1u;
  }

  flash_lock();

  /* Readback verify: byte-for-byte against the image that was built above. */
  for (i = 0u; i < CFG_IMAGE_LEN; i++)
  {
    if (*(const volatile uint8_t *)(CFG_FLASH_ADDR + i) != image[i])
    {
      return 1u;
    }
  }

  return 0u;
}

/*******************************************************************************
* cfg_load: read the config page, validate magic, version and the 16-bit
* checksum; only then apply the three bytes via cmd_set_keymap().  Returns
* 0 = valid config loaded, 1 = no/invalid config (keymap untouched).
*
* Main-loop only (the call site is shared with cfg_save()).
*******************************************************************************/
uint8_t cfg_load(void)
{
  uint8_t  image[CFG_IMAGE_LEN];
  uint16_t sum;
  uint8_t  i;

  for (i = 0u; i < CFG_IMAGE_LEN; i++)
  {
    image[i] = *(const volatile uint8_t *)(CFG_FLASH_ADDR + i);
  }

  if (load_u32_le(&image[CFG_OFF_MAGIC]) != CFG_MAGIC)
  {
    return 1u;
  }
  if (image[CFG_OFF_VERSION] != (uint8_t)CFG_VERSION)
  {
    return 1u;
  }

  sum = cfg_checksum(image);
  if ((image[CFG_OFF_CHECKSUM] != (uint8_t)(sum & 0xFFu)) ||
      (image[CFG_OFF_CHECKSUM + 1u] != (uint8_t)(sum >> 8)))
  {
    return 1u;
  }

  for (i = 0u; i < CMD_KEY_COUNT; i++)
  {
    cmd_set_keymap(i, image[CFG_OFF_KEYMAP + i]);
  }

  return 0u;
}
