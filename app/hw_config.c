/**
  ******************************************************************************
  * @file    hw_config.c
  * @brief   Board-level USB support (register-level) for the
  *          TLS-STM32F103C8T6 core board (Blue Pill compatible).
  *
  * USB pins: PA11 = USB_DM, PA12 = USB_DP (dedicated USB FS transceiver).
  * The board has a hardwired 4.7k pull-up from D+ to the 5 V rail, so there is
  * no software-controlled attach; usb_disconnect_pulse() briefly drives both
  * data lines low (SE0) to force the host to see a disconnect.
  ******************************************************************************
  */

#include "hw_config.h"
#include "usb_lib.h"
#include "usb_conf.h"
#include "usb_pwr.h"

/* Cortex-M3 SysTick (used only for a short busy delay) */
#define SYSTICK_CTRL    (*(volatile uint32_t *)0xE000E010)
#define SYSTICK_VAL     (*(volatile uint32_t *)0xE000E018)

#define SYSTICK_ENABLE  (1u << 0)
#define SYSTICK_COUNTFLAG (1u << 16)

/* USB pin bit masks on GPIOA CRH (PA11: bits 15:12, PA12: bits 19:16) */
#define PA11_CRH_MASK   (0xFu << 12)
#define PA12_CRH_MASK   (0xFu << 16)
#define GPIO_CNF_IN_FLOAT   (0x4u << 0)   /* CNF=01 MODE=00 -> input floating */
#define GPIO_CNF_OUT_PP_2M  (0x2u << 0)   /* CNF=00 MODE=10 -> output PP 2 MHz */

static void delay_ms_busy(uint32_t ms);

/*******************************************************************************
* Set_System: main.c owns the 72 MHz clock setup.
*******************************************************************************/
void Set_System(void)
{
}

/*******************************************************************************
* Set_USBClock: USB clock = PLL / 1.5 = 48 MHz (USBPRE = 0), enable USBEN.
*******************************************************************************/
void Set_USBClock(void)
{
  RCC->CFGR &= ~(uint32_t)(1UL << 22);      /* USBPRE = 0: PLL / 1.5 */
  RCC->APB1ENR |= (uint32_t)(1UL << 23);    /* USBEN = 1 */
}

/*******************************************************************************
* USB_Interrupts_Config: enable the low-priority USB interrupt (IRQ 20).
*******************************************************************************/
void USB_Interrupts_Config(void)
{
  NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
}

/*******************************************************************************
* GPIO_Configuration: PA11/PA12 as floating inputs (the USB transceiver
* drives them; the pins must not have pull-up/pull-down enabled).
*******************************************************************************/
void GPIO_Configuration(void)
{
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
  GPIOA->CRH &= ~(PA11_CRH_MASK | PA12_CRH_MASK);
  GPIOA->CRH |= (GPIO_CNF_IN_FLOAT << 12) | (GPIO_CNF_IN_FLOAT << 16);
}

/*******************************************************************************
* USB_Cable_Config: no-op - the D+ pull-up is hardwired on this board.
*******************************************************************************/
void USB_Cable_Config(FunctionalState NewState)
{
  (void)NewState;
}

/*******************************************************************************
* usb_disconnect_pulse: drive PA11+PA12 push-pull low for ~100 ms (SE0), then
* release them back to floating inputs.  Must be called BEFORE USB_Init().
*******************************************************************************/
void usb_disconnect_pulse(void)
{
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;

  GPIOA->CRH &= ~(PA11_CRH_MASK | PA12_CRH_MASK);
  GPIOA->CRH |= (GPIO_CNF_OUT_PP_2M << 12) | (GPIO_CNF_OUT_PP_2M << 16);
  GPIOA->BRR = (1u << 11) | (1u << 12);      /* both data lines low */

  delay_ms_busy(100);

  GPIOA->CRH &= ~(PA11_CRH_MASK | PA12_CRH_MASK);
  GPIOA->CRH |= (GPIO_CNF_IN_FLOAT << 12) | (GPIO_CNF_IN_FLOAT << 16);
}

/*******************************************************************************
* Get_SerialNum / low-power hooks: not used on this board.
*******************************************************************************/
void Get_SerialNum(void)
{
}

void Enter_LowPowerMode(void)
{
}

void Leave_LowPowerMode(void)
{
}

/*******************************************************************************
* delay_ms_busy: 1 ms per SysTick wrap (main.c starts SysTick before USB init).
* Falls back to a crude loop if SysTick is not running.
*******************************************************************************/
static void delay_ms_busy(uint32_t ms)
{
  if ((SYSTICK_CTRL & SYSTICK_ENABLE) != 0)
  {
    while (ms-- != 0)
    {
      SYSTICK_VAL = 0;
      while ((SYSTICK_CTRL & SYSTICK_COUNTFLAG) == 0)
      {
      }
    }
  }
  else
  {
    while (ms-- != 0)
    {
      for (volatile uint32_t i = 0; i < 9000u; i++)
      {
      }
    }
  }
}
