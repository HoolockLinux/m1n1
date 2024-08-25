/* SPDX-License-Identifier: MIT */

/*
 * Useful references:
 * - Intel® Quark™ SE Microcontroller C1000 Document Number: 334712-005EN (p. 164)
 *   https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/quark-c1000-datasheet.pdf
 * - Hi-Speed Universal Serial Bus (USB) 2.0 On-The-Go (OTG) Controller Data Sheet Rev. A
 *   https://web.archive.org/web/20160329022330if_/http://www.quicklogic.com/assets/pdf/data-sheets/QL-Hi-Speed-USB-2.0-OTG-Controller-Data-Sheet.pdf
 * - USB in a NutShell
 *   https://www.beyondlogic.org/usbnutshell/usb1.shtml
 * and thanks to https://github.com/ataradov/usb-sniffer
 */

#include "../build/build_tag.h"

#include <assert.h>

#include "adt.h"
#include "dart.h"
#include "malloc.h"
#include "memory.h"
#include "ringbuffer.h"
#include "string.h"
#include "types.h"
#include "uart.h"
#include "usb_dwc2.h"
#include "usb_dwc2_regs.h"
#include "usb_types.h"
#include "utils.h"

#define MAX_ENDPOINTS   8
#define CDC_BUFFER_SIZE SZ_1M

#define usb_debug_printf(fmt, ...) // uart_printf("usb-dwc2: " fmt, ##__VA_ARGS__)
// many printf commented due to strict timing requirements
// macos is less strict than linux

#define usb_error_printf(fmt, ...) uart_printf("usb-dwc2[ERR]: " fmt, ##__VA_ARGS__)

#define STRING_DESCRIPTOR_LANGUAGES    0
#define STRING_DESCRIPTOR_MANUFACTURER 1
#define STRING_DESCRIPTOR_PRODUCT      2
#define STRING_DESCRIPTOR_SERIAL       3

#define CDC_DEVICE_CLASS 0x02

#define CDC_USB_VID 0x1209
#define CDC_USB_PID 0x316d

#define CDC_INTERFACE_CLASS         0x02
#define CDC_INTERFACE_CLASS_DATA    0x0a
#define CDC_INTERFACE_SUBCLASS_ACM  0x02
#define CDC_INTERFACE_PROTOCOL_NONE 0x00
#define CDC_INTERFACE_PROTOCOL_AT   0x01

#define TRB_BUFFER_SIZE  SZ_16K
#define XFER_BUFFER_SIZE (SZ_16K * MAX_ENDPOINTS * 2)
#define PAD_BUFFER_SIZE  SZ_16K

#define XFER_BUFFER_BYTES_PER_EP (XFER_BUFFER_SIZE / MAX_ENDPOINTS)

#define XFER_SIZE SZ_16K

#define SCRATCHPAD_IOVA   0xbeef0000
#define EVENT_BUFFER_IOVA 0xdead0000
#define XFER_BUFFER_IOVA  0xbabe0000
#define TRB_BUFFER_IOVA   0xf00d0000
#define DMA_BUFFER_SIZE   (0x4000 / 4) // diviced between 4 endpoints

/* these map to the control endpoint 0x00/0x80 */
#define USB_LEP_CTRL_OUT 0
#define USB_LEP_CTRL_IN  1

/* maps to interrupt endpoint 0x81 */
#define USB_LEP_CDC_INTR_IN 2

/* these map to physical endpoints 0x02 and 0x83 */
#define USB_LEP_CDC_BULK_OUT 3
#define USB_LEP_CDC_BULK_IN  4

/* maps to interrupt endpoint 0x84 */
#define USB_LEP_CDC_INTR_IN_2 5

/* these map to physical endpoints 0x05 and 0x86 */
#define USB_LEP_CDC_BULK_OUT_2 6
#define USB_LEP_CDC_BULK_IN_2  7

#define BULK_EP_MAX_PACKET_SIZE 0x200
#define RX_FIFO_SIZE            ((4 * 1 + 6) + 2 * ((BULK_EP_MAX_PACKET_SIZE / 4) + 8) + 1)
#define TX_FIFO_SIZE            (2 * (BULK_EP_MAX_PACKET_SIZE / 4))
#define RECV_DATA               1

static const u8 phyEndpoints[] = {0x0, 0x80, 0x81, 0x2, 0x83, 0x84, 0x05, 0x86};

/* content doesn't matter at all, this is the setting linux writes by default */
static const u8 cdc_default_line_coding[] = {0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08};

enum ep0_state {
    USB_DWC2_EP0_STATE_IDLE,
    USB_DWC2_EP0_STATE_SETUP_PENDING,
    USB_DWC2_EP0_STATE_SETUP_HANDLE,
    USB_DWC2_EP0_STATE_DATA_SEND,
    USB_DWC2_EP0_STATE_DATA_RECV,
    USB_DWC2_EP0_STATE_DATA_SEND_DONE,
    USB_DWC2_EP0_STATE_DATA_RECV_DONE,
    USB_DWC2_EP0_STATE_DATA_RECV_STATUS,
    USB_DWC2_EP0_STATE_DATA_RECV_STATUS_DONE,
    USB_DWC2_EP0_STATE_DATA_SEND_STATUS,
    USB_DWC2_EP0_STATE_DATA_SEND_STATUS_DONE
};

typedef struct dwc2_endpoint {
    bool xfer_in_progress;
    bool zlp_pending;
    u8 *transfer_data;
    u32 transfer_size;
    u32 transferred;
    bool in_flight;
    void *xfer_buffer;
    bool transfer_max;
    u16 max_packet_size;
} dwc2_endpoint_t;

typedef struct dwc2_dev {
    /* USB DRD */
    u64 regs;

    enum ep0_state ep0_state;
    const void *ep0_buffer;
    u32 ep0_buffer_len;
    void *ep0_read_buffer;
    void *dma_page_p;
    u32 ep0_read_buffer_len;

    const union usb_setup_packet *setup_pkt;

    dwc2_endpoint_t endpoints[MAX_ENDPOINTS];

    struct {
        ringbuffer_t *host2device;
        ringbuffer_t *device2host;
        u8 ep_intr;
        u8 ep_in;
        u8 ep_out;
        bool ready;
        /* USB ACM CDC serial */
        u8 cdc_line_coding[7];
    } pipe[CDC_ACM_PIPE_MAX];

} dwc2_dev_t;

u32 next_tx_fifo_addr = 0;
static const struct usb_string_descriptor str_manufacturer =
    make_usb_string_descriptor("Asahi Linux");
static const struct usb_string_descriptor str_product =
    make_usb_string_descriptor("m1n1 uartproxy " BUILD_TAG);
static const struct usb_string_descriptor str_serial_dummy = make_usb_string_descriptor("P-0");
static const struct usb_string_descriptor *str_serial;

static const struct usb_string_descriptor_languages str_langs = {
    .bLength = sizeof(str_langs) + 2,
    .bDescriptorType = USB_STRING_DESCRIPTOR,
    .wLANGID = {USB_LANGID_EN_US},
};

struct cdc_dev_desc {
    const struct usb_configuration_descriptor configuration;
    const struct usb_interface_descriptor interface_management;
    const struct cdc_union_functional_descriptor cdc_union_func;
    const struct usb_endpoint_descriptor endpoint_notification;
    const struct usb_interface_descriptor interface_data;
    const struct usb_endpoint_descriptor endpoint_data_in;
    const struct usb_endpoint_descriptor endpoint_data_out;
    const struct usb_interface_descriptor sec_interface_management;
    const struct cdc_union_functional_descriptor sec_cdc_union_func;
    const struct usb_endpoint_descriptor sec_endpoint_notification;
    const struct usb_interface_descriptor sec_interface_data;
    const struct usb_endpoint_descriptor sec_endpoint_data_in;
    const struct usb_endpoint_descriptor sec_endpoint_data_out;
} PACKED;

static const struct usb_device_descriptor usb_cdc_device_descriptor = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DEVICE_DESCRIPTOR,
    .bcdUSB = 0x0200,
    .bDeviceClass = CDC_DEVICE_CLASS,
    .bDeviceSubClass = 0, // unused
    .bDeviceProtocol = 0, // unused
    .bMaxPacketSize0 = 64,
    .idVendor = CDC_USB_VID,
    .idProduct = CDC_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRING_DESCRIPTOR_MANUFACTURER,
    .iProduct = STRING_DESCRIPTOR_PRODUCT,
    .iSerialNumber = STRING_DESCRIPTOR_SERIAL,
    .bNumConfigurations = 1,
};

