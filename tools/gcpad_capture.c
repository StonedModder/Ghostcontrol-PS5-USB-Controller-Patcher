/* gcpad_capture.c — map a DragonRise (0079:0006) GameCube-style pad on Linux
 *
 * Dumps the HID report descriptor, then asks you to press each GameCube
 * control and reports which HID button number / axis byte it uses, so
 * GCPAD_BTN_MAP and GCPAD_CSTICK_* in payload/controller_gcpad.c can be
 * set to match your pad. Results are also written to gcpad_capture.txt.
 *
 * Build: gcc -O2 -o gcpad_capture tools/gcpad_capture.c -lusb-1.0
 * Run:   sudo ./gcpad_capture
 */
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VID 0x0079
#define PID 0x0006
#define EP_IN 0x81

static libusb_device_handle *h;
static FILE *out;

#define P(...) do { printf(__VA_ARGS__); if (out) fprintf(out, __VA_ARGS__); } while (0)

static int read_report(uint8_t r[8], int timeout_ms) {
    int n = 0;
    int rc = libusb_interrupt_transfer(h, EP_IN, r, 8, &n, timeout_ms);
    return (rc == 0 && n >= 7) ? n : 0;
}

/* Return a current report. The pad streams every 10ms even when idle,
 * so read a bounded number (~100ms) rather than draining until silence. */
static void latest(uint8_t r[8]) {
    uint8_t t[8];
    for (int i = 0; i < 10; i++)
        if (read_report(t, 30)) memcpy(r, t, 8);
}

static int buttons(const uint8_t r[8]) { return (r[5] >> 4) | (r[6] << 4); }

static void prompt(const char *name, int is_axis) {
    uint8_t idle[8] = {0}, r[8];
    latest(idle);
    printf("\n>> %s %s, then HOLD it...\n", is_axis ? "Push" : "Press", name);
    fflush(stdout);
    for (;;) {
        if (!read_report(r, 1000)) continue;
        int db = buttons(r) & ~buttons(idle);
        int hat = r[5] & 0x0F, ihat = idle[5] & 0x0F;
        if (!is_axis && db) {
            for (int i = 0; i < 12; i++)
                if (db & (1 << i)) P("%-14s -> HID button %d (map index %d)\n", name, i + 1, i);
            break;
        }
        if (!is_axis && hat != ihat && hat < 8) {
            P("%-14s -> hat value %d\n", name, hat);
            break;
        }
        if (is_axis) {
            int best = -1, bd = 0;
            for (int i = 0; i < 5; i++) {
                int d = abs((int)r[i] - (int)idle[i]);
                if (d > bd) { bd = d; best = i; }
            }
            if (bd > 60) {
                P("%-14s -> axis byte %d (%02x -> %02x)\n", name, best, idle[best], r[best]);
                break;
            }
        }
    }
    printf("   release it...\n");
    for (int k = 0; k < 200; k++) {  /* wait for return to idle (max ~6s) */
        if (read_report(r, 30) && buttons(r) == buttons(idle) &&
            (r[5] & 0x0F) == (idle[5] & 0x0F) &&
            abs((int)r[0] - idle[0]) < 30 && abs((int)r[1] - idle[1]) < 30 &&
            abs((int)r[3] - idle[3]) < 30 && abs((int)r[4] - idle[4]) < 30) break;
    }
}

int main(void) {
    if (libusb_init(NULL) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    h = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!h) { fprintf(stderr, "0079:0006 not found or no permission (run with sudo)\n"); return 1; }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0) != 0) { fprintf(stderr, "claim interface 0 failed\n"); return 1; }
    out = fopen("gcpad_capture.txt", "w");

    uint8_t d[512];
    int n = libusb_control_transfer(h, 0x81, LIBUSB_REQUEST_GET_DESCRIPTOR,
                                    (LIBUSB_DT_REPORT << 8), 0, d, sizeof(d), 1000);
    P("HID report descriptor (%d bytes):\n", n);
    for (int i = 0; i < n; i++) P("%02x%s", d[i], (i % 16 == 15) ? "\n" : " ");
    P("\n");

    uint8_t r[8] = {0};
    latest(r);
    P("Idle report: %02x %02x %02x %02x %02x %02x %02x %02x\n",
      r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);

    static const char *btns[] = { "A", "B", "X", "Y", "Z", "L (full press)",
                                  "R (full press)", "Start", "D-pad UP", "D-pad RIGHT" };
    for (unsigned i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) prompt(btns[i], 0);
    prompt("main stick RIGHT", 1);
    prompt("main stick UP", 1);
    prompt("C-stick RIGHT", 1);
    prompt("C-stick UP", 1);

    printf("\nAny extra buttons on the pad (Select/Home/Z-L)? Press each one now; Ctrl+C when done.\n");
    for (;;) prompt("extra button", 0);
}
