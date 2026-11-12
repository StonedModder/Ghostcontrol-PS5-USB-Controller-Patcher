#include "host_int.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const pad_output k_player_colours[HOST_MAX_PADS] = {
    { 0, 0, 0x00, 0x00, 0x40, 1 },      /* blue */
    { 0, 0, 0x40, 0x00, 0x00, 2 },      /* red */
    { 0, 0, 0x00, 0x40, 0x00, 3 },      /* green */
    { 0, 0, 0x20, 0x00, 0x20, 4 },      /* pink */
};


/* Private allocation tail: host_t remains the frozen host_int.h layout.
 * Every production host is owned by host_open/host_close. No global registry
 * or saved bond participates in observed connection provenance. */
typedef struct {
    host_t host;                    /* initial member, freed through host_t */
    int classic_acl[HOST_MAX_PADS]; /* successful Classic ACL completion */
    host_origin origins[HOST_MAX_PADS];
    int qualified;
    int lost_connections;
    int setup_scan_uncertain;

    /* Identities learned from inquiry responses during this run, and never
     * written anywhere: the console's own pads are recognised again on the
     * next start by asking the radio, not by trusting a file. */
    unsigned char ban[HOST_BAN_MAX][6];
    int n_ban;
    int ban_full;                   /* the list is full: no more blind calls */

    /* A gamepad found without an identity: the answer is waited for, once. */
    unsigned char defer_rec[14];
    long defer_at;                  /* when it was found; 0: none */

    /* The last pad called, for a bounded fresh-link retry. */
    unsigned char last_call[14];
    long repage_at;                 /* when the next call is due; 0: none */
    int in_repage;
    int fresh_left;                 /* links that may be made again this search */

    /* Addresses that answered "connection already exists", not called again
     * for a while. */
    struct { unsigned char addr[6]; long until; } hold[4];
    int hold_next;

    /* The inquiry command in flight, and the wait after a refused one. */
    long t_inquiry;
    long inquiry_at;
    long inquiry_cancel_at;
    long inquiry_backoff;
} host_storage;

static int *classic_acl(host_t *h, const link_t *l)
{
    return &((host_storage *)h)->classic_acl[slot_of(h, l)];
}

static pthread_mutex_t origin_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t origin_next;
#ifdef GC_BT_ORIGIN_TEST
/* Narrow HOST-only exhaustion seam, absent from default objects. */
void host_test_origin_exhaust(void)
{
    pthread_mutex_lock(&origin_mu); origin_next = UINT64_MAX; pthread_mutex_unlock(&origin_mu);
}
#endif
static host_origin captured_origin(host_t *h, const link_t *l)
{
    return ((host_storage *)h)->origins[slot_of(h, l)];
}
int link_origin_begin(host_t *h, link_t *l, unsigned handle)
{
    host_origin origin = {0};
    if (handle > 0xfff || h->dead || h->closing) return 0;
    pthread_mutex_lock(&origin_mu);
    if (origin_next != UINT64_MAX) origin.occurrence = ++origin_next;
    pthread_mutex_unlock(&origin_mu);
    if (!origin.occurrence) return 0;
    origin.handle = (uint16_t)handle;
    ((host_storage *)h)->origins[slot_of(h, l)] = origin;
    return 1;
}
static void transport_lost(host_t *h)
{
    int i;
    h->dead = 1; h->cc_op = 0; h->cc_len = 0;
    memset(h->cc, 0, sizeof h->cc);
    for (i = 0; i < HOST_MAX_PADS; ++i) {
        if (h->links[i].used) {
            ((host_storage *)h)->lost_connections = 1;
            link_free(h, &h->links[i]);
        }
        /* A send may fail within a state-machine step. Clear again at the
         * owner boundary so caller-local bookkeeping cannot survive loss. */
        memset(&h->links[i], 0, sizeof h->links[i]);
        ((host_storage *)h)->classic_acl[i] = 0;
        memset(&((host_storage *)h)->origins[i], 0, sizeof(host_origin));
    }
}
/* ---- helpers -------------------------------------------------------------- */

const char *addr_str(const unsigned char *a)
{
    static char buf[4][18];
    static int which;
    char *s = buf[which++ & 3];

    snprintf(s, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    return s;
}

int send_cmd(host_t *h, unsigned op, const void *p, int len)
{
    int ok;
    if (h->dead) return 0;
    if (op == 0x200C) { ++h->le_scan_pending; h->le_scan_owned = 1; }
    ok = h->hci.ops->cmd(h->hci.ctx, op, p, len);
    if (h->hci.ops->pump(h->hci.ctx, 0) < 0) { transport_lost(h); return 0; }
    return ok;
}

int slot_of(host_t *h, const link_t *l) { return (int)(l - h->links); }

link_t *link_by_addr(host_t *h, const unsigned char *a)
{
    int i;

    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used && memcmp(h->links[i].addr, a, 6) == 0) return &h->links[i];
    return NULL;
}

static link_t *link_by_handle(host_t *h, unsigned handle)
{
    int i;

    handle &= 0x0FFF;
    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used && h->links[i].connected && h->links[i].handle == handle)
            return &h->links[i];
    return NULL;
}

static void chan_init(chan *c, unsigned psm, unsigned scid)
{
    memset(c, 0, sizeof *c);
    c->psm = psm;
    c->scid = scid;
}

link_t *link_new(host_t *h, const unsigned char *addr)
{
    int i;

    for (i = 0; i < HOST_MAX_PADS; i++) {
        link_t *l = &h->links[i];
        if (l->used) continue;
        memset(l, 0, sizeof *l);
        *classic_acl(h, l) = 0;
        memset(&((host_storage *)h)->origins[i], 0, sizeof(host_origin));
        if (h->dead || h->closing) return NULL;
        l->used = 1;
        memcpy(l->addr, addr, 6);
        l->t_create = now_ms();
        chan_init(&l->sdp, PSM_SDP, CID_SDP);
        chan_init(&l->ctrl, PSM_HID_CTRL, CID_CTRL);
        chan_init(&l->intr, PSM_HID_INTR, CID_INTR);
        pad_state_reset(&l->st);
        l->out = k_player_colours[i];
        return l;
    }
    return NULL;
}

void link_free(host_t *h, link_t *l)
{
    host_origin origin = captured_origin(h, l);
    int ready = l->ready;
    l->ready = 0;
    if (ready && ((host_storage *)h)->qualified)
        h->ev.on_disconnect_qualified(h->ev.ud, slot_of(h, l), &origin);
    else if (ready && h->ev.on_disconnect) h->ev.on_disconnect(h->ev.ud, slot_of(h, l));
    le_abandon(h, l);           /* cancel any LE connection still in flight */
    *classic_acl(h, l) = 0;
    memset(l, 0, sizeof *l);
}

void link_disconnect(host_t *h, link_t *l, const char *why)
{
    unsigned char p[3];

    if (!l->connected) {
        if (l->remote_pending) {
            if (!l->cancel_sent) {
                l->cancel_sent = 1;
                if (l->le) send_cmd(h, 0x200E, NULL, 0);
                else if (l->remote_pending == 1)
                    send_cmd(h, OP_CREATE_CONNECTION_CANCEL, l->addr, 6);
            }
            return; /* Only a terminal controller event retires this owner. */
        }
        log_line("pad %s: dropped (%s)", addr_str(l->addr), why);
        link_free(h, l);
        return;
    }
    if (l->disconnecting) return;
    log_line("pad %s: disconnecting (%s)", addr_str(l->addr), why);
    put16(p, l->handle);
    p[2] = 0x13;                        /* remote user terminated */
    send_cmd(h, OP_DISCONNECT, p, 3);
    l->disconnecting = 1;
}

/* ---- paired-pad store ----------------------------------------------------- */

db_rec *db_find(host_t *h, const unsigned char *addr)
{
    int i;

    for (i = 0; i < h->ndb; i++)
        if (memcmp(h->db[i].addr, addr, 6) == 0) return &h->db[i];
    return NULL;
}

/* The store file: a magic string, then one fixed-size record per pad:
 *   addr 6, key 16, key type 1, vid 2, pid 2, le 1, addr type 1, ediv 2,
 *   rand 8, irk 16, has irk 1, padding */
/* Pairing can run before the UI has created the service's state directory. */
static int db_make_parent(const char *path)
{
    char parent[300];
    if (strlen(path) >= sizeof parent) { errno = ENAMETOOLONG; return 0; }
    strcpy(parent, path);
    char *last = strrchr(parent, '/');
    if (!last) return 1;
    if (last == parent) return 1;
    *last = '\0';
    for (char *at = parent + 1; ; ++at) {
        if (*at && *at != '/') continue;
        char saved = *at;
        *at = '\0';
        struct stat st;
        if (mkdir(parent, 0755) != 0 && (errno != EEXIST || stat(parent, &st) != 0 || !S_ISDIR(st.st_mode)))
            return 0;
        *at = saved;
        if (!saved) break;
    }
    return 1;
}

int db_save(host_t *h)
{
    char tmp[300];
    FILE *f;
    int i, ok;

    /* Written beside, then renamed over: a crash leaves the old or the new
     * store, never half of one. */
    snprintf(tmp, sizeof tmp, "%s.new", h->db_path);
    f = fopen(tmp, "wb");
    if (!f && errno == ENOENT && db_make_parent(h->db_path)) f = fopen(tmp, "wb");
    if (!f) {
        log_line("store: cannot write %s: errno %d", tmp, errno);
        return 0;
    }
    fwrite(DB_MAGIC, 1, 8, f);
    for (i = 0; i < h->ndb; i++) {
        const db_rec *d = &h->db[i];
        unsigned char r[DB_REC];

        memset(r, 0, sizeof r);
        memcpy(r, d->addr, 6);
        memcpy(r + 6, d->key, 16);
        r[22] = d->type;
        put16(r + 23, d->vid);
        put16(r + 25, d->pid);
        r[27] = d->le;
        r[28] = d->addr_type;
        put16(r + 29, d->ediv);
        memcpy(r + 31, d->rand, 8);
        memcpy(r + 39, d->irk, 16);
        r[55] = d->has_irk;
        fwrite(r, 1, sizeof r, f);
    }
    ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, h->db_path) != 0) {
        log_line("store: could not save %s: errno %d", h->db_path, errno);
        remove(tmp);
        return 0;
    }
    return 1;
}

static void db_load(host_t *h)
{
    FILE *f = fopen(h->db_path, "rb");
    unsigned char r[DB_REC];

    h->ndb = 0;
    if (!f) return;
    if (fread(r, 1, 8, f) != 8 || memcmp(r, DB_MAGIC, 8) != 0) {
        log_line("store: %s is not a GhostTooth store, ignored", h->db_path);
        fclose(f);
        return;
    }
    while (h->ndb < DB_MAX && fread(r, 1, sizeof r, f) == sizeof r) {
        db_rec *d = &h->db[h->ndb++];
        memcpy(d->addr, r, 6);
        memcpy(d->key, r + 6, 16);
        d->type = r[22];
        d->vid = (uint16_t)le16(r + 23);
        d->pid = (uint16_t)le16(r + 25);
        d->le = r[27];
        d->addr_type = r[28];
        d->ediv = (uint16_t)le16(r + 29);
        memcpy(d->rand, r + 31, 8);
        memcpy(d->irk, r + 39, 16);
        d->has_irk = r[55];
    }
    fclose(f);
    log_line("store: %d paired pad(s)", h->ndb);
}

