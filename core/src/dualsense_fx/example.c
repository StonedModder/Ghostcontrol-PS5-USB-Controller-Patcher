/* example.c — what using dualsense_fx.h looks like, and a self-test.
 *
 * Build:  cc -std=c99 -Wall -Wextra -o example example.c
 * Run:    ./example
 *
 * The library never touches a device, so this example prints what it WOULD
 * send. Swap the printf for your own write and you are done.
 */
#include <stdio.h>
#include "dualsense_fx.h"

static void show(const char *name, ds_fx_cfg fx, int total_ms, int every_ms) {
    int t;
    printf("\n%s  (period %d ms, step %d ms)\n", name, fx.period_ms, fx.step_ms);
    for (t = 0; t < total_ms; t += every_ms) {
        ds_rgb c = ds_fx_colour(&fx, (unsigned long)t);
        printf("  t=%5d  rgb(%3d,%3d,%3d)  ", t, c.r, c.g, c.b);
        {   /* a crude bar so the shape is visible in a terminal */
            int i, lum = (c.r * 77 + c.g * 151 + c.b * 28) >> 8;
            for (i = 0; i < lum / 8; i++) putchar('#');
        }
        putchar('\n');
    }
}

int main(void) {
    ds_fx_cfg fx = ds_fx_default();

    fx.effect = DS_FX_RAINBOW; fx.period_ms = 3000;
    show("RAINBOW", fx, 3000, 250);

    fx.effect = DS_FX_BREATHE; fx.period_ms = 2000;
    fx.base.r = 0; fx.base.g = 120; fx.base.b = 255;
    show("BREATHE", fx, 2000, 200);

    /* Sampled every 25 ms: the flashes are 55 ms wide, so a coarser interval
       walks straight past them and the pattern looks wrong when it is not. */
    fx.effect = DS_FX_POLICE;  fx.period_ms = 1000;
    show("POLICE", fx, 1000, 25);

    /* --- self-test: the properties that matter --- */
    {
        int t, fails = 0;
        ds_fx_cfg r = ds_fx_default();
        r.effect = DS_FX_RAINBOW; r.period_ms = 3000; r.step_ms = 33;

        /* every colour in range, and never black (rainbow is full value) */
        for (t = 0; t < 6000; t++) {
            ds_rgb c = ds_fx_colour(&r, (unsigned long)t);
            if (c.r < 0 || c.r > 255 || c.g < 0 || c.g > 255 || c.b < 0 || c.b > 255) fails++;
            if (c.r + c.g + c.b < 200) fails++;   /* always lit */
        }
        /* the step really does hold a colour */
        {
            ds_rgb a = ds_fx_colour(&r, 1000), b = ds_fx_colour(&r, 1010);
            if (a.r != b.r || a.g != b.g || a.b != b.b) fails++;
        }
        /* and it really does move between steps */
        {
            ds_rgb a = ds_fx_colour(&r, 1000), b = ds_fx_colour(&r, 1200);
            if (a.r == b.r && a.g == b.g && a.b == b.b) fails++;
        }
        /* It wraps cleanly - checked with quantisation OFF. With step_ms set,
           t=3000 snaps back to 2970, which is the END of the cycle and not the
           start, so comparing it against t=0 tests the quantiser rather than
           the wrap. That is a bug in the test, and it failed exactly that way
           the first time this ran. */
        {
            ds_fx_cfg q = r; q.step_ms = 0;
            ds_rgb a = ds_fx_colour(&q, 0), b = ds_fx_colour(&q, 3000);
            if (a.r != b.r || a.g != b.g || a.b != b.b) fails++;
        }
        /* Police really does flash twice per colour, then rest. Counted over a
           whole period at 1 ms resolution rather than eyeballed. */
        {
            ds_fx_cfg p = ds_fx_default();
            int i, redruns = 0, bluruns = 0, wasr = 0, wasb = 0;
            p.effect = DS_FX_POLICE; p.period_ms = 1000; p.step_ms = 0;
            for (i = 0; i < 1000; i++) {
                ds_rgb c = ds_fx_colour(&p, (unsigned long)i);
                int isr = (c.r > 0), isb = (c.b > 0);
                if (isr && !wasr) redruns++;
                if (isb && !wasb) bluruns++;
                wasr = isr; wasb = isb;
            }
            if (redruns != 2 || bluruns != 2) {
                printf("  police: expected 2 red + 2 blue flashes, got %d + %d\n",
                       redruns, bluruns);
                fails++;
            }
        }
        /* report layout */
        {
            unsigned char rpt[DS_USB_REPORT_LEN];
            ds_rgb c; c.r = 10; c.g = 20; c.b = 30;
            ds_fx_fill_usb_report(rpt, c);
            if (rpt[0] != 0x02 || rpt[1] != 0x02 || rpt[2] != 0x04) fails++;
            if (rpt[45] != 10 || rpt[46] != 20 || rpt[47] != 30) fails++;
        }
        printf("\nself-test: %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
        return fails ? 1 : 0;
    }
}
