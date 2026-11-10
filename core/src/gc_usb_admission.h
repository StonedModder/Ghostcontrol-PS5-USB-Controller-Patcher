/* SPDX-License-Identifier: GPL-3.0-or-later */
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
 * "USB admission refusal table and an empty route template (written, unwired)",
 * not with this function. */
unsigned gc_usb_admission_route_enabled_count(void);
