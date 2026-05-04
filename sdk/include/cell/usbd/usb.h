/* cell/usbd/usb.h - USB 2.0 standard descriptor types and constants.
 *
 * Clean-room header authored from the USB 2.0 specification (Chapter 9,
 * USB Device Framework).  These are public USB-IF constants, not
 * proprietary.  The cell SDK ABI uses these struct layouts for its
 * libusbd entry points.
 */
#ifndef __PS3DK_CELL_USBD_USB_H__
#define __PS3DK_CELL_USBD_USB_H__

#include <stdint.h>

/* ---- USB 2.0 Section 9.3: Setup Data --------------------------- */

typedef struct UsbDeviceRequest {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} UsbDeviceRequest;

#define USB_REQTYPE_DIR_BITS             0x80
#define USB_REQTYPE_DIR_TO_DEVICE        0x00
#define USB_REQTYPE_DIR_TO_HOST          0x80
#define USB_REQTYPE_TYPE_BITS            0x60
#define USB_REQTYPE_TYPE_STANDARD        0x00
#define USB_REQTYPE_TYPE_CLASS           0x20
#define USB_REQTYPE_TYPE_VENDOR          0x40
#define USB_REQTYPE_TYPE_RESERVED        0x60
#define USB_REQTYPE_RECIP_BITS           0x1f
#define USB_REQTYPE_RECIP_DEVICE         0x00
#define USB_REQTYPE_RECIP_INTERFACE      0x01
#define USB_REQTYPE_RECIP_ENDPOINT       0x02
#define USB_REQTYPE_RECIP_OTHER          0x03

/* ---- USB 2.0 Section 9.4: Standard Device Requests ------------- */

#define USB_REQUEST_GET_STATUS           0x00
#define USB_REQUEST_CLEAR_FEATURE        0x01
#define USB_REQUEST_SET_FEATURE          0x03
#define USB_REQUEST_SET_ADDRESS          0x05
#define USB_REQUEST_GET_DESCRIPTOR       0x06
#define USB_REQUEST_SET_DESCRIPTOR       0x07
#define USB_REQUEST_GET_CONFIGURATION    0x08
#define USB_REQUEST_SET_CONFIGURATION    0x09
#define USB_REQUEST_GET_INTERFACE        0x0a
#define USB_REQUEST_SET_INTERFACE        0x0b
#define USB_REQUEST_SYNCH_FRAME          0x0c

/* ---- USB 2.0: Descriptor Types --------------------------------- */

#define USB_DESCRIPTOR_TYPE_DEVICE        0x01
#define USB_DESCRIPTOR_TYPE_CONFIGURATION 0x02
#define USB_DESCRIPTOR_TYPE_STRING        0x03
#define USB_DESCRIPTOR_TYPE_INTERFACE     0x04
#define USB_DESCRIPTOR_TYPE_ENDPOINT      0x05
#define USB_DESCRIPTOR_TYPE_HID           0x21
#define USB_DESCRIPTOR_TYPE_REPORT        0x22

/* ---- USB 2.0 Section 9.4: Feature Selectors -------------------- */

#define USB_FEATURE_ENDPOINT_HALT         0x00
#define USB_FEATURE_DEVICE_REMOTE_WAKEUP  0x01

/* ---- USB-IF: Device Class Codes --------------------------------- */

#define USB_CLASS_PER_INTERFACE           0x00
#define USB_CLASS_AUDIO                   0x01
#define USB_CLASS_COMMUNICATIONS          0x02
#define USB_CLASS_HID                     0x03
#define USB_CLASS_MONITOR                 0x04
#define USB_CLASS_PHYSICAL                0x05
#define USB_CLASS_POWER                   0x06
#define USB_CLASS_PRINTER                 0x07
#define USB_CLASS_STORAGE                 0x08
#define USB_CLASS_HUB                     0x09
#define USB_CLASS_DATA                    0x0a
#define USB_CLASS_VENDOR_SPECIFIC         0xff

/* ---- USB 2.0 Section 9.6.1: Device Descriptor ------------------ */

typedef struct UsbDeviceDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubclass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} UsbDeviceDescriptor;

/* ---- USB 2.0 Section 9.6.3: Configuration Descriptor ----------- */

typedef struct UsbConfigurationDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  MaxPower;
} UsbConfigurationDescriptor;

