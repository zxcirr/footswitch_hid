/**
  ******************************************************************************
  * @file    usb_prop.c
  * @author  MCD Application Team
  * @version V4.0.0
  * @date    21-January-2013
  * @brief   All processings related to Custom HID Demo
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; COPYRIGHT 2013 STMicroelectronics</center></h2>
  *
  * Licensed under MCD-ST Liberty SW License Agreement V2, (the "License");
  * You may not use this file except in compliance with the License.
  * You may obtain a copy of the License at:
  *
  *        http://www.st.com/software_license_agreement_liberty_v2
  *
  * Unless required by applicable law or agreed to in writing, software 
  * distributed under the License is distributed on an "AS IS" BASIS, 
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
  ******************************************************************************
  */


/* Includes ------------------------------------------------------------------*/

#include "hw_config.h" 
#include "usb_lib.h"
#include "usb_conf.h"
#include "usb_prop.h"
#include "usb_desc.h"
#include "usb_pwr.h"
#include "usb_app.h"
#include "cmd.h"


/* Private typedef -----------------------------------------------------------*/ 
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
uint32_t ProtocolValue = 1;          /* 1 = report protocol (matches descriptor) */
__IO uint8_t EXTI_Enable;
__IO uint8_t Request = 0;
uint8_t Report_Buf[8];               /* current 8-byte keyboard input report */
uint8_t OutReport_Buf[1];            /* LED output report (ACKed, unused) */
__IO uint8_t USB_Configured = 0;     /* set by SET_CONFIGURATION */

/* Feature-report command channel (cmd.h) -----------------------------------*/
static uint8_t  cmd_buf[CMD_FEATURE_LEN];      /* Feature OUT payload (written by DataStageOut ISR) */
static uint8_t  resp_buf[CMD_FEATURE_LEN];     /* current published response */
static uint8_t  resp_snap[CMD_FEATURE_LEN];    /* per-transfer snapshot */
volatile uint8_t resp_len;                     /* valid bytes in resp_buf (never 0 after init) */
static volatile uint8_t feature_cmd_pending;   /* set when a Feature SET is armed, cleared in Status_In */
volatile uint8_t feature_save_pending;         /* set by Status_In when cmd_handle() returned 1; serviced by main */
/* -------------------------------------------------------------------------- */
/*  Structures initializations */
/* -------------------------------------------------------------------------- */

DEVICE Device_Table =
  {
    EP_NUM,
    1
  };

DEVICE_PROP Device_Property =
  {
    CustomHID_init,
    CustomHID_Reset,
    CustomHID_Status_In,
    CustomHID_Status_Out,
    CustomHID_Data_Setup,
    CustomHID_NoData_Setup,
    CustomHID_Get_Interface_Setting,
    CustomHID_GetDeviceDescriptor,
    CustomHID_GetConfigDescriptor,
    CustomHID_GetStringDescriptor,
    0,
    0x40 /*MAX PACKET SIZE*/
  };
USER_STANDARD_REQUESTS User_Standard_Requests =
  {
    CustomHID_GetConfiguration,
    CustomHID_SetConfiguration,
    CustomHID_GetInterface,
    CustomHID_SetInterface,
    CustomHID_GetStatus,
    CustomHID_ClearFeature,
    CustomHID_SetEndPointFeature,
    CustomHID_SetDeviceFeature,
    CustomHID_SetDeviceAddress
  };

ONE_DESCRIPTOR Device_Descriptor =
  {
    (uint8_t*)CustomHID_DeviceDescriptor,
    CUSTOMHID_SIZ_DEVICE_DESC
  };

ONE_DESCRIPTOR Config_Descriptor =
  {
    (uint8_t*)CustomHID_ConfigDescriptor,
    CUSTOMHID_SIZ_CONFIG_DESC
  };

ONE_DESCRIPTOR CustomHID_Report_Descriptor =
  {
    (uint8_t *)CustomHID_ReportDescriptor,
    CUSTOMHID_SIZ_REPORT_DESC
  };

ONE_DESCRIPTOR CustomHID_Hid_Descriptor =
  {
    (uint8_t*)CustomHID_ConfigDescriptor + CUSTOMHID_OFF_HID_DESC,
    CUSTOMHID_SIZ_HID_DESC
  };

