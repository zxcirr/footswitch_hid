/**
  * usb_app.h - thin application wrapper around the ST USB FS device stack.
  *
  * Frozen interface used by main.c.
  */
#ifndef USB_APP_H
#define USB_APP_H

#include <stdint.h>

/* Global TX-busy flag (set while an EP1 IN report is in flight) */
extern volatile uint8_t USB_Tx_Busy;

/* Force a bus disconnect pulse, then bring up the USB device. */
void usb_app_init(void);

/* Queue one 8-byte keyboard report.  Returns 1 if queued, 0 if dropped
 * (not configured, or EP1 TX busy) so the caller can retry. */
uint8_t usb_app_send_report(const uint8_t report[8]);

/* 1 once SET_CONFIGURATION has been received. */
uint8_t usb_app_is_configured(void);

/* 1 while an EP1 IN transfer is in flight. */
uint8_t usb_app_is_busy(void);

#endif /* USB_APP_H */