db_rec *db_put(host_t *h, const unsigned char *addr)
{
    db_rec *d = db_find(h, addr);

    if (d) return d;
    if (h->ndb == DB_MAX) {             /* full: forget the oldest */
        memmove(h->db, h->db + 1, sizeof h->db[0] * (DB_MAX - 1));
        h->ndb--;
    }
    d = &h->db[h->ndb++];
    memset(d, 0, sizeof *d);
    memcpy(d->addr, addr, 6);
    return d;
}

int db_forget(host_t *h, const unsigned char *addr)
{
    db_rec *d = db_find(h, addr);

    if (!d) return 0;
    memmove(d, d + 1, sizeof *d * (size_t)(h->ndb - (d - h->db) - 1));
    h->ndb--;
    if (!db_save(h)) {
        log_line("store: forgot %s in memory but could not persist: errno %d",
                 addr_str(addr), errno);
        return 0;
    }
    return 1;
}

/* ---- GATT cache: per-pad discovery results, so reconnections skip the
 * full GATT walk and go straight to enabling notifications. ----------- */

void gatt_cache_save(host_t *h)
{
    char tmp[300], path[300];
    FILE *f;
    int i, ok;

    snprintf(path, sizeof path, "%s.gatt", h->db_path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(GATT_CACHE_MAGIC, 1, 8, f);
    for (i = 0; i < h->ngatt_cache; i++)
        fwrite(&h->gatt_cache[i], 1, sizeof(gatt_cache_rec), f);
    ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) remove(tmp);
}

void gatt_cache_load(host_t *h)
{
    char path[300];
    FILE *f;
    unsigned char magic[8];

    snprintf(path, sizeof path, "%s.gatt", h->db_path);
    h->ngatt_cache = 0;
    f = fopen(path, "rb");
    if (!f) return;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, GATT_CACHE_MAGIC, 8) != 0) {
        fclose(f);
        return;
    }
    while (h->ngatt_cache < GATT_CACHE_MAX &&
           fread(&h->gatt_cache[h->ngatt_cache], 1, sizeof(gatt_cache_rec), f) == sizeof(gatt_cache_rec))
        h->ngatt_cache++;
    fclose(f);
    if (h->ngatt_cache > 0)
        log_line("gatt cache: %d pad(s)", h->ngatt_cache);
}

gatt_cache_rec *gatt_cache_find(host_t *h, const unsigned char *addr)
{
    int i;
    for (i = 0; i < h->ngatt_cache; i++)
        if (memcmp(h->gatt_cache[i].addr, addr, 6) == 0) return &h->gatt_cache[i];
    return NULL;
}

void gatt_cache_put(host_t *h, link_t *l)
{
    gatt_cache_rec *c;
    le_link *e = &l->le_s;

    if (e->nch == 0) return;
    c = gatt_cache_find(h, l->addr);
    if (!c) {
        if (h->ngatt_cache >= GATT_CACHE_MAX) {
            memmove(h->gatt_cache, h->gatt_cache + 1,
                    sizeof(gatt_cache_rec) * (GATT_CACHE_MAX - 1));
            h->ngatt_cache--;
        }
        c = &h->gatt_cache[h->ngatt_cache++];
        memcpy(c->addr, l->addr, 6);
    }
    c->nch = (uint8_t)e->nch;
    c->svc_start = e->svc_start;
    c->svc_end = e->svc_end;
    c->map_handle = e->map_handle;
    memcpy(c->ch, e->ch, sizeof(gatt_char) * (size_t)e->nch);
    gatt_cache_save(h);
}

/* ---- ACL outt ---------------------------------------------------------------- */

static void pool_expire(acl_pool *p)
{
    long now = now_ms();

    while (p->count && now - p->inflight[p->head] > INFLIGHT_MS) {
        p->head = (p->head + 1) % INFLIGHT_MAX;
        p->count--;
        if (p->credits < p->max) p->credits++;
    }
}

/* Packets the controller reports sent: their buffers are free again. */
static void pool_done(acl_pool *p, int n)
{
    int i;

    for (i = 0; i < n && p->count; i++) {
        p->head = (p->head + 1) % INFLIGHT_MAX;
        p->count--;
    }
    p->credits += n;
    if (p->credits > p->max) p->credits = p->max;
}

static acl_pool *pool_of(host_t *h, const link_t *l)
{
    return l->le && h->le_separate ? &h->lep : &h->br;
}

/* Sends one L2CAP frame, unfragmented. With no controller buffer free the
 * frame is dropped; everything sent here is retried or refreshed. */
int l2_send(host_t *h, link_t *l, unsigned cid, const unsigned char *d, int len)
{
    unsigned char pkt[HCI_PKT_MAX];
    acl_pool *pool = pool_of(h, l);
    unsigned mtu = l->le && h->le_separate ? h->le_mtu : h->acl_mtu;

    if (h->dead || !l->connected) return 0;
    if (8 + len > (int)sizeof pkt || (mtu && 4 + len > (int)mtu)) {
        log_line("l2cap: %d-byte frame too large", len);
        return 0;
    }
    pool_expire(pool);
    if (pool->credits <= 0) {
        h->tx_no_credit++;
        log_line("l2cap: no controller buffer free, frame dropped");
        return 0;
    }
    /* Start of a frame: auto-flushable on Classic, non-flushable on LE. */
    put16(pkt, (l->handle & 0x0FFF) | (l->le ? 0x0000 : 0x2000));
    put16(pkt + 2, (unsigned)(4 + len));
    put16(pkt + 4, (unsigned)len);
    put16(pkt + 6, cid);
    memcpy(pkt + 8, d, (size_t)len);
    if (!h->hci.ops->acl_send(h->hci.ctx, pkt, 8 + len)) {
        h->tx_failed++;
        if (h->hci.ops->pump(h->hci.ctx, 0) < 0) transport_lost(h);
        return 0;
    }

    if (h->hci.ops->pump(h->hci.ctx, 0) < 0) { transport_lost(h); return 0; }
    pool->credits--;
    if (pool->count == INFLIGHT_MAX) {
        pool->head = (pool->head + 1) % INFLIGHT_MAX;
        pool->count--;
    }
    pool->inflight[(pool->head + pool->count) % INFLIGHT_MAX] = now_ms();
    pool->count++;
    return 1;
}

static int sig_send(host_t *h, link_t *l, unsigned char code, unsigned char id,
                    const unsigned char *d, int len)
{
    unsigned char p[64];

    if (len > (int)sizeof p - 4) return 0;
    p[0] = code;
    p[1] = id;
    put16(p + 2, (unsigned)len);
    if (len) memcpy(p + 4, d, (size_t)len);
    return l2_send(h, l, CID_SIGNALING, p, 4 + len);
}

static unsigned char next_sig_id(host_t *h)
{
    if (++h->sig_id == 0) h->sig_id = 1;
    return h->sig_id;
}

/* ---- L2CAP channels ------------------------------------------------------- */

static chan *chan_by_scid(link_t *l, unsigned cid)
{
    if (l->sdp.st != CH_CLOSED && l->sdp.scid == cid) return &l->sdp;
    if (l->ctrl.st != CH_CLOSED && l->ctrl.scid == cid) return &l->ctrl;
    if (l->intr.st != CH_CLOSED && l->intr.scid == cid) return &l->intr;
    return NULL;
}

static chan *chan_by_psm(link_t *l, unsigned psm)
{
    if (psm == PSM_HID_CTRL) return &l->ctrl;
    if (psm == PSM_HID_INTR) return &l->intr;
    return NULL;            /* SDP is only ever opened by us */
}

static void chan_set(chan *c, chan_state st)
{
    c->st = st;
    c->t_state = now_ms();
    if (st != CH_OPEN) c->cfg_ours_ok = c->cfg_theirs_ok = 0;
    if (st == CH_CONFIG) c->cfg_tries = 0;
}

static void chan_send_cfg(host_t *h, link_t *l, chan *c)
{
    unsigned char r[4];

    put16(r, c->dcid);
    put16(r + 2, 0);                    /* flags; no options: defaults */
    sig_send(h, l, SIG_CFG_REQ, next_sig_id(h), r, 4);
    c->t_cfg = now_ms();
    c->cfg_tries++;
}

static void chan_check_open(link_t *l, chan *c)
{
    if (c->st == CH_CONFIG && c->cfg_ours_ok && c->cfg_theirs_ok) {
        chan_set(c, CH_OPEN);
        c->refused = 0;
        c->retry_at = 0;
        log_line("pad %s: channel %#x open", addr_str(l->addr), c->psm);
    }
}

static void chan_connect(host_t *h, link_t *l, chan *c)
{
    unsigned char r[4];

    /* A pad that refused this channel is asked again only after its wait:
     * one refusal must not turn into a request per poll. */
    if (now_ms() < c->retry_at) return;
    chan_set(c, CH_CONNECTING);
    put16(r, c->psm);
    put16(r + 2, c->scid);
    sig_send(h, l, SIG_CONN_REQ, next_sig_id(h), r, 4);
}

static void chan_close(host_t *h, link_t *l, chan *c)
{
    unsigned char r[4];

    if (c->st == CH_OPEN || c->st == CH_CONFIG) {
        put16(r, c->dcid);
        put16(r + 2, c->scid);
        sig_send(h, l, SIG_DISC_REQ, next_sig_id(h), r, 4);
    }
    chan_set(c, CH_CLOSED);
}

/* The pad closed a channel, or it never finished: without both HID
 * channels the pad is of no use. */
static void chan_lost(host_t *h, link_t *l, chan *c)
{
    chan_set(c, CH_CLOSED);
    if (c != &l->sdp && l->ready) link_disconnect(h, l, "HID channel closed");
}

static void chan_tick(host_t *h, link_t *l, chan *c, long now)
{
    if (c->st == CH_CONNECTING && now - c->t_state > T_CHAN_CONNECT) {
        log_line("pad %s: channel %#x: no connection response", addr_str(l->addr), c->psm);
        chan_set(c, CH_CLOSED);         /* opened again by the link's step */
    } else if (c->st == CH_CONFIG && now - c->t_state > T_CFG_TIMEOUT) {
        /* Only the pad can repeat its configuration request; if it went to
         * the system's driver, start the channel over. */
        log_line("pad %s: channel %#x: configuration stuck, reopening (ours %d theirs %d tries %d dcid %#x)", addr_str(l->addr), c->psm, c->cfg_ours_ok, c->cfg_theirs_ok, c->cfg_tries, c->dcid);
        chan_close(h, l, c);
    } else if (c->st == CH_CONFIG && !c->cfg_ours_ok && now - c->t_cfg >= T_CFG_RESEND) {
        if (c->cfg_tries >= CFG_TRIES) {
            log_line("pad %s: channel %#x: configuration never answered", addr_str(l->addr), c->psm);
            chan_close(h, l, c);
        } else {
            chan_send_cfg(h, l, c);
        }
    }
}

