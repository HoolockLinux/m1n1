/* SPDX-License-Identifier: MIT */

#ifndef USB_DWC2_H
#define USB_DWC2_H

#include "types.h"
#include "usb_types.h"

#define EP0_MAX_PACKET_SIZE 64

typedef struct dwc2_dev dwc2_dev_t;

dwc2_dev_t *usb_dwc2_init(u64 regs);
void usb_dwc2_shutdown(dwc2_dev_t *dev);

void usb_dwc2_handle_events(dwc2_dev_t *dev);

ssize_t usb_dwc2_can_read(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe);
bool usb_dwc2_can_write(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe);

u8 usb_dwc2_getbyte(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe);
void usb_dwc2_putbyte(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte);

size_t usb_dwc2_read(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count);
size_t usb_dwc2_write(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
size_t usb_dwc2_queue(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
void usb_dwc2_flush(dwc2_dev_t *dev, cdc_acm_pipe_id_t pipe);
void usb_dwc2_handle_interrupts(dwc2_dev_t *dev);
#endif
