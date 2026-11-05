/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Refusal table, family knowledge-match and empty route template. See
 * gc_usb_admission.h for the scope, the non-goals and the citations. Pure: no
 * platform header, no ioctl, no state, no allocation - only this header plus
 * <stddef.h> for size_t.
 */
#include "gc_usb_admission.h"

#include <stddef.h>

/* ── Skip table (Ghostpad-main/src/controller/controller.c:100-109) ─────── */

int gc_usb_admission_skip(uint8_t device_class, uint8_t interface_class,
                          uint8_t interface_subclass, uint8_t interface_protocol)
{
    /* Hub. The interface class repeats the device class in the drop's rule
     * (controller.c:103), so both slots are tested. */
    if (device_class == 0x09 || interface_class == 0x09) return 1;
    /* Mass storage: controller.c:104. */
    if (device_class == 0x08 || interface_class == 0x08) return 1;
    /* 0xEF misc / 0xE0 wireless. The drop tests device 0xEF or interface 0xE0
     * and never the other two combinations (controller.c:105); this table
     * refuses the whole cross product. That is a deliberate difference, not a
     * fix: it is strictly more conservative and can only ever refuse more, so
     * it admits nothing the drop's form would not. Pinned by a case in the
     * gate. */
    if (device_class == 0xef || device_class == 0xe0 ||
        interface_class == 0xef || interface_class == 0xe0) return 1;
    /* HID boot keyboard (protocol 1) and boot mouse (protocol 2). A boot
     * interface carries no report descriptor to qualify, so it can never be a
     * generic gamepad (controller.c:106-107, core/src/gc_usb.h:28-33). */
    if (interface_class == 0x03 && interface_subclass == 0x01 &&
        (interface_protocol == 0x01 || interface_protocol == 0x02)) return 1;
    return 0;
}

/* ── Family table ───────────────────────────────────────────────────────── */

/* Canonical-only coverage, one cited source per family. These rows restate
 * knowledge the resident tables already own; they are not a second registry and
 * nothing may admit a device from them. The full sets stay where they are:
 * controller_ds3.c:43-99 (55 pairs), controller_logitech.c:29-34,
 * controller_xbox.c:206-221, gc_main.c:812-817, :830-836. */
typedef struct {
    uint16_t vid, pid;
    GcUsbFamily family;
} GcUsbFamilyPair;

static const GcUsbFamilyPair g_family_pairs[] = {
    /* Xbox GIP, by VID/PID. core/src/controller_xbox.c:206-221 names these in
     * its init-packet table; gc_main.c:330 names the 0x3537 pair. */
    { 0x045e, 0x02ea, GC_USB_FAMILY_GIP }, /* gc_main.c:816-817, xbox.c:210 */
    { 0x045e, 0x0b00, GC_USB_FAMILY_GIP }, /* controller_xbox.c:211-212 */
    { 0x0e6f, 0x0165, GC_USB_FAMILY_GIP }, /* controller_xbox.c:207 */
    { 0x0f0d, 0x0067, GC_USB_FAMILY_GIP }, /* controller_xbox.c:208 (HORI) */
    { 0x24c6, 0x541a, GC_USB_FAMILY_GIP }, /* controller_xbox.c:215-220 */
    { 0x24c6, 0x542a, GC_USB_FAMILY_GIP }, /* controller_xbox.c:215-220 */
    { 0x24c6, 0x543a, GC_USB_FAMILY_GIP }, /* controller_xbox.c:215-220 */
    { 0x3537, 0x1010, GC_USB_FAMILY_GIP }, /* gc_main.c:330 */

    /* DS3, canonical pair only: Sony's own pad, which controller_ds3.c:48
     * lists as the canonical entry and gc_main.c:334 checks by VID_SONY +
     * PID_DS3 (controller_ds3.h:47). The other 54 pairs are not duplicated
     * here. */
    { 0x054c, 0x0268, GC_USB_FAMILY_DS3 },

    /* Nintendo. gc_main.c:812-815 names both pairs and :833-834 maps exactly
     * these two to CTRL_NINTENDO. PID-scoped, not VID-scoped: 0x2dc8 is a
     * third-party vendor whose other PIDs this table does not claim. */
    { 0x057e, 0x2009, GC_USB_FAMILY_NINTENDO },
    { 0x2dc8, 0x310b, GC_USB_FAMILY_NINTENDO },

    /* Logitech generic HID pads: controller_logitech.h:52-57 and the predicate
     * at controller_logitech.c:29-34. PID-scoped too - 0x046d:0xcad1 is a
     * DS3-protocol pad (controller_ds3.c:47) and is deliberately not here. */
    { 0x046d, 0xc20e, GC_USB_FAMILY_LOGITECH },
    { 0x046d, 0xc216, GC_USB_FAMILY_LOGITECH },
    { 0x046d, 0xc218, GC_USB_FAMILY_LOGITECH },
    { 0x046d, 0xc219, GC_USB_FAMILY_LOGITECH },
    { 0x046d, 0xc21f, GC_USB_FAMILY_LOGITECH },
};