static void on_signaling(host_t *h, link_t *l, const unsigned char *d, int len)
{
    while (len >= 4) {
        unsigned char code = d[0], id = d[1], r[16];
        int clen = (int)le16(d + 2);
        const unsigned char *c = d + 4;
        chan *ch;

        if (4 + clen > len) break;

        switch (code) {
        case SIG_CONN_REQ:      /* psm, source cid */
            if (clen < 4) break;
            ch = chan_by_psm(l, le16(c));
            put16(r + 2, le16(c + 2));
            put16(r + 6, 0);
            if (!ch) {
                put16(r, 0);
                put16(r + 4, 0x0002);   /* PSM not supported */
                sig_send(h, l, SIG_CONN_RSP, id, r, 8);
                break;
            }
            /* The pad opens its own channel: take it, even over one of
             * ours still being set up. */
            chan_set(ch, CH_CONFIG);
            ch->dcid = le16(c + 2);
            put16(r, ch->scid);
            put16(r + 4, 0);            /* success */
            sig_send(h, l, SIG_CONN_RSP, id, r, 8);
            chan_send_cfg(h, l, ch);
            log_line("pad %s: accepted channel %#x", addr_str(l->addr), ch->psm);
            break;

        case SIG_CONN_RSP:      /* dcid, scid, result, status */
            if (clen < 8) break;
            ch = chan_by_scid(l, le16(c + 2));
            if (!ch || ch->st != CH_CONNECTING) break;
            if (le16(c + 4) == 1) break;            /* pending */
            if (le16(c + 4) != 0) {
                unsigned result = le16(c + 4);
                long delay = 250L << (ch->refused < 4 ? ch->refused : 4);
                log_line("pad %s: channel %#x refused (%u), trying again in %ld ms",
                         addr_str(l->addr), ch->psm, result, delay);
                ch->refused++;
                chan_set(ch, CH_CLOSED);
                ch->retry_at = now_ms() + delay;
                /* "No resources" for SDP over and over on a link just made:
                 * the pad is busy with something of its own (field reports on
                 * an 8BitDo). A fresh link works at once, so it is made
                 * instead of waiting out every refusal -- at most
                 * FRESH_LINK_TRIES per search, and the search is not
                 * stretched for it. */
                if (result == 4 && ch->psm == PSM_SDP && l->pairing && ch->refused >= 3 &&
                    ((host_storage *)h)->fresh_left > 0 &&
                    memcmp(((host_storage *)h)->last_call, l->addr, 6) == 0) {
                    ((host_storage *)h)->fresh_left--;
                    ((host_storage *)h)->repage_at = now_ms() + T_FRESH_LINK;
                    link_disconnect(h, l, "no resources for a channel: a fresh link is made");
                    break;
                }
                if (ch->refused >= CHAN_REFUSED_MAX)
                    link_disconnect(h, l, "channel refused eight times");
                break;
            }
            ch->dcid = le16(c);
            chan_set(ch, CH_CONFIG);
            chan_send_cfg(h, l, ch);
            break;

        case SIG_CFG_REQ:       /* dcid (ours), flags, options */
            if (clen < 4) break;
            ch = chan_by_scid(l, le16(c));
            /* Before our connection response arrived the pad's cid is not
             * known: let the channel time out and start over. */
            if (!ch || ch->st == CH_CONNECTING) break;
            put16(r, ch->dcid);
            put16(r + 2, 0);
            put16(r + 4, 0);            /* success, options as asked */
            sig_send(h, l, SIG_CFG_RSP, id, r, 6);
            ch->cfg_theirs_ok = 1;
            chan_check_open(l, ch);
            break;

        case SIG_CFG_RSP:       /* scid (ours), flags, result */
            if (clen < 6) break;
            ch = chan_by_scid(l, le16(c));
            if (!ch) break;
            if (le16(c + 4) == 0) {
                ch->cfg_ours_ok = 1;
                chan_check_open(l, ch);
            } else {
                log_line("pad %s: channel %#x configuration result %u",
                         addr_str(l->addr), ch->psm, le16(c + 4));
            }
            break;

        case SIG_DISC_REQ:      /* dcid (ours), scid */
            if (clen < 4) break;
            sig_send(h, l, SIG_DISC_RSP, id, c, 4);
            ch = chan_by_scid(l, le16(c));
            if (ch) chan_lost(h, l, ch);
            break;

        case SIG_DISC_RSP:
            break;

        case SIG_ECHO_REQ:
            sig_send(h, l, SIG_ECHO_RSP, id, c, clen < 32 ? clen : 32);
            break;

        case SIG_INFO_REQ:
            if (clen < 2) break;
            memset(r, 0, sizeof r);
            put16(r, le16(c));
            if (le16(c) == 2) {                 /* extended features: none */
                sig_send(h, l, SIG_INFO_RSP, id, r, 8);
            } else if (le16(c) == 3) {          /* fixed channels: signaling */
                r[4] = 0x02;
                sig_send(h, l, SIG_INFO_RSP, id, r, 12);
            } else {
                put16(r + 2, 1);                /* not supported */
                sig_send(h, l, SIG_INFO_RSP, id, r, 4);
            }
            break;

        case SIG_REJECT:
            log_line("pad %s: command rejected (reason %u)", addr_str(l->addr),
                     clen >= 2 ? le16(c) : 0);
            break;

        default:
            break;
        }
        d += 4 + clen;
        len -= 4 + clen;
    }
}

/* ---- SDP client: the Device ID record ------------------------------------- */

/* One data element header: sets type and content length, returns the
 * header length or 0 if it does not fit. */
static int de_header(const unsigned char *p, int avail, int *type, int *clen)
{
    static const int fixed[5] = { 1, 2, 4, 8, 16 };
    int size;

    if (avail < 1) return 0;
    *type = p[0] >> 3;
    size = p[0] & 7;
    if (*type == 0) { *clen = 0; return 1; }
    if (size < 5) { *clen = fixed[size]; return 1 + *clen <= avail ? 1 : 0; }
    if (size == 5) { if (avail < 2) return 0; *clen = p[1]; return 2 + *clen <= avail ? 2 : 0; }
    if (size == 6) { if (avail < 3) return 0; *clen = (int)be16(p + 1); return 3 + *clen <= avail ? 3 : 0; }
    if (avail < 5) return 0;
    {   /* 32-bit length: compared unsigned, as a huge one is no int */
        uint32_t big = be32(p + 1);
        if (big > (uint32_t)(avail - 5)) return 0;
        *clen = (int)big;
    }
    return 5;
}

/* Walks an attribute list depth-first, flattening the sequences: the
 * Device ID's vendor (0x0201) and product (0x0202) are each an attribute
 * id followed by a 16-bit value. Returns 1 if both were found. */
int sdp_find_ids(const unsigned char *p, int n, uint16_t *vid, uint16_t *pid)
{
    int i = 0, want = 0, got = 0;

    while (i < n) {
        int type, clen, hl = de_header(p + i, n - i, &type, &clen);

        if (!hl) return 0;
        if (type == 6 || type == 7) {   /* sequence: step inside */
            i += hl;
            continue;
        }
        if (type == 1 && clen == 2) {
            unsigned v = be16(p + i + hl);
            if (want == 1) { *vid = (uint16_t)v; got |= 1; want = 0; }
            else if (want == 2) { *pid = (uint16_t)v; got |= 2; want = 0; }
            else if (v == 0x0201) want = 1;
            else if (v == 0x0202) want = 2;
        } else {
            want = 0;
        }
        i += hl + clen;
    }
    return got == 3;
}

/* The HID descriptor list: {{0x22, descriptor bytes}}. Finds the bytes
 * that follow a one-byte 0x22 (report descriptor type). */
static int sdp_find_descriptor(const unsigned char *p, int n, const unsigned char **desc, int *dlen)
{
    int i = 0, after_type = 0;

    while (i < n) {
        int type, clen, hl = de_header(p + i, n - i, &type, &clen);

        if (!hl) return 0;
        if (type == 6 || type == 7) {
            i += hl;
            continue;
        }
        if (type == 4 && after_type) {          /* text string: the descriptor */
            *desc = p + i + hl;
            *dlen = clen;
            return 1;
        }
        after_type = type == 1 && clen == 1 && p[i + hl] == 0x22;
        i += hl + clen;
    }
    return 0;
}

static void sdp_send_request(host_t *h, link_t *l)
{
    /* Service Search Attribute requests, each answer up to 256 bytes:
     *   ids:  {UUID 0x1200 PnP Information}, attributes 0x0200-0x0205
     *   desc: {UUID 0x1124 HID}, attribute 0x0206 HIDDescriptorList */
    static const unsigned char ids[] = {
        0x35, 0x03, 0x19, 0x12, 0x00,  0x01, 0x00,
        0x35, 0x05, 0x0A, 0x02, 0x00, 0x02, 0x05,
    };
    static const unsigned char desc[] = {
        0x35, 0x03, 0x19, 0x11, 0x24,  0x01, 0x00,
        0x35, 0x03, 0x09, 0x02, 0x06,
    };
    const unsigned char *body = l->sdp_query == SDP_Q_IDS ? ids : desc;
    int blen = l->sdp_query == SDP_Q_IDS ? (int)sizeof ids : (int)sizeof desc;
    unsigned char req[64];
    int n = 5;

    memcpy(req + n, body, (size_t)blen);
    n += blen;
    req[n++] = (unsigned char)l->sdp_cont_len;
    memcpy(req + n, l->sdp_cont, (size_t)l->sdp_cont_len);
    n += l->sdp_cont_len;

    l->sdp_tid = (l->sdp_tid + 1) & 0xFFFF;
    req[0] = 0x06;
    req[1] = (unsigned char)(l->sdp_tid >> 8);
    req[2] = (unsigned char)l->sdp_tid;
    req[3] = (unsigned char)((n - 5) >> 8);
    req[4] = (unsigned char)(n - 5);
    l2_send(h, l, l->sdp.dcid, req, n);
    l->sdp_sent = 1;
    l->sdp_tries++;
    l->t_sdp = now_ms();
}

static void sdp_start(link_t *l, int query)
{
    l->sdp_query = query;
    l->sdp_sent = l->sdp_tries = 0;
    l->sdp_len = l->sdp_cont_len = 0;
}

/* A query is over: use what it brought, or what failing means. */
static void sdp_finish(host_t *h, link_t *l, int ok)
{
    int query = l->sdp_query;

    l->sdp_query = 0;
    if (query == SDP_Q_IDS) {
        uint16_t vid = 0, pid = 0;
        if (ok && sdp_find_ids(l->sdp_buf, l->sdp_len, &vid, &pid)) {
            db_rec *r = db_put(h, l->addr);
            r->vid = vid;
            r->pid = pid;
            db_save(h);
            log_line("pad %s: vendor %04x product %04x", addr_str(l->addr), vid, pid);
        } else {
            log_line("pad %s: no Device ID record, treating it as generic", addr_str(l->addr));
        }
        l->vid = vid;
        l->pid = pid;
        l->ids_known = 1;
    } else if (query == SDP_Q_DESC) {
        const unsigned char *desc;
        int dlen;
        if (ok && sdp_find_descriptor(l->sdp_buf, l->sdp_len, &desc, &dlen) &&
            generic_setup(l->pctx.bytes, desc, dlen, l->vid, l->pid, h->map_dir)) {
            l->prof = &generic_profile;
            l->desc_len = dlen > (int)sizeof l->desc ? (int)sizeof l->desc : dlen;
            if (l->desc_len > 0) memcpy(l->desc, desc, (size_t)l->desc_len);
        } else {
            link_disconnect(h, l, "no usable HID descriptor");
        }
    }
}