static const struct cdc_dev_desc cdc_configuration_descriptor = {
    .configuration =
        {
            .bLength = sizeof(cdc_configuration_descriptor.configuration),
            .bDescriptorType = USB_CONFIGURATION_DESCRIPTOR,
            .wTotalLength = sizeof(cdc_configuration_descriptor),
            .bNumInterfaces = 4,
            .bConfigurationValue = 1,
            .iConfiguration = 0,
            .bmAttributes = USB_CONFIGURATION_ATTRIBUTE_RES1 | USB_CONFIGURATION_SELF_POWERED,
            .bMaxPower = 250,

        },
    .interface_management =
        {
            .bLength = sizeof(cdc_configuration_descriptor.interface_management),
            .bDescriptorType = USB_INTERFACE_DESCRIPTOR,
            .bInterfaceNumber = 0,
            .bAlternateSetting = 0,
            .bNumEndpoints = 1,
            .bInterfaceClass = CDC_INTERFACE_CLASS,
            .bInterfaceSubClass = CDC_INTERFACE_SUBCLASS_ACM,
            .bInterfaceProtocol = CDC_INTERFACE_PROTOCOL_NONE,
            .iInterface = 0,

        },
    .cdc_union_func =
        {
            .bFunctionLength = sizeof(cdc_configuration_descriptor.cdc_union_func),
            .bDescriptorType = USB_CDC_INTERFACE_FUNCTIONAL_DESCRIPTOR,
            .bDescriptorSubtype = USB_CDC_UNION_SUBTYPE,
            .bControlInterface = 0,
            .bDataInterface = 1,
        },
    /*
     * we never use this endpoint, but it should exist and always be idle.
     * it needs to exist in the descriptor though to make hosts correctly recognize
     * us as a ACM CDC device.
     */
    .endpoint_notification =
        {
            .bLength = sizeof(cdc_configuration_descriptor.endpoint_notification),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_IN(phyEndpoints[USB_LEP_CDC_INTR_IN] & 0xf),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_INTERRUPT,
            .wMaxPacketSize = 64,
            .bInterval = 10,

        },
    .interface_data =
        {
            .bLength = sizeof(cdc_configuration_descriptor.interface_data),
            .bDescriptorType = USB_INTERFACE_DESCRIPTOR,
            .bInterfaceNumber = 1,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = CDC_INTERFACE_CLASS_DATA,
            .bInterfaceSubClass = 0, // unused
            .bInterfaceProtocol = 0, // unused
            .iInterface = 0,
        },
    .endpoint_data_in =
        {
            .bLength = sizeof(cdc_configuration_descriptor.endpoint_data_in),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_OUT(phyEndpoints[USB_LEP_CDC_BULK_OUT]),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 10,
        },
    .endpoint_data_out =
        {
            .bLength = sizeof(cdc_configuration_descriptor.endpoint_data_out),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_IN(phyEndpoints[USB_LEP_CDC_BULK_IN] & 0xf),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 10,
        },

    /*
     * CDC ACM interface for virtual uart
     */

    .sec_interface_management =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_interface_management),
            .bDescriptorType = USB_INTERFACE_DESCRIPTOR,
            .bInterfaceNumber = 2,
            .bAlternateSetting = 0,
            .bNumEndpoints = 1,
            .bInterfaceClass = CDC_INTERFACE_CLASS,
            .bInterfaceSubClass = CDC_INTERFACE_SUBCLASS_ACM,
            .bInterfaceProtocol = CDC_INTERFACE_PROTOCOL_NONE,
            .iInterface = 0,

        },
    .sec_cdc_union_func =
        {
            .bFunctionLength = sizeof(cdc_configuration_descriptor.sec_cdc_union_func),
            .bDescriptorType = USB_CDC_INTERFACE_FUNCTIONAL_DESCRIPTOR,
            .bDescriptorSubtype = USB_CDC_UNION_SUBTYPE,
            .bControlInterface = 2,
            .bDataInterface = 3,
        },
    /*
     * we never use this endpoint, but it should exist and always be idle.
     * it needs to exist in the descriptor though to make hosts correctly recognize
     * us as a ACM CDC device.
     */
    .sec_endpoint_notification =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_endpoint_notification),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_IN(phyEndpoints[USB_LEP_CDC_INTR_IN_2] & 0xf),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_INTERRUPT,
            .wMaxPacketSize = 64,
            .bInterval = 10,

        },
    .sec_interface_data =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_interface_data),
            .bDescriptorType = USB_INTERFACE_DESCRIPTOR,
            .bInterfaceNumber = 3,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = CDC_INTERFACE_CLASS_DATA,
            .bInterfaceSubClass = 0, // unused
            .bInterfaceProtocol = 0, // unused
            .iInterface = 0,
        },
    .sec_endpoint_data_in =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_endpoint_data_in),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_OUT(phyEndpoints[USB_LEP_CDC_BULK_OUT_2]),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 10,
        },
    .sec_endpoint_data_out =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_endpoint_data_out),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_IN(phyEndpoints[USB_LEP_CDC_BULK_IN_2] & 0xf),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 10,
        },
};

static const struct usb_device_qualifier_descriptor usb_cdc_device_qualifier_descriptor = {
    .bLength = sizeof(struct usb_device_qualifier_descriptor),
    .bDescriptorType = USB_DEVICE_QUALIFIER_DESCRIPTOR,
    .bcdUSB = 0x0200,
    .bDeviceClass = CDC_DEVICE_CLASS,
    .bDeviceSubClass = 0, // unused
    .bDeviceProtocol = 0, // unused
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 0,
};

static const char *ep0_state_names[] = {
    "STATE_IDLE",
    "STATE_SETUP_PENDING",
    "STATE_SETUP_HANDLE",
    "STATE_DATA_SEND",
    "STATE_DATA_RECV",
    "STATE_DATA_SEND_DONE",
    "STATE_DATA_RECV_DONE",
    "STATE_DATA_RECV_STATUS",
    "STATE_DATA_RECV_STATUS_DONE",
    "STATE_DATA_SEND_STATUS",
    "STATE_DATA_SEND_STATUS_DONE",
};

static void usb_set_address(dwc2_dev_t *dev, u8 address);
static void usb_dwc2_ep_hw_recv(dwc2_dev_t *dev, u8 ep, u32 hw_xfer_size, u32 packet_count);
static void usb_dwc2_ep_hw_send(dwc2_dev_t *dev, u8 ep, u32 hw_xfer_size, u32 packet_count);
static void usb_dwc2_ep_abort(dwc2_dev_t *dev, u8 ep);
static int usb_dwc2_start_status_phase(dwc2_dev_t *dev, u8 ep);
static void usb_dwc2_cdc_start_bulk_out_xfer(dwc2_dev_t *dev, u8 endpoint_number);
static void usb_dwc2_cdc_start_bulk_in_xfer(dwc2_dev_t *dev, u8 endpoint_number);

#ifdef LOG_REGISTER_RW
static void USB_DEBUG_PRINT_REGISTERS(dwc2_dev_t *dev)
{
#define USB_DEBUG_REG_VALUE(reg) usb_debug_printf(#reg " = 0x%x\n", read32(dev->regs + reg));

    USB_DEBUG_REG_VALUE(DWC2_DIEPCTL(2));
    USB_DEBUG_REG_VALUE(DWC2_DIEPINT(2));
    USB_DEBUG_REG_VALUE(DWC2_DIEPTSIZ(2));
    USB_DEBUG_REG_VALUE(DWC2_DIEPDMA(2));
    USB_DEBUG_REG_VALUE(DWC2_DTXFSTS(2));

    USB_DEBUG_REG_VALUE(DWC2_DOEPCTL(2));
    USB_DEBUG_REG_VALUE(DWC2_DOEPINT(2));
    USB_DEBUG_REG_VALUE(DWC2_DOEPTSIZ(2));
    USB_DEBUG_REG_VALUE(DWC2_DOEPDMA(2));

    USB_DEBUG_REG_VALUE(DWC2_GOTGCTL);
    USB_DEBUG_REG_VALUE(DWC2_GOTGINT);
    USB_DEBUG_REG_VALUE(DWC2_GAHBCFG);
    USB_DEBUG_REG_VALUE(DWC2_GUSBCFG);
    USB_DEBUG_REG_VALUE(DWC2_GRSTCTL);
    USB_DEBUG_REG_VALUE(DWC2_GINTSTS);
    USB_DEBUG_REG_VALUE(DWC2_GINTMSK);
    USB_DEBUG_REG_VALUE(DWC2_GRXSTSR);
    USB_DEBUG_REG_VALUE(DWC2_GRXSTSP);
    USB_DEBUG_REG_VALUE(DWC2_GRXFSIZ);
    USB_DEBUG_REG_VALUE(DWC2_GNPTXFSIZ);
    USB_DEBUG_REG_VALUE(DWC2_GNPTXSTS);
    USB_DEBUG_REG_VALUE(DWC2_GI2CCTL);
    USB_DEBUG_REG_VALUE(DWC2_GPVNDCTL);
    USB_DEBUG_REG_VALUE(DWC2_GGPIO);
    USB_DEBUG_REG_VALUE(DWC2_GUID);
    USB_DEBUG_REG_VALUE(DWC2_GSNPSID);
    USB_DEBUG_REG_VALUE(DWC2_GHWCFG1);
    USB_DEBUG_REG_VALUE(DWC2_GHWCFG2);
    USB_DEBUG_REG_VALUE(DWC2_GHWCFG3);
    USB_DEBUG_REG_VALUE(DWC2_GHWCFG4);
    USB_DEBUG_REG_VALUE(DWC2_GLPMCFG);
    USB_DEBUG_REG_VALUE(DWC2_GPWRDN);
    USB_DEBUG_REG_VALUE(DWC2_GDFIFOCFG);
    USB_DEBUG_REG_VALUE(DWC2_ADPCTL);

    USB_DEBUG_REG_VALUE(DWC2_HPTXFSIZ);
    USB_DEBUG_REG_VALUE(DWC2_DTXFSIZ(0));
    USB_DEBUG_REG_VALUE(DWC2_DTXFSIZ(1));
    USB_DEBUG_REG_VALUE(DWC2_DTXFSIZ(2));
    USB_DEBUG_REG_VALUE(DWC2_DTXFSIZ(3));
    USB_DEBUG_REG_VALUE(DWC2_DTXFSIZ(4));

    USB_DEBUG_REG_VALUE(DWC2_DCFG);
    USB_DEBUG_REG_VALUE(DWC2_DCTL);
    USB_DEBUG_REG_VALUE(DWC2_DSTS);
    USB_DEBUG_REG_VALUE(DWC2_DIEPMSK);
    USB_DEBUG_REG_VALUE(DWC2_DOEPMSK);
    USB_DEBUG_REG_VALUE(DWC2_DAINT);
    USB_DEBUG_REG_VALUE(DWC2_DAINTMSK);

    USB_DEBUG_REG_VALUE(DWC2_DIEPCTL(0));
    USB_DEBUG_REG_VALUE(DWC2_DIEPINT(0));
    USB_DEBUG_REG_VALUE(DWC2_DIEPTSIZ(0));
    USB_DEBUG_REG_VALUE(DWC2_DIEPDMA(0));
    USB_DEBUG_REG_VALUE(DWC2_DTXFSTS(0));

    USB_DEBUG_REG_VALUE(DWC2_DOEPCTL(0));
    USB_DEBUG_REG_VALUE(DWC2_DOEPINT(0));
    USB_DEBUG_REG_VALUE(DWC2_DOEPTSIZ(0));
    USB_DEBUG_REG_VALUE(DWC2_DOEPDMA(0));

    USB_DEBUG_REG_VALUE(DWC2_DIEPCTL(1));
    USB_DEBUG_REG_VALUE(DWC2_DIEPINT(1));
    USB_DEBUG_REG_VALUE(DWC2_DIEPTSIZ(1));
    USB_DEBUG_REG_VALUE(DWC2_DIEPDMA(1));
    USB_DEBUG_REG_VALUE(DWC2_DTXFSTS(1));

    USB_DEBUG_REG_VALUE(DWC2_DOEPCTL(1));
    USB_DEBUG_REG_VALUE(DWC2_DOEPINT(1));
    USB_DEBUG_REG_VALUE(DWC2_DOEPTSIZ(1));
    USB_DEBUG_REG_VALUE(DWC2_DOEPDMA(1));
}

