/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB admission: a refusal table, a family knowledge-match, and an empty route
 * template.
 *
 * The module answers three questions, none of which is "may this device be
 * used":
 *   gc_usb_admission_skip()   - is this class/subclass/protocol one the tree
 *                               refuses to consider at all;
 *   gc_usb_admission_family() - does this VID/PID (with the interface, when the
 *                               caller has one) match one of the four
 *                               exceptional families CTL-02 names, or nothing
 *                               this table can name;
 *   gc_usb_admission_route()  - what the route template says for a family:
 *                               today always a refusal, because the template is
 *                               empty by construction.
 *
 * "Family" here is knowledge-matching and nothing else. It never admits a
 * device, selects or opens an endpoint, claims a device is supported, or
 * authorises a handler. The resident tables stay the authority
 * (core/src/gc_main.c:829-838) and the accepted classifier is
 * core/src/gc_usb.h:19-34, gated by core/tests/test_gc_usb.c:9-17; a family
 * answer from this module must not be used to select or open anything.
 *
 * Coverage is deliberately canonical-only: one or a few cited pairs per family,
 * not a second copy of the resident registry. So GC_USB_FAMILY_UNQUALIFIED
 * means "this table does not qualify it", never "this is not a gamepad" - a
 * pair the tree knows but this table does not list is unqualified too, and that
 * is the conservative answer the plan asks for (audit/ghostpad-intake/
 * EVALUATION.md, P4: "an unknown VID/PID resolving to UNQUALIFIED rather than to
 * a family guess").
 *
 * It does NOT include core/src/gc_usb.h, dev/usb or any platform header, does
 * not read a descriptor or call an ioctl, holds no state, allocates nothing and
 * takes no lock. Every function is pure: the same inputs give the same answer.
 *
 * Citations:
 *  - Ghostpad-main/src/controller/controller.c:100-109 - the drop's skip table
 *    (hub 0x09, mass storage 0x08, 0xEF/0xE0, and HID boot keyboard/mouse
 *    0x03/0x01/0x01|0x02), re-expressed here as a four-argument predicate.
 *  - Ghostpad-main/src/controller/controller.c:122-162 - their descriptor-first
 *    probe: GIP by subclass/protocol, generic HID only for interface class 0x03
 *    and not skipped. Their fallback at :164-201 detaches drivers and opens
 *    endpoints on an external bus; it is refused here (EVALUATION.md, Excluded
 *    7) and no part of this module can reach it.
 *  - Ghostpad-main/src/include/controller.h:92-94 - XBOX_GIP_SUBCLASS 0x47 /
 *    XBOX_GIP_PROTOCOL 0xD0, checked at controller.c:141-142 without the
 *    interface class. Ours requires class 0xff as well (core/src/gc_usb.h:24,
 *    core/src/gc_main.c:823-824), so the stricter reading is followed here and
 *    the difference is pinned by a case in the gate.
 *  - core/src/gc_usb.h:6-34 - GcUsbInterfaceKind and the accepted classifier.
 *    This header re-declares the kind values it needs so the module stays
 *    independent of that header; the numbers are the same on purpose.
 *  - core/tests/test_gc_usb.c:9-17 - CTL-01 selection, the gate this module is
 *    a possible future consumer of and must not replace.
 *  - audit/ghostpad-intake/EVALUATION.md, "P4 - Conservative unknown-device
 *    refusal and family route skeleton" - the specification, including its own
 *    scope limit: "only the unknown-device refusal table and an empty route
 *    template, with no new admission enabled, or it should be dropped".
 */
#pragma once

#include <stdint.h>

/* Mirrors GcUsbInterfaceKind (core/src/gc_usb.h:6-15) value for value, so a
 * caller can hand its own classification straight in. Declared here because
 * this module must not include that header or dev/usb. */
typedef enum {
    GC_USB_ADMISSION_IFACE_NONE = 0, /* no interface evidence */
    GC_USB_ADMISSION_IFACE_HID,      /* core/src/gc_usb.h:8 */
    GC_USB_ADMISSION_IFACE_HID_RAW,  /* core/src/gc_usb.h:12 */
    GC_USB_ADMISSION_IFACE_XUSB,     /* core/src/gc_usb.h:13 */
    GC_USB_ADMISSION_IFACE_GIP       /* core/src/gc_usb.h:14 */
} GcUsbAdmissionInterfaceKind;

/* Caller-owned. This is what the module reads; it is not a descriptor, and
 * filling it is the caller's statement about the device, not this module's. */
typedef struct {
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    /* bAlternateSetting. Only 0 is evidence of anything: the accepted
     * classifier rejects any other setting (core/src/gc_usb.h:23). */
    uint8_t alternate_setting;
    GcUsbAdmissionInterfaceKind kind;
} GcUsbInterface;

/* 1 when the tree refuses to consider this device/interface at all.
 * A refused device is refused for every family and every route. */
int gc_usb_admission_skip(uint8_t device_class, uint8_t interface_class,
                          uint8_t interface_subclass, uint8_t interface_protocol);

/* The four exceptional families CTL-02 names (WORK-CHECKLIST.md:195), plus the
 * conservative default. Any pair that is not one of them - unknown, or known to
 * the resident tables but not listed here - answers UNQUALIFIED. */
typedef enum {
    GC_USB_FAMILY_UNQUALIFIED = 0, /* not qualified by this table: the default */
    GC_USB_FAMILY_GIP,             /* core/src/controller_xbox.c:206-221 */
    GC_USB_FAMILY_DS3,             /* core/src/controller_ds3.c:43-99 */
    GC_USB_FAMILY_NINTENDO,        /* core/src/gc_main.c:812-815, :833-834 */
    GC_USB_FAMILY_LOGITECH,        /* core/src/controller_logitech.c:29-34 */
    GC_USB_FAMILY_COUNT            /* not a family */
} GcUsbFamily;

/* GC_USB_FAMILY_UNQUALIFIED unless the pair (or the interface's GIP signature)
 * matches one of the four exceptional families. interface may be NULL: then
 * only the pair is matched. A non-NULL interface can only ever remove a match,
 * never create one, except for the GIP descriptor triple itself, which is
 * family evidence. */
GcUsbFamily gc_usb_admission_family(uint16_t vendor_id, uint16_t product_id,
                                    const GcUsbInterface *interface);

/* One stable lowercase token for a family, or NULL for UNQUALIFIED and for any
 * value outside the enum. Logging only: never an admission decision. */
const char *gc_usb_admission_family_name(GcUsbFamily family);

/* The resident handler a route would name. NONE means no route was produced. */
typedef enum {
    GC_USB_ROUTE_TARGET_NONE = 0,
    GC_USB_ROUTE_TARGET_XBOX,     /* core/src/controller_xbox.c */
    GC_USB_ROUTE_TARGET_DS3,      /* core/src/controller_ds3.c */
    GC_USB_ROUTE_TARGET_NINTENDO, /* core/src/controller_nintendo.c */
    GC_USB_ROUTE_TARGET_LOGITECH  /* core/src/controller_logitech.c */
} GcUsbRouteTarget;

/* Caller-owned output. gc_usb_admission_route() writes every field it owns, so
 * a fresh plan cannot be read half-initialised. */
typedef struct {
    int routed; /* 1 only when a route was produced. Never, in this tree. */
    GcUsbFamily family;
    GcUsbRouteTarget target; /* NONE unless routed */
    /* Stable lowercase token when routed == 0; NULL when routed == 1. One of
     * "route_disabled_template_empty" (a template row exists and is disabled)
     * or "route_has_no_entry" (no row for this family value). */
    const char *refusal;
    /* One sentence saying why no route is enabled. Never NULL. */
    const char *reason;
} GcRoutePlan;

/* Returns 0 after writing plan only when a template row for this family is
 * enabled and names a target. Returns -1 (and writes the refusal and the
 * reason) otherwise, including for a NULL plan. Every row of the template is
 * disabled in this tree, so today this always refuses - by construction, from
 * the table, not from a branch that could be flipped by accident. */
int gc_usb_admission_route(GcUsbFamily family, GcRoutePlan *plan);

/* How many template rows are enabled. 0 in this tree; the gate asserts it, and
 * a future change would have to argue with the owner decision named in
 * docs/TYPED-OUTPUT-API-CONTRACT.md, "Addendum - Ghostpad intake", sub-heading
 * "USB admission refusal table and an empty route template (written, unwired)",
 * not with this function. */
unsigned gc_usb_admission_route_enabled_count(void);
