/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "evstream.h"

#include <string.h>

/* Bytes an event occupies, from its own 2-byte header. The caller guarantees
 * at least two bytes are buffered. */
static int event_span(const uint8_t *header)
{
    return 2 + (int)header[1];
}

/* Moves what fits into the tail of the stream buffer and reports how much
 * moved, so the caller can advance its own cursor by the same amount. */
static int absorb(evstream *s, const uint8_t *piece, int len)
{
    int space = (int)sizeof s->buf - s->len;
    int take = (len < space) ? len : space;

    memcpy(s->buf + s->len, piece, (size_t)take);
    s->len += take;
    return take;
}

/* Hands every event already complete at the head of the buffer to the sink and
 * closes the gap behind each one, until what is left is a partial event. */
static void drain(evstream *s, evstream_emit emit, void *ctx)
{
    for (;;) {
        int span;

        if (s->len < 2) return;             /* not even a header yet */
        span = event_span(s->buf);
        if (s->len < span) return;          /* rest still in flight */

        emit(ctx, s->buf, span);

        s->len -= span;
        if (s->len > 0) memmove(s->buf, s->buf + span, (size_t)s->len);
    }
}

void evstream_init(evstream *s)
{
    memset(s, 0, sizeof *s);
}

void evstream_feed(evstream *s, const uint8_t *piece, int len, long now,
                   evstream_emit emit, void *ctx)
{
    if (len <= 0) return;

    /* A half event that has waited past the timeout is never going to be
     * completed; keeping it would make every later byte land at the wrong
     * offset, so it is thrown away and counted instead. */
    if (s->len > 0 && now - s->t_last > EVSTREAM_TIMEOUT) {
        s->len = 0;
        s->dropped++;
    }
    s->t_last = now;

    while (len > 0) {
        int taken = absorb(s, piece, len);

        piece += taken;
        len -= taken;
        drain(s, emit, ctx);
    }
}