u64 debug_reg_base = 0;
static inline void write32(u64 addr, u32 data)
{
    if (addr != debug_reg_base + DWC2_GINTSTS)
        usb_debug_printf("wr%x %x\n", addr - debug_reg_base, data);
    write32(addr, data);
}

static inline void set32(u64 addr, u32 data)
{
    usb_debug_printf("or%x %x\n", addr - debug_reg_base, data);
    set32(addr, data);
}

static inline void clear32(u64 addr, u32 data)
{
    usb_debug_printf("cl%x %x\n", addr - debug_reg_base, data);
    clear32(addr, data);
}
#endif

static int usb_dwc2_ep_activate(dwc2_dev_t *dev, u8 ep, u8 type, u32 max_packet_len)
{
    u8 pep = phyEndpoints[ep];
    u8 is_endpoint_in = !(!(pep & 0x80));
    u64 ep_ctl_reg = 0, daint_mask_shift;
    pep &= 0xf;
    // usb_debug_printf ("activate EP%u, is_in =%d\n", pep, is_endpoint_in);
    dev->endpoints[ep].max_packet_size = max_packet_len;
    u32 val = DWC2_DXEPCTLi_SetD0Pid | DWC2_DXEPCTL_SetNAK;

    if (type != DWC2_EP_TYPE_CONTROL && type != DWC2_EP_TYPE_BULK && type != DWC2_EP_TYPE_INTR) {
        usb_error_printf("Unsupported endpoint type [%u] for activation\n", type);
        return -1;
    }
    if (is_endpoint_in) {
        ep_ctl_reg = dev->regs + DWC2_DIEPCTL(pep);
        // usb_debug_printf("dev->regs + DWC2_DIEPCTL(pep) = %x\n", dev->regs +
        // DWC2_DIEPCTL(pep));
        write32(ep_ctl_reg, 0);
        u8 next_tx_fifo = pep;
        val |= next_tx_fifo << 22; // TX_FIFO_SHIFT
        write32(dev->regs + DWC2_DTXFSIZ(next_tx_fifo), TX_FIFO_SIZE << 16 | next_tx_fifo_addr);
        // usb_debug_printf("DWC2_DTXFSIZ(next_tx_fifo)= %x\n", DWC2_DTXFSIZ(next_tx_fifo));
        next_tx_fifo_addr += TX_FIFO_SIZE;
        if (ep == USB_LEP_CDC_INTR_IN || ep == USB_LEP_CDC_INTR_IN_2)
            val |= 1 << 26; // CNAK
        daint_mask_shift = pep;
    } else {
        ep_ctl_reg = dev->regs + DWC2_DOEPCTL(pep);
        write32(ep_ctl_reg, 0);
        daint_mask_shift = pep + 16;
    }
    if (ep_ctl_reg)
        write32(ep_ctl_reg, val | type << 18 | DWC2_DXEPCTL_ActivateEP | max_packet_len);
    set32(dev->regs + DWC2_DAINTMSK, (1 << daint_mask_shift));
    return 0;
}

static u8 ep_to_num(u8 epno)
{
    for (u8 i = 0; i < sizeof(phyEndpoints) / sizeof(u8); i++)
        if (phyEndpoints[i] == epno)
            return i;

    usb_error_printf("invalid phyEndpoints[%u] to be transfer back to ep", epno);
    return 0xff;
}

static void usb_dwc2_ep_set_stall(dwc2_dev_t *dev, u8 ep, u8 stall)
{
    u8 pep = phyEndpoints[ep];
    if (stall) {
        if (pep & 0x80) { // dir_in
            pep &= 0xf;
            set32(dev->regs + DWC2_DIEPCTL(pep), DWC2_DXEPCTL_Stall);
        } else {
            set32(dev->regs + DWC2_DOEPCTL(pep), DWC2_DXEPCTL_Stall);
        }
    } else {
        if (pep & 0x80) { // dir_in
            pep &= 0xf;
            clear32(dev->regs + DWC2_DIEPCTL(pep), DWC2_DXEPCTL_Stall);
        } else {
            clear32(dev->regs + DWC2_DOEPCTL(pep), DWC2_DXEPCTL_Stall);
        }
    }
}

static void usb_build_serial(void)
{
    if (str_serial)
        return;

    const char *serial = adt_getprop(adt, 0, "serial-number", NULL);
    if (!serial || !serial[0]) {
        str_serial = &str_serial_dummy;
        return;
    }

    size_t len = strlen(serial);
    size_t size = sizeof(struct usb_string_descriptor) + 2 * len;

    struct usb_string_descriptor *desc = calloc(1, size);
    memset(desc, 0, size);
    desc->bLength = size;
    desc->bDescriptorType = USB_STRING_DESCRIPTOR;
    for (size_t i = 0; i < len; i++)
        desc->bString[i] = serial[i];

    str_serial = desc;
}

static void usb_cdc_get_string_descriptor(u32 index, const void **descriptor, u16 *descriptor_len)
{
    switch (index) {
        case STRING_DESCRIPTOR_LANGUAGES:
            *descriptor = &str_langs;
            *descriptor_len = str_langs.bLength;
            break;
        case STRING_DESCRIPTOR_MANUFACTURER:
            *descriptor = &str_manufacturer;
            *descriptor_len = str_manufacturer.bLength;
            break;
        case STRING_DESCRIPTOR_PRODUCT:
            *descriptor = &str_product;
            *descriptor_len = str_product.bLength;
            break;
        case STRING_DESCRIPTOR_SERIAL:
            usb_build_serial();
            *descriptor = str_serial;
            *descriptor_len = str_serial->bLength;
            break;
        default:
            *descriptor = NULL;
            *descriptor_len = 0;
    }
}

static int
usb_dwc2_handle_ep0_get_descriptor(dwc2_dev_t *dev,
                                   const struct usb_setup_packet_get_descriptor *get_descriptor)
{
    const void *descriptor = NULL;
    u16 descriptor_len = 0;

    // usb_debug_printf("handle_ep0_get_descriptor, type=%u\n", get_descriptor->type);

    switch (get_descriptor->type) {
        case USB_DEVICE_DESCRIPTOR:
            descriptor = &usb_cdc_device_descriptor;
            descriptor_len = usb_cdc_device_descriptor.bLength;
            break;
        case USB_CONFIGURATION_DESCRIPTOR:
            descriptor = &cdc_configuration_descriptor;
            descriptor_len = cdc_configuration_descriptor.configuration.wTotalLength;
            break;
        case USB_STRING_DESCRIPTOR:
            usb_cdc_get_string_descriptor(get_descriptor->index, &descriptor, &descriptor_len);
            break;
        case USB_DEVICE_QUALIFIER_DESCRIPTOR:
            descriptor = &usb_cdc_device_qualifier_descriptor;
            descriptor_len = usb_cdc_device_qualifier_descriptor.bLength;
            break;
        default:
            usb_error_printf("Unknown descriptor type: %d\n", get_descriptor->type);
            break;
    }

    if (descriptor) {
        dev->ep0_buffer = descriptor;
        dev->ep0_buffer_len = min(get_descriptor->wLength, descriptor_len);
        return 0;
    } else {
        return -1;
    }
}