static void on_sdp(host_t *h, link_t *l, const unsigned char *d, int len)
{
    int bytes, cl;

    if (!l->sdp_query || !l->sdp_sent || len < 5) return;
    if (be16(d + 1) != l->sdp_tid) return;          /* an answer to an older try */
    if (d[0] != 0x07 || len < 8) {
        sdp_finish(h, l, 0);
        return;
    }
    bytes = (int)be16(d + 5);
    if (8 + bytes > len || l->sdp_len + bytes > (int)sizeof l->sdp_buf) {
        sdp_finish(h, l, 0);
        return;
    }
    memcpy(l->sdp_buf + l->sdp_len, d + 7, (size_t)bytes);
    l->sdp_len += bytes;
    cl = d[7 + bytes];
    if (cl > (int)sizeof l->sdp_cont || 8 + bytes + cl > len) {
        sdp_finish(h, l, 0);
        return;
    }
    if (cl) {                                       /* more to come: ask for it */
        memcpy(l->sdp_cont, d + 8 + bytes, (size_t)cl);
        l->sdp_cont_len = cl;
        l->sdp_tries = 0;
        sdp_send_request(h, l);
        return;
    }
    sdp_finish(h, l, 1);
}

/* Moves an SDP query along: channel, request, retries. */
static void sdp_step(host_t *h, link_t *l, long now)
{
    chan_tick(h, l, &l->sdp, now);
    if (l->sdp.st == CH_CLOSED) {
        chan_connect(h, l, &l->sdp);
    } else if (l->sdp.st == CH_OPEN && (!l->sdp_sent || now - l->t_sdp > T_SDP_RESEND)) {
        if (l->sdp_tries >= SDP_TRIES) {
            log_line("pad %s: SDP not answered", addr_str(l->addr));
            sdp_finish(h, l, 0);
        } else {
            sdp_send_request(h, l);
        }
    }
}

/* ---- HID ------------------------------------------------------------------ */

/* Sends an output report: buf[1..n] is the report, id first; buf[0] is
 * room for the Classic HID header. */
static int send_report(host_t *h, link_t *l, unsigned char *buf, int n)
{
    if (l->le) return le_send_report(h, l, buf + 1, n);
    buf[0] = HID_DATA_OUTPUT;
    return l2_send(h, l, l->intr.dcid, buf, n + 1);
}

static int send_output(host_t *h, link_t *l)
{
    unsigned char buf[128];
    int n;

    if (!l->ready || !l->prof || !l->prof->build_output) return 0;
    n = l->prof->build_output(l->pctx.bytes, &l->out, buf + 1, (int)sizeof buf - 1);
    if (n <= 0) return 0;
    l->t_out = now_ms();
    return send_report(h, l, buf, n);
}

static void on_hid(host_t *h, link_t *l, const chan *c, const unsigned char *d, int len)
{
    if (len < 1) return;
    if (c == &l->ctrl) {
        if (d[0] == HID_CONTROL_UNPLUG) link_disconnect(h, l, "virtual cable unplugged");
        return;                         /* handshakes and feature data unused */
    }
    if (d[0] != HID_DATA_INPUT || len < 2) return;
    link_input(h, l, d + 1, len - 1);
}

/* One input report, id first, from either transport. */
void link_input(host_t *h, link_t *l, const unsigned char *rep, int len)
{
    int r;

    if (h->dead || h->closing || !l->prof || len < 1) return;
    r = l->prof->parse(l->pctx.bytes, rep, len, &l->st);
    if (r == PARSE_FULL) l->full = 1;
    host_origin origin = captured_origin(h, l);
    if (((host_storage *)h)->qualified) {
        if (r != PARSE_NONE && l->ready)
            h->ev.on_state_qualified(h->ev.ud, slot_of(h, l), &origin, &l->st);
        if (!h->dead && l->ready)
            h->ev.on_report_qualified(h->ev.ud, slot_of(h, l), &origin, rep, len);
    } else {
        if (r != PARSE_NONE && l->ready && h->ev.on_state)
            h->ev.on_state(h->ev.ud, slot_of(h, l), &l->st);
        if (l->ready && h->ev.on_report)
            h->ev.on_report(h->ev.ud, slot_of(h, l), rep, len);
    }
}

/* Ready callbacks and snapshots describe the same observed peer, in HCI
 * (least-significant byte first) order. LE bond identities stay confined to
 * the paired-store APIs; neither those nor a default le==0 qualify Classic. */
static void fill_pad_info(const host_t *h, const link_t *l, pad_info *info)
{
    memset(info, 0, sizeof *info);
    info->vid = l->vid;
    info->pid = l->pid;
    info->profile = l->prof ? l->prof->name : "";
    memcpy(info->addr, l->addr, 6);
    if (l->desc_len > 0) {
        info->hid_desc = l->desc;
        info->hid_desc_len = l->desc_len;
    }
    info->le = l->le;
    if (!l->connected || l->disconnecting) return;
    if (l->le) {
        info->instance_transport = GC_INSTANCE_TRANSPORT_LE;
    } else if (((const host_storage *)h)->classic_acl[l - h->links]) {
        info->instance_transport = GC_INSTANCE_TRANSPORT_CLASSIC;
        if (l->ready && l->auth_ok && l->enc_on &&
            l->ctrl.st == CH_OPEN && l->intr.st == CH_OPEN)
            info->instance_proof = GC_INSTANCE_PROOF_CLASSIC_PEER;
    }
}

void link_ready(host_t *h, link_t *l)
{
    pad_info info;

    host_origin origin = captured_origin(h, l);
    if (h->dead || h->closing || !origin.occurrence || !l->prof) return;
    l->ready = 1;
    fill_pad_info(h, l, &info);
    log_line("pad %s: ready as %s in slot %d", addr_str(l->addr), l->prof->name, slot_of(h, l));
    if (((host_storage *)h)->qualified)
        h->ev.on_connect_qualified(h->ev.ud, slot_of(h, l), &origin, &info);
    else if (h->ev.on_connect) h->ev.on_connect(h->ev.ud, slot_of(h, l), &info);
}

/* Sends the pad's wake-up sequence one report at a time, and repeats it
 * while the pad has not switched to its full report. */
void init_tick(host_t *h, link_t *l, long now)
{
    unsigned char buf[128];
    int n;

    if (l->init_done || !l->prof || !l->prof->init_report) return;
    if (l->init_step < 0) {                     /* between rounds */
        if (l->full) l->init_done = 1;
        else if (now - l->t_init >= T_INIT_RETRY) l->init_step = 0;
        return;
    }
    if (now - l->t_init < T_INIT_STEP) return;

    n = l->prof->init_report(l->pctx.bytes, l->init_step, &l->out, buf + 1, (int)sizeof buf - 1);
    if (n > 0) {
        send_report(h, l, buf, n);
        l->init_step++;
        l->t_init = now;
        return;
    }
    l->init_rounds++;
    if (!l->prof->needs_full || l->full) {
        l->init_done = 1;
    } else if (l->init_rounds >= INIT_ROUNDS) {
        log_line("pad %s: never switched to its full report", addr_str(l->addr));
        l->init_done = 1;
    } else {
        l->init_step = -1;
        l->t_init = now;
    }
}

/* ---- inbound ACL ---------------------------------------------------------- */

static void on_frame(host_t *h, link_t *l, unsigned cid, const unsigned char *d, int len)
{
    chan *c;

    if (l->le) {
        le_on_frame(h, l, cid, d, len);
        return;
    }
    if (cid == CID_SIGNALING) {
        on_signaling(h, l, d, len);
        return;
    }
    c = chan_by_scid(l, cid);
    if (!c) return;
    if (c == &l->sdp) on_sdp(h, l, d, len);
    else on_hid(h, l, c, d, len);
}

static void on_acl(host_t *h, const unsigned char *pkt, int len)
{
    unsigned hdr;
    int pb, dlen;
    link_t *l;

    if (len < 4) { h->acl_malformed++; return; }
    hdr = le16(pkt);
    pb = (int)(hdr >> 12) & 3;
    dlen = (int)le16(pkt + 2);
    l = link_by_handle(h, hdr);
    if (!l) { h->acl_stale++; return; }             /* not one of our links */
    if (4 + dlen > len) { h->acl_malformed++; return; }   /* cut short */

    if (pb == 1) {                      /* continuation */
        if (!l->rx_need || l->rx_len + dlen > (int)sizeof l->rx) {
            l->rx_need = 0;
            h->acl_malformed++;
            return;
        }
        memcpy(l->rx + l->rx_len, pkt + 4, (size_t)dlen);
        l->rx_len += dlen;
    } else {                            /* start: a lost tail is abandoned */
        if (dlen < 4 || dlen > (int)sizeof l->rx) { h->acl_malformed++; return; }
        memcpy(l->rx, pkt + 4, (size_t)dlen);
        l->rx_len = dlen;
        l->rx_need = 4 + (int)le16(pkt + 4);
    }
    if (l->rx_need && l->rx_len >= l->rx_need) {
        int need = l->rx_need;
        l->rx_need = 0;
        on_frame(h, l, le16(l->rx + 2), l->rx + 4, need - 4);
    }
}

/* ---- HCI events: who a pad says it is, before any connection ------------- */

/* The class of device in an inquiry record (or a Connection Request). */
static uint32_t cod_of(const unsigned char *rec)
{
    return (uint32_t)rec[8] | (uint32_t)rec[9] << 8 | (uint32_t)rec[10] << 16;
}

/* Sony's own pads for the console: the DualSense and the DualSense Edge. The
 * system pairs and keeps them, and a second host pairing breaks the console's
 * own link. They are never paged, whatever the profile table lists. */
static int model_is_dualsense(unsigned vid, unsigned pid)
{
    return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2);
}

/* The model a pad announces before any connection: the extended inquiry
 * response's Device ID record, type 0x10 (source 1, vendor 2, product 2,
 * version 2 -- the length byte counts the type as well, so 8). */
static int eir_device_id(const unsigned char *eir, int n, unsigned *vid, unsigned *pid)
{
    int i;

    if (!eir) return 0;
    for (i = 0; i + 1 < n && eir[i] > 0 && i + 1 + eir[i] <= n; i += 1 + eir[i]) {
        if (eir[i + 1] == 0x10 && eir[i] >= 8) {
            /* length, type, source, vendor (2), product (2), version (2) */
            *vid = le16(eir + i + 3);
            *pid = le16(eir + i + 5);
            return 1;
        }
    }
    return 0;
}

/* The pad's name from its inquiry response (complete name wins over the short
 * one), "" if it sent none. */
static void eir_name(const unsigned char *eir, int n, char *out, int max)
{
    int i;

    out[0] = 0;
    if (!eir) return;
    for (i = 0; i + 1 < n && eir[i] > 0 && i + 1 + eir[i] <= n; i += 1 + eir[i]) {
        int type = eir[i + 1], len = eir[i] - 1;
        if ((type == 0x09 || type == 0x08) && len > 0) {
            if (len > max - 1) len = max - 1;
            memcpy(out, eir + i + 2, (size_t)len);
            out[len] = 0;
            if (type == 0x09) return;
        }
    }
}

/* Whether an extended answer carries at least one whole field. An empty one is
 * the case where the model is still unknown. */
static int eir_has_fields(const unsigned char *eir, int n)
{
    return eir && n >= 2 && eir[0] > 0 && 1 + eir[0] <= n;
}

/* Addresses the radio told us belong to the console's own pads, for this run
 * only. Nothing about this list is written to the store. */
static int is_banned(host_t *h, const unsigned char *addr)
{
    int i;

    for (i = 0; i < ((host_storage *)h)->n_ban; i++)
        if (memcmp(((host_storage *)h)->ban[i], addr, 6) == 0) return 1;
    return 0;
}

/* An address that answered "connection already exists" is not called again
 * straight away: the answer says the chip still holds that link, and calling
 * it again in the same poll would be a create/fail loop on a shared radio. */
