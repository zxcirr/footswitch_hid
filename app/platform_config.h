/**
  * platform_config.h - board shim for Blue Pill (STM32F103C8T6).
  * ST's original pulls in eval-board headers; this project only needs CMSIS.
  */
#ifndef __PLATFORM_CONFIG_H
#define __PLATFORM_CONFIG_H

#define STM32F10X_MD            /* C8T6 = medium density (required by stm32f10x.h) */
#include "stm32f10x.h"

/* Unique device ID registers (STM32F1x) */
#define ID1 (0x1FFFF7E8)
#define ID2 (0x1FFFF7EC)
#define ID3 (0x1FFFF7F0)

/* Eval-board BSP no-op stubs: the as-is CustomHID app layer calls these
 * (usb_endp.c LEDs, usb_prop.c ADC); Blue Pill has no eval BSP. Replaced
 * when the app layer is adapted (later task). */
typedef enum { Bit_RESET = 0, Bit_SET } BitAction;
typedef enum { LED1 = 0, LED2, LED3, LED4 } Led_TypeDef;
static __INLINE void STM_EVAL_LEDOn(Led_TypeDef Led)  { (void)Led; }
static __INLINE void STM_EVAL_LEDOff(Led_TypeDef Led) { (void)Led; }
static __INLINE void ADC_SoftwareStartConvCmd(ADC_TypeDef *ADCx, FunctionalState NewState)
{ (void)ADCx; (void)NewState; }

#define PWR_Regulator_LowPower PWR_CR_LPDS   /* ST stdPeriphLib name (usb_pwr.c) */

#endif /* __PLATFORM_CONFIG_H */