static void usb_dwc2_ep0_handle_standard_device(dwc2_dev_t *dev,
                                                const union usb_setup_packet *setup)
{
    // usb_debug_printf("ep0_handle_standard_device: bRequest=%u\n", setup->raw.bRequest);
    switch (setup->raw.bRequest) {
        case USB_REQUEST_SET_ADDRESS:
            // usb_debug_printf("handle USB_REQUEST_SET_ADDRESS, addr=%u, addr=%u\n",
            // setup->set_address.address, setup->raw.wValue & 0x7f);
            usb_set_address(dev, setup->set_address.address);
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_STATUS;
            // debug_printf("S");
            break;

        case USB_REQUEST_SET_CONFIGURATION:
            switch (setup->set_configuration.configuration) {
                case 0:
                    usb_debug_printf("set_config 0; DEACTIVE TBD\n");
                    // for(int i=2; i < 8; i++){
                    //     usb_dwc2_enable_ep(dev, i, 0);
                    // }
                    dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_STATUS;
                    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++)
                        dev->pipe[i].ready = false;
                    break;
                case 1:
                    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
                        /* prepare INTR endpoint so that we don't have to reconfigure this device
                         * later */
                        usb_dwc2_ep_activate(dev, dev->pipe[i].ep_intr, DWC2_EP_TYPE_INTR, 64);

                        /* prepare BULK endpoints so that we don't have to reconfigure this device
                         * later */
                        usb_dwc2_ep_activate(dev, dev->pipe[i].ep_in, DWC2_EP_TYPE_BULK, 512);
                        usb_dwc2_ep_activate(dev, dev->pipe[i].ep_out, DWC2_EP_TYPE_BULK, 512);
                    }
                    dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_STATUS;
                    break;
                default:
                    usb_dwc2_ep_set_stall(dev, 0, 1);
                    dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
                    break;
            }
            break;

        case USB_REQUEST_GET_DESCRIPTOR:
            if (usb_dwc2_handle_ep0_get_descriptor(dev, &setup->get_descriptor) < 0) {
                usb_dwc2_ep_set_stall(dev, 0, 1);
                dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
            } else {
                dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND;
            }
            break;

        case USB_REQUEST_GET_STATUS: {
            static const u16 device_status = 0x0001; // self-powered
            dev->ep0_buffer = &device_status;
            dev->ep0_buffer_len = 2;
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND;
            break;
        }

        default:
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
            usb_error_printf("unsupported SETUP packet bRequest=0x%x bmRequestType=0x%x\n",
                             setup->raw.bRequest, setup->raw.bmRequestType);
    }
}

static void usb_dwc2_ep0_handle_standard_interface(dwc2_dev_t *dev,
                                                   const union usb_setup_packet *setup)
{
    switch (setup->raw.bRequest) {
        case USB_REQUEST_GET_STATUS: {
            static const u16 device_status = 0x0000; // reserved
            dev->ep0_buffer = &device_status;
            dev->ep0_buffer_len = 2;
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND;
            break;
        }
        default:
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
            usb_error_printf("unsupported SETUP packet\n");
    }
}

static int usb_dwc2_start_setup_phase(dwc2_dev_t *dev)
{
    usb_dwc2_ep_hw_recv(dev, USB_LEP_CTRL_OUT, 64, 1);
    return 0;
}

static int usb_dwc2_ep0_start_data_send_phase(dwc2_dev_t *dev)
{
    // usb_debug_printf("ep0_start_data_send_phase: device was requested to xfer %d on ep 0\n",
    // dev->ep0_buffer_len);
    memset(dev->endpoints[USB_LEP_CTRL_IN].xfer_buffer, 0, 64);
    memcpy(dev->endpoints[USB_LEP_CTRL_IN].xfer_buffer, dev->ep0_buffer, dev->ep0_buffer_len);
    u32 pkt_count = (dev->ep0_buffer_len + 63) / 64;
    if (dev->ep0_buffer_len % 64 == 0) {
        pkt_count++;
    }
    usb_dwc2_ep_hw_send(dev, USB_LEP_CTRL_IN, dev->ep0_buffer_len, pkt_count);
    return 0;
}

static int usb_dwc2_ep0_start_data_recv_phase(dwc2_dev_t *dev)
{
    // usb_debug_printf("ep0_start_data_send_phase: device was requested to recv %d on ep 0\n",
    // dev->ep0_read_buffer_len);
    memset(dev->endpoints[USB_LEP_CTRL_OUT].xfer_buffer, 0xbb, dev->ep0_read_buffer_len);
    usb_dwc2_ep_hw_recv(dev, USB_LEP_CTRL_OUT, dev->ep0_read_buffer_len,
                        1); // should check if ep0_read_buffer_len is too large
    return 0;
}

static void usb_dwc2_ep0_handle_standard_endpoint(dwc2_dev_t *dev,
                                                  const union usb_setup_packet *setup)
{
    switch (setup->raw.bRequest) {
        case USB_REQUEST_GET_STATUS: {
            static const u16 device_status = 0x0000; // reserved
            dev->ep0_buffer = &device_status;
            dev->ep0_buffer_len = 2;
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND;
            break;
        }
        case USB_REQUEST_CLEAR_FEATURE: {
            switch (setup->feature.wFeatureSelector) {
                case USB_FEATURE_ENDPOINT_HALT:
                    usb_debug_printf("Host cleared EP 0x%x stall\n", setup->feature.wEndpoint);
                    usb_dwc2_ep_set_stall(dev, ep_to_num(setup->feature.wEndpoint), 0);
                    usb_dwc2_start_status_phase(dev, USB_LEP_CTRL_IN);
                    dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_STATUS_DONE;
                    break;
                default:
                    usb_dwc2_ep_set_stall(dev, 0, 1);
                    dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
                    usb_error_printf("unsupported CLEAR FEATURE: 0x%x\n",
                                     setup->feature.wFeatureSelector);
                    break;
            }
            break;
        }
        default:
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
            usb_error_printf("unsupported SETUP packet\n");
    }
}

static void usb_dwc2_ep0_handle_standard(dwc2_dev_t *dev, const union usb_setup_packet *setup)
{
    // usb_debug_printf("ep0_handle_standard: questType=0x%x\n", setup->raw.bmRequestType &
    // USB_REQUEST_TYPE_RECIPIENT_MASK);

    switch (setup->raw.bmRequestType & USB_REQUEST_TYPE_RECIPIENT_MASK) {
        case USB_REQUEST_TYPE_RECIPIENT_DEVICE:
            usb_dwc2_ep0_handle_standard_device(dev, setup);
            break;

        case USB_REQUEST_TYPE_RECIPIENT_INTERFACE:
            usb_dwc2_ep0_handle_standard_interface(dev, setup);
            break;

        case USB_REQUEST_TYPE_RECIPIENT_ENDPOINT:
            usb_dwc2_ep0_handle_standard_endpoint(dev, setup);
            break;

        default:
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
            usb_error_printf("unimplemented request recipient\n");
    }
}

static void usb_dwc2_ep0_handle_class(dwc2_dev_t *dev, const union usb_setup_packet *setup)
{
    int pipe = setup->raw.wIndex / 2;

    if (pipe >= CDC_ACM_PIPE_MAX)
        goto unsupported_setup_pkt;

    usb_debug_printf("ep0_handle_class: bRequest=0x%x pipe=%u\n", setup->raw.bRequest, pipe);

    switch (setup->raw.bRequest) {
        case USB_REQUEST_CDC_GET_LINE_CODING:
            dev->ep0_buffer_len = min(setup->raw.wLength, sizeof(dev->pipe[pipe].cdc_line_coding));
            dev->ep0_buffer = dev->pipe[pipe].cdc_line_coding;
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND;
            break;

        case USB_REQUEST_CDC_SET_CTRL_LINE_STATE:
            if (setup->raw.wValue & 1) { // DTR
                dev->pipe[pipe].ready = false;
                usb_debug_printf("ACM device opened\n");
                dev->pipe[pipe].ready = true;
                if (pipe == 1)
                    usb_dwc2_cdc_start_bulk_out_xfer(dev, USB_LEP_CDC_BULK_OUT_2);
                else if (pipe == 0)
                    usb_dwc2_cdc_start_bulk_out_xfer(dev, USB_LEP_CDC_BULK_OUT);
            } else {
                dev->pipe[pipe].ready = false;
                usb_debug_printf("ACM device closed\n");
            }
            usb_dwc2_start_status_phase(dev, USB_LEP_CTRL_IN);
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_STATUS_DONE;
            break;

        case USB_REQUEST_CDC_SET_LINE_CODING:
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
            // disable USB_REQUEST_CDC_SET_LINE_CODING for now, since it's useless for now
            // A handler for setup transcations with data stage has less timewindow
            //  dev->ep0_read_buffer = dev->pipe[pipe].cdc_line_coding;
            //  dev->ep0_read_buffer_len =
            //      min(setup->raw.wLength, sizeof(dev->pipe[pipe].cdc_line_coding));
            //  dev->ep0_state = USB_DWC2_EP0_STATE_DATA_RECV;
            break;

        default:
        unsupported_setup_pkt:
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
            usb_error_printf("unsupported SETUP packet\n");
    }
}

static void usb_dwc2_ep0_handle_setup(dwc2_dev_t *dev)
{
    const union usb_setup_packet *setup = dev->endpoints[USB_LEP_CTRL_OUT].xfer_buffer;
    dev->setup_pkt = setup;

    // usb_debug_printf("ep0_handle_setup: setup->raw.bmRequestType = %u\n",
    // setup->raw.bmRequestType);

    switch (setup->raw.bmRequestType & USB_REQUEST_TYPE_MASK) {
        case USB_REQUEST_TYPE_STANDARD:
            usb_dwc2_ep0_handle_standard(dev, setup);
            break;
        case USB_REQUEST_TYPE_CLASS:
            usb_dwc2_ep0_handle_class(dev, setup);
            break;
        default:
            usb_error_printf("unsupported request type\n");
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
    }
}

void usb_dwc2_handle_events(dwc2_dev_t *dev)
{
    // usb_debug_printf("------checking int-----\n");
    usb_dwc2_handle_interrupts(dev);
}