ONE_DESCRIPTOR String_Descriptor[4] =
  {
    {(uint8_t*)CustomHID_StringLangID, CUSTOMHID_SIZ_STRING_LANGID},
    {(uint8_t*)CustomHID_StringVendor, CUSTOMHID_SIZ_STRING_VENDOR},
    {(uint8_t*)CustomHID_StringProduct, CUSTOMHID_SIZ_STRING_PRODUCT},
    {(uint8_t*)CustomHID_StringSerial, CUSTOMHID_SIZ_STRING_SERIAL}
  };

/* Extern variables ----------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/

/*CustomHID_SetReport_Feature function prototypes*/
uint8_t *CustomHID_SetReport_Feature(uint16_t Length);
uint8_t *CustomHID_GetReport(uint16_t Length);

/* Extern function prototypes ------------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/*******************************************************************************
* Function Name  : usb_feature_publish.
* Description    : Publish the Feature-report response returned by the next
*                  GET_REPORT(Feature).  Up to CMD_FEATURE_LEN bytes of msg are
*                  copied and NUL-padded; the published length is always >= 1
*                  (a zero-length IN reply makes the stack STALL the request).
*                  IRQs are masked while resp_buf/resp_len are updated, so it
*                  is safe from both the USB interrupt and the main loop.
* Input          : msg - NUL-terminated string (NULL publishes an empty reply).
* Output         : None.
* Return         : None.
*******************************************************************************/
void usb_feature_publish(const char *msg)
{
  uint8_t len = 0;
  uint8_t i;

  if (msg != NULL)
  {
    while ((len < CMD_FEATURE_LEN) && (msg[len] != '\0'))
    {
      len++;
    }
  }

  __disable_irq();
  for (i = 0; i < CMD_FEATURE_LEN; i++)
  {
    if (i < len)
    {
      resp_buf[i] = (uint8_t)msg[i];
    }
    else
    {
      resp_buf[i] = 0;
    }
  }
  resp_len = (len != 0) ? len : 1;
  __enable_irq();
}

/*******************************************************************************
* Function Name  : CustomHID_init.
* Description    : Custom HID init routine.
* Input          : None.
* Output         : None.
* Return         : None.
*******************************************************************************/
void CustomHID_init(void)
{
  /* Update the serial number string descriptor with the data from the unique 
  ID*/
  Get_SerialNum();

  /* Publish a placeholder so an early GET_REPORT(Feature) never returns zero
  bytes (the library turns a zero-length IN reply into a STALL) */
  usb_feature_publish("OK ready\n");

  pInformation->Current_Configuration = 0;
  /* Connect the device */
  PowerOn();

  /* Perform basic device initialization operations */
  USB_SIL_Init();

  bDeviceState = UNCONNECTED;
}

