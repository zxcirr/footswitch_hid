/**
  ******************************************************************************
  * @file    usb_endp.c
  * @brief   Endpoint callbacks (keyboard: only EP1 IN is used).
  *
  * Derived from ST's STM32_USB-FS-Device_Lib V4.0.0 Custom_HID example.
  ******************************************************************************
  */

#include "hw_config.h"
#include "usb_lib.h"
#include "usb_istr.h"
#include "usb_app.h"

/*******************************************************************************
* Function Name  : EP1_IN_Callback.
* Description    : EP1 IN transfer complete -> release the TX busy flag so the
*                  next keyboard report can be queued.
*******************************************************************************/
void EP1_IN_Callback(void)
{
  USB_Tx_Busy = 0;
}