static void usb_dwc2_ep0_handle_xfer_done(dwc2_dev_t *dev)
{
    // usb_debug_printf("ep0_handle_xfer_done: %d, %s\n",
    //     dev->ep0_state, ep0_state_names[dev->ep0_state]);
    switch (dev->ep0_state) {
        case USB_DWC2_EP0_STATE_SETUP_HANDLE:
            usb_dwc2_ep0_handle_setup(dev);
            break;

        case USB_DWC2_EP0_STATE_DATA_RECV_STATUS_DONE:
        case USB_DWC2_EP0_STATE_DATA_SEND_STATUS_DONE:
            usb_dwc2_start_setup_phase(dev);
            dev->ep0_state = USB_DWC2_EP0_STATE_SETUP_HANDLE;
            break;

        case USB_DWC2_EP0_STATE_DATA_SEND_DONE:
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_RECV_STATUS;
            break;

        case USB_DWC2_EP0_STATE_DATA_RECV_DONE:
            memcpy(dev->ep0_read_buffer, dev->endpoints[USB_LEP_CTRL_OUT].xfer_buffer,
                   dev->ep0_read_buffer_len);
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_STATUS;
            break;

        case USB_DWC2_EP0_STATE_IDLE:
        default:
            usb_error_printf("invalid state in usb_dwc2_ep0_handle_xfer_done: %d, %s\n",
                             dev->ep0_state, ep0_state_names[dev->ep0_state]);
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
    }
}

static void usb_dwc2_ep0_handle_xfer_not_ready(dwc2_dev_t *dev)
{
    // usb_debug_printf("ep0_handle_xfer_not_ready: %d, %s\n",
    //                          dev->ep0_state, ep0_state_names[dev->ep0_state]);
    switch (dev->ep0_state) {
        case USB_DWC2_EP0_STATE_SETUP_HANDLE:
        case USB_DWC2_EP0_STATE_DATA_SEND_STATUS_DONE:
        case USB_DWC2_EP0_STATE_DATA_RECV_STATUS_DONE:
            break; // not the right time, skip them since we check this manually
        case USB_DWC2_EP0_STATE_IDLE:
            usb_dwc2_start_setup_phase(dev);
            dev->ep0_state = USB_DWC2_EP0_STATE_SETUP_HANDLE;
            break;

        case USB_DWC2_EP0_STATE_DATA_SEND:
            if (usb_dwc2_ep0_start_data_send_phase(dev))
                usb_debug_printf("cannot start xtrl xfer data phase for EP 1.\n");
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_DONE;
            break;

        case USB_DWC2_EP0_STATE_DATA_RECV:
            if (usb_dwc2_ep0_start_data_recv_phase(dev))
                usb_debug_printf("cannot start xtrl xfer data phase for EP 0.\n");
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_RECV_DONE;
            break;

        case USB_DWC2_EP0_STATE_DATA_RECV_STATUS:
            usb_dwc2_start_status_phase(dev, USB_LEP_CTRL_OUT);
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_RECV_STATUS_DONE;
            break;

        case USB_DWC2_EP0_STATE_DATA_SEND_STATUS:
            usb_dwc2_start_status_phase(dev, USB_LEP_CTRL_IN);
            dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_STATUS_DONE;
            break;

        default:
            usb_error_printf(
                "invalid state in usb_dwc2_ep0_handle_xfer_not_ready: %d, %s for ep %d\n",
                dev->ep0_state, ep0_state_names[dev->ep0_state], 0);
            usb_dwc2_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;
    }
}

ringbuffer_t *usb_dwc2_cdc_get_ringbuffer(dwc2_dev_t *dev, u8 endpoint_number)
{
    switch (endpoint_number) {
        case USB_LEP_CDC_BULK_IN:
            return dev->pipe[CDC_ACM_PIPE_0].device2host;
        case USB_LEP_CDC_BULK_OUT:
            return dev->pipe[CDC_ACM_PIPE_0].host2device;
        case USB_LEP_CDC_BULK_IN_2:
            return dev->pipe[CDC_ACM_PIPE_1].device2host;
        case USB_LEP_CDC_BULK_OUT_2:
            return dev->pipe[CDC_ACM_PIPE_1].host2device;
        default:
            return NULL;
    }
}

static void usb_dwc2_cdc_start_bulk_out_xfer(dwc2_dev_t *dev, u8 endpoint_number)
{
    if (dev->endpoints[endpoint_number].xfer_in_progress)
        return;
    // usb_debug_printf("cdc_start_bulk_out_xfer:###recving on %u\n", endpoint_number);
    ringbuffer_t *host2device = usb_dwc2_cdc_get_ringbuffer(dev, endpoint_number);
    if (!host2device)
        return;

    if (ringbuffer_get_free(host2device) < XFER_SIZE)
        return;

    memset(dev->endpoints[endpoint_number].xfer_buffer, 0xaa, XFER_SIZE);
    usb_dwc2_ep_hw_recv(dev, endpoint_number, 512, 1);
    dev->endpoints[endpoint_number].xfer_in_progress = true;
}

static void usb_dwc2_cdc_start_bulk_in_xfer(dwc2_dev_t *dev, u8 endpoint_number)
{
    if (dev->endpoints[endpoint_number].xfer_in_progress)
        return;

    ringbuffer_t *device2host = usb_dwc2_cdc_get_ringbuffer(dev, endpoint_number);
    if (!device2host)
        return;

    size_t len =
        ringbuffer_read(dev->endpoints[endpoint_number].xfer_buffer, XFER_SIZE, device2host);

    if (!len && !dev->endpoints[endpoint_number].zlp_pending)
        return;

    u32 pkt_count = (len + 511) / 512;
    if (dev->ep0_buffer_len % 512 == 0) {
        pkt_count++;
        dev->endpoints[endpoint_number].zlp_pending = 1;
    }
    usb_debug_printf("cdc_start_bulk_in_xfer: hw_send(%lu, %u) from endpoint_index=%u\n", len,
                     pkt_count, endpoint_number);
    usb_dwc2_ep_hw_send(dev, endpoint_number, len, pkt_count);
    dev->endpoints[endpoint_number].xfer_in_progress = true;
    // dev->endpoints[endpoint_number].zlp_pending = (len % 512) == 0;
    // USB_DEBUG_PRINT_REGISTERS(dev);
}

static void usb_dwc2_cdc_handle_bulk_out_xfer_done(dwc2_dev_t *dev, u8 ep)
{
    ringbuffer_t *host2device = usb_dwc2_cdc_get_ringbuffer(dev, ep);
    if (!host2device)
        return;
    u8 ipep = phyEndpoints[USB_LEP_CDC_BULK_OUT];
    ;
    if (ep == USB_LEP_CDC_BULK_OUT_2)
        ipep = phyEndpoints[USB_LEP_CDC_BULK_OUT_2];
    size_t xfer_siz = 0x200 - (read32(dev->regs + DWC2_DOEPTSIZ(ipep)) & 0x7fff);
    if (ringbuffer_get_free(host2device) < xfer_siz) {
        usb_debug_printf("out_xfer_buffer size overflow\n");
        xfer_siz = ringbuffer_get_free(host2device);
    }
    ringbuffer_write(dev->endpoints[ep].xfer_buffer, xfer_siz, host2device);
    usb_debug_printf("handle_bulk_out_xfer_done: recvd %zd bytes from bulk out\n", xfer_siz);
    // hexdump(dev->endpoints[ep].xfer_buffer, xfer_siz);
    dev->endpoints[ep].xfer_in_progress = false;

    // usb_debug_printf("handle_bulk_out_xfer_done: DWC2_DOEPTSIZ(2)=%x\n", read32(dev->regs +
    // DWC2_DOEPTSIZ(2))); usb_debug_printf("handle_bulk_out_xfer_done: ep's buffer=%p\n",
    // dev->endpoints[ep].xfer_buffer); while(1) {}
}

static int usb_dwc2_start_status_phase(dwc2_dev_t *dev, u8 ep)
{
    if (phyEndpoints[ep] & 0x80) // dir in
        usb_dwc2_ep_hw_send(dev, ep, 0, 1);
    else
        usb_dwc2_ep_hw_recv(dev, ep, 64, 1);
    return 0;
}

// static int usb_dwc2_enable_ep(dwc2_dev_t *dev, u8 ep, u8 enable) {
//     u8 pep = phyEndpoints[ep];
//     if (pep & 0x80) {//dir_in
//         pep &= 0xf;
//         if(enable){
//             set32(dev->regs + DWC2_DIEPCTL(pep), 1<<15);
//         }
//         else clear32(dev->regs + DWC2_DIEPCTL(pep), 1<<15);
//     }
//     else {
//         if(enable)
//             set32(dev->regs + DWC2_DOEPCTL(pep), 1<<15);
//         else clear32(dev->regs + DWC2_DOEPCTL(pep), 1<<15);
//     }
//     return 0;
// }