static int is_held(host_t *h, const unsigned char *addr)
{
    long now = now_ms();
    int i;

    for (i = 0; i < 4; i++)
        if (((host_storage *)h)->hold[i].until > now &&
            memcmp(((host_storage *)h)->hold[i].addr, addr, 6) == 0) return 1;
    return 0;
}

static void hold_add(host_t *h, const unsigned char *addr)
{
    host_storage *bs = (host_storage *)h;
    int i = bs->hold_next++ & 3;

    memcpy(bs->hold[i].addr, addr, 6);
    bs->hold[i].until = now_ms() + T_EXIST_HOLD;
}

static void ban_add(host_t *h, const unsigned char *addr)
{
    host_storage *bs = (host_storage *)h;

    if (is_banned(h, addr)) return;
    if (bs->n_ban >= HOST_BAN_MAX) {
        if (!bs->ban_full) log_line("inquiry: exclusion list full; no pad is called without an identity");
        bs->ban_full = 1;
        return;
    }
    memcpy(bs->ban[bs->n_ban++], addr, 6);
}

/* A call already made to an address that turned out to be excluded is
 * cancelled, never allowed to complete. */
static void cancel_call_to(host_t *h, const unsigned char *addr)
{
    link_t *l = link_by_addr(h, addr);

    if (!l || l->connected || !l->remote_pending || l->cancel_sent) return;
    l->cancel_sent = 1;
    send_cmd(h, OP_CREATE_CONNECTION_CANCEL, addr, 6);
    log_line("pad %s: the pending call is cancelled: it is a DualSense", addr_str(addr));
}

static void pair_with(host_t *h, const unsigned char *rec)
{
    unsigned char p[13];
    link_t *l;

    if (h->closing || link_by_addr(h, rec)) return;
    if (db_find(h, rec)) {
        log_line("pad %s: in pairing mode again, forgetting its old key", addr_str(rec));
        db_forget(h, rec);
    }
    l = link_new(h, rec);
    if (!l) {
        log_line("pairing: no free slot");
        return;
    }
    l->pairing = 1;
    l->cod = cod_of(rec);
    memcpy(((host_storage *)h)->last_call, rec, sizeof ((host_storage *)h)->last_call);
    if (!((host_storage *)h)->in_repage)
        ((host_storage *)h)->fresh_left = FRESH_LINK_TRIES;
    if (h->inquiring) send_cmd(h, OP_INQUIRY_CANCEL, NULL, 0);

    memcpy(p, rec, 6);
    put16(p + 6, 0xCC18);               /* DM1..DH5 */
    p[8] = rec[6];                      /* page scan repetition mode */
    p[9] = 0;
    put16(p + 10, le16(rec + 11) | 0x8000);   /* clock offset, valid */
    p[12] = 1;                          /* allow role switch */
    l->remote_pending = 1;
    send_cmd(h, OP_CREATE_CONNECTION, p, 13);
    log_line("pad %s: connecting to pair", addr_str(rec));
}

/* An inquiry record: addr 6, page scan mode 1, reserved 1, class 3,
 * clock offset 2, RSSI 1. `eir` is the extended inquiry response when the
 * result came with one, NULL for the older result events. */
static void on_found(host_t *h, const unsigned char *rec, const unsigned char *eir, int eir_len)
{
    uint32_t cod = cod_of(rec);
    unsigned vid = 0, pid = 0;
    int have_id = eir_device_id(eir, eir_len, &vid, &pid);

    if (now_ms() >= h->pair_until) return;
    if (is_banned(h, rec)) return;                  /* already known: not looked at */
    if (is_held(h, rec)) return;                    /* its stale link is still there */
    if (!profile_class_is_gamepad(cod)) {
        log_line("inquiry: %s class %06x is not a gamepad, ignored", addr_str(rec), (unsigned)cod);
        return;
    }
    if (have_id && model_is_dualsense(vid, pid)) {
        /* The console's own pad. Its identity is remembered for this run so
         * later answers without an EIR are not paged either, and a call
         * already made from an earlier, less complete answer is cancelled. */
        static unsigned char last[6];
        static long last_at;
        if (memcmp(last, rec, 6) != 0 || now_ms() - last_at > 30000) {
            memcpy(last, rec, 6);
            last_at = now_ms();
            log_line("inquiry: %s is a DualSense (%04x:%04x), left alone", addr_str(rec), vid, pid);
        }
        ban_add(h, rec);
        cancel_call_to(h, rec);
        if (((host_storage *)h)->defer_at && memcmp(((host_storage *)h)->defer_rec, rec, 6) == 0) {
            ((host_storage *)h)->defer_at = 0;
            log_line("inquiry: the wait for %s is dropped: it is a DualSense", addr_str(rec));
        }
        return;
    }
    /* An extended answer with no field at all says nothing about the model.
     * Wait -- once, bounded -- for a complete answer before calling it. */
    if (eir && !eir_has_fields(eir, eir_len)) {
        host_storage *bs = (host_storage *)h;
        if (!bs->defer_at) {
            memcpy(bs->defer_rec, rec, sizeof bs->defer_rec);
            bs->defer_at = now_ms();
            log_line("inquiry: %s answered without data; waiting up to %d s for a complete answer",
                     addr_str(rec), T_EIR_WAIT / 1000);
        }
        return;
    }
    if (((host_storage *)h)->ban_full && !have_id) {
        log_line("inquiry: %s has no identity and the exclusion list is full; not called", addr_str(rec));
        return;
    }
    if (((host_storage *)h)->defer_at && memcmp(((host_storage *)h)->defer_rec, rec, 6) == 0)
        ((host_storage *)h)->defer_at = 0;
    log_line("found gamepad %s (class %06x)", addr_str(rec), (unsigned)cod);
    pair_with(h, rec);
    {
        link_t *nl = link_by_addr(h, rec);
        if (nl) eir_name(eir, eir_len, nl->name, (int)sizeof nl->name);
    }
}

/* "Connection already exists" while paging a pad. The radio is shared with the
 * system's driver, so an unknown handle can be the console's own DualSense
 * link: handles are never swept, and a link is never closed on a guess. */
static void report_existing_link(host_t *h, const unsigned char *addr)
{
    if (h->purge_at && now_ms() - h->purge_at < 60000) return;
    h->purge_at = now_ms();
    log_line("pad %s: a connection already exists and the controller named no handle for it; "
             "left alone (it may be the console's own link)", addr_str(addr));
}

/* The one "connection already exists" answer that names a handle: a Connection
 * Complete with status 0x0B carrying the existing link's handle for the address
 * this process was paging. That link is the one we asked for, so it -- and only
 * it -- is closed, to make the next call succeed. */
static void close_named_existing_link(host_t *h, link_t *l, unsigned handle)
{
    unsigned char p[3];

    if (!l->pairing || !l->remote_pending || l->connected || handle == 0 || handle > 0x0EFF) return;
    if (is_banned(h, l->addr)) return;
    put16(p, handle);
    p[2] = 0x13;                        /* remote user terminated */
    send_cmd(h, OP_DISCONNECT, p, 3);
    log_line("pad %s: connection already exists on handle %#05x, the link this process asked for: closing it",
             addr_str(l->addr), handle);
}

