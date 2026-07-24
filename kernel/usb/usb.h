/*
 * usb.h - USB protocol definitions and device tracking structures
 *
 * Implements USB 2.0/3.x standard device descriptor parsing, configuration
 * management, and device tracking.  Works as the protocol layer above
 * the xHCI host controller driver.
 *
 * /* USB (v4.2.6) */
 */
#ifndef USB_H
#define USB_H

#include <stdint.h>

/* ================================================================
 * USB Class Codes
 * ================================================================ */
#define USB_CLASS_HID               3
#define USB_CLASS_MASS_STORAGE      8
#define USB_CLASS_HUB               9
#define USB_CLASS_VENDOR_SPECIFIC   0xFF

/* ================================================================
 * USB Subclass Codes (HID)
 * ================================================================ */
#define USB_SUBCLASS_HID_BOOT       1

/* ================================================================
 * USB Protocol Codes (HID)
 * ================================================================ */
#define USB_PROTOCOL_HID_KEYBOARD   1
#define USB_PROTOCOL_HID_MOUSE      2

/* ================================================================
 * USB Standard Request Types
 * ================================================================ */
#define USB_REQ_DIR_HOST_TO_DEVICE  0x00
#define USB_REQ_DIR_DEVICE_TO_HOST  0x80
#define USB_REQ_TYPE_STANDARD       0x00
#define USB_REQ_TYPE_CLASS          0x20
#define USB_REQ_TYPE_VENDOR         0x40
#define USB_REQ_RECIP_DEVICE        0x00
#define USB_REQ_RECIP_INTERFACE     0x01
#define USB_REQ_RECIP_ENDPOINT      0x02
#define USB_REQ_RECIP_OTHER         0x03

/* ================================================================
 * USB Standard Requests (bRequest)
 * ================================================================ */
#define USB_REQ_GET_STATUS          0
#define USB_REQ_CLEAR_FEATURE       1
#define USB_REQ_SET_FEATURE         3
#define USB_REQ_SET_ADDRESS         5
#define USB_REQ_GET_DESCRIPTOR      6
#define USB_REQ_SET_DESCRIPTOR      7
#define USB_REQ_GET_CONFIGURATION   8
#define USB_REQ_SET_CONFIGURATION   9
#define USB_REQ_GET_INTERFACE       10
#define USB_REQ_SET_INTERFACE       11
#define USB_REQ_SYNCH_FRAME         12

/* HID Class Requests */
#define USB_HID_REQ_GET_REPORT      0x01
#define USB_HID_REQ_GET_IDLE        0x02
#define USB_HID_REQ_GET_PROTOCOL    0x03
#define USB_HID_REQ_SET_REPORT      0x09
#define USB_HID_REQ_SET_IDLE        0x0A
#define USB_HID_REQ_SET_PROTOCOL    0x0B

/* ================================================================
 * USB Descriptor Types
 * ================================================================ */
#define USB_DESC_DEVICE             1
#define USB_DESC_CONFIGURATION      2
#define USB_DESC_STRING             3
#define USB_DESC_INTERFACE          4
#define USB_DESC_ENDPOINT           5
#define USB_DESC_DEVICE_QUALIFIER   6
#define USB_DESC_OTHER_SPEED        7
#define USB_DESC_HID                0x21
#define USB_DESC_HID_REPORT         0x22
#define USB_DESC_HID_PHYSICAL       0x23

/* ================================================================
 * USB Endpoint Direction
 * ================================================================ */
#define USB_EP_DIR_OUT              0x00
#define USB_EP_DIR_IN               0x80

/* ================================================================
 * USB Endpoint Transfer Types
 * ================================================================ */
#define USB_EP_TYPE_CONTROL         0x00
#define USB_EP_TYPE_ISOCHRONOUS     0x01
#define USB_EP_TYPE_BULK            0x02
#define USB_EP_TYPE_INTERRUPT       0x03

/* ================================================================
 * USB Setup Packet (8 bytes)
 * ================================================================ */
struct usb_setup_packet {
    uint8_t  bmRequestType;   /* Bitmap: D7=Dir, D6:5=Type, D4:0=Recipient */
    uint8_t  bRequest;        /* Specific request */
    uint16_t wValue;          /* Value (little-endian) */
    uint16_t wIndex;          /* Index (little-endian) */
    uint16_t wLength;         /* Length of data stage (little-endian) */
} __attribute__((packed));