/* GIP interface signature. The drop checks subclass/protocol alone
 * (Ghostpad-main/src/controller/controller.c:141-142, constants at
 * Ghostpad-main/src/include/controller.h:92-94); we require interface class
 * 0xff as well, which is what our accepted classifier does
 * (core/src/gc_usb.h:24, core/src/gc_main.c:823-824). The stricter reading is
 * used here, and the gate pins the difference. */
#define GIP_INTERFACE_CLASS    0xffu
#define GIP_INTERFACE_SUBCLASS 0x47u
#define GIP_INTERFACE_PROTOCOL 0xd0u

/* The descriptor triple alone, with no VID/PID. Needs the interface and a
 * non-alternate setting (core/src/gc_usb.h:23). */
static int interface_is_gip(const GcUsbInterface *interface)
{
    return interface != NULL && interface->alternate_setting == 0 &&
           interface->interface_class == GIP_INTERFACE_CLASS &&
           interface->interface_subclass == GIP_INTERFACE_SUBCLASS &&
           interface->interface_protocol == GIP_INTERFACE_PROTOCOL;
}

/* A pair row may only survive an interface the caller has itself classified.
 * NONE means the caller did not fill it: no contradiction, so the pair stands.
 * HID and HID_RAW both fit the protocol-specific families, which the resident
 * mapping also gives GC_USB_INTERFACE_HID_RAW (core/src/gc_main.c:845-847)
 * while gc_usb.h:30-32 can yield either for a non-boot class-3 interface. */
static int interface_kind_agrees(GcUsbFamily family,
                                 GcUsbAdmissionInterfaceKind kind)
{
    if (kind == GC_USB_ADMISSION_IFACE_NONE) return 1;
    if (family == GC_USB_FAMILY_GIP) return kind == GC_USB_ADMISSION_IFACE_GIP;
    return kind == GC_USB_ADMISSION_IFACE_HID ||
           kind == GC_USB_ADMISSION_IFACE_HID_RAW;
}

GcUsbFamily gc_usb_admission_family(uint16_t vendor_id, uint16_t product_id,
                                    const GcUsbInterface *interface)
{
    if (interface_is_gip(interface)) return GC_USB_FAMILY_GIP;

    for (size_t index = 0; index < sizeof(g_family_pairs) / sizeof(g_family_pairs[0]);
         ++index) {
        const GcUsbFamilyPair *pair = &g_family_pairs[index];
        if (pair->vid != vendor_id || pair->pid != product_id) continue;
        if (!interface_kind_agrees(pair->family, interface ? interface->kind
                                                          : GC_USB_ADMISSION_IFACE_NONE))
            return GC_USB_FAMILY_UNQUALIFIED;
        /* The skip table is applied first: a pair found on an interface this
         * table would skip is not qualified, whatever the pair says. */
        if (interface != NULL &&
            gc_usb_admission_skip(0, interface->interface_class,
                                  interface->interface_subclass,
                                  interface->interface_protocol))
            return GC_USB_FAMILY_UNQUALIFIED;
        return pair->family;
    }
    return GC_USB_FAMILY_UNQUALIFIED;
}

