#pragma once
#include <stdint.h>

#ifndef XBOX_PARSER_TEST
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#endif

#include "gc_types.h"

/*
 * Xbox One (VID=0x045E PID=0x02EA) — GIP protocol over USB
 *
 * Endpoints: IN=0x82 OUT=0x02 (interface 1, FS speed, maxpkt=64)
 *
 * GIP input report format (18 bytes total):
 *   [0]=0x20 cmd  [1]=seq  [2]=opts  [3]=0x0E (payload=14)
 *   [4..5] = buttons uint16 LE
 *   [6..7] = LT uint16 (0-1023)
 *   [8..9] = RT uint16 (0-1023)
 *   [10..11]= LX int16 (center=0)
 *   [12..13]= LY int16
 *   [14..15]= RX int16
 *   [16..17]= RY int16
 *
 * GIP wire button layout (from Linux xpad.c, xpad_process_packet_xboxone):
 *   b[4] GIP_BTN1: bit2=Menu(→Options) bit3=View(→Create)
 *                  bit4=A(→Cross) bit5=B(→Circle) bit6=X(→Square) bit7=Y(→Triangle)
 *   b[5] GIP_BTN2: bit0=DUp bit1=DDn bit2=DLt bit3=DRt
 *                  bit4=LB(→L1) bit5=RB(→R1) bit6=LS(→L3) bit7=RS(→R3)
 * NOTE: GIP wire bits differ from XInput wButtons constants (XInput driver remaps).
 * Guide arrives as separate GIP cmd=0x07 packet.
 */

#define XBOX_EP_IN   0x82
#define XBOX_EP_OUT  0x02

/* GIP command bytes */
#define GIP_CMD_ACK      0x01
#define GIP_CMD_ANNOUNCE 0x02
#define GIP_CMD_STATUS   0x03
#define GIP_CMD_RUMBLE   0x09
#define GIP_CMD_INPUT    0x20

#ifndef XBOX_PARSER_TEST
typedef int (*XboxKeepRunningFn)(void *context);
#endif

typedef struct {
    uint32_t input_count;
    uint8_t out_available;
    uint8_t output_sequence;
    /* Guide arrives in a separate GIP packet. Retain the last ordinary pad
     * report so a Guide edge changes only the PS bit. */
    ScePadData cached_pad;
    uint8_t cached_pad_valid;
    uint8_t guide_down;
    /* The INPUT report that ended the handshake, until the feed collects it.
     * GIP reports only on change, so dropping it would lose held controls. */
    uint8_t initial_report[64];
    uint32_t initial_report_length;
#ifndef XBOX_PARSER_TEST
    /* USB_FS may finish cancellation after the handshake call returns. Keep
     * all buffers and frame-length storage alive for the USB thread lifetime. */
    uint8_t transfer_input[64];
    uint8_t transfer_output[64];
    void *transfer_buffer[2];
    uint32_t transfer_length[2];
    uint32_t transfer_requested[2];
    XboxKeepRunningFn keep_running;
    void *keep_running_context;
#endif
} XboxControllerState;

typedef enum {
    XBOX_COMPLETION_OK = 1,
    XBOX_COMPLETION_STATUS_ERROR = -1,
    XBOX_COMPLETION_FRAME_ERROR = -2,
    XBOX_COMPLETION_LENGTH_ERROR = -3
} XboxCompletionValidation;

/* Pure validator shared by the target USB path and host-side tests. */
XboxCompletionValidation xbox_validate_completion(
    int is_input, int status, uint32_t actual_frames,
    uint32_t actual_length, uint32_t requested_length);

/* Parse GIP input report into ScePadData */
void xbox_parse_input(const uint8_t *buf, ScePadData *o);

/* Parse an ordinary input report and retain it for separate Guide packets. */
void xbox_parse_and_cache_input(const uint8_t *buf, ScePadData *out_pad,
                                XboxControllerState *state);

/* Parse the separate Guide command payload. Returns 1 for a valid packet. */
int xbox_parse_guide(const uint8_t *buf, uint32_t len,
                     ScePadData *out_pad, XboxControllerState *state);
int xbox_requires_powera_rumble_init(uint16_t vid, uint16_t pid);

/* Copy out the INPUT report consumed by the handshake, once. Returns its
 * length, or 0 when there is none or it does not fit in capacity. */
uint32_t xbox_take_initial_report(XboxControllerState *state,
                                  uint8_t *report, uint32_t capacity);

#ifndef XBOX_PARSER_TEST
/* GIP handshake: catch ANNOUNCE → ACK → POWER.
 * Call immediately after opening IN+OUT endpoints.
 * eps[0]=IN eps[1]=OUT. Returns 1 once the controller announces or streams,
 * 0 on its bounded timeout, and a negative errno on cancellation/failure. */
int xbox_gip_handshake(int fd, struct usb_fs_endpoint *eps,
                       uint16_t vid, uint16_t pid,
                       XboxControllerState *state,
                       XboxKeepRunningFn keep_running, void *context);

/* Handle one IN packet. Returns 1 if pad updated, 0 to skip, -1 to reinit. */
int  xbox_handle_packet(int fd, struct usb_fs_endpoint *eps,
                        const uint8_t *buf, uint32_t len,
                        ScePadData *out_pad, XboxControllerState *state);
#endif
