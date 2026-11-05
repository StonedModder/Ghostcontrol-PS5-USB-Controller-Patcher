/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Toolbox-facing Bluetooth status formatter.
 *
 * Bluetooth now runs inside the resident core (gc_bt.c) as one more input
 * source beside USB and GhostPad. This file only turns the live engine
 * snapshot into the JSON the Toolbox already reads, and validates the
 * address a Forget request carries. There are no status files, request
 * files, or a separate process any more. */
#define _POSIX_C_SOURCE 200809L
#include "gc_bluetooth.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void append_addr(char out[18], const unsigned char addr[6])
{
    /* Identity address, most-significant byte first for display. */
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static int append(char **cursor, size_t *left, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*cursor, *left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= *left)
        return -1;
    *cursor += n;
    *left -= (size_t)n;
    return 0;
}

static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 2 < out_size; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20)
            continue;
        if (c == '"' || c == '\\') {
            if (j + 3 >= out_size) break;
            out[j++] = '\\';
        }
        out[j++] = (char)c;
    }
    out[j] = '\0';
}

static int append_age(char **cursor, size_t *left, const char *key, long age_ms, int last)
{
    if (age_ms < 0)
        return append(cursor, left, "\"%s\":null%s", key, last ? "" : ",");
    return append(cursor, left, "\"%s\":%ld%s", key, age_ms, last ? "" : ",");
}

/* BT-R01: measured radio diagnostics. null when no engine produced a
 * snapshot; "radio" is null until a radio has been opened at least once.
 * Never includes a radio address or pairing material. */
static int append_diagnostics(const GcBtDiag *d, char **cursor, size_t *left)
{
    if (!d->at_sample_ms)
        return append(cursor, left, "null");
    if (append(cursor, left, "{\"radioOpen\":%s,", d->radio_open ? "true" : "false") != 0)
        return -1;
    if (append_age(cursor, left, "sampleAgeMs", d->sample_age_ms, 0) != 0)
        return -1;
    if (!d->measured) {
        if (append(cursor, left, "\"radio\":null,") != 0)
            return -1;
    } else {
        char usb[32] = "null";
        if (d->have_usb)
            snprintf(usb, sizeof usb, "\"%04x:%04x\"", d->usb_vid, d->usb_pid);
        if (append(cursor, left,
                   "\"radio\":{\"current\":%s,\"usb\":%s,\"interface\":%d,"
                   "\"endpoints\":{\"events\":\"0x%02x\",\"aclIn\":\"0x%02x\",\"aclOut\":\"0x%02x\","
                   "\"fromDescriptor\":%s},\"hciFunctions\":%d,\"aclMtu\":%u,"
                   "\"aclBuffers\":%d,\"le\":%s,",
                   d->radio_open ? "true" : "false", usb, d->iface, d->ep_events, d->ep_acl_in,
                   d->ep_acl_out, d->endpoints_from_descriptor ? "true" : "false",
                   d->hci_functions, d->acl_mtu, d->acl_buffers, d->le ? "true" : "false") != 0)
            return -1;
        if (append(cursor, left,
                   "\"counters\":{\"reads\":%u,\"readsEmpty\":%u,\"events\":%u,\"acl\":%u,"
                   "\"eventQueueDrops\":%u,\"aclQueueDrops\":%u,\"partialEventDrops\":%u,"
                   "\"aclStale\":%u,\"aclMalformed\":%u,\"txNoCredit\":%u,\"txFailed\":%u,"
                   "\"probes\":%u,\"resetAttempts\":%u},",
                   (unsigned)d->reads, (unsigned)d->reads_empty, (unsigned)d->events,
                   (unsigned)d->acl, (unsigned)d->event_queue_drops,
                   (unsigned)d->acl_queue_drops, (unsigned)d->partial_event_drops,
                   (unsigned)d->acl_stale, (unsigned)d->acl_malformed,
                   (unsigned)d->tx_no_credit, (unsigned)d->tx_failed,
                   (unsigned)d->probes, (unsigned)d->reset_attempts) != 0)
            return -1;
        if (append_age(cursor, left, "lastRxAgeMs", d->last_rx_age_ms, 1) != 0)
            return -1;
        if (append(cursor, left, "},") != 0)
            return -1;
    }
    char failure[256];
    json_escape(d->last_failure, failure, sizeof failure);
    if (append(cursor, left,
               "\"failures\":{\"openAttempts\":%u,\"openFailures\":%u,\"transportLosses\":%u,",
               (unsigned)d->open_attempts, (unsigned)d->open_failures,
               (unsigned)d->transport_losses) != 0)
        return -1;
    if (d->last_failure[0]) {
        if (append(cursor, left, "\"last\":\"%s\",", failure) != 0)
            return -1;
    } else if (append(cursor, left, "\"last\":null,") != 0) {
        return -1;
    }
    if (append_age(cursor, left, "lastAgeMs", d->last_failure_age_ms, 1) != 0)
        return -1;
    return append(cursor, left, "}}");
}

int gc_bluetooth_format_status(const GcBtStatus *status, char *response,
                               size_t response_size)
{
    if (!status || !response || response_size < 2)
        return -1;
    char *cursor = response;
    size_t left = response_size;
    char reason[256];
    json_escape(status->reason, reason, sizeof(reason));

    if (append(&cursor, &left,
               "{\"ok\":true,\"version\":\"%s\",\"bluetooth\":\"%s\","
               "\"reason\":\"%s\",\"pairing\":%d,\"paired\":[",
               status->version, status->state, reason, status->pairing) != 0)
        return -1;

    for (int i = 0; i < status->paired_count; ++i) {
        char addr[18];
        append_addr(addr, status->paired[i].addr);
        if (append(&cursor, &left,
                   "%s{\"address\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\","
                   "\"le\":%s,\"connected\":%s}",
                   i ? "," : "", addr,
                   status->paired[i].vid, status->paired[i].pid,
                   status->paired[i].le ? "true" : "false",
                   status->paired[i].connected ? "true" : "false") != 0)
            return -1;
    }

    if (append(&cursor, &left, "],\"controllers\":[") != 0)
        return -1;

    for (int i = 0; i < status->controller_count; ++i) {
        char addr[18], profile[96];
        append_addr(addr, status->controllers[i].addr);
        json_escape(status->controllers[i].profile, profile, sizeof(profile));
        if (append(&cursor, &left,
                   "%s{\"slot\":%d,\"profile\":\"%s\",\"address\":\"%s\","
                   "\"vid\":\"%04x\",\"pid\":\"%04x\",\"le\":%s,"
                   "\"virtualPad\":%s}",
                   i ? "," : "", status->controllers[i].slot, profile, addr,
                   status->controllers[i].vid, status->controllers[i].pid,
                   status->controllers[i].le ? "true" : "false",
                   status->controllers[i].virtual_pad ? "true" : "false") != 0)
            return -1;
    }

    if (append(&cursor, &left, "],\"diagnostics\":") != 0)
        return -1;
    if (append_diagnostics(&status->diag, &cursor, &left) != 0)
        return -1;
    if (append(&cursor, &left, "}\n") != 0)
        return -1;
    return 0;
}

int gc_bluetooth_canonical_address(const char *argument, char out[18])
{
    if (!argument || strlen(argument) != 17)
        return -1;
    for (size_t index = 0; index < 17; ++index) {
        if (index % 3 == 2) {
            if (argument[index] != ':')
                return -1;
            out[index] = ':';
        } else {
            unsigned char value = (unsigned char)argument[index];
            if (!isxdigit(value))
                return -1;
            out[index] = (char)toupper(value);
        }
    }
    out[17] = '\0';
    return 0;
}