const char *gc_usb_admission_family_name(GcUsbFamily family)
{
    switch (family) {
    case GC_USB_FAMILY_GIP:      return "gip";
    case GC_USB_FAMILY_DS3:      return "ds3";
    case GC_USB_FAMILY_NINTENDO: return "nintendo";
    case GC_USB_FAMILY_LOGITECH: return "logitech";
    case GC_USB_FAMILY_UNQUALIFIED:
    case GC_USB_FAMILY_COUNT:
        break;
    }
    return NULL;
}

/* ── Route template: empty by construction ──────────────────────────────── */

#define ROUTE_REFUSAL_DISABLED "route_disabled_template_empty"
#define ROUTE_REFUSAL_NO_ENTRY "route_has_no_entry"

/* Every row is disabled and carries the same reason, as one string, so the
 * template cannot grow a second, weaker reason unnoticed. */
#define ROUTE_REASON \
    "no family route is authorised: generic HID handles these families today " \
    "(core/src/gc_main.c:829-838) and the drop supplies architecture only, no " \
    "family-specific route evidence (audit/ghostpad-intake/EVALUATION.md, P4)"

typedef struct {
    GcUsbFamily family;
    GcUsbRouteTarget target;
    int enabled;
    const char *reason;
} GcRouteEntry;

/* The template. Four candidate routes, none enabled: enabling one would be a
 * resident admission change, which is an owner decision that has not been taken
 * (audit/ghostpad-intake/EVALUATION.md, P4 scope limit). */
static const GcRouteEntry g_route_template[] = {
    { GC_USB_FAMILY_GIP,      GC_USB_ROUTE_TARGET_XBOX,     0, ROUTE_REASON },
    { GC_USB_FAMILY_DS3,      GC_USB_ROUTE_TARGET_DS3,      0, ROUTE_REASON },
    { GC_USB_FAMILY_NINTENDO, GC_USB_ROUTE_TARGET_NINTENDO, 0, ROUTE_REASON },
    { GC_USB_FAMILY_LOGITECH, GC_USB_ROUTE_TARGET_LOGITECH, 0, ROUTE_REASON },
};

int gc_usb_admission_route(GcUsbFamily family, GcRoutePlan *plan)
{
    if (plan == NULL) return -1;

    /* Every field the plan owns, written explicitly: no memset, no <string.h>,
     * and a caller cannot read a stale value. */
    plan->routed = 0;
    plan->family = family;
    plan->target = GC_USB_ROUTE_TARGET_NONE;
    plan->refusal = ROUTE_REFUSAL_NO_ENTRY;
    plan->reason = ROUTE_REASON;

    for (size_t index = 0;
         index < sizeof(g_route_template) / sizeof(g_route_template[0]); ++index) {
        const GcRouteEntry *entry = &g_route_template[index];
        if (entry->family != family) continue;
        if (entry->enabled) {
            /* No row in this tree is enabled. This branch exists so the table,
             * not this function, is the decision point; reaching it would mean
             * someone enabled a family route without the owner decision named
             * in the header and in docs/TYPED-OUTPUT-API-CONTRACT.md. */
            plan->routed = 1;
            plan->target = entry->target;
            plan->reason = entry->reason;
            plan->refusal = NULL;
            return 0;
        }
        plan->refusal = ROUTE_REFUSAL_DISABLED;
        return -1;
    }
    return -1;
}

unsigned gc_usb_admission_route_enabled_count(void)
{
    unsigned enabled = 0;
    for (size_t index = 0;
         index < sizeof(g_route_template) / sizeof(g_route_template[0]); ++index)
        if (g_route_template[index].enabled) ++enabled;
    return enabled;
}