static void on_event(host_t *h, const unsigned char *ev, int len)
{
    unsigned char p[32];
    link_t *l;
    int i;

    if (len < 2) return;
    if (h->closing && ev[0] != 0x01 && ev[0] != 0x03 && ev[0] != 0x04 &&
        ev[0] != 0x05 && ev[0] != 0x0E && ev[0] != 0x0F && ev[0] != 0x3E) return;
    switch (ev[0]) {
    case 0x01:  /* Inquiry Complete */
        log_line("inquiry: finished (status %#04x)", len > 2 ? ev[2] : 0xFF);
        h->inquiring = 0;
        break;

    case 0x02:  /* Inquiry Result: addr, scan mode, 2 reserved, class, clock */
        for (i = 0; i < ev[2] && 3 + (i + 1) * 14 <= len; i++) {
            const unsigned char *r = ev + 3 + i * 14;
            unsigned char rec[14];
            memcpy(rec, r, 7);          /* to the RSSI layout on_found reads */
            rec[7] = 0;
            memcpy(rec + 8, r + 9, 5);
            rec[13] = 0;
            on_found(h, rec, NULL, 0);
        }
        break;

    case 0x22:  /* Inquiry Result with RSSI */
        for (i = 0; i < ev[2] && 3 + (i + 1) * 14 <= len; i++)
            on_found(h, ev + 3 + i * 14, NULL, 0);
        break;

    case 0x2F:  /* Extended Inquiry Result: record, then its response */
        if (len >= 17) on_found(h, ev + 3, ev + 17, len - 17);
        break;

    case 0x04:  /* Connection Request: addr, class, link type */
        if (len < 12 || ev[11] != 1) break;
        {
            db_rec *r = db_find(h, ev + 2);
            if (!r || r->le || link_by_addr(h, ev + 2)) break;      /* not ours */
        }
        if (h->closing) {
            memcpy(p, ev + 2, 6); p[6] = 0x0D;
            send_cmd(h, OP_REJECT_CONNECTION, p, 7); break;
        }
        l = link_new(h, ev + 2);
        memcpy(p, ev + 2, 6);
        if (!l) {
            p[6] = 0x0D;                /* limited resources */
            send_cmd(h, OP_REJECT_CONNECTION, p, 7);
            break;
        }
        p[6] = 0x00;                    /* become central, as a host does */
        l->remote_pending = 2;
        send_cmd(h, OP_ACCEPT_CONNECTION, p, 7);
        log_line("pad %s: reconnecting", addr_str(ev + 2));
        break;

    case 0x03:  /* Connection Complete: status, handle, addr, type, enc */
        if (len < 13 || !(l = link_by_addr(h, ev + 5)) || l->connected) break;
        if (ev[2] != 0) {
            if (ev[2] == 0x0B) {
                unsigned named = le16(ev + 3) & 0x0FFF;
                hold_add(h, l->addr);
                if (named) close_named_existing_link(h, l, named);
                else report_existing_link(h, l->addr);
            }
            log_line("pad %s: connection failed (status %#04x)", addr_str(l->addr), ev[2]);
            link_free(h, l);
            break;
        }
        /* Event 0x03 is Classic; only a successful ACL (not SCO/eSCO)
         * completion on a non-LE link establishes observed Classic transport.
         * Keep legacy connection handling unchanged for unqualified records. */
        *classic_acl(h, l) = !l->le && ev[11] == 1;
        l->remote_pending = 0;
        if (!h->closing && !link_origin_begin(h, l, le16(ev + 3))) {
            l->connected = 1; l->handle = le16(ev + 3) & 0x0FFF;
            link_disconnect(h, l, "origin unavailable"); break;
        }
        l->connected = 1;
        l->handle = le16(ev + 3) & 0x0FFF;
        l->t_conn = now_ms();
        if (h->closing) link_disconnect(h, l, "closing completion");
        log_line("pad %s: connected, handle %#05x", addr_str(l->addr), l->handle);
        break;

    case 0x05:  /* Disconnection Complete: status, handle, reason */
        if (len < 6 || !(l = link_by_handle(h, le16(ev + 3)))) break;
        if (ev[2] != 0) break; /* Rejection is not link termination. */
        log_line("pad %s: disconnected (reason %#04x)", addr_str(l->addr), ev[5]);
        link_free(h, l);
        break;

    case 0x06:  /* Authentication Complete: status, handle */
        if (len < 5 || !(l = link_by_handle(h, le16(ev + 3)))) break;
        if (ev[2] == 0) {
            l->auth_ok = 1;
            l->t_auth_ok = now_ms();
            break;
        }
        log_line("pad %s: authentication failed (status %#04x)", addr_str(l->addr), ev[2]);
        if (!l->pairing) db_forget(h, l->addr);     /* the pad lost its key */
        link_disconnect(h, l, "authentication failed");
        break;

    case 0x08:  /* Encryption Change: status, handle, enabled */
        if (len < 6 || !(l = link_by_handle(h, le16(ev + 3)))) break;
        if (l->le) {
            le_on_encryption(h, l, ev[2], ev[5]);
            break;
        }
        /* A disabled or failed encryption transition cannot retain peer proof. */
        l->enc_on = ev[2] == 0 && ev[5] != 0;
        if (l->enc_on)
            l->auth_ok = 1;             /* encryption implies it */
        break;

    case 0x30:  /* Encryption Key Refresh Complete: status, handle */
        if (len < 5 || !(l = link_by_handle(h, le16(ev + 3))) || !l->le) break;
        le_on_encryption(h, l, ev[2], ev[2] == 0);
        break;

    case 0x3E:  /* LE Meta */
        if (len >= 3) le_on_meta(h, ev + 2, len - 2);
        break;

    case 0x0E:  /* Command Complete: credits, opcode, return parameters */
        if (len < 6) break;
        h->cc_op = le16(ev + 3);
        h->cc_len = len < (int)sizeof h->cc ? len : (int)sizeof h->cc;
        memcpy(h->cc, ev, (size_t)h->cc_len);
        if (le16(ev + 3) == 0x200C && h->le_scan_pending > 0) --h->le_scan_pending;
        if (le16(ev + 3) == OP_INQUIRY_CANCEL && ev[5] == 0) h->inquiring = 0;
        if (!h->closing) le_on_cmd_complete(h, le16(ev + 3), ev + 5, len - 5);
        break;

    case 0x0F:  /* Command Status: status, credits, opcode */
        if (len >= 6 && le16(ev + 4) == OP_INQUIRY) {
            log_line("inquiry: controller answered status %#04x", ev[2]);
            if (ev[2] == 0) ((host_storage *)h)->inquiry_backoff = 0;
            if (ev[2] != 0) {
                host_storage *bs = (host_storage *)h;
                /* Refused: the radio is busy (the system's driver has it).
                 * Nothing is re-sent before the wait doubles. */
                h->inquiring = 0;
                bs->inquiry_backoff = bs->inquiry_backoff ? bs->inquiry_backoff * 2 : T_INQUIRY_RETRY;
                if (bs->inquiry_backoff > T_INQUIRY_RETRY_MAX) bs->inquiry_backoff = T_INQUIRY_RETRY_MAX;
                bs->inquiry_at = now_ms() + bs->inquiry_backoff;
            }
        }
        if (len >= 6 && le16(ev + 4) == OP_INQUIRY_CANCEL && ev[2] != 0) {
            /* "Command disallowed" to a cancel is the controller saying no
             * inquiry is running, which is exactly the state a lost Inquiry
             * Complete left us believing otherwise. Either way the flag is
             * cleared and the next poll may start one; the watchdog timer is
             * not re-armed here, so a controller that keeps refusing is only
             * asked again at the watchdog interval. */
            if (h->inquiring) log_line("inquiry: cancel refused (status %#04x): no inquiry is running", ev[2]);
            if (ev[2] == 0x0C) h->inquiring = 0;
        }
        if (len >= 6 && (le16(ev + 4) == 0x200D || le16(ev + 4) == 0x200E)) {
            /* LE owns cancellation/backoff, including refused Create. The
             * Classic cleanup below must not retire its pending link. */
            if (h->closing) {
                if (le16(ev + 4) == 0x200D && ev[2] != 0)
                    for (i = 0; i < HOST_MAX_PADS; ++i)
                        if (h->links[i].used && h->links[i].le && !h->links[i].connected)
                            link_free(h, &h->links[i]);
            } else le_on_create_status(h, ev + 2, 4);
            break;
        }
        if (len < 6 || ev[2] == 0) break;
        if (le16(ev + 4) == OP_CREATE_CONNECTION && ev[2] == 0x0B) {
            /* A Command Status names no handle, and the radio is shared:
             * nothing is closed on this answer. The links it refers to are
             * retired below, so the search moves on. */
            for (i = 0; i < HOST_MAX_PADS; i++)
                if (h->links[i].used && h->links[i].pairing && !h->links[i].connected) {
                    hold_add(h, h->links[i].addr);
                    report_existing_link(h, h->links[i].addr);
                }
        }
        if (le16(ev + 4) == OP_CREATE_CONNECTION || le16(ev + 4) == 0x200D) {
            int want_le = le16(ev + 4) == 0x200D;
            for (i = 0; i < HOST_MAX_PADS; i++) {
                l = &h->links[i];
                if (l->used && !l->connected && l->le == want_le &&
                    (l->pairing || want_le)) {
                    log_line("pad %s: connection refused (status %#04x)", addr_str(l->addr), ev[2]);
                    link_free(h, l);
                }
            }
        } else if (le16(ev + 4) == OP_AUTH_REQUESTED) {
            /* Usually a collision with the pad's own authentication:
             * wait and see, and ask again if nothing comes of it. */
            for (i = 0; i < HOST_MAX_PADS; i++) {
                l = &h->links[i];
                if (l->used && l->auth_sent && !l->auth_ok) {
                    l->auth_sent = 0;
                    l->t_conn = now_ms();
                }
            }
        }
        break;

    case 0x13:  /* Number Of Completed Packets: (handle, count) pairs */
        for (i = 0; i < ev[2] && 3 + i * 4 + 4 <= len; i++) {
            int done = (int)le16(ev + 3 + i * 4 + 2);
            l = link_by_handle(h, le16(ev + 3 + i * 4));
            if (l) pool_done(pool_of(h, l), done);
        }
        break;

    case 0x16:  /* PIN Code Request: legacy pairing */
        if (len < 8 || !(l = link_by_addr(h, ev + 2))) break;
        memset(p, 0, sizeof p);
        memcpy(p, ev + 2, 6);
        /* Nintendo's Wii pads paired with their Sync button take the host's
         * own address as their PIN, its 6 bytes as read from the controller.
         * Every other pad keeps the legacy "0000" and Classic Simple Pairing
         * behaviour, which these pads never reach. */
        if (strncmp(l->name, "Nintendo RVL-", 13) == 0 || l->cod == 0x002504) {
            p[6] = 6;
            memcpy(p + 7, h->bdaddr, 6);
            log_line("pad %s: PIN requested; answering with this controller's address (Wii pads)",
                     addr_str(l->addr));
        } else {
            p[6] = 4;
            memcpy(p + 7, "0000", 4);
        }
        send_cmd(h, OP_PIN_CODE_REPLY, p, 23);
        break;

    case 0x17:  /* Link Key Request */
        if (len < 8 || !link_by_addr(h, ev + 2)) break;
        {
            db_rec *r = db_find(h, ev + 2);
            if (r) {
                memcpy(p, ev + 2, 6);
                memcpy(p + 6, r->key, 16);
                send_cmd(h, OP_LINK_KEY_REPLY, p, 22);
            } else {
                send_cmd(h, OP_LINK_KEY_NEG_REPLY, ev + 2, 6);
            }
        }
        break;

    case 0x18:  /* Link Key Notification: addr, key, type */
        if (len < 25 || !(l = link_by_addr(h, ev + 2))) break;
        {
            db_rec *r = db_put(h, ev + 2);
            memcpy(r->key, ev + 8, 16);
            r->type = ev[24];
            const int saved = db_save(h);
            log_line("pad %s: paired, %s", addr_str(ev + 2),
                     saved ? "key stored" : "key only in memory; reconnect after restart requires pairing");
        }
        break;

    case 0x31:  /* IO Capability Request */
        if (len < 8 || !link_by_addr(h, ev + 2)) break;
        memcpy(p, ev + 2, 6);
        p[6] = 0x03;                    /* NoInputNoOutput */
        p[7] = 0x00;                    /* no OOB data */
        p[8] = 0x04;                    /* general bonding, no MITM */
        send_cmd(h, OP_IO_CAP_REPLY, p, 9);
        break;

    case 0x33:  /* User Confirmation Request */
        if (len < 8 || !link_by_addr(h, ev + 2)) break;
        send_cmd(h, OP_USER_CONFIRM_REPLY, ev + 2, 6);
        break;

    default:
        break;
    }
}

/* ---- per-link state machine ---------------------------------------------- */

/* Advances one link from whatever has happened so far. Each step waits
 * first for the pad to take it, then takes it itself, so it works for pads
 * that drive their own reconnection and for those that wait to be asked. */
static void link_step(host_t *h, link_t *l, long now)
{
    unsigned char p[3];

    if (l->le) {
        le_step(h, l, now);
        return;
    }
    if (!l->connected) {
        if (now - l->t_create > T_CONN_TIMEOUT) link_disconnect(h, l, "no connection");
        return;
    }
    if (l->disconnecting) return;
    if (!l->ready && now - l->t_conn > T_SETUP_TIMEOUT) {
        link_disconnect(h, l, "setup did not finish");
        return;
    }

    /* Authentication and encryption. */
    if (!l->auth_ok) {
        if (!l->auth_sent && now - l->t_conn >= (l->pairing ? 0 : T_SELF_AUTH)) {
            put16(p, l->handle);
            send_cmd(h, OP_AUTH_REQUESTED, p, 2);
            l->auth_sent = 1;
        }
        return;
    }
    if (!l->enc_on) {
        if (!l->enc_sent && now - l->t_auth_ok >= (l->pairing ? 0 : T_SELF_ENCRYPT)) {
            put16(p, l->handle);
            p[2] = 1;
            send_cmd(h, OP_SET_ENCRYPTION, p, 3);
            l->enc_sent = 1;
        }
        return;
    }
    if (!l->t_secure) {
        l->t_secure = now;
        log_line("pad %s: link encrypted", addr_str(l->addr));
    }

    /* Which pad is it: the store, the pad's Device ID record, and for pads
     * with no profile, the pad's own HID descriptor. */
    if (!l->ids_known && !l->sdp_query) {
        db_rec *r = db_find(h, l->addr);
        if (r && (r->vid || r->pid)) {
            l->vid = r->vid;
            l->pid = r->pid;
            l->ids_known = 1;
        } else {
            sdp_start(l, SDP_Q_IDS);
        }
    }
    if (l->ids_known && !l->prof && !l->sdp_query) {
        l->prof = profile_find(l->vid, l->pid);
        if (l->prof && l->prof->setup) l->prof->setup(l->pctx.bytes, l->vid, l->pid);
        if (!l->prof) {
            log_line("pad %s: %04x:%04x has no profile, reading its HID descriptor",
                     addr_str(l->addr), l->vid, l->pid);
            sdp_start(l, SDP_Q_DESC);
        }
    }
    if (l->sdp_query) {
        sdp_step(h, l, now);
        return;
    }
    if (l->disconnecting || !l->prof) return;
    if (l->sdp.st == CH_OPEN) chan_close(h, l, &l->sdp);

    /* HID channels: control, then interrupt. */
    chan_tick(h, l, &l->ctrl, now);
    chan_tick(h, l, &l->intr, now);
    if (h->dead || !l->prof) return; /* a channel send may have retired this link */
    if (l->ctrl.st == CH_CLOSED && now - l->t_secure >= T_SELF_CHANNELS)
        chan_connect(h, l, &l->ctrl);
    if (l->ctrl.st == CH_OPEN && l->intr.st == CH_CLOSED &&
        now - l->ctrl.t_state >= T_INTR_AFTER)
        chan_connect(h, l, &l->intr);

    if (!l->ready) {
        if (l->ctrl.st == CH_OPEN && l->intr.st == CH_OPEN) link_ready(h, l);
        return;
    }

    init_tick(h, l, now);
    /* init_tick's send may lose the transport and clear this link */
    if (h->dead || !l->prof) return;
    if (l->prof->rumble_refresh_ms && (l->out.strong || l->out.weak) &&
        now - l->t_out >= l->prof->rumble_refresh_ms)
        send_output(h, l);
}