/* ================================================================
 * USB Standard Device Descriptor (18 bytes)
 * ================================================================ */
struct usb_device_descriptor {
    uint8_t  bLength;            /* 18 */
    uint8_t  bDescriptorType;    /* USB_DESC_DEVICE = 1 */
    uint16_t bcdUSB;             /* USB specification release number (BCD) */
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;    /* Max packet size for endpoint 0 */
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;          /* Device release number (BCD) */
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

/* ================================================================
 * USB Configuration Descriptor (9 bytes)
 * ================================================================ */
struct usb_config_descriptor {
    uint8_t  bLength;            /* 9 */
    uint8_t  bDescriptorType;    /* USB_DESC_CONFIGURATION = 2 */
    uint16_t wTotalLength;       /* Total length of this + subordinate descriptors */
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;          /* Maximum power consumption (2 mA units) */
} __attribute__((packed));

/* ================================================================
 * USB Interface Descriptor (9 bytes)
 * ================================================================ */
struct usb_interface_descriptor {
    uint8_t  bLength;            /* 9 */
    uint8_t  bDescriptorType;    /* USB_DESC_INTERFACE = 4 */
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

/* ================================================================
 * USB Endpoint Descriptor (7 bytes)
 * ================================================================ */
struct usb_endpoint_descriptor {
    uint8_t  bLength;            /* 7 */
    uint8_t  bDescriptorType;    /* USB_DESC_ENDPOINT = 5 */
    uint8_t  bEndpointAddress;   /* D7=Dir, D6:4=0, D3:0=EP number */
    uint8_t  bmAttributes;       /* D1:0=Transfer Type, D3:2=Sync Type, D5:4=Usage Type, D7:6=Reserved */
    uint16_t wMaxPacketSize;     /* Max packet size (bits 12:13 = additional transactions) */
    uint8_t  bInterval;          /* Polling interval */
} __attribute__((packed));

/* ================================================================
 * USB HID Descriptor (9+ bytes)
 * ================================================================ */
struct usb_hid_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;    /* USB_DESC_HID = 0x21 */
    uint16_t bcdHID;             /* HID specification release number */
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;    /* Number of subordinate descriptors */
    struct {
        uint8_t  bDescriptorType;  /* USB_DESC_HID_REPORT = 0x22 */
        uint16_t wDescriptorLength;
    } __attribute__((packed)) desc[1]; /* Variable-length array */
} __attribute__((packed));

/* ================================================================
 * USB Device Tracking Structure
 * ================================================================ */
#define USB_MAX_ENDPOINTS         16
#define USB_MAX_INTERFACES        8

struct usb_endpoint {
    uint8_t  ep_address;         /* Endpoint address (D7=Dir) */
    uint8_t  ep_type;            /* Transfer type */
    uint8_t  ep_id;              /* DCI / endpoint ID */
    uint16_t max_packet_size;
    uint8_t  interval;
    uint8_t  active;
};

struct usb_interface {
    uint8_t  interface_number;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint8_t  num_endpoints;
    struct usb_endpoint endpoints[USB_MAX_ENDPOINTS];
};

struct usb_device {
    uint8_t  slot_id;
    uint8_t  address;
    uint8_t  root_port;
    uint8_t  speed;
    uint8_t  configured;

    struct usb_device_descriptor    dev_desc;
    struct usb_config_descriptor    cfg_desc;
    uint8_t  num_interfaces;
    struct usb_interface            interfaces[USB_MAX_INTERFACES];

    /* Parent controller */
    struct xhci_controller *hc;

    /* HID-specific data */
    struct usb_hid_descriptor *hid_desc;
    uint8_t  *report_descriptor;
    uint16_t  report_desc_len;

    /* Linked list */
    struct usb_device *next;
};

/* ================================================================
 * USB Device API
 * ================================================================ */
struct usb_device *usb_device_list(void);
void usb_device_add(struct usb_device *dev);
struct usb_device *usb_find_by_class(uint8_t class_code);

#endif /* USB_H */