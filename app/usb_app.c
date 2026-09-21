/**
  * usb_app.c - thin application wrapper around the ST USB FS device stack.
  */

#include "usb_app.h"
#include "usb_lib.h"
#include "usb_conf.h"
#include "usb_prop.h"
#include "hw_config.h"

#define HID_REPORT_LEN  8

volatile uint8_t USB_Tx_Busy = 0;

/*******************************************************************************
* usb_app_init: PA12 disconnect pulse -> 48 MHz USB clock -> USB_Init().
* The board has a hardwired 4.7k pull-up on D+ (no transistor), so a short
* SE0 pulse is the only way to make the host re-enumerate after an MCU reset.
*******************************************************************************/
void usb_app_init(void)
{
  usb_disconnect_pulse();
  Set_USBClock();
  GPIO_Configuration();
  USB_Interrupts_Config();
  USB_Init();
}

/*******************************************************************************
* usb_app_send_report: queue an 8-byte input report on EP1 IN.
* Returns 1 when queued, 0 when dropped (not configured / TX busy) so main.c
* can retry.  Called from the main loop; EP1_IN_Callback() clears USB_Tx_Busy.
*******************************************************************************/
uint8_t usb_app_send_report(const uint8_t report[8])
{
  __disable_irq();
  if ((USB_Configured == 0) || (USB_Tx_Busy != 0))
  {
    __enable_irq();
    return 0;
  }

  USB_Tx_Busy = 1;
  for (uint8_t i = 0; i < HID_REPORT_LEN; i++)
  {
    Report_Buf[i] = report[i];      /* keep GET_REPORT in sync */
  }
  UserToPMABufferCopy((uint8_t *)report, ENDP1_TXADDR, HID_REPORT_LEN);
  SetEPTxCount(ENDP1, HID_REPORT_LEN);
  SetEPTxStatus(ENDP1, EP_TX_VALID);
  __enable_irq();
  return 1;
}

uint8_t usb_app_is_configured(void)
{
  return (USB_Configured != 0) ? 1 : 0;
}

uint8_t usb_app_is_busy(void)
{
  return (USB_Tx_Busy != 0) ? 1 : 0;
}
