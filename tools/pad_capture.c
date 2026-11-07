/* pad_capture.c — map any USB HID gamepad's input report on Linux
 *
 * Reads interrupt IN reports straight from the device (libusb), learns
 * which bytes jitter while idle (analog axes), then asks you to press each
 * control and prints exactly which byte / bit / nibble / axis changed.
 * Results are also written to pad_capture.txt.
 *
 * Build: gcc -O2 -o tools/pad_capture tools/pad_capture.c -lusb-1.0
 * Run:   sudo ./tools/pad_capture VID:PID [profile] [ep_in]
 *        profile = n64 | gamecube | generic (default generic)
 *        ep_in   = IN endpoint, default 0x81
 */
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAXR 64

static libusb_device_handle *h;
static FILE *out;
static unsigned char ep_in = 0x81;
static int rlen = 0;                       /* report length seen while idle */
static uint8_t idle[MAXR], imin[MAXR], imax[MAXR];

#define P(...) do { printf(__VA_ARGS__); if (out) fprintf(out, __VA_ARGS__); } while (0)

static int rd(uint8_t *r, int timeout_ms) {
    int n = 0;
    int rc = libusb_interrupt_transfer(h, ep_in, r, MAXR, &n, timeout_ms);
    return rc == 0 ? n : 0;
}

/* A byte has "really" changed if it is outside its idle jitter range by a
 * margin (analog bytes) or differs at all (bytes that never moved at idle). */
static int changed(const uint8_t *r, int i) {
    if (imin[i] == imax[i]) return r[i] != idle[i];
    return r[i] + 40 < imin[i] || r[i] > imax[i] + 40;
}

static int any_change(const uint8_t *r) {
    for (int i = 0; i < rlen; i++) if (changed(r, i)) return 1;
    return 0;
}

static void calibrate(void) {
    uint8_t r[MAXR];
    int got = 0;
    for (int k = 0; k < 200 && got < 60; k++) {       /* ~0.6-2s of idle */
        int n = rd(r, 50);
        if (n <= 0) continue;
        if (!got) { rlen = n; memcpy(imin, r, MAXR); memcpy(imax, r, MAXR); }
        for (int i = 0; i < n; i++) {
            if (r[i] < imin[i]) imin[i] = r[i];
            if (r[i] > imax[i]) imax[i] = r[i];
        }
        memcpy(idle, r, MAXR);
        got++;
    }
}

static void describe(const char *name, const uint8_t *r) {
    for (int i = 0; i < rlen; i++) {
        if (!changed(r, i)) continue;
        uint8_t x = r[i] ^ idle[i];
        if (imin[i] != imax[i] || __builtin_popcount(x) > 2) {
            if ((x & 0xF0) == 0 && (idle[i] & 0x0F) >= 8)
                P("%-16s -> byte %d low nibble (hat) %x -> %x\n", name, i, idle[i] & 0x0F, r[i] & 0x0F);
            else
                P("%-16s -> byte %d axis %02x -> %02x\n", name, i, idle[i], r[i]);
        } else if ((x & 0xF0) == 0 && (idle[i] & 0x0F) >= 8 && (r[i] & 0x0F) < 8) {
            P("%-16s -> byte %d low nibble (hat) %x -> %x\n", name, i, idle[i] & 0x0F, r[i] & 0x0F);
        } else {
            for (int b = 0; b < 8; b++)
                if (x & (1 << b))
                    P("%-16s -> byte %d bit %d (mask 0x%02x) %s\n", name, i, b, 1 << b,
                      (r[i] & (1 << b)) ? "set" : "cleared");
        }
    }
}

static void prompt(const char *name) {
    uint8_t r[MAXR], last[MAXR];
    printf("\n>> %s, then HOLD it...\n", name);
    fflush(stdout);
    int stable = 0;
    for (;;) {                       /* wait for a change that holds for 3 reports */
        if (rd(r, 1000) <= 0) continue;
        if (!any_change(r)) { stable = 0; continue; }
        if (stable && memcmp(r, last, rlen) == 0) stable++;
        else stable = 1;
        memcpy(last, r, MAXR);
        if (stable >= 3) break;
    }
    describe(name, r);
    printf("   release it...\n");
    for (int k = 0; k < 600; k++) {  /* back to idle (max ~6s) */
        if (rd(r, 20) > 0 && !any_change(r)) break;
    }
}

static const char *P_N64[] = {
    "Press A", "Press B", "Press Z", "Press L", "Press R", "Press Start",
    "Press C-UP", "Press C-DOWN", "Press C-LEFT", "Press C-RIGHT",
    "Press D-pad UP", "Press D-pad RIGHT",
    "Push stick RIGHT", "Push stick UP", NULL };
static const char *P_GC[] = {
    "Press A", "Press B", "Press X", "Press Y", "Press Z", "Press L (full)",
    "Press R (full)", "Press Start", "Press D-pad UP", "Press D-pad RIGHT",
    "Push main stick RIGHT", "Push main stick UP",
    "Push C-stick RIGHT", "Push C-stick UP", NULL };
static const char *P_GEN[] = {
    "Press the bottom face button", "Press the right face button",
    "Press the left face button", "Press the top face button",
    "Press left shoulder", "Press right shoulder",
    "Press left trigger", "Press right trigger",
    "Press Select/Back/-", "Press Start/+", "Press Home/Guide",
    "Click left stick", "Click right stick",
    "Press D-pad UP", "Press D-pad RIGHT",
    "Push left stick RIGHT", "Push left stick UP",
    "Push right stick RIGHT", "Push right stick UP", NULL };

int main(int argc, char **argv) {
    unsigned vid, pid;
    if (argc < 2 || sscanf(argv[1], "%x:%x", &vid, &pid) != 2) {
        fprintf(stderr, "usage: sudo %s VID:PID [n64|gamecube|generic] [ep_in]\n", argv[0]);
        return 1;
    }
    const char **list = P_GEN;
    if (argc > 2 && !strcmp(argv[2], "n64")) list = P_N64;
    if (argc > 2 && !strcmp(argv[2], "gamecube")) list = P_GC;
    if (argc > 3) ep_in = (unsigned char)strtoul(argv[3], NULL, 0);

    if (libusb_init(NULL) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    h = libusb_open_device_with_vid_pid(NULL, vid, pid);
    if (!h) { fprintf(stderr, "%04x:%04x not found or no permission (run with sudo)\n", vid, pid); return 1; }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0) != 0) { fprintf(stderr, "claim interface 0 failed\n"); return 1; }
    out = fopen("pad_capture.txt", "w");
    if (out) setvbuf(out, NULL, _IOLBF, 0);   /* line-buffered: survives Ctrl+C */

    printf("Leave the controller untouched for 2 seconds...\n");
    calibrate();
    if (!rlen) { fprintf(stderr, "no reports from EP 0x%02x\n", ep_in); return 1; }
    P("Device %04x:%04x  EP 0x%02x  report length %d\nIdle:", vid, pid, ep_in, rlen);
    for (int i = 0; i < rlen; i++) P(" %02x", idle[i]);
    P("\nBytes that jitter at idle:");
    for (int i = 0; i < rlen; i++) if (imin[i] != imax[i]) P(" %d(%02x-%02x)", i, imin[i], imax[i]);
    P("\n");

    for (int i = 0; list[i]; i++) prompt(list[i]);
    printf("\nPress any other buttons (Home, Capture, ZR...) one at a time; Ctrl+C when done.\n");
    for (;;) prompt("Press extra button");
}
