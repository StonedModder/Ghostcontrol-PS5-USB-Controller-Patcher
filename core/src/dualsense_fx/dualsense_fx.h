/* dualsense_fx.h — DualSense lightbar effects as a drop-in C library.
 * Header-only, no dependencies, no allocation, no libm, no I/O.
 * Public domain / CC0 — use it however you like.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS
 *
 *   ds_fx_colour() is a PURE FUNCTION of elapsed milliseconds. Give it an
 *   effect and how long the effect has been running; it hands back an RGB
 *   triple. It never touches the device, so it does not care whether you reach
 *   your controller over USB HID, Bluetooth, hidapi, SDL, or a socket to
 *   something else entirely.
 *
 *   That also makes it trivially testable and thread-safe: no globals, no
 *   state, same input gives the same output.
 *
 *   Effects: SOLID, POLICE, BREATHE, RAINBOW, STROBE.
 *
 * ---------------------------------------------------------------------------
 * HOW TO USE IT
 *
 *     #include "dualsense_fx.h"
 *
 *     ds_fx_cfg fx = ds_fx_default();
 *     fx.effect    = DS_FX_RAINBOW;
 *     fx.period_ms = 3000;                  // one full hue sweep
 *
 *     // in your write loop, at whatever rate you already write reports:
 *     ds_rgb c = ds_fx_colour(&fx, now_ms() - started_ms);
 *     my_set_lightbar(c.r, c.g, c.b);
 *
 *   If you are talking to a DualSense over USB HID and want the report laid
 *   out for you, ds_fx_fill_usb_report() at the bottom does that too - but it
 *   is entirely optional and the effects work without it.
 *
 * ---------------------------------------------------------------------------
 * TWO THINGS THAT ARE NOT OBVIOUS, both learned the hard way on hardware
 *
 *   1. QUANTISE THE COLOUR, NOT THE WRITES.  step_ms exists for a reason.
 *      Writes have to stay fast to hold the endpoint - 500 Hz in the original
 *      - but if the COLOUR changes on every one of those writes, the lightbar
 *      shimmers badly. The cause is frames from another writer (the system's
 *      own HID stack) landing between yours: a stray frame is nearly invisible
 *      when your neighbouring frames are identical, and very visible when
 *      every one differs. So compute a colour, then send that same colour for
 *      the whole step. 33 ms (~30 colour updates/sec) looks smooth and is
 *      stable. Set step_ms = 0 only if you know you are the sole writer.
 *
 *   2. PHASE FROM ELAPSED TIME, NOT FROM A FRAME COUNTER.  Everything here is
 *      driven by milliseconds, so changing your write rate does not change how
 *      fast the effect runs. Deriving phase from a frame count couples the two
 *      and every rate change silently retimes every effect.
 *
 * Integer maths throughout - no float, no libm - so it drops into freestanding
 * and embedded builds unchanged.
 * ---------------------------------------------------------------------------
 */
#ifndef DUALSENSE_FX_H
#define DUALSENSE_FX_H

