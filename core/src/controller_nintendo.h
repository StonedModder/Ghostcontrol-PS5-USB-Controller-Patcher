#pragma once
#include <stdint.h>
#include "gc_types.h"

#ifndef NINTENDO_PARSER_TEST
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#endif

/* Handshake states */
#define HS_WAIT_81_01  0
#define HS_WAIT_81_02  1
#define HS_STREAMING   2

static inline int nintendo_report_length_valid(uint8_t report_id,
                                                uint32_t length)
{
    switch (report_id) {
    case 0x00:
    case 0x30: return length >= 12;
    case 0x3f: return length >= 9;
    case 0x21: return length >= 13; /* handler reads byte 12 */
    case 0x81: return length >= 2;  /* handler reads subtype byte 1 */
    default: return length >= 1;
    }
}

void nintendo_parse_0x30(const uint8_t *b, ScePadData *o);
void nintendo_parse_0x3f(const uint8_t *b, ScePadData *o);

#ifndef NINTENDO_PARSER_TEST
/* Send Nintendo subcommand on OUT ep (eps[1]).
 * seq: rolling counter, incremented per call. */
int  nintendo_send_subcmd(int fd, struct usb_fs_endpoint *eps,
                          uint8_t *seq, uint8_t subcmd,
                          const uint8_t *data, uint32_t data_len);

/* Handle one IN packet in the Nintendo state machine.
 * Returns 1 if pad was updated, 0 to skip, and -1 on required OUT failure.
 * hs_state and seq are in/out: updated by the function. */
int  nintendo_handle_packet(int fd, struct usb_fs_endpoint *eps,
                            const uint8_t *buf, uint32_t len,
                            int *hs_state, uint8_t *seq,
                            ScePadData *out_pad);
#endif