/*******************************************************************************
* Function Name  : CustomHID_Reset.
* Description    : Custom HID reset routine.
* Input          : None.
* Output         : None.
* Return         : None.
*******************************************************************************/
void CustomHID_Reset(void)
{
  /* Set CustomHID_DEVICE as not configured */
  pInformation->Current_Configuration = 0;
  pInformation->Current_Interface = 0;/*the default Interface*/
  
  /* Current Feature initialization */
  pInformation->Current_Feature = CustomHID_ConfigDescriptor[7];
 
  SetBTABLE(BTABLE_ADDRESS);

  /* Initialize Endpoint 0 */
  SetEPType(ENDP0, EP_CONTROL);
  SetEPTxStatus(ENDP0, EP_TX_STALL);
  SetEPRxAddr(ENDP0, ENDP0_RXADDR);
  SetEPTxAddr(ENDP0, ENDP0_TXADDR);
  Clear_Status_Out(ENDP0);
  SetEPRxCount(ENDP0, Device_Property.MaxPacketSize);
  SetEPRxValid(ENDP0);

  /* Initialize Endpoint 1 (interrupt IN only: keyboard reports) */
  SetEPType(ENDP1, EP_INTERRUPT);
  SetEPTxAddr(ENDP1, ENDP1_TXADDR);
  SetEPTxCount(ENDP1, 8);
  SetEPTxStatus(ENDP1, EP_TX_NAK);

  /* Set this device to response on default address */
  SetDeviceAddress(0);
  USB_Tx_Busy = 0;      /* a bus reset may hit an in-flight report */
  feature_cmd_pending = 0; /* abort any armed Feature SET: cmd_buf is stale */
  bDeviceState = ATTACHED;
}
/*******************************************************************************
* Function Name  : CustomHID_SetConfiguration.
* Description    : Update the device state to configured and command the ADC 
*                  conversion.
* Input          : None.
* Output         : None.
* Return         : None.
*******************************************************************************/
void CustomHID_SetConfiguration(void)
{
  if (pInformation->Current_Configuration != 0)
  {
    /* Device configured: keyboard reports may now be queued on EP1 IN */
    bDeviceState = CONFIGURED;
    USB_Configured = 1;
  }
}
/*******************************************************************************
* Function Name  : CustomHID_SetConfiguration.
* Description    : Update the device state to addressed.
* Input          : None.
* Output         : None.
* Return         : None.
*******************************************************************************/
void CustomHID_SetDeviceAddress (void)
{
  bDeviceState = ADDRESSED;
}
/*******************************************************************************
* Function Name  : CustomHID_Status_In.
* Description    : Joystick status IN routine.
* Input          : None.
* Output         : None.
* Return         : None.
*******************************************************************************/
void CustomHID_Status_In(void)
{  
  BitAction Led_State;
  
  if (feature_cmd_pending != 0)
  {
    /* A Feature SET just finished: cmd_buf now holds the request. */
    uint8_t reply[CMD_FEATURE_LEN];

    feature_cmd_pending = 0;
    if (cmd_handle(cmd_buf, reply) != 0)   /* 1 = SAVE requested */
    {
      feature_save_pending = 1;            /* serviced by the main loop */
    }
    usb_feature_publish((const char *)reply);
    return;
  }

  if (Report_Buf[1] == 0)
  {
    Led_State = Bit_RESET;
  }
  else 
  {
    Led_State = Bit_SET;
  }
  
  switch (Report_Buf[0])  
  {
    /*Change LED's status according to the host report*/
    
  case 1: /* Led 1 */ 
    if (Led_State != Bit_RESET)
    {
      STM_EVAL_LEDOn(LED1);
    }
    else
    {
      STM_EVAL_LEDOff(LED1);
    }
    break;
  case 2:   /* Led 2 */    
    if (Led_State != Bit_RESET)
    {
      STM_EVAL_LEDOn(LED2);
    }
    else
    {
      STM_EVAL_LEDOff(LED2);
    }
    break;
  case 3:/* Led 3 */    
    if (Led_State != Bit_RESET)
    {
      STM_EVAL_LEDOn(LED3);
    }
    else
    {
      STM_EVAL_LEDOff(LED3);
    }
    break;
  case 4:/* Led 4 */    
    if (Led_State != Bit_RESET)
    {
      STM_EVAL_LEDOn(LED4);
    }
    else
    {
      STM_EVAL_LEDOff(LED4);
    }
    break;
  default:
    STM_EVAL_LEDOff(LED1);
    STM_EVAL_LEDOff(LED2);
    STM_EVAL_LEDOff(LED3);
    STM_EVAL_LEDOff(LED4); 
    break;
  }
}

/*******************************************************************************
* Function Name  : CustomHID_Status_Out
* Description    : Joystick status OUT routine.
* Input          : None.
* Output         : None.
* Return         : None.
*******************************************************************************/
void CustomHID_Status_Out (void)
{
}