static void usb_dwc2_handle_interrupts_ep(dwc2_dev_t *dev)
{
    u32 daint = read32(dev->regs + DWC2_DAINT);
    if (daint && !(daint & (BIT(16) | BIT(0)))) // timing requirements
        usb_debug_printf("handle_interrupts_ep: DAINT = %x\n", daint);
    // dispatch handler for each endpoints
    if (daint & BIT(0)) { // ep0_in
        u32 diepint = read32(dev->regs + DWC2_DIEPINT(0));
        write32(dev->regs + DWC2_DIEPINT(0), diepint);
        usb_debug_printf("ep0_in_handle_interrupt:  DWC2_DIEPINT(0)=%x\n", diepint);
        if (diepint & DWC2_DOEPINT_XFER_COMPL) { // XferCompl
            usb_dwc2_ep0_handle_xfer_done(dev);
            usb_dwc2_ep0_handle_xfer_not_ready(dev);
        }
        usb_debug_printf("IN DONE:  state=%s\n", ep0_state_names[dev->ep0_state]);
    }

    if (daint & BIT(16 + 0)) { // ep0_out
        u32 doepint = read32(dev->regs + DWC2_DOEPINT(0));
        write32(dev->regs + DWC2_DOEPINT(0), doepint);
        // usb_debug_printf("ep0_out_handle_interrupt: DWC2_DOEPINT(0)=%x\n", doepint);
        bool setup_packet_recvd = doepint & DWC2_DOEPINT_STUP_PKT_RCVD;
        bool setup_phase_done = doepint & DWC2_DOEPINT_SETUP;
        if (setup_packet_recvd || setup_phase_done) {
            // usb_debug_printf("got (part of) setup_packet DMA done now with %s, doepint=%x\n",
            // ep0_state_names[dev->ep0_state], doepint);
            if (dev->ep0_state == USB_DWC2_EP0_STATE_SETUP_HANDLE)
                dev->ep0_state = USB_DWC2_EP0_STATE_SETUP_PENDING;
        }
        if (setup_phase_done) {
            // usb_debug_printf("setup_phase_done now with %s, doepint=%x\n",
            // ep0_state_names[dev->ep0_state], doepint);
            assert(dev->ep0_state == USB_DWC2_EP0_STATE_SETUP_PENDING);
            dev->ep0_state = USB_DWC2_EP0_STATE_SETUP_HANDLE;
            usb_dwc2_ep0_handle_xfer_done(dev);
            usb_dwc2_ep0_handle_xfer_not_ready(dev);
        } else if (doepint & DWC2_DOEPINT_XFER_COMPL) {
            if (dev->ep0_state == USB_DWC2_EP0_STATE_DATA_RECV_STATUS_DONE) {
                // usb_debug_printf("DWC2_DOEPINT_XFER_COMPL now with %s, doepint=%x\n",
                // ep0_state_names[dev->ep0_state], doepint);
                usb_dwc2_ep0_handle_xfer_done(dev);
            } else if (dev->ep0_state == USB_DWC2_EP0_STATE_DATA_RECV_DONE) {
                // usb_debug_printf("DWC2_DOEPINT_XFER_COMPL2 now with %s, doepint=%x\n",
                // ep0_state_names[dev->ep0_state], doepint);
                usb_dwc2_ep0_handle_xfer_done(dev);      // process data_packet in data Stage
                usb_dwc2_ep0_handle_xfer_not_ready(dev); // send status in
            } else if (!setup_packet_recvd) {
                usb_error_printf(
                    "BAD STATUS with COMPL now with %s, doepint=%x DWC2_DOEPCTL(0)=%lx\n",
                    ep0_state_names[dev->ep0_state], doepint, dev->regs + DWC2_DOEPCTL(0));
                dev->ep0_state = USB_DWC2_EP0_STATE_DATA_SEND_STATUS;
                usb_dwc2_ep0_handle_xfer_not_ready(dev);
                // hexdump(dev->endpoints[USB_LEP_CTRL_OUT].xfer_buffer, 0x20);
                // usb_dwc2_ep_set_stall(dev, 0, 1);
                // set32(dev->regs + DWC2_DOEPCTL(0), BIT(26));
            }
        }
        // usb_debug_printf("OUT DONE: state=%s\n", ep0_state_names[dev->ep0_state]);
    }
    if (daint & BIT(16 + 2)) { // USB_LEP_CDC_BULK_OUT
        u32 doepint = read32(dev->regs + DWC2_DOEPINT(2));
        write32(dev->regs + DWC2_DOEPINT(2), doepint);
        usb_debug_printf("ep0_out_handle_interrupt: DWC2_DOEPINT(2)=%x#USB_LEP_CDC_BULK_OUT\n",
                         doepint);
        if (doepint & DWC2_DOEPINT_XFER_COMPL) {
            /* The bit can be set before the transfer actually completes on some devices... */
            udelay(2);

            usb_dwc2_cdc_handle_bulk_out_xfer_done(dev, USB_LEP_CDC_BULK_OUT);
            usb_dwc2_cdc_start_bulk_out_xfer(dev, USB_LEP_CDC_BULK_OUT);
        }
    }
    if (daint & BIT(0 + 3)) { // USB_LEP_CDC_BULK_IN
        u32 diepint = read32(dev->regs + DWC2_DIEPINT(3));
        write32(dev->regs + DWC2_DIEPINT(3), diepint);
        usb_debug_printf("ep0_out_handle_interrupt: DWC2_DIEPINT(3)=%x#USB_LEP_CDC_BULK_IN\n",
                         diepint);
        // usb_debug_printf("dev->endpoints[USB_LEP_CDC_BULK_OUT].xfer_buffer at %p\n",
        //                  dev->endpoints[USB_LEP_CDC_BULK_OUT].xfer_buffer);
        dev->endpoints[USB_LEP_CDC_BULK_IN].xfer_in_progress = false;
        // usb_dwc2_cdc_start_bulk_out_xfer(dev, USB_LEP_CDC_BULK_OUT);
        // usb_debug_printf("ep1 send done, remain %u bytes\n",
        // ringbuffer_get_used(usb_dwc2_cdc_get_ringbuffer(dev, USB_LEP_CDC_BULK_IN)) );
        if (ringbuffer_get_used(usb_dwc2_cdc_get_ringbuffer(dev, USB_LEP_CDC_BULK_IN))) {
            usb_dwc2_cdc_start_bulk_in_xfer(dev, USB_LEP_CDC_BULK_IN);
        }
    }
    if (daint & BIT(16 + 5)) { // USB_LEP_CDC_BULK_OUT_2
        u32 doepint = read32(dev->regs + DWC2_DOEPINT(5));
        write32(dev->regs + DWC2_DOEPINT(5), doepint);
        usb_debug_printf("ep0_out_handle_interrupt: DWC2_DOEPINT(5)=%x#USB_LEP_CDC_BULK_OUT_2\n",
                         doepint);
        if (doepint & DWC2_DOEPINT_XFER_COMPL) {
            /* The bit can be set before the transfer actually completes on some devices... */
            udelay(2);

            usb_dwc2_cdc_handle_bulk_out_xfer_done(dev, USB_LEP_CDC_BULK_OUT_2);
            usb_dwc2_cdc_start_bulk_out_xfer(dev, USB_LEP_CDC_BULK_OUT_2);
        }
    }
    if (daint & BIT(0 + 6)) { // USB_LEP_CDC_BULK_IN_2
        u32 diepint = read32(dev->regs + DWC2_DIEPINT(6));
        write32(dev->regs + DWC2_DIEPINT(6), diepint);
        usb_debug_printf("ep0_out_handle_interrupt: DWC2_DIEPINT(6)=%x#USB_LEP_CDC_BULK_IN_2\n",
                         diepint);
        // usb_debug_printf("dev->endpoints[USB_LEP_CDC_BULK_OUT_2].xfer_buffer at %p\n",
        //                  dev->endpoints[USB_LEP_CDC_BULK_OUT_2].xfer_buffer);
        dev->endpoints[USB_LEP_CDC_BULK_IN_2].xfer_in_progress = false;
        // usb_dwc2_cdc_start_bulk_out_xfer(dev, USB_LEP_CDC_BULK_OUT_2);
        // usb_debug_printf("ep1 send done, remain %u bytes\n",
        // ringbuffer_get_used(usb_dwc2_cdc_get_ringbuffer(dev, USB_LEP_CDC_BULK_IN_2)) );
        if (ringbuffer_get_used(usb_dwc2_cdc_get_ringbuffer(dev, USB_LEP_CDC_BULK_IN_2))) {
            usb_dwc2_cdc_start_bulk_in_xfer(dev, USB_LEP_CDC_BULK_IN_2);
        }
    }
}

static void usb_set_address(dwc2_dev_t *dev, u8 address)
{
    // usb_debug_printf("Set address %u\n", address);
    u32 dcfg = read32(dev->regs + DWC2_DCFG);
    dcfg = (dcfg & ~0x7f0) | (((u32)address << 4) & 0x7f0);
    write32(dev->regs + DWC2_DCFG, dcfg);
}

static void usb_dwc2_ep_hw_recv(dwc2_dev_t *dev, u8 ep, u32 hw_xfer_size, u32 packet_count)
{
    // usb_debug_printf("ep_hw_recv with endpoint_index = %u, %u | %u \n", ep, hw_xfer_size,
    // packet_count);
    if (phyEndpoints[ep] & 0x80) { // dir_in
        usb_error_printf("ep_hw_recv with dir_in endpoint =%u \n", phyEndpoints[ep]);
        return;
    }

    dma_rmb();
    u8 pep = phyEndpoints[ep];

    // write the lower 32 bits the high bit is handled at the PHY level
    write32(dev->regs + DWC2_DOEPDMA(pep), (uintptr_t)dev->endpoints[ep].xfer_buffer);
    write32(dev->regs + DWC2_DOEPTSIZ(pep), (packet_count << 19) | hw_xfer_size);
    if (!ep) { // EP0
        // usb_debug_printf("usb_dwc2_ep_hw_recv with EP0out now:  state=%s\n",
        // ep0_state_names[dev->ep0_state]);
        if (dev->ep0_state == USB_DWC2_EP0_STATE_DATA_RECV)
            set32(dev->regs + DWC2_DOEPCTL(pep), DWC2_DXEPCTLi_EnableEP | DWC2_DXEPCTL_ClearNAK);
        else
            set32(dev->regs + DWC2_DOEPCTL(pep), DWC2_DXEPCTLi_EnableEP);
    } else
        set32(dev->regs + DWC2_DOEPCTL(pep), DWC2_DXEPCTLi_EnableEP | DWC2_DXEPCTL_ClearNAK);
}

