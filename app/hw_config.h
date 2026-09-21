/**
  ******************************************************************************
  * @file    hw_config.h
  * @brief   Board-level USB support for the TLS-STM32F103C8T6 core board.
  *          Register-level only (no StdPeriph); clocks are owned by main.c.
  ******************************************************************************
  */

#ifndef __HW_CONFIG_H
#define __HW_CONFIG_H

#include "platform_config.h"
#include "usb_type.h"

/* Exported functions ------------------------------------------------------- */
void Set_System(void);              /* no-op: main.c sets up 72 MHz */
void Set_USBClock(void);            /* USBPRE=0 (48 MHz) + USBEN */
void USB_Interrupts_Config(void);   /* NVIC IRQ20 (USB_LP_CAN1_RX0) */
void GPIO_Configuration(void);      /* PA11/PA12 floating input */
void USB_Cable_Config(FunctionalState NewState);  /* no-op: pull-up hardwired */
void usb_disconnect_pulse(void);    /* PA11+PA12 low ~100 ms, then floating */
void Get_SerialNum(void);           /* no-op: no serial string */
void Enter_LowPowerMode(void);
void Leave_LowPowerMode(void);

#endif  /* __HW_CONFIG_H */