/*******************************************************************************
* Function Name  : CustomHID_Data_Setup
* Description    : Handle the data class specific requests.
* Input          : Request Nb.
* Output         : None.
* Return         : USB_UNSUPPORT or USB_SUCCESS.
*******************************************************************************/
RESULT CustomHID_Data_Setup(uint8_t RequestNo)
{
  uint8_t *(*CopyRoutine)(uint16_t);
  
  if (pInformation->USBwIndex != 0) 
    return USB_UNSUPPORT;    
  
  CopyRoutine = NULL;
  
  if ((RequestNo == GET_DESCRIPTOR)
      && (Type_Recipient == (STANDARD_REQUEST | INTERFACE_RECIPIENT))
        )
  {
    
    if (pInformation->USBwValue1 == REPORT_DESCRIPTOR)
    {
      CopyRoutine = CustomHID_GetReportDescriptor;
    }
    else if (pInformation->USBwValue1 == HID_DESCRIPTOR_TYPE)
    {
      CopyRoutine = CustomHID_GetHIDDescriptor;
    }
    
  } /* End of GET_DESCRIPTOR */
  
  /*** GET_PROTOCOL, GET_REPORT, SET_REPORT ***/
  else if ( (Type_Recipient == (CLASS_REQUEST | INTERFACE_RECIPIENT)) )
  {         
    switch( RequestNo )
    {
    case GET_PROTOCOL:
      CopyRoutine = CustomHID_GetProtocolValue;
      break;
    case GET_REPORT:
      CopyRoutine = CustomHID_GetReport;
      break;
    case SET_REPORT:
      CopyRoutine = CustomHID_SetReport_Feature;
      Request = SET_REPORT;
      break;
    default:
      break;
    }
  }
  
  if (CopyRoutine == NULL)
  {
    return USB_UNSUPPORT;
  }
  
  pInformation->Ctrl_Info.CopyData = CopyRoutine;
  pInformation->Ctrl_Info.Usb_wOffset = 0;
  (*CopyRoutine)(0);
  return USB_SUCCESS;
}

/*******************************************************************************
* Function Name  : CustomHID_SetReport_Feature
* Description    : Set Feature request handling
* Input          : Length.
* Output         : None.
* Return         : Buffer
*******************************************************************************/
uint8_t *CustomHID_SetReport_Feature(uint16_t Length)
{
  if (pInformation->USBwValue1 == 3)          /* Feature report = command channel */
  {
    if (Length == 0)
    {
      uint16_t n = pInformation->USBwLength;
      uint8_t i;

      /* Clear the receive buffer before arming: a short or non-NUL-padded
      report must not leave stale bytes for the parser. */
      for (i = 0; i < CMD_FEATURE_LEN; i++)
      {
        cmd_buf[i] = 0;
      }

      if (n > CMD_FEATURE_LEN)
      {
        n = CMD_FEATURE_LEN;
      }
      if (n == 0)
      {
        n = CMD_FEATURE_LEN;                  /* never arm a zero-length OUT */
      }
      pInformation->Ctrl_Info.Usb_wLength = n; /* receive length = SETUP wLength */
      pInformation->Ctrl_Info.PacketSize  = Device_Property.MaxPacketSize;
      feature_cmd_pending = 1;
      return NULL;
    }
    return cmd_buf;
  }

  /* else: legacy 1-byte LED Output report */
  if (Length == 0)
  {
    pInformation->Ctrl_Info.Usb_wLength = 1;   /* LED output report: 1 byte */
    pInformation->Ctrl_Info.PacketSize  = Device_Property.MaxPacketSize;
    return NULL;
  }
  else
  {
    return OutReport_Buf;
  }
}

/*******************************************************************************
* Function Name  : CustomHID_NoData_Setup
* Description    : handle the no data class specific requests
* Input          : Request Nb.
* Output         : None.
* Return         : USB_UNSUPPORT or USB_SUCCESS.
*******************************************************************************/
RESULT CustomHID_NoData_Setup(uint8_t RequestNo)
{
  if (Type_Recipient == (CLASS_REQUEST | INTERFACE_RECIPIENT))
  {
    if (RequestNo == SET_PROTOCOL)
    {
      return CustomHID_SetProtocol();
    }
    if (RequestNo == SET_IDLE)
    {
      /* Reports are sent on change only; accept and ignore the idle rate. */
      return USB_SUCCESS;
    }
  }

  return USB_UNSUPPORT;
}

/*******************************************************************************
* Function Name  : CustomHID_GetDeviceDescriptor.
* Description    : Gets the device descriptor.
* Input          : Length
* Output         : None.
* Return         : The address of the device descriptor.
*******************************************************************************/
uint8_t *CustomHID_GetDeviceDescriptor(uint16_t Length)
{
  return Standard_GetDescriptorData(Length, &Device_Descriptor);
}

/*******************************************************************************
* Function Name  : CustomHID_GetConfigDescriptor.
* Description    : Gets the configuration descriptor.
* Input          : Length
* Output         : None.
* Return         : The address of the configuration descriptor.
*******************************************************************************/
uint8_t *CustomHID_GetConfigDescriptor(uint16_t Length)
{
  return Standard_GetDescriptorData(Length, &Config_Descriptor);
}