/* ---- pumping ---------------------------------------------------------------- */

static void pump(host_t *h, int timeout_ms)
{
    unsigned char buf[HCI_PKT_MAX];
    int n;

    if (h->dead) return;
    if (h->hci.ops->pump(h->hci.ctx, timeout_ms) < 0) { transport_lost(h); return; }
    if (!h->closing && h->ev.idle) h->ev.idle(h->ev.ud);
    if (h->hci.ops->next_packet) {
        int kind;
        while (!h->dead && (n = h->hci.ops->next_packet(h->hci.ctx, &kind, buf, sizeof buf)) > 0) {
            if ((kind == HCI_PACKET_EVENT && (n < 2 || n != 2 + buf[1])) ||
                (kind == HCI_PACKET_ACL && (n < 4 || n != 4 + (int)le16(buf + 2))) ||
                (kind != HCI_PACKET_EVENT && kind != HCI_PACKET_ACL)) {
                transport_lost(h); return;
            }
            h->last_rx = h->heard_ms = now_ms();
            if (kind == HCI_PACKET_EVENT) { ++h->events_handled; on_event(h, buf, n); }
            else if (!h->closing) { ++h->acl_handled; on_acl(h, buf, n); }
        }
        if (h->dead || n < 0) transport_lost(h);
        return;
    }
    /* Historical nonresident mode only. Never grants qualified provenance. */
    while (!h->dead && (n = h->hci.ops->next_event(h->hci.ctx, buf, sizeof buf)) > 0) {
        h->last_rx = h->heard_ms = now_ms(); ++h->events_handled; on_event(h, buf, n);
    }
    if (n < 0) { transport_lost(h); return; }
    while (!h->dead && (n = h->hci.ops->next_acl(h->hci.ctx, buf, sizeof buf)) > 0) {
        h->last_rx = h->heard_ms = now_ms(); if (!h->closing) { ++h->acl_handled; on_acl(h, buf, n); }
    }
    if (n < 0) transport_lost(h);
}

/* Cancellation is asynchronous too: do not send it every service tick while
 * waiting for its completion/status event. Failed sends need the same bound. */
static void cancel_inquiry(host_t *h)
{
    host_storage *bs = (host_storage *)h;
    const long now = now_ms();
    if (now < bs->inquiry_cancel_at) return;
    bs->inquiry_cancel_at = now + T_INQUIRY_RETRY;
    send_cmd(h, OP_INQUIRY_CANCEL, NULL, 0);
}

static void start_inquiry(host_t *h)
{
    /* General inquiry, ~10 s, unlimited responses. */
    static const unsigned char inq[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };

    if (send_cmd(h, OP_INQUIRY, inq, 5)) {
        host_storage *bs = (host_storage *)h;

        h->inquiring = 1;
        bs->t_inquiry = now_ms();
        bs->inquiry_cancel_at = 0;
        bs->inquiry_at = 0;
        /* Reset the backoff only when the controller accepts the inquiry. */
        log_line("inquiry: started");
    } else {
        host_storage *bs = (host_storage *)h;
        /* A failed send has no Command Status to schedule the next attempt. */
        bs->inquiry_backoff = bs->inquiry_backoff ? bs->inquiry_backoff * 2 : T_INQUIRY_RETRY;
        if (bs->inquiry_backoff > T_INQUIRY_RETRY_MAX) bs->inquiry_backoff = T_INQUIRY_RETRY_MAX;
        bs->inquiry_at = now_ms() + bs->inquiry_backoff;
        static int fails;
        if (fails++ < 5) log_line("inquiry: the command could not be sent");
    }
}

void host_poll(host_t *h, int timeout_ms)
{
    long now;
    int i, linking = 0;

    if (!h) return;                     /* no Bluetooth: nothing to do */

    pump(h, timeout_ms);
    if (h->dead) { transport_lost(h); return; }
    if (h->closing) return;
    now = now_ms();
    for (i = 0; i < HOST_MAX_PADS; i++) {
        if (!h->links[i].used) continue;
        link_step(h, &h->links[i], now);
        if (h->dead) { transport_lost(h); return; }
        if (h->links[i].used && h->links[i].pairing && !h->links[i].ready &&
            !h->links[i].le) linking = 1;
    }
    if (h->dead) { transport_lost(h); return; }
    le_poll(h, now);
    if (h->dead) { transport_lost(h); return; }

    /* Bound silence recovery without resetting a potentially shared radio.
     * The engine releases this failed transport and waits for explicit Retry. */
    if (now - h->last_rx > T_PROBE_IDLE) {
        if (h->probes >= PROBE_TRIES && now - h->probe_at > T_PROBE_WAIT) {
            if (!h->dead) {
                log_line("controller silent after %d probes: transport lost", h->probes);
                h->dead = 1;
            }
        } else if (h->probes < PROBE_TRIES && now - h->probe_at > T_PROBE_WAIT) {
            send_cmd(h, OP_READ_BUFFER_SIZE, NULL, 0);     /* any reply will do */
            h->probe_at = now;
            h->probes++;
            h->probes_sent++;
        }
    } else {
        h->probes = 0;
        h->reset_tried = 0;
    }

    {
        host_storage *bs = (host_storage *)h;

        /* A gamepad found with no identity in its answer: the wait is over,
         * so it is called now (bounded, never indefinite). */
        if (bs->defer_at && now - bs->defer_at >= T_EIR_WAIT) {
            unsigned char rec[14];
            memcpy(rec, bs->defer_rec, sizeof rec);
            bs->defer_at = 0;
            log_line("found gamepad %s; no complete answer came in time, calling it", addr_str(rec));
            pair_with(h, rec);
            linking = 0;
            for (i = 0; i < HOST_MAX_PADS; i++)
                if (h->links[i].used && h->links[i].pairing && !h->links[i].ready &&
                    !h->links[i].le)
                    linking = 1;
        }
        /* A pad that had no channel resources gets one fresh link, once. */
        if (bs->repage_at && now >= bs->repage_at) {
            unsigned char rec[14];
            memcpy(rec, bs->last_call, sizeof rec);
            bs->repage_at = 0;
            bs->in_repage = 1;
            pair_with(h, rec);
            bs->in_repage = 0;
            linking = 0;
            for (i = 0; i < HOST_MAX_PADS; i++)
                if (h->links[i].used && h->links[i].pairing && !h->links[i].ready &&
                    !h->links[i].le)
                    linking = 1;
        }
        /* An inquiry that outlived any inquiry: its completion was lost on
         * the shared radio. Ask for the end; a refused cancel says it already
         * ended, and the flag is cleared either way by the answer. */
        if (h->inquiring && now - bs->t_inquiry > T_INQUIRY_WATCHDOG) {
            bs->t_inquiry = now;        /* asked again at most once per interval */
            log_line("inquiry: no completion after %d s, ending it", T_INQUIRY_WATCHDOG / 1000);
            cancel_inquiry(h);
        }
        /* Keep searching through the pairing window, one pad at a time. */
        if (now < h->pair_until && !h->inquiring && !linking && !bs->repage_at &&
            !bs->defer_at && now >= bs->inquiry_at)
            start_inquiry(h);
    }
    if (now >= h->pair_until && h->inquiring) {
        cancel_inquiry(h);
    }
}

int host_pair(host_t *h, int seconds)
{
    if (h->dead || h->closing) return 0;
    /* A new search starts from a clean slate: no wait inherited from the last
     * one. The session's excluded addresses are kept -- they are identities
     * of the console's own pads, not search state. */
    ((host_storage *)h)->defer_at = 0;
    ((host_storage *)h)->repage_at = 0;
    ((host_storage *)h)->fresh_left = FRESH_LINK_TRIES;
    h->pair_until = now_ms() + (long)seconds * 1000;
    log_line("pairing: searching for %d s", seconds);
    return 1;
}

int host_set_output(host_t *h, int slot, const pad_output *out)
{
    link_t *l;

    if (h->dead || h->closing || slot < 0 || slot >= HOST_MAX_PADS) return 0;
    l = &h->links[slot];
    if (!l->used || !l->ready) return 0;
    l->out = *out;
    return send_output(h, l);
}

int host_connected(const host_t *h, int slot)
{
    return slot >= 0 && slot < HOST_MAX_PADS && h->links[slot].used && h->links[slot].ready;
}

int host_paired_count(const host_t *h) { return h->ndb; }

void host_diag_get(const host_t *h, host_diag *out)
{
    memset(out, 0, sizeof *out);
    if (!h) return;
    if (h->hci.ops && h->hci.ops->stats) {
        h->hci.ops->stats(h->hci.ctx, &out->transport);
        out->transport_measured = 1;
    } else {
        out->transport.iface = -1;
    }
    out->acl_mtu = h->acl_mtu;
    out->acl_buffers = h->br.max;
    out->le = h->le_ok;
    out->events_handled = h->events_handled;
    out->acl_handled = h->acl_handled;
    out->acl_stale = h->acl_stale;
    out->acl_malformed = h->acl_malformed;
    out->tx_no_credit = h->tx_no_credit;
    out->tx_failed = h->tx_failed;
    out->probes_sent = h->probes_sent;
    out->reset_attempts = h->reset_attempts;
    out->last_rx_ms = h->heard_ms;
}

int host_transport_lost(const host_t *h) { return h->dead; }

int host_pairing_left(const host_t *h)
{
    long left = h->pair_until - now_ms();
    return left > 0 ? (int)((left + 999) / 1000) : 0;
}

/* The address a link's pad is stored under: its identity, for LE pads
 * behind private addresses. */
static const unsigned char *link_identity(const link_t *l)
{
    static const unsigned char zero[6];
    if (l->le && memcmp(l->le_s.id_addr, zero, 6) != 0) return l->le_s.id_addr;
    return l->addr;
}

int host_paired_get(const host_t *h, int i, host_paired_info *out)
{
    const db_rec *d;
    int k;

    if (i < 0 || i >= h->ndb) return 0;
    d = &h->db[i];
    memset(out, 0, sizeof *out);
    memcpy(out->addr, d->addr, 6);
    out->vid = d->vid;
    out->pid = d->pid;
    out->le = d->le;
    for (k = 0; k < HOST_MAX_PADS; k++) {
        const link_t *l = &h->links[k];
        if (l->used && l->ready && memcmp(link_identity(l), d->addr, 6) == 0) out->connected = 1;
    }
    return 1;
}

int host_pad_get(const host_t *h, int slot, pad_info *info, pad_state *st)
{
    const link_t *l;

    if (slot < 0 || slot >= HOST_MAX_PADS) return 0;
    l = &h->links[slot];
    if (!l->used || !l->ready) return 0;
    fill_pad_info(h, l, info);
    *st = l->st;
    return 1;
}

