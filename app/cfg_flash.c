/**
  ******************************************************************************
  * @file    cfg_flash.c
  * @brief   Register-level FPEC persistence for the 3-pedal configuration
  *          (single bindings + macro steps/timing), stored in the LAST 1 KB
  *          page of the 64 KB flash (CFG_FLASH_ADDR).
  *
  * Only CMSIS register definitions are used (via platform_config.h) - no
  * StdPeriph and no libc string functions.  The on-flash image is the fixed
  * 64-byte v2 layout documented in cfg.h (explicit byte offsets, no
  * struct/padding assumptions).  A legacy 16-byte v1 image (keymap only) is
  * migrated on load: single bindings kept, default timing, config marked
  * dirty so the next SAVE rewrites the page as v2.
  *
  * FPEC sequence per RM0008: unlock (KEYR magic keys) -> page erase (PER/AR/
  * STRT, poll BSY) -> half-word program (PG, 16-bit store per half-word, poll
  * BSY) -> lock.  Any timeout or PGERR/WRPRTERR aborts: error flags cleared,
  * FPEC re-locked, failure returned.
  *
  * !! cfg_save()/cfg_load() MUST be called from the main loop, NEVER from an
  *    ISR: page erase stalls CPU instruction fetch from flash for ~20-40 ms.
  *
  * Flash-image guard: the firmware is ~11 KB (ends ~0x080029C0), far below
  * 0xFC00, so nothing here collides with the linker script.  `make flash
  * -e sector` preserves this page while `-e chip` wipes it.
  ******************************************************************************
  */

#include "cfg.h"
#include "cmd.h"
#include "platform_config.h"

/* ---- v2 on-flash image offsets (64 bytes, half-word aligned payload) ----- */
#define CFG_OFF_MAGIC     0u    /* [0..3]   */
#define CFG_OFF_VERSION   4u    /* [4]      */
#define CFG_OFF_PEDAL     5u    /* [5..61]  three 19-byte blocks */
#define CFG_PEDAL_LEN     19u
#define CFG_PEDAL_MODE    0u    /* block +0     */
#define CFG_PEDAL_CODE    1u    /* block +1     */
#define CFG_PEDAL_PRESS   2u    /* block +2..3  */
#define CFG_PEDAL_GAP     4u    /* block +4..5  */
#define CFG_PEDAL_COUNT   6u    /* block +6     */
#define CFG_PEDAL_STEPS   7u    /* block +7..18 */
#define CFG_OFF_CHECKSUM  62u   /* [62..63] sum of bytes [0..61] */
#define CFG_PAD_BYTE      0xFFu /* erased-state filler */

/* ---- legacy v1 image (16 bytes: keymap only), for migration -------------- */
#define CFG1_VERSION      1u
#define CFG1_OFF_KEYMAP   5u    /* [5..7]  */
#define CFG1_OFF_CHECKSUM 8u    /* [8..9]  sum of bytes [0..7] */

/* FPEC unlock keys (RM0008, fixed constants). */
#define FLASH_KEY1        0x45670123u
#define FLASH_KEY2        0xCDEF89ABu

/* Bounded busy-wait budget per erase/program operation.  Page erase takes
 * ~20-40 ms at 72 MHz; the poll loop is a few cycles per iteration, so the
 * budget is well above 100 ms - generous margin without hanging forever on a
 * stuck BSY. */
#define FLASH_TIMEOUT     2000000u

