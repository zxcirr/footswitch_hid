/**
  ******************************************************************************
  * @file    usb_desc.h
  * @brief   USB HID boot-keyboard descriptors (derived from ST's
  *          STM32_USB-FS-Device_Lib V4.0.0 Custom_HID example, adapted for a
  *          3-key footswitch keyboard: 1 interface, 1 interrupt IN endpoint).
  ******************************************************************************
  */

#ifndef __USB_DESC_H
#define __USB_DESC_H

/* Descriptor types */
#define USB_DEVICE_DESCRIPTOR_TYPE              0x01
#define USB_CONFIGURATION_DESCRIPTOR_TYPE       0x02
#define USB_STRING_DESCRIPTOR_TYPE              0x03
#define USB_INTERFACE_DESCRIPTOR_TYPE           0x04
#define USB_ENDPOINT_DESCRIPTOR_TYPE            0x05

#define HID_DESCRIPTOR_TYPE                     0x21
#define CUSTOMHID_SIZ_HID_DESC                  0x09
#define CUSTOMHID_OFF_HID_DESC                  0x12   /* offset of HID desc in config blob */

#define CUSTOMHID_SIZ_DEVICE_DESC               18
#define CUSTOMHID_SIZ_CONFIG_DESC               34
#define CUSTOMHID_SIZ_REPORT_DESC               63
#define CUSTOMHID_SIZ_STRING_LANGID             4
#define CUSTOMHID_SIZ_STRING_VENDOR             2
#define CUSTOMHID_SIZ_STRING_PRODUCT            2
#define CUSTOMHID_SIZ_STRING_SERIAL             2

#define STANDARD_ENDPOINT_DESC_SIZE             0x09

/* Exported data ------------------------------------------------------------- */
extern const uint8_t CustomHID_DeviceDescriptor[CUSTOMHID_SIZ_DEVICE_DESC];
extern const uint8_t CustomHID_ConfigDescriptor[CUSTOMHID_SIZ_CONFIG_DESC];
extern const uint8_t CustomHID_ReportDescriptor[CUSTOMHID_SIZ_REPORT_DESC];
extern const uint8_t CustomHID_StringLangID[CUSTOMHID_SIZ_STRING_LANGID];
extern const uint8_t CustomHID_StringVendor[CUSTOMHID_SIZ_STRING_VENDOR];
extern const uint8_t CustomHID_StringProduct[CUSTOMHID_SIZ_STRING_PRODUCT];
extern uint8_t CustomHID_StringSerial[CUSTOMHID_SIZ_STRING_SERIAL];

#endif /* __USB_DESC_H */