int host_pad_get_qualified(host_t *h, int slot, host_origin *origin, pad_info *info, pad_state *state)
{
    if (!h || h->dead || h->closing || !origin || !info || !state ||
        !host_pad_get(h, slot, info, state)) return 0;
    *origin = ((host_storage *)h)->origins[slot];
    return origin->occurrence != 0;
}

int host_forget(host_t *h, const unsigned char addr[6])
{
    int k;

    if (h->closing || !db_find(h, addr)) return 0;
    for (k = 0; k < HOST_MAX_PADS; k++) {
        link_t *l = &h->links[k];
        if (l->used && memcmp(link_identity(l), addr, 6) == 0)
            link_disconnect(h, l, "forgotten from the web page");
    }
    if (!db_forget(h, addr)) {
        log_line("pad %s: forgotten in memory; store write failed", addr_str(addr));
        return 0;
    }
    log_line("pad %s: forgotten", addr_str(addr));
    return 1;
}

/* ---- controller setup ----------------------------------------------------- */

/* Sends a command and waits for its Command Complete. The reply can go to
 * the system's driver, so the command is sent again; those used here are
 * safe to repeat. */
int hci_sync(host_t *h, unsigned op, const void *p, int plen)
{
    int attempt;

    for (attempt = 1; attempt <= 3; attempt++) {
        long deadline = now_ms() + (attempt == 3 ? 3000 : 1000);

        h->cc_op = 0;
        if (!send_cmd(h, op, p, plen)) return 0;
        while (!h->dead && h->cc_op != op && now_ms() < deadline) pump(h, 20);
        if (h->dead) { transport_lost(h); return 0; }
        if (h->cc_op == op) return h->cc_len > 5 && h->cc[5] == 0;
    }
    log_line("command %#06x: no reply", op);
    if (!h->diag_done && h->hci.ops->diag) {
        h->diag_done = 1;
        h->hci.ops->diag(h->hci.ctx);
    }
    return 0;
}

/* Prepares the controller, changing as little of it as it can. The
 * controller is shared with the system's driver, so:
 *   - no reset, no name, no class of device, no inquiry mode;
 *   - the event mask: needed, as Secure Simple Pairing events (IO
 *     capability, user confirmation) are off in the default mask. There is
 *     no command to read it back, so it is left as set;
 *   - Simple Pairing mode: only written if off. It cannot be turned off
 *     again once on, by specification;
 *   - page scan: needed for paired pads to reconnect by themselves. The
 *     value found is kept and put back on close, unless someone else has
 *     changed it meanwhile. */
static int setup(host_t *h)
{
    static const unsigned char mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F };
    static const unsigned char one[1] = { 1 };
    unsigned char scan[1];

    h->scan_orig = h->scan_set = -1;
    if (!hci_sync(h, OP_READ_BUFFER_SIZE, NULL, 0) || h->cc_len < 13) return 0;
    h->acl_mtu = le16(h->cc + 6);
    h->br.credits = h->br.max = (int)le16(h->cc + 9);
    log_line("controller: %d ACL buffers of %u bytes", h->br.max, h->acl_mtu);

    if (!hci_sync(h, OP_SET_EVENT_MASK, mask, 8)) return 0;
    if (!hci_sync(h, OP_READ_SSP_MODE, NULL, 0) || h->cc_len < 7 || h->cc[6] != 1) {
        if (!hci_sync(h, OP_WRITE_SSP_MODE, one, 1)) return 0;
        log_line("controller: Simple Pairing turned on");
    }
    if (!hci_sync(h, OP_READ_SCAN_ENABLE, NULL, 0) || h->cc_len < 7) return 0;
    h->scan_orig = h->cc[6];
    if (!(h->scan_orig & 0x02)) {
        scan[0] = (unsigned char)(h->scan_orig | 0x02);
        ((host_storage *)h)->setup_scan_uncertain = 1;
        if (!hci_sync(h, OP_WRITE_SCAN_ENABLE, scan, 1)) {
            /* A rejecting Command Complete proves that write was rejected;
             * absent/failed observation cannot prove it never took effect. */
            if (h->cc_op == OP_WRITE_SCAN_ENABLE && h->cc_len > 5 && h->cc[5] != 0)
                ((host_storage *)h)->setup_scan_uncertain = 0;
            return 0;
        }
        ((host_storage *)h)->setup_scan_uncertain = 0;
        h->scan_set = scan[0];
        log_line("controller: page scan turned on (was %#x)", h->scan_orig);
    }

    if (hci_sync(h, OP_READ_BD_ADDR, NULL, 0) && h->cc_len >= 12) memcpy(h->bdaddr, h->cc + 6, 6);
    log_line("controller address %s", addr_str(h->bdaddr));
    if (h->dead) return 0;
    le_setup(h);
    return !h->dead;
}

int host_open_checked(hci_t hci, const char *db_path, const host_events *ev, host_t **out)
{
    *out = NULL;
    host_storage *storage = calloc(1, sizeof *storage);
    host_t *h = storage ? &storage->host : NULL;

    if (!h) return -1;
    int q = ev ? !!ev->on_connect_qualified + !!ev->on_state_qualified +
        !!ev->on_disconnect_qualified + !!ev->on_report_qualified : 0;
    int legacy = ev && (ev->on_connect || ev->on_state || ev->on_disconnect || ev->on_report);
    if (!hci.ops || (q && (q != 4 || legacy || !hci.ops->next_packet))) { free(h); return -1; }
    storage->qualified = q == 4;
    h->hci = hci;
    if (ev) h->ev = *ev;
    snprintf(h->db_path, sizeof h->db_path, "%s", db_path);
    {
        const char *slash = strrchr(db_path, '/');
        int dir = slash ? (int)(slash - db_path) : 1;
        snprintf(h->map_dir, sizeof h->map_dir, "%.*s/maps", dir, slash ? db_path : ".");
    }
    db_load(h);
    gatt_cache_load(h);
    h->last_rx = now_ms();
    *out = h; /* even failed setup retains its actual allocation and HCI */
    return setup(h) ? 0 : -1;
}
host_t *host_open(hci_t hci, const char *db_path, const host_events *ev)
{
    host_t *h = NULL;
    if (host_open_checked(hci, db_path, ev, &h) != 0) { free(h); return NULL; }
    return h;
}

static const char *transport_release_blocker(host_t *h)
{
    if (!h) return NULL;
    if (!h->hci.ops || !h->hci.ops->close_checked)
        return "bt_transport_checked_close_unavailable";
    if (h->hci.ops->release_blocker) {
        const char *why = h->hci.ops->release_blocker(h->hci.ctx);
        if (why) return why;
    }
    if (((host_storage *)h)->setup_scan_uncertain) return "bt_setup_scan_write_uncertain";
    if (((host_storage *)h)->lost_connections) return "bt_lost_transport_link_release_uncertain";
    if (h->dead) {
        for (int i = 0; i < HOST_MAX_PADS; ++i)
            if (h->links[i].used) return "bt_lost_transport_link_release_uncertain";
        if (h->le_scanning || h->le_scan_owned || h->scan_set >= 0)
            return "bt_lost_transport_scan_restore_uncertain";
    }
    return NULL;
}
const char *host_release_blocker(host_t *h)
{
    const char *why = transport_release_blocker(h);
    if (why || !h || !h->closing) return why;
    if (h->inquiring) return "bt_inquiry_shutdown_pending";
    if (h->le_scan_owned) return "bt_le_scan_shutdown_pending";
    for (int i = 0; i < HOST_MAX_PADS; ++i)
        if (h->links[i].used) return "bt_link_shutdown_pending";
    return NULL;
}
int host_close_checked(host_t *h)
{
    if (!h) return 0;
    h->closing = 1;
    h->pair_until = 0;
    if (transport_release_blocker(h)) return -1;
    if (h->inquiring && !hci_sync(h, OP_INQUIRY_CANCEL, NULL, 0)) return -1;
    for (int i = 0; i < HOST_MAX_PADS; ++i)
        if (h->links[i].used) link_disconnect(h, &h->links[i], "closing");
    long deadline = now_ms() + 2000;
    for (;;) {
        int any = 0;
        for (int i = 0; i < HOST_MAX_PADS; ++i) any |= h->links[i].used;
        if (!any) break;
        if (h->dead || now_ms() >= deadline) return -1;
        pump(h, 50);
    }
    /* Same commands/contracts as restore/le_shutdown, but retain original
     * ownership until Command Complete actually confirms each operation. */
    if (h->le_scan_owned) {
        /* Do not mistake an older enable completion for the disable ACK. */
        deadline = now_ms() + 2000;
        while (h->le_scan_pending) {
            if (h->dead || now_ms() >= deadline) return -1;
            pump(h, 20);
        }
        const uint8_t off[2] = {0, 0};
        /* OP_LE_SET_SCAN_ENABLE, existing le.c:30/199 contract. */
        if (!hci_sync(h, 0x200C, off, 2)) return -1;
        h->le_scanning = 0; h->le_scan_out = 0; h->le_scan_owned = 0;
    }
    if (h->scan_set >= 0) {
        unsigned char scan[1] = { (unsigned char)h->scan_orig };
        if (!hci_sync(h, OP_READ_SCAN_ENABLE, NULL, 0) || h->cc_len < 7) return -1;
        if (h->cc[6] == h->scan_set && !hci_sync(h, OP_WRITE_SCAN_ENABLE, scan, 1)) return -1;
        h->scan_set = -1; /* restored or an observed successor owns it */
    }
    /* Restore commands also pump completions: a late successful connection
     * remains owned until its disconnect completes, never hidden by restore. */
    if (host_release_blocker(h)) return -1;
    if (h->hci.ops->close_checked(h->hci.ctx) != 0) return -1;
    free(h);
    return 0;
}
/* Legacy void-only providers retain their historical teardown contract.
 * This path is never a checked release witness and the resident uses only
 * host_close_checked. Providers with a checked hook always fail closed. */
void host_close(host_t *h)
{
    if (!h) return;
    if (h->hci.ops && h->hci.ops->close_checked) {
        (void)host_close_checked(h); return;
    }
    h->closing = 1;
    for (int i = 0; i < HOST_MAX_PADS; ++i) {
        if (!h->links[i].used) continue;
        if (h->dead) link_free(h, &h->links[i]);
        else link_disconnect(h, &h->links[i], "closing");
    }
    long deadline = now_ms() + 2000;
    for (;;) {
        int any = 0;
        for (int i = 0; i < HOST_MAX_PADS; ++i) any |= h->links[i].used;
        if (!any || now_ms() >= deadline) break;
        pump(h, 50);
    }
    for (int i = 0; i < HOST_MAX_PADS; ++i)
        if (h->links[i].used) link_free(h, &h->links[i]);
    if (!h->dead) {
        le_shutdown(h);
        if (h->scan_set >= 0 && hci_sync(h, OP_READ_SCAN_ENABLE, NULL, 0) &&
            h->cc_len >= 7 && h->cc[6] == h->scan_set) {
            unsigned char scan[1] = { (unsigned char)h->scan_orig };
            (void)hci_sync(h, OP_WRITE_SCAN_ENABLE, scan, 1);
        }
    }
    if (h->hci.ops && h->hci.ops->close) h->hci.ops->close(h->hci.ctx);
    free(h);
}

void host_abandon(host_t *h)
{
    if (!h) return;
    h->closing = 1;
    for (int i = 0; i < HOST_MAX_PADS; ++i)
        if (h->links[i].used) link_free(h, &h->links[i]);
    if (h->hci.ops && h->hci.ops->close) h->hci.ops->close(h->hci.ctx);
    free(h);
}