/*******************************************************************************
* load_u32_le / store_u32_le: little-endian u32 assembled/stored byte-wise
* (no alignment or endianness assumption).
*******************************************************************************/
static uint32_t load_u32_le(const uint8_t *p)
{
  return ((uint32_t)p[0]) |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void store_u32_le(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 0);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/*******************************************************************************
* load_u16_le / store_u16_le: little-endian u16, same rules.
*******************************************************************************/
static uint16_t load_u16_le(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void store_u16_le(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)(v >> 8);
}

/*******************************************************************************
* cfg_checksum: 16-bit sum of the image bytes covered by the checksum field
* ([0..CFG_OFF_CHECKSUM), i.e. magic, version and all three pedal blocks);
* wraps modulo 2^16.
*******************************************************************************/
static uint16_t cfg_checksum(const uint8_t *image)
{
  uint16_t sum = 0u;

  for (uint16_t i = 0u; i < CFG_OFF_CHECKSUM; i++)
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
* cfg_image_build: assemble the 64-byte v2 image from a three-pedal snapshot
* (magic, version, per-pedal blocks, checksum).  Pure function - no flash
* access, safe to call from anywhere.  Step slots at or above step_count stay
* 0xFF (erased state).
*******************************************************************************/
void cfg_image_build(uint8_t image[CFG_IMAGE_LEN], const cmd_pedal_t ped[CMD_KEY_COUNT])
{
  uint16_t sum;
  uint8_t i;

  for (i = 0u; i < CFG_IMAGE_LEN; i++)
  {
    image[i] = CFG_PAD_BYTE;
  }

  store_u32_le(&image[CFG_OFF_MAGIC], CFG_MAGIC);
  image[CFG_OFF_VERSION] = (uint8_t)CFG_VERSION;

  for (i = 0u; i < CMD_KEY_COUNT; i++)
  {
    uint8_t *blk = &image[CFG_OFF_PEDAL + (uint16_t)(i * CFG_PEDAL_LEN)];
    uint8_t s;

    blk[CFG_PEDAL_MODE] = ped[i].mode;
    blk[CFG_PEDAL_CODE] = ped[i].code;
    store_u16_le(&blk[CFG_PEDAL_PRESS], ped[i].press_ms);
    store_u16_le(&blk[CFG_PEDAL_GAP], ped[i].gap_ms);
    blk[CFG_PEDAL_COUNT] = ped[i].step_count;
    for (s = 0u; s < ped[i].step_count && s < CMD_MACRO_MAX_STEPS; s++)
    {
      blk[CFG_PEDAL_STEPS + (uint16_t)(2u * s)] = ped[i].mod_mask[s];
      blk[CFG_PEDAL_STEPS + (uint16_t)(2u * s) + 1u] = ped[i].keycode[s];
    }
  }

  sum = cfg_checksum(image);
  store_u16_le(&image[CFG_OFF_CHECKSUM], sum);
}

/*******************************************************************************
* cfg_save: erase the config page, program the caller's prebuilt 64-byte
* image, then read the page back and verify every byte (magic/version/pedal
* blocks/checksum and the 0xFF padding in one pass).  Returns 0 only if the
* readback matches exactly.
*
* Main-loop only - never call from an ISR: page erase stalls CPU flash fetch
* for ~20-40 ms.  `make flash -e sector` preserves this page, `-e chip` wipes
* it.
*******************************************************************************/
uint8_t cfg_save(const uint8_t image[CFG_IMAGE_LEN])
{
  uint8_t i;

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

  /* Readback verify: byte-for-byte against the image that was programmed. */
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
* pedal_block_valid: full field validation of one 19-byte v2 pedal block.
* The whole image is rejected when any block fails, so an invalid persisted
* config can never reach the RAM state.
*******************************************************************************/
static uint8_t pedal_block_valid(const uint8_t *blk)
{
  uint16_t press = load_u16_le(&blk[CFG_PEDAL_PRESS]);
  uint16_t gap = load_u16_le(&blk[CFG_PEDAL_GAP]);
  uint8_t count = blk[CFG_PEDAL_COUNT];
  uint8_t s;

  if (blk[CFG_PEDAL_MODE] > CMD_MODE_MACRO) { return 0u; }
  if (cmd_code_ok(blk[CFG_PEDAL_CODE]) == 0u) { return 0u; }
  if ((press == 0u) || (press > CMD_MACRO_TIME_MAX)) { return 0u; }
  if ((gap == 0u) || (gap > CMD_MACRO_TIME_MAX)) { return 0u; }
  if (count > CMD_MACRO_MAX_STEPS) { return 0u; }
  if ((blk[CFG_PEDAL_MODE] == CMD_MODE_MACRO) && (count == 0u)) { return 0u; }

  for (s = 0u; s < count; s++)
  {
    if (cmd_step_ok(blk[CFG_PEDAL_STEPS + (uint16_t)(2u * s)],
                    blk[CFG_PEDAL_STEPS + (uint16_t)(2u * s) + 1u]) == 0u)
    {
      return 0u;
    }
  }
  return 1u;
}

/*******************************************************************************
* cfg_load_v1: migrate a legacy 16-byte v1 image (magic/version already
* checked by the caller): verify the v1 checksum (sum of bytes [0..7]) and
* the three keymap codes, then install them as single-mode bindings with
* default timing and no steps.  The config is marked dirty: the RAM view now
* differs from what the next v2 save would write, so INFO nudges the user to
* re-SAVE.  Returns 0 = migrated, 1 = invalid (RAM untouched).
*******************************************************************************/
static uint8_t cfg_load_v1(const uint8_t *image)
{
  uint16_t sum = 0u;
  uint8_t i;

  for (i = 0u; i < CFG1_OFF_CHECKSUM; i++)
  {
    sum = (uint16_t)(sum + image[i]);
  }
  if (load_u16_le(&image[CFG1_OFF_CHECKSUM]) != sum) { return 1u; }

  for (i = 0u; i < CMD_KEY_COUNT; i++)
  {
    if (cmd_code_ok(image[CFG1_OFF_KEYMAP + i]) == 0u) { return 1u; }
  }

  for (i = 0u; i < CMD_KEY_COUNT; i++)
  {
    cmd_pedal_t p;
    uint8_t s;

    p.mode = CMD_MODE_SINGLE;
    p.code = image[CFG1_OFF_KEYMAP + i];
    p.press_ms = CMD_MACRO_PRESS_DEFAULT;
    p.gap_ms = CMD_MACRO_GAP_DEFAULT;
    p.step_count = 0u;
    for (s = 0u; s < CMD_MACRO_MAX_STEPS; s++)
    {
      p.mod_mask[s] = 0u;
      p.keycode[s] = 0u;
    }
    cmd_set_pedal(i, &p);
  }

  cmd_mark_dirty();
  return 0u;
}

/*******************************************************************************
* cfg_load: read the config page, dispatch on the version byte, validate the
* image (checksum + every field of every pedal block) and only then apply it
* via cmd_set_pedal().  Returns 0 = valid config loaded (v1 counts: it is
* migrated and marked dirty), 1 = no/invalid config (RAM untouched).
*
* Main-loop only (the call site is shared with cfg_save()).
*******************************************************************************/
uint8_t cfg_load(void)
{
  uint8_t image[CFG_IMAGE_LEN];
  uint8_t i;

  for (i = 0u; i < CFG_IMAGE_LEN; i++)
  {
    image[i] = *(const volatile uint8_t *)(CFG_FLASH_ADDR + i);
  }

  if (load_u32_le(&image[CFG_OFF_MAGIC]) != CFG_MAGIC)
  {
    return 1u;
  }

  if (image[CFG_OFF_VERSION] == CFG1_VERSION)
  {
    return cfg_load_v1(image);
  }
  if (image[CFG_OFF_VERSION] != (uint8_t)CFG_VERSION)
  {
    return 1u;
  }

  if (load_u16_le(&image[CFG_OFF_CHECKSUM]) != cfg_checksum(image))
  {
    return 1u;
  }

  for (i = 0u; i < CMD_KEY_COUNT; i++)
  {
    if (pedal_block_valid(&image[CFG_OFF_PEDAL + (uint16_t)(i * CFG_PEDAL_LEN)]) == 0u)
    {
      return 1u;   /* reject the whole image: never install an invalid config */
    }
  }

  for (i = 0u; i < CMD_KEY_COUNT; i++)
  {
    const uint8_t *blk = &image[CFG_OFF_PEDAL + (uint16_t)(i * CFG_PEDAL_LEN)];
    cmd_pedal_t p;
    uint8_t s;

    p.mode = blk[CFG_PEDAL_MODE];
    p.code = blk[CFG_PEDAL_CODE];
    p.press_ms = load_u16_le(&blk[CFG_PEDAL_PRESS]);
    p.gap_ms = load_u16_le(&blk[CFG_PEDAL_GAP]);
    p.step_count = blk[CFG_PEDAL_COUNT];
    for (s = 0u; s < CMD_MACRO_MAX_STEPS; s++)
    {
      if (s < p.step_count)
      {
        p.mod_mask[s] = blk[CFG_PEDAL_STEPS + (uint16_t)(2u * s)];
        p.keycode[s] = blk[CFG_PEDAL_STEPS + (uint16_t)(2u * s) + 1u];
      }
      else
      {
        p.mod_mask[s] = 0u;
        p.keycode[s] = 0u;
      }
    }
    cmd_set_pedal(i, &p);
  }

  return 0u;
}
