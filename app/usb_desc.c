/**
  ******************************************************************************
  * @file    usb_desc.c
  * @brief   USB HID descriptors: two interfaces, one top-level collection each.
  *
  * Derived from ST's STM32_USB-FS-Device_Lib V4.0.0 Custom_HID example and
  * rewritten for a 3-key footswitch keyboard (QMK/VIA-style raw-HID layout):
  *   - VID:PID = 0x1209:0x0001 (pid.codes open-source VID; change freely)
  *   - interface 0: boot keyboard (class 3 / subclass 1 / protocol 1),
  *     1 interrupt IN endpoint (0x81, 8 bytes, 1 ms), 63-byte report
  *     descriptor with ONE top-level Application collection (boot keyboard)
  *   - interface 1: vendor HID (class 3 / subclass 0 / protocol 0),
  *     1 interrupt IN endpoint (0x82, 8 bytes, 1 ms, never transmits),
  *     19-byte report descriptor with ONE top-level Application collection
  *     (Usage Page 0xFF00 / Usage 0x01 / Feature 8 bits x 48)
  *   - no report IDs anywhere.  Windows HidUsb parses HID per USB interface,
  *     and a report descriptor with >1 top-level collection and no report ID
  *     is rejected (STATUS_COULD_NOT_INTERPRET / 0xC00000B9); two single-TLC
  *     interfaces keep both HID devices acceptable and expose the vendor
  *     collection as its own device object (Shared access, no \KBD hack)
  *   - bcdDevice = 0.03 (rev bump: Windows caches descriptors per hardware ID
  *     USB\VID_1209&PID_0001&REV_xxxx, so bumping the revision forces a fresh
  *     read instead of reusing the stale cached copy)
  *   - string descriptors: LANGID 0x0409, "FootSwitch" + "HID Pedal" (no serial)
  ******************************************************************************
  */

#include "usb_lib.h"
#include "usb_desc.h"

/* USB Standard Device Descriptor.
 * bcdDevice = 0x0003 ("0.03"): Windows caches the parsed descriptors per
 * hardware ID (USB\VID_1209&PID_0001&REV_xxxx); the revision bump makes Windows
 * re-read them instead of reusing the stale cached copy. */
const uint8_t CustomHID_DeviceDescriptor[CUSTOMHID_SIZ_DEVICE_DESC] =
{
  0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x40,
  0x09, 0x12, 0x01, 0x00, 0x03, 0x00, 0x01, 0x02,
  0x00, 0x01
};

/* Configuration set (59 bytes): config(9) + iface0(9) + HID0(9) + EP1(7)
 *                                  + iface1(9) + HID1(9) + EP2(7) */
const uint8_t CustomHID_ConfigDescriptor[CUSTOMHID_SIZ_CONFIG_DESC] =
{
  /* Config: wTotalLength=59 (0x3B), bNumInterfaces=2, bus powered, 100 mA */
  0x09, 0x02, 0x3B, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
  /* Interface 0: boot keyboard, 1 endpoint, class 3 / sub 1 / proto 1 */
  0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
  /* HID 0: bcdHID 1.11, report descriptor 0x22 = 63 (0x3F) bytes */
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
  /* Endpoint 1 IN: interrupt, 8 bytes, 1 ms (keyboard reports) */
  0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01,
  /* Interface 1: vendor HID, 1 endpoint, class 3 / sub 0 / proto 0 */
  0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
  /* HID 1: bcdHID 1.11, report descriptor 0x22 = 19 (0x13) bytes */
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x13, 0x00,
  /* Endpoint 2 IN: interrupt, 8 bytes, 1 ms (vendor interface; never transmits, NAKs) */
  0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x01
};

/* HID Report Descriptor (63 bytes): canonical boot keyboard (HID 1.11
 * Appendix B.1) - 8-byte input report: modifier byte, reserved byte, 6 key
 * codes - as ONE top-level Application collection with no report IDs. */
const uint8_t CustomHID_ReportDescriptor[CUSTOMHID_SIZ_REPORT_DESC] =
{
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
  0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
  0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
  0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
  0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
  0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
  0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

/* HID Report Descriptor for interface 1 (19 bytes): vendor-defined 48-byte
 * Feature report in its own single top-level Application collection
 * (usage page 0xFF00, usage 0x01), no report IDs. */
const uint8_t CustomHID_VendorReportDescriptor[CUSTOMHID_SIZ_VENDOR_REPORT_DESC] =
{
  0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x15,
  0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x30,
  0xB1, 0x02, 0xC0
};

/* String descriptors: LANGID 0x0409, "FootSwitch", "HID Pedal", empty serial */
const uint8_t CustomHID_StringLangID[CUSTOMHID_SIZ_STRING_LANGID] =
{
  0x04, 0x03, 0x09, 0x04
};

const uint8_t CustomHID_StringVendor[CUSTOMHID_SIZ_STRING_VENDOR] =
{
  0x16, 0x03, 0x46, 0x00, 0x6F, 0x00, 0x6F, 0x00,
  0x74, 0x00, 0x53, 0x00, 0x77, 0x00, 0x69, 0x00,
  0x74, 0x00, 0x63, 0x00, 0x68, 0x00
};

const uint8_t CustomHID_StringProduct[CUSTOMHID_SIZ_STRING_PRODUCT] =
{
  0x14, 0x03, 0x48, 0x00, 0x49, 0x00, 0x44, 0x00,
  0x20, 0x00, 0x50, 0x00, 0x65, 0x00, 0x64, 0x00,
  0x61, 0x00, 0x6C, 0x00
};

uint8_t CustomHID_StringSerial[CUSTOMHID_SIZ_STRING_SERIAL] =
{
  0x02, 0x03
};