static void usb_dwc2_ep_hw_send(dwc2_dev_t *dev, u8 ep, u32 hw_xfer_size, u32 packet_count)
{
    u8 pep = phyEndpoints[ep] & 0xf;
    if (ep == USB_LEP_CDC_BULK_IN_2)
        usb_debug_printf("ep_hw_send with EP%u endpoint_index = %u, %u | %u \n", pep, ep,
                         hw_xfer_size, packet_count);
    if (!(phyEndpoints[ep] & 0x80)) { // dir_out
        usb_error_printf("usb_dwc2_ep_hw_send with dir_out endpoint =%u E \n", phyEndpoints[ep]);
        return;
    }

    dma_rmb();
    write32(dev->regs + DWC2_DIEPDMA(pep), (uintptr_t)dev->endpoints[ep].xfer_buffer);
    write32(dev->regs + DWC2_DIEPTSIZ(pep), (packet_count << 19) | hw_xfer_size);
    set32(dev->regs + DWC2_DIEPCTL(pep), DWC2_DXEPCTLi_EnableEP | DWC2_DXEPCTL_ClearNAK);
    if (pep == 0)
        set32(dev->regs + DWC2_DOEPCTL(pep), DWC2_DXEPCTL_ClearNAK); // set cak
    dev->endpoints[ep].in_flight = hw_xfer_size;
}

static void usb_dwc2_handle_usbrst(dwc2_dev_t *dev)
{
    // usb_debug_printf("handle_usbrst: Reset now\n");
    next_tx_fifo_addr = TX_FIFO_SIZE + RX_FIFO_SIZE; // EP0_TX+RX
    dev->endpoints[0].xfer_in_progress = false;
    // for (int i = 1; i < MAX_ENDPOINTS; ++i) {
    //     dev->endpoints[i].xfer_in_progress = false;
    //     memset(dev->endpoints[i].xfer_buffer, 0, XFER_BUFFER_BYTES_PER_EP);
    // }
    for (int i = 1; i < MAX_ENDPOINTS; i++) {
        usb_dwc2_ep_abort(dev, i);
    }
    usb_set_address(dev, 0);
    write32(dev->regs + DWC2_DOEPMSK, 0);
    write32(dev->regs + DWC2_DIEPMSK, 0);
    write32(dev->regs + DWC2_DAINTMSK, 0);
    write32(dev->regs + DWC2_DIEPINT(0), DWC2_DIEPINT_XferCompl | DWC2_DIEPINT_EPDisabled |
                                             DWC2_DIEPINT_AHBErr | DWC2_DIEPINT_TimeOUT |
                                             DWC2_DIEPINT_InTokenTXFifoEmpty);
    write32(dev->regs + DWC2_DOEPINT(0), DWC2_DOEPINT_XFER_COMPL | DWC2_DOEPINT_EPDisabled |
                                             DWC2_DOEPINT_AHBErr | DWC2_DOEPINT_SETUP);
    write32(dev->regs + DWC2_GRXFSIZ, RX_FIFO_SIZE);
    write32(dev->regs + DWC2_GNPTXFSIZ, (TX_FIFO_SIZE << 16) | RX_FIFO_SIZE); //   64 bytes
    write32(dev->regs + DWC2_DTXFSIZ(1), 0x0040022b);                         //  256 bytes
    write32(dev->regs + DWC2_DTXFSIZ(2), 0x0040022b);                         // 256 bytes
    write32(dev->regs + DWC2_DTXFSIZ(3), 0x0040022b);                         // 256 bytes
    write32(dev->regs + DWC2_DTXFSIZ(4), 0x0040022b);                         // 256 bytes
    write32(dev->regs + DWC2_DOEPCTL(0), 0);
    write32(dev->regs + DWC2_DIEPCTL(0), 0);
    write32(dev->regs + DWC2_GINTSTS,
            read32(dev->regs + DWC2_GINTSTS)); // write bit 1 to clear int status before enable it
    set32(dev->regs + DWC2_GINTMSK, DWC2_GINTMSK_IEPIntMsk | DWC2_GINTMSK_OEPIntMsk);
    write32(dev->regs + DWC2_DOEPMSK,
            DWC2_DOEPMSK_XferComplMsk | DWC2_DOEPMSK_AHBErrMsk | DWC2_DOEPMSK_SetUPMsk);
    write32(dev->regs + DWC2_DIEPMSK,
            DWC2_DIEPMSK_XferComplMsk | DWC2_DIEPMSK_AHBErrMsk | DWC2_DIEPMSK_TimeOUTMsk);
    write32(dev->regs + DWC2_DAINTMSK, 0);
    // usb_dwc2_ep_enable_recv(dev, USB_LEP_CTRL_OUT);
    /* clear STALL mode for all endpoints */
    // USB_DEBUG_PRINT_REGISTERS(dev);
    // usb_debug_printf("GINTMSK=%x after rst\n", read32(dev->regs + DWC2_GINTMSK));
}

static void usb_dwc2_ep_abort(dwc2_dev_t *dev, u8 ep)
{
    dev->endpoints[ep].in_flight = 0;
    dev->endpoints[ep].xfer_in_progress = 0;
    u8 pep = phyEndpoints[ep];
    if (pep & 0x80) { // dir_in
        pep &= 0xf;
        // usb_debug_printf("EP%u IN abort\n", pep);
        if (read32(dev->regs + DWC2_DIEPCTL(pep)) & DWC2_DXEPCTLi_EnableEP) {
            set32(dev->regs + DWC2_DIEPCTL(pep), DWC2_DXEPCTLi_DisableEP);
            while (1) {
                if (read32(dev->regs + DWC2_DIEPINT(pep)) & DWC2_DIEPINT_EPDisabled) {
                    break;
                }
            }
        }
        write32(dev->regs + DWC2_DIEPINT(pep), read32(dev->regs + DWC2_DIEPINT(pep)));
        return;
    }
    // usb_debug_printf("EP%u OUT abort\n", pep); // dir_out
    if (read32(dev->regs + DWC2_DOEPCTL(pep)) & DWC2_DXEPCTLi_EnableEP) {
        write32(dev->regs + DWC2_GINTSTS, DWC2_GINTSTS_GOUTNakEff);
        set32(dev->regs + DWC2_DCTL, DWC2_DCTL_SGOUTNak);
        while (1) {
            if (read32(dev->regs + DWC2_GINTSTS) & DWC2_GINTSTS_GOUTNakEff) {
                break;
            }
        }
        write32(dev->regs + DWC2_GINTSTS, DWC2_GINTSTS_GOUTNakEff);
        set32(dev->regs + DWC2_DOEPCTL(pep), DWC2_DXEPCTLi_DisableEP | DWC2_DXEPCTL_SetNAK);
        while (1) {
            if (read32(dev->regs + DWC2_DOEPINT(pep)) & DWC2_DOEPINT_EPDisabled) {
                break;
            }
        }
        set32(dev->regs + DWC2_DCTL, DWC2_DCTL_CGOUTNak);
    }
    write32(dev->regs + DWC2_DOEPINT(pep), read32(dev->regs + DWC2_DOEPINT(pep)));
}

static void usb_dwc2_handle_event_connect_done(dwc2_dev_t *dev)
{
    u32 speed = read32(dev->regs + DWC2_DSTS) & DWC2_DSTS_CONNECTSPD;

    // usb_debug_printf("current speed %x from DSTS\n", speed);

    if (speed != DWC2_DSTS_HIGHSPEED) {
        usb_error_printf(
            "WARNING: we only support high speed right now but %x was requested in DSTS\n",
            read32(dev->regs + DWC2_DSTS));
        return;
    }
    // usb_debug_printf("enum done; receive next packet on EP0 OUT\n");
    usb_dwc2_ep_activate(dev, USB_LEP_CTRL_OUT, 0, EP0_MAX_PACKET_SIZE);
    usb_dwc2_ep_activate(dev, USB_LEP_CTRL_IN, 0, EP0_MAX_PACKET_SIZE);
    usb_dwc2_start_setup_phase(dev); // recving packet on EP0-OUT
    dev->ep0_state = USB_DWC2_EP0_STATE_SETUP_HANDLE;
    dev->ep0_read_buffer_len = 0;
}

#define SUPPORTED_GINST                                                                            \
    (DWC2_GINTSTS_USBRst | DWC2_GINTSTS_ENUMDone | DWC2_GINTSTS_IEPInt | DWC2_GINTSTS_OEPInt)

void usb_dwc2_handle_interrupts(dwc2_dev_t *dev)
{
    if (!dev)
        return;

    u32 gintsts = 0;
    while (1) {
        u32 val = read32(dev->regs + DWC2_GINTSTS);
        // usb_debug_printf("DWC2_GINTSTS=%x DAINT=%x\n", val, read32(dev->regs + DWC2_DAINT));
        write32(dev->regs + DWC2_GINTSTS,
                val & SUPPORTED_GINST); // clear supported flags,set 1 to clear
        gintsts = gintsts | val;        // current sts
        if (gintsts & DWC2_GINTSTS_USBRst) {
            usb_dwc2_handle_usbrst(dev);
        }
        if ((gintsts & DWC2_GINTSTS_ENUMDone) &&
            !(val & DWC2_GINTSTS_USBSuspend)) { // not in suspend state
            usb_dwc2_handle_event_connect_done(dev);
        }
        if (gintsts & (DWC2_GINTSTS_OEPInt | DWC2_GINTSTS_IEPInt)) {
            usb_dwc2_handle_interrupts_ep(dev);
            // usb_debug_printf("daintmask=0x%x\n", read32(dev->regs + DWC2_DAINTMSK));
            // EP interrupt
        }
        if (!(gintsts & SUPPORTED_GINST)) {
            // usb_debug_printf("non supported interrupt happened, gintsts=0x%x\n", gintsts);
            break;
        } else {
            gintsts &= ~SUPPORTED_GINST; // clear supported interrupt flag
        }
    }
}

dwc2_dev_t *usb_dwc2_init(u64 regs)
{
    /* version check */
    u32 snpsid = read32(regs + DWC2_GSNPSID);
    if ((snpsid & DWC2_GSNPSID_MASK) != 0x4f540000) {
        usb_error_printf("No DWC2 core found at: 0x%lx: %08x\n", regs, snpsid);
        return NULL;
    }

    uart_printf("usb-dwc2: Core version %04x\n", snpsid & 0xffff);

    dwc2_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;

    memset(dev, 0, sizeof(*dev));
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++)
        memcpy(dev->pipe[i].cdc_line_coding, cdc_default_line_coding,
               sizeof(cdc_default_line_coding));

    dev->regs = regs;
