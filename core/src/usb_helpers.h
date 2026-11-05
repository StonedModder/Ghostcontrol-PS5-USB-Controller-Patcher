#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

/* Synchronous endpoint-1 send. The caller must serialize access to the ugen
 * session and must not leave another endpoint transfer outstanding: FreeBSD's
 * USB_FS_COMPLETE is a session-wide FIFO, not an endpoint-selective wait.
 *
 * The helper owns a copy of the packet until endpoint 1 is completed or
 * synchronously cancelled, so caller stack buffers may be used safely. */
int  usb_send_out(int fd, struct usb_fs_endpoint *ep,
                  const uint8_t *data, uint32_t len, const char *tag);
int  usb_send_cmd(int fd, struct usb_fs_endpoint *ep, uint8_t a, uint8_t b);
/* Free a quarantined helper-owned OUT buffer only after USB_FS_UNINIT (or an
 * equivalent synchronous endpoint teardown) has succeeded. */
void usb_out_release_after_uninit(struct usb_fs_endpoint *ep);
