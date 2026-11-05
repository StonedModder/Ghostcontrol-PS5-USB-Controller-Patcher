#pragma once
#include <stdint.h>
#ifndef MAMBA_PARSER_TEST
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#else
struct usb_fs_endpoint;
#endif
#include "gc_types.h"

/* Identity metadata only; no initialization or hardware-readiness claim. */
#define GC_LIEFENG2_XINPUT_VID 0xe2b7u
#define GC_LIEFENG2_XINPUT_PID 0x122cu

#define MAMBA_XINPUT_VID        0x045eu
#define MAMBA_XINPUT_PID        0x028eu
#define MAMBA_DONGLE_VID        0x1a34u
#define MAMBA_DONGLE_PID        0xf517u
#define MAMBA_XINPUT_SUBCLASS   0x5du
#define MAMBA_XINPUT_PROTOCOL   0x01u

#define GC8BITDO_2C_XINPUT_VID     0x2dc8u
#define GC8BITDO_2C_XINPUT_PID     0x310au
#define GC8BITDO_2C_XINPUT_EP_IN   0x84u
#define GC8BITDO_2C_XINPUT_EP_OUT  0x05u

#define MAMBA_XINPUT_EP_IN      0x81u
#define MAMBA_XINPUT_EP_OUT     0x02u
#define MAMBA_XINPUT_EP_OUT_ALT 0x01u

int mamba_is_xinput_vidpid(uint16_t vid, uint16_t pid);
int mamba_is_xinput_interface(uint8_t subclass, uint8_t protocol);
const char *mamba_name(uint16_t vid, uint16_t pid);
void mamba_xinput_parse_input(const uint8_t *buf, ScePadData *out_pad);
#ifndef MAMBA_PARSER_TEST
int mamba_xinput_send_enable(int fd, struct usb_fs_endpoint *eps);
#endif
int mamba_xinput_handle_packet(int fd, struct usb_fs_endpoint *eps,
                               const uint8_t *buf, uint32_t len,
                               ScePadData *out_pad);