#define USB_CONFIGURATION_RESERVED_ZERO    0x1f
#define USB_CONFIGURATION_REMOTE_WAKEUP    0x20
#define USB_CONFIGURATION_SELF_POWERED     0x40
#define USB_CONFIGURATION_RESERVED_ONE     0x80

/* ---- USB 2.0 Section 9.6.5: Interface Descriptor --------------- */

typedef struct UsbInterfaceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubclass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} UsbInterfaceDescriptor;

/* ---- USB 2.0 Section 9.6.7: Endpoint Descriptor ---------------- */

typedef struct UsbEndpointDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} UsbEndpointDescriptor;

/* bmAttributes transfer-type field */
#define USB_ENDPOINT_TRANSFER_TYPE_BITS         0x03
#define USB_ENDPOINT_TRANSFER_TYPE_SHIFT        0
#define USB_ENDPOINT_TRANSFER_TYPE_CONTROL      0x00
#define USB_ENDPOINT_TRANSFER_TYPE_ISOCHRONOUS  0x01
#define USB_ENDPOINT_TRANSFER_TYPE_BULK         0x02
#define USB_ENDPOINT_TRANSFER_TYPE_INTERRUPT    0x03

/* bEndpointAddress fields */
#define USB_ENDPOINT_NUMBER_BITS         0x1f
#define USB_ENDPOINT_NUMBER_SHIFT        0
#define USB_ENDPOINT_DIRECTION_BITS      0x80
#define USB_ENDPOINT_DIRECTION_SHIFT     7
#define USB_ENDPOINT_DIRECTION_OUT       0x00
#define USB_ENDPOINT_DIRECTION_IN        0x80

/* ---- USB 2.0 Section 9.6.9: String Descriptor ------------------ */

typedef struct UsbStringDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bString[0];
} UsbStringDescriptor;

/* ---- HID Class-Specific Descriptor ------------------------------ */

typedef struct UsbHidSubDescriptorInfo {
    uint8_t bDescriptorType;
    uint8_t wDescriptorLength0;
    uint8_t wDescriptorLength1;
} UsbHidSubDescriptorInfo;

typedef struct UsbHidDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bcdHID0;
    uint8_t  bcdHID1;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    UsbHidSubDescriptorInfo SubDescriptorInfo[0];
} UsbHidDescriptor;

/* ---- USB 2.0 max packet sizes ---------------------------------- */

#define USB_MAX_LS_CONTROL_PACKET_SIZE     8
#define USB_MAX_FS_CONTROL_PACKET_SIZE     64
#define USB_MAX_ISOCHRONOUS_PACKET_SIZE    1023
#define USB_MAX_LS_INTERRUPT_PACKET_SIZE   8
#define USB_MAX_FS_INTERRUPT_PACKET_SIZE   64
#define USB_MAX_BULK_PACKET_SIZE           64

/* ---- OHCI / EHCI completion codes ------------------------------ */

#define HC_CC_NOERR     0x0
#define OHCI_CC_CRC     0x1
#define OHCI_CC_BFV     0x2
#define OHCI_CC_DTM     0x3
#define OHCI_CC_STALL   0x4
#define OHCI_CC_NOTRESP 0x5
#define OHCI_CC_PIDCF   0x6
#define OHCI_CC_UEPID   0x7
#define OHCI_CC_DOR     0x8
#define OHCI_CC_DUR     0x9
#define OHCI_CC_RSVDA   0xa
#define OHCI_CC_RSVDB   0xb
#define OHCI_CC_BOR     0xc
#define OHCI_CC_BUR     0xd
#define OHCI_CC_NOTACC1 0xe
#define OHCI_CC_NOTACC2 0xf
#define EHCI_CC_MISSMF  0x10
#define EHCI_CC_XACT    0x20
#define EHCI_CC_BABBLE  0x30
#define EHCI_CC_DATABUF 0x40
#define EHCI_CC_HALTED  0x50

/* Isochronous PSW completion codes */
#define USBD_CC_NOERR    0x0
#define USBD_CC_MISSMF   0x1
#define USBD_CC_XACT     0x2
#define USBD_CC_BABBLE   0x4
#define USBD_CC_DATABUF  0x8

#endif /* __PS3DK_CELL_USBD_USB_H__ */
