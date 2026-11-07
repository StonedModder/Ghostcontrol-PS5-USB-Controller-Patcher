#pragma once
#include <stdint.h>

/*
 * Xbox One / Series (GIP) controllers — Microsoft and licensed third-party
 * pads (PowerA, PDP, Hori, Razer...) — all expose an interface with
 * class 0xFF, subclass 0x47, protocol 0xD0. Linux xpad matches on the same
 * signature (XPAD_XBOXONE_VENDOR). Matching on it lets any such pad use the
 * Xbox path without listing every vendor ID.
 *
 * d/len: a raw configuration descriptor (config + interface + endpoint ...).
 * Returns 1 if any interface carries the GIP signature, else 0.
 */
static inline int gip_cdesc_has_gip_iface(const uint8_t *d, int len) {
    int i = 0;
    while (i + 2 <= len) {
        int bl = d[i], type = d[i + 1];
        if (bl < 2 || i + bl > len) break;          /* malformed: stop */
        if (type == 0x04 && bl >= 9 &&              /* INTERFACE descriptor */
            d[i + 5] == 0xFF && d[i + 6] == 0x47 && d[i + 7] == 0xD0)
            return 1;
        i += bl;
    }
    return 0;
}