#ifdef LOG_REGISTER_RW
    debug_reg_base = regs;
#endif

    dev->dma_page_p = memalign(SZ_16K, max(DMA_BUFFER_SIZE * MAX_ENDPOINTS, SZ_16K));
    if (!dev->dma_page_p)
        goto error;

    memset(dev->dma_page_p, 0, max(DMA_BUFFER_SIZE * MAX_ENDPOINTS, SZ_16K));

    dma_rmb();

    usb_debug_printf("allocated dma_page at %p\n", dev->dma_page_p);

    /* prepare endpoint buffers */
    for (int i = 0; i < MAX_ENDPOINTS; ++i) {
        u32 xferbuffer_offset = i * DMA_BUFFER_SIZE;
        dev->endpoints[i].xfer_buffer = dev->dma_page_p + xferbuffer_offset;
    }

    /* prepare CDC ACM interfaces */
    dev->pipe[CDC_ACM_PIPE_0].ep_intr = USB_LEP_CDC_INTR_IN;
    dev->pipe[CDC_ACM_PIPE_0].ep_in = USB_LEP_CDC_BULK_IN;
    dev->pipe[CDC_ACM_PIPE_0].ep_out = USB_LEP_CDC_BULK_OUT;

    dev->pipe[CDC_ACM_PIPE_1].ep_intr = USB_LEP_CDC_INTR_IN_2;
    dev->pipe[CDC_ACM_PIPE_1].ep_in = USB_LEP_CDC_BULK_IN_2;
    dev->pipe[CDC_ACM_PIPE_1].ep_out = USB_LEP_CDC_BULK_OUT_2;

    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
        dev->pipe[i].host2device = ringbuffer_alloc(CDC_BUFFER_SIZE);
        if (!dev->pipe[i].host2device)
            goto error;
        dev->pipe[i].device2host = ringbuffer_alloc(CDC_BUFFER_SIZE);
        if (!dev->pipe[i].device2host)
            goto error;
    }

    uint32_t hsphy_type = FIELD_GET(DWC2_GHWCFG2_HSPHYTYPE, read32(regs + DWC2_GHWCFG2));
    uint32_t gusbcfg = read32(regs + DWC2_GUSBCFG);

    switch (hsphy_type) {
        case 0b00: /* No HS */
            return NULL;
        case 0b01: /* UTMI+ */
            if (gusbcfg & DWC2_GUSBCFG_PHYIF16) {
                gusbcfg = DWC2_GUSBCFG_PHYIF16 | FIELD_PREP(GUSBCFG_USBTRDTIM_MASK, 5);
                break;
            }
            /* fallthrough */
        case 0b10: /* ULPI */
            gusbcfg = FIELD_PREP(GUSBCFG_USBTRDTIM_MASK, 9);
            break;
        case 0b11: /* ULPI and UTMI+ */
            gusbcfg = DWC2_GUSBCFG_PHYIF16 | FIELD_PREP(GUSBCFG_USBTRDTIM_MASK, 5);
            break;
    }

    set32(regs + DWC2_DCTL, DWC2_DCTL_SftDisCon);
    write32(regs + DWC2_GAHBCFG, FIELD_PREP(DWC2_GAHBCFG_HBSTLEN_MASK, 7) | DWC2_GAHBCFG_DMA_EN);
    write32(regs + DWC2_GUSBCFG, gusbcfg);
    write32(regs + DWC2_DCFG, DCFG_NZ_STS_OUT_HSHK);
    write32(regs + DWC2_GINTMSK, 0);
    write32(regs + DWC2_DOEPMSK, 0);
    write32(regs + DWC2_DIEPMSK, 0);
    write32(regs + DWC2_DAINTMSK, 0);
    write32(regs + DWC2_DIEPINT(0), DWC2_DIEPINT_XferCompl | DWC2_DIEPINT_EPDisabled |
                                        DWC2_DIEPINT_AHBErr | DWC2_DIEPINT_TimeOUT |
                                        DWC2_DIEPINT_InTokenTXFifoEmpty);
    write32(regs + DWC2_DOEPINT(0), DWC2_DOEPINT_XFER_COMPL | DWC2_DOEPINT_EPDisabled |
                                        DWC2_DOEPINT_AHBErr | DWC2_DOEPINT_SETUP);
    write32(regs + DWC2_GINTMSK, DWC2_GINTSTS_ENUMDoneMsk | DWC2_GINTSTS_USBRstMsk);
    clear32(regs + DWC2_DCTL, DWC2_DCTL_SftDisCon);

    /* prepare control endpoint 0 IN and OUT */
    if (usb_dwc2_ep_activate(dev, USB_LEP_CTRL_IN, 0, 64))
        goto error;
    if (usb_dwc2_ep_activate(dev, USB_LEP_CTRL_OUT, 0, 64))
        goto error;

    dev->ep0_state = USB_DWC2_EP0_STATE_IDLE;

    usb_debug_printf("init() done, usb_dwc2_handle_events = %p\n", usb_dwc2_handle_events);

    return dev;

error:
    usb_dwc2_shutdown(dev);
    return NULL;
}

void usb_dwc2_shutdown(dwc2_dev_t *dev)
{
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++)
        dev->pipe[i].ready = false;

    /* Disconnect USB gadget */
    set32(dev->regs + DWC2_DCTL, DWC2_DCTL_SftDisCon);

    /* Disable DMA */
    clear32(dev->regs + DWC2_GAHBCFG, DWC2_GAHBCFG_DMA_EN);

    /* Reset the controller */
    set32(dev->regs + DWC2_GRSTCTL, DWC2_GRSTCTL_CSFTRST);

    if (poll32(dev->regs + DWC2_GRSTCTL, DWC2_GRSTCTL_CSFTRST, 0, 10000))
        usb_error_printf("Failed to reset the controller\n");

    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
        ringbuffer_free(dev->pipe[i].device2host);
        ringbuffer_free(dev->pipe[i].host2device);
    }

    free(dev->dma_page_p);
    free(dev);
}

u8 usb_dwc2_getbyte(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    ringbuffer_t *host2device = dev->pipe[pipe].host2device;
    if (!host2device)
        return 0;

    u8 ep = dev->pipe[pipe].ep_out;

    u8 c;
    while (ringbuffer_read(&c, 1, host2device) < 1) {
        usb_dwc2_handle_events(dev);
        usb_dwc2_cdc_start_bulk_out_xfer(dev, ep);
    }
    return c;
}

void usb_dwc2_putbyte(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte)
{
    ringbuffer_t *device2host = dev->pipe[pipe].device2host;
    if (!device2host)
        return;

    u8 ep = dev->pipe[pipe].ep_in;

    while (ringbuffer_write(&byte, 1, device2host) < 1) {
        usb_dwc2_handle_events(dev);
        usb_dwc2_cdc_start_bulk_in_xfer(dev, ep);
    }
}

size_t usb_dwc2_queue(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count)
{
    const u8 *p = buf;
    size_t wrote, sent = 0;

    if (!dev || !dev->pipe[pipe].ready)
        return 0;

    ringbuffer_t *device2host = dev->pipe[pipe].device2host;
    if (!device2host)
        return 0;

    u8 ep = dev->pipe[pipe].ep_in;

    while (count) {
        wrote = ringbuffer_write(p, count, device2host);
        count -= wrote;
        p += wrote;
        sent += wrote;
        if (count) {
            usb_dwc2_handle_events(dev);
            usb_dwc2_cdc_start_bulk_in_xfer(dev, ep);
        }
    }

    return sent;
}

size_t usb_dwc2_write(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count)
{
    if (!dev)
        return -1;

    // usb_debug_printf("***dwc2_write,count=%u\n", count);
    u8 ep = dev->pipe[pipe].ep_in;
    size_t ret = usb_dwc2_queue(dev, pipe, buf, count);

    usb_dwc2_cdc_start_bulk_in_xfer(dev, ep);
    // USB_DEBUG_PRINT_REGISTERS(dev);

    return ret;
}

size_t usb_dwc2_read(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count)
{
    // usb_debug_printf("***dwc2_read,count=%u\n", count);
    u8 *p = buf;
    size_t read, recvd = 0;

    if (!dev || !dev->pipe[pipe].ready)
        return 0;

    ringbuffer_t *host2device = dev->pipe[pipe].host2device;
    if (!host2device)
        return 0;

    u8 ep = dev->pipe[pipe].ep_out;

    while (count) {
        read = ringbuffer_read(p, count, host2device);
        count -= read;
        p += read;
        recvd += read;
        usb_dwc2_handle_events(dev);
        usb_dwc2_cdc_start_bulk_out_xfer(dev, ep);
    }

    return recvd;
}

ssize_t usb_dwc2_can_read(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    if (!dev || !dev->pipe[pipe].ready)
        return 0;

    ringbuffer_t *host2device = dev->pipe[pipe].host2device;
    if (!host2device)
        return 0;

    return ringbuffer_get_used(host2device);
}

bool usb_dwc2_can_write(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    (void)pipe;
    if (!dev)
        return false;

    return dev->pipe[pipe].ready;
}

void usb_dwc2_flush(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    if (!dev || !dev->pipe[pipe].ready)
        return;

    ringbuffer_t *device2host = dev->pipe[pipe].device2host;
    if (!device2host)
        return;

    u8 ep = dev->pipe[pipe].ep_in;

    while (ringbuffer_get_used(device2host) != 0 || dev->endpoints[ep].xfer_in_progress) {
        usb_dwc2_handle_events(dev);
    }
}