/*******************************************************************************
* Function Name  : CustomHID_GetStringDescriptor
* Description    : Gets the string descriptors according to the needed index
* Input          : Length
* Output         : None.
* Return         : The address of the string descriptors.
*******************************************************************************/
uint8_t *CustomHID_GetStringDescriptor(uint16_t Length)
{
  uint8_t wValue0 = pInformation->USBwValue0;
  if (wValue0 > 3)
  {
    return NULL;
  }
  else 
  {
    return Standard_GetDescriptorData(Length, &String_Descriptor[wValue0]);
  }
}

/*******************************************************************************
* Function Name  : CustomHID_GetReportDescriptor.
* Description    : Gets the HID report descriptor.
* Input          : Length
* Output         : None.
* Return         : The address of the configuration descriptor.
*******************************************************************************/
uint8_t *CustomHID_GetReportDescriptor(uint16_t Length)
{
  return Standard_GetDescriptorData(Length, &CustomHID_Report_Descriptor);
}

/*******************************************************************************
* Function Name  : CustomHID_GetHIDDescriptor.
* Description    : Gets the HID descriptor.
* Input          : Length
* Output         : None.
* Return         : The address of the configuration descriptor.
*******************************************************************************/
uint8_t *CustomHID_GetHIDDescriptor(uint16_t Length)
{
  return Standard_GetDescriptorData(Length, &CustomHID_Hid_Descriptor);
}

/*******************************************************************************
* Function Name  : CustomHID_GetReport.
* Description    : Returns the current 8-byte keyboard input report.
* Input          : Length
* Output         : None.
* Return         : The address of the report buffer.
*******************************************************************************/
uint8_t *CustomHID_GetReport(uint16_t Length)
{
  if (pInformation->USBwValue1 == 3)          /* Feature: config response */
  {
    if (Length == 0)
    {
      uint8_t i;

      /* Latch the full published report (resp_buf is NUL-padded by
      usb_feature_publish()) so resp_buf cannot change between the two
      CopyData() calls of one transfer.  Always answer the declared
      CMD_FEATURE_LEN-byte report: hosts may expect the full size, and a
      48-byte data stage is still a single short packet (EP0 MPS = 64). */
      __disable_irq();
      for (i = 0; i < CMD_FEATURE_LEN; i++)
      {
        resp_snap[i] = resp_buf[i];
      }
      __enable_irq();

      pInformation->Ctrl_Info.Usb_wLength = CMD_FEATURE_LEN;
      return NULL;
    }
    return resp_snap;
  }

  /* else: 8-byte keyboard Input report */
  if (Length == 0)
  {
    pInformation->Ctrl_Info.Usb_wLength = 8;
    return NULL;
  }
  else
  {
    return Report_Buf;
  }
}

/*******************************************************************************
* Function Name  : CustomHID_Get_Interface_Setting.
* Description    : tests the interface and the alternate setting according to the
*                  supported one.
* Input          : - Interface : interface number.
*                  - AlternateSetting : Alternate Setting number.
* Output         : None.
* Return         : USB_SUCCESS or USB_UNSUPPORT.
*******************************************************************************/
RESULT CustomHID_Get_Interface_Setting(uint8_t Interface, uint8_t AlternateSetting)
{
  if (AlternateSetting > 0)
  {
    return USB_UNSUPPORT;
  }
  else if (Interface > 0)
  {
    return USB_UNSUPPORT;
  }
  return USB_SUCCESS;
}

/*******************************************************************************
* Function Name  : CustomHID_SetProtocol
* Description    : Joystick Set Protocol request routine.
* Input          : None.
* Output         : None.
* Return         : USB SUCCESS.
*******************************************************************************/
RESULT CustomHID_SetProtocol(void)
{
  uint8_t wValue0 = pInformation->USBwValue0;
  ProtocolValue = wValue0;
  return USB_SUCCESS;
}

/*******************************************************************************
* Function Name  : CustomHID_GetProtocolValue
* Description    : get the protocol value
* Input          : Length.
* Output         : None.
* Return         : address of the protocol value.
*******************************************************************************/
uint8_t *CustomHID_GetProtocolValue(uint16_t Length)
{
  if (Length == 0)
  {
    pInformation->Ctrl_Info.Usb_wLength = 1;
    return NULL;
  }
  else
  {
    return (uint8_t *)(&ProtocolValue);
  }
}

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