#ifdef __cplusplus
extern "C" {
#endif

#define DS_FX_SOLID    0
#define DS_FX_POLICE   1
#define DS_FX_BREATHE  2
#define DS_FX_RAINBOW  3
#define DS_FX_STROBE   4

typedef struct { int r, g, b; } ds_rgb;

typedef struct {
    int    effect;      /* DS_FX_*                                          */
    int    period_ms;   /* one full cycle; must be > 0                      */
    int    step_ms;     /* hold each colour this long (see note 1); 0 = off */
    ds_rgb base;        /* colour for SOLID, BREATHE and STROBE             */
} ds_fx_cfg;

static ds_fx_cfg ds_fx_default(void) {
    ds_fx_cfg c;
    c.effect    = DS_FX_SOLID;
    c.period_ms = 1000;
    c.step_ms   = 33;
    c.base.r = 0; c.base.g = 0; c.base.b = 255;
    return c;
}

/* Hue 0..1535 (six 60-degree segments) at full saturation and value.
   Exposed because a hue ramp is useful on its own. */
static ds_rgb ds_fx_hue1536(int h) {
    ds_rgb c;
    if (h < 0) h = 0;
    h %= 1536;
    {
        int seg = h / 256, f = h % 256;
        switch (seg) {
            case 0:  c.r = 255;     c.g = f;       c.b = 0;       break;
            case 1:  c.r = 255 - f; c.g = 255;     c.b = 0;       break;
            case 2:  c.r = 0;       c.g = 255;     c.b = f;       break;
            case 3:  c.r = 0;       c.g = 255 - f; c.b = 255;     break;
            case 4:  c.r = f;       c.g = 0;       c.b = 255;     break;
            default: c.r = 255;     c.g = 0;       c.b = 255 - f; break;
        }
    }
    return c;
}

/* The whole library. Pure: same elapsed_ms always gives the same colour. */
static ds_rgb ds_fx_colour(const ds_fx_cfg *cfg, unsigned long elapsed_ms) {
    ds_rgb c;
    int period, el;

    c.r = 0; c.g = 0; c.b = 0;
    if (!cfg) return c;

    c = cfg->base;
    period = cfg->period_ms > 0 ? cfg->period_ms : 1000;

    /* See note 1: hold each colour for a step so the bar does not shimmer. */
    el = (int)(elapsed_ms & 0x7FFFFFFFUL);
    if (cfg->step_ms > 1) el -= el % cfg->step_ms;

    switch (cfg->effect) {

    case DS_FX_RAINBOW: {
        c = ds_fx_hue1536(((el % period) * 1536) / period);
        break;
    }

    case DS_FX_BREATHE: {
        /* Raised cosine, then squared for gamma so the dim end lingers the way
           a real breath does. A linear ramp reads as a triangle wave, not a
           breath, because perceived brightness is not linear in drive. */
        int ph  = el % period;
        int hf  = period / 2; if (hf < 1) hf = 1;
        int tri = (ph < hf) ? ph : (period - ph);
        int x   = (tri * 1000) / hf;                    /* 0..1000        */
        int sm  = (x * x * (3000 - 2 * x)) / 1000000;   /* smoothstep     */
        sm = (sm * sm) / 1000;                          /* gamma          */
        c.r = (c.r * sm) / 1000;
        c.g = (c.g * sm) / 1000;
        c.b = (c.b * sm) / 1000;
        break;
    }

    case DS_FX_POLICE: {
        /* US dual-flash: two quick red strobes, pause, two blue, pause. */
        int t    = el % period;
        int half = period / 2;
        int red  = (t < half);
        int u    = red ? t : (t - half);
        int on   = (u < 55) ? 1 : (u < 110) ? 0 : (u < 165) ? 1 : 0;
        if (!on)      { c.r = 0;   c.g = 0; c.b = 0;   }
        else if (red) { c.r = 255; c.g = 0; c.b = 0;   }
        else          { c.r = 0;   c.g = 0; c.b = 255; }
        break;
    }

    case DS_FX_STROBE: {
        /* Lit for an eighth of the period. Short on-time is what makes it read
           as a strobe rather than a blink. */
        if ((el % period) >= (period / 8)) { c.r = 0; c.g = 0; c.b = 0; }
        break;
    }

    case DS_FX_SOLID:
    default:
        break;
    }

    if (c.r < 0) c.r = 0; if (c.r > 255) c.r = 255;
    if (c.g < 0) c.g = 0; if (c.g > 255) c.g = 255;
    if (c.b < 0) c.b = 0; if (c.b > 255) c.b = 255;
    return c;
}

/* ---------------------------------------------------------------------------
 * OPTIONAL: lay out a DualSense USB HID output report.
 *
 * ⛔ THE REPORT IS 63 BYTES, NOT 48. Linux's dualsense_output_report_usb is
 *    1 (report id) + 47 (common) + 15 (reserved). A 48-byte write is ACCEPTED
 *    and returns 48, which is exactly why sending short reports goes unnoticed
 *    for a long time - the bytes you never send are whatever the controller
 *    had there before.
 *
 * ⛔ THE TWO FLAG BYTES ARE NOT OPTIONAL. rpt[1] = 0x02 says "the lightbar
 *    field is valid" and rpt[2] = 0x04 says "apply it". Without them the
 *    colour bytes are ignored and the write still succeeds.
 *
 * Bluetooth uses a different report id, a different offset and a CRC32 -
 * deliberately not attempted here.
 * --------------------------------------------------------------------------- */
#define DS_USB_REPORT_LEN 63

static void ds_fx_fill_usb_report(unsigned char *rpt, ds_rgb c) {
    int i;
    if (!rpt) return;
    for (i = 0; i < DS_USB_REPORT_LEN; i++) rpt[i] = 0;
    rpt[0]  = 0x02;              /* output report id                        */
    rpt[1]  = 0x02;              /* valid flag: lightbar                    */
    rpt[2]  = 0x04;              /* apply flag                              */
    rpt[45] = (unsigned char)c.r;
    rpt[46] = (unsigned char)c.g;
    rpt[47] = (unsigned char)c.b;
}

#ifdef __cplusplus
}
#endif
#endif /* DUALSENSE_FX_H */
