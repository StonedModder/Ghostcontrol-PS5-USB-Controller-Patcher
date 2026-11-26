/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host tests for the virtual-DualSense rumble read-back parser
 * (core/src/bt/vpad_rumble_rs.h). They exercise the production decode with no
 * console or SDK: real PS5 hardware captures plus the lightbar/flag/split-motor
 * and sustained-effect cases a maintainer audit called out. ASan/UBSan on.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "vpad_rumble_rs.h"

static int failures;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failures++;
}

/* Decode a 256-byte buffer and compare both motors. */
static int decode_eq(const uint8_t *buf, size_t len,
                     int want_strong, int want_weak)
{
    uint8_t s = 0xAA, w = 0xAA;
    if (!vpad_rs_decode(buf, len, &s, &w)) return 0;
    return s == (uint8_t)want_strong && w == (uint8_t)want_weak;
}

/* ---- real hardware captures (GetRemoteSetting, DualShock 4) ---------------- */
/* Bytes 0..27 of each capture; the rest of the 256-byte buffer is zero. */
#define CAP(name, ...) \
    static const uint8_t name[256] = { __VA_ARGS__ }

CAP(cap_idle,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x05,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x00);
CAP(cap_strong_only,                                  /* record 0 active @ 20 */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x25,0x14,0x00,0x03,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x00);
CAP(cap_both_20,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x25,0x14,0x00,0x03,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x25,0x14,0x00,0x03);
CAP(cap_both_max,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x21,0xff,0x03,0xff,
    0xff,0xff,0x3f,0x00,0x00,0x00,0x00,0x21,0xff,0x03,0xff,0xff,0xff,0x3f);

static void test_hardware_captures(void)
{
    check("hw: idle buffer -> 0,0",          decode_eq(cap_idle, 256, 0, 0));
    check("hw: strong-only (hdr 0x25) -> 20,0",
          decode_eq(cap_strong_only, 256, 20, 0));
    check("hw: both motors @ 20 -> 20,20",    decode_eq(cap_both_20, 256, 20, 20));
    check("hw: both motors @ max -> 255,255", decode_eq(cap_both_max, 256, 255, 255));
}

/* ---- audit cases: things that must NOT read as rumble ---------------------- */
static void test_no_false_rumble(void)
{
    uint8_t buf[256];

    /* Lightbar-only 00 00 40 with motors idle: a 0x40 outside the motor value
     * bytes must stay invisible. (audit: "-> level 64, want 0") */
    memset(buf, 0, sizeof buf);
    buf[VPAD_RS_REC0_HDR] = 0x05;   /* idle */
    buf[VPAD_RS_REC1_HDR] = 0x05;   /* idle */
    buf[13] = 0x00; buf[14] = 0x00; buf[15] = 0x40;
    check("lightbar 00 00 40, motors idle -> 0,0", decode_eq(buf, sizeof buf, 0, 0));

    /* Lightbar-only FF 00 00. (audit: "-> 255, want 0") */
    memset(buf, 0, sizeof buf);
    buf[VPAD_RS_REC0_HDR] = 0x05;
    buf[VPAD_RS_REC1_HDR] = 0x05;
    buf[13] = 0xFF; buf[14] = 0x00; buf[15] = 0x00;
    check("lightbar FF 00 00, motors idle -> 0,0", decode_eq(buf, sizeof buf, 0, 0));

    /* Flag byte [1]=3 with motors idle. (audit: "-> 3, want 0") */
    memset(buf, 0, sizeof buf);
    buf[1] = 0x03;
    buf[VPAD_RS_REC0_HDR] = 0x05;
    buf[VPAD_RS_REC1_HDR] = 0x05;
    check("flag byte [1]=3, motors idle -> 0,0", decode_eq(buf, sizeof buf, 0, 0));

    /* Idle record carrying a stale value byte: active bit clear must win. */
    memset(buf, 0, sizeof buf);
    buf[VPAD_RS_REC0_HDR] = 0x05; buf[VPAD_RS_REC0_VAL] = 0xFF;  /* stale 255 */
    buf[VPAD_RS_REC1_HDR] = 0x05; buf[VPAD_RS_REC1_VAL] = 0x7F;
    check("idle records with stale value bytes -> 0,0", decode_eq(buf, sizeof buf, 0, 0));
}

/* ---- audit case: the two motors must be independent ------------------------ */
static void test_strong_weak_split(void)
{
    uint8_t buf[256];

    /* large=0, small=200: strong must be 0, not 200. (audit: "-> strong 200") */
    memset(buf, 0, sizeof buf);
    buf[VPAD_RS_REC0_HDR] = 0x05;                               /* strong idle */
    buf[VPAD_RS_REC1_HDR] = 0x25; buf[VPAD_RS_REC1_VAL] = 200;  /* weak active  */
    check("strong=0 weak=200 kept separate -> 0,200",
          decode_eq(buf, sizeof buf, 0, 200));

    /* Mirror: strong active, weak idle. */
    memset(buf, 0, sizeof buf);
    buf[VPAD_RS_REC0_HDR] = 0x25; buf[VPAD_RS_REC0_VAL] = 150;
    buf[VPAD_RS_REC1_HDR] = 0x05;
    check("strong=150 weak=0 kept separate -> 150,0",
          decode_eq(buf, sizeof buf, 150, 0));
}

/* ---- audit case: a sustained effect must not be cut off -------------------- */
static void test_sustained_not_cut(void)
{
    /* The decode is stateless and timeout-free: polling the same active buffer
     * for a simulated second (60 Hz) must return 20 every time, never drop to
     * 0. (audit: "sustained 20 for 1000 ms -> 0, want 20") */
    int i, held = 1;
    for (i = 0; i < 60; i++)
        if (!decode_eq(cap_both_20, 256, 20, 20)) { held = 0; break; }
    check("sustained level 20 over 60 polls stays 20,20", held);

    /* A release mid-stream (active bit clears) is seen immediately, even while
     * other bytes animate. (audit: lightbar animation must not mask a release) */
    {
        uint8_t buf[256];
        memcpy(buf, cap_both_20, sizeof buf);
        buf[13] = 0x11; buf[15] = 0x40;          /* animate unrelated bytes */
        if (!decode_eq(buf, sizeof buf, 20, 20)) held = 0;
        buf[VPAD_RS_REC0_HDR] = 0x05;            /* strong released */
        buf[VPAD_RS_REC1_HDR] = 0x05;            /* weak released  */
        buf[13] = 0x22; buf[15] = 0x40;          /* animation continues */
        if (!decode_eq(buf, sizeof buf, 0, 0)) held = 0;
        check("release during animation seen immediately -> 0,0", held);
    }
}

/* ---- bounds -------------------------------------------------------------- */
static void test_bounds(void)
{
    uint8_t buf[256] = {0};
    uint8_t s = 0xAA, w = 0xAA;

    check("NULL buffer rejected",   vpad_rs_decode(NULL, 256, &s, &w) == 0);
    check("short buffer rejected",  vpad_rs_decode(buf, VPAD_RS_MIN_LEN - 1, &s, &w) == 0);
    check("short buffer leaves outputs untouched", s == 0xAA && w == 0xAA);
    check("exact-min buffer accepted", vpad_rs_decode(buf, VPAD_RS_MIN_LEN, &s, &w) == 1);
}

int main(void)
{
    test_hardware_captures();
    test_no_false_rumble();
    test_strong_weak_split();
    test_sustained_not_cut();
    test_bounds();

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall rumble-parse checks passed\n");
    return 0;
}
