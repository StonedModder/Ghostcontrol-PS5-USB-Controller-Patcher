/* The host's internals, shared by the Classic (host.c) and LE (le.c) halves
 * of the stack. Not for use outside them. */
#ifndef PH_HOST_INT_H
#define PH_HOST_INT_H

#include "host.h"
#include "profiles.h"

#include <stdint.h>

/* ---- protocol constants ------------------------------------------------- */

/* HCI opcodes: OGF << 10 | OCF. */
#define OP_INQUIRY              0x0401
#define OP_INQUIRY_CANCEL       0x0402
#define OP_CREATE_CONNECTION    0x0405
#define OP_CREATE_CONNECTION_CANCEL 0x0408
#define OP_DISCONNECT           0x0406
#define OP_ACCEPT_CONNECTION    0x0409
#define OP_REJECT_CONNECTION    0x040A
#define OP_LINK_KEY_REPLY       0x040B
#define OP_LINK_KEY_NEG_REPLY   0x040C
#define OP_PIN_CODE_REPLY       0x040D
#define OP_AUTH_REQUESTED       0x0411
#define OP_SET_ENCRYPTION       0x0413
#define OP_IO_CAP_REPLY         0x042B
#define OP_USER_CONFIRM_REPLY   0x042C
#define OP_SET_EVENT_MASK       0x0C01
#define OP_WRITE_LOCAL_NAME     0x0C13
#define OP_WRITE_SCAN_ENABLE    0x0C1A
#define OP_WRITE_CLASS_OF_DEV   0x0C24
#define OP_WRITE_INQUIRY_MODE   0x0C45
#define OP_WRITE_SSP_MODE       0x0C56
#define OP_READ_BUFFER_SIZE     0x1005
#define OP_READ_BD_ADDR         0x1009
#define OP_READ_SCAN_ENABLE     0x0C19
#define OP_READ_SSP_MODE        0x0C55

#define CID_SIGNALING   0x0001
#define PSM_SDP         0x0001
#define PSM_HID_CTRL    0x0011
#define PSM_HID_INTR    0x0013

/* Our ends of a link's channels (CIDs are per link). */
#define CID_SDP         0x0040
#define CID_CTRL        0x0041
#define CID_INTR        0x0042

/* L2CAP signaling codes. */
#define SIG_REJECT      0x01
#define SIG_CONN_REQ    0x02
#define SIG_CONN_RSP    0x03
#define SIG_CFG_REQ     0x04
#define SIG_CFG_RSP     0x05
#define SIG_DISC_REQ    0x06
#define SIG_DISC_RSP    0x07
#define SIG_ECHO_REQ    0x08
#define SIG_ECHO_RSP    0x09
#define SIG_INFO_REQ    0x0A
#define SIG_INFO_RSP    0x0B

/* HID transaction headers. */
#define HID_DATA_INPUT  0xA1
#define HID_DATA_OUTPUT 0xA2
#define HID_CONTROL_UNPLUG 0x15

/* ---- timing (ms) -------------------------------------------------------- */

#define T_SELF_AUTH      800    /* a reconnecting pad may authenticate itself */
#define T_SELF_ENCRYPT  1500    /* ... and start encryption itself */
#define T_SELF_CHANNELS 1200    /* ... and open its own HID channels */
#define T_INTR_AFTER     500    /* interrupt channel after control */
#define T_CONN_TIMEOUT 20000    /* paging a pad */
#define T_SETUP_TIMEOUT 30000   /* connection to ready */
#define T_CHAN_CONNECT  2500
#define T_CFG_RESEND    1200
#define CFG_TRIES          6
#define T_CFG_TIMEOUT   4000    /* the pad's own request may have been lost */
#define T_SDP_RESEND    2000
#define SDP_TRIES          3
#define T_INIT_STEP       60    /* between wake-up reports */
#define T_INIT_RETRY     800    /* before repeating an unanswered sequence */
#define INIT_ROUNDS        5

#define T_INQUIRY_WATCHDOG 13000 /* a 10.24 s inquiry is over by then: the
                                 * completion event was lost on the shared radio */
#define T_INQUIRY_RETRY     500 /* wait after a refused inquiry command ... */
#define T_INQUIRY_RETRY_MAX 8000 /* ... doubling up to this */
#define T_EIR_WAIT         8000 /* a gamepad that answered without an identity:
                                 * how long to wait for a complete answer
                                 * before calling it blind */
#define T_FRESH_LINK       3000 /* between dropping a link with no channel
                                 * resources and calling the pad again */
#define FRESH_LINK_TRIES      1 /* such links made again in one search */
#define CHAN_REFUSED_MAX      8 /* channel refusals before the link is dropped */
#define T_EXIST_HOLD       5000 /* an address that answered "already exists" is
                                 * not called again for this long: a stale link
                                 * must not become a call per poll */
#define LINK_NAME_MAX        40 /* a pad's name from its inquiry response */
#define HOST_BAN_MAX         16 /* session-only excluded addresses; never stored */

#define SDP_Q_IDS          1    /* Device ID: vendor and product */
#define SDP_Q_DESC         2    /* HID report descriptor, for the generic profile */
#define T_PROBE_IDLE   20000    /* nothing heard for this long: probe */
#define T_PROBE_WAIT    2000    /* between probes */
#define PROBE_TRIES        3

/* LE connection retry timing. */
#define T_LE_SCAN_OFF    300    /* scan disable to be answered */
#define T_LE_STATUS     1000    /* Create Connection status to arrive */
#define T_LE_CANCEL     1000    /* cancel to be answered */
#define T_LE_CANCEL_END 1000    /* Connection Complete (0x02) to arrive */
#define T_LE_ADDR_GAP   2000    /* per-address retry gap */
#define T_LE_ADDR_REST  5000    /* address rest after all retries */
#define LE_RETRIES          3
static const long k_le_backoff[LE_RETRIES] = { 300, 800, 1500 };

/* ACL flow control on the shared controller: a few Number Of Completed
 * Packets reports go to the system's driver, so a packet without a report
 * after INFLIGHT_MS is taken as sent. */
#define INFLIGHT_MAX      64
#define INFLIGHT_MS       60

#define DB_MAX            16
#define DB_REC            64    /* one record in the store file */
#define DB_MAGIC     "GHOSTT02"

/* ---- state -------------------------------------------------------------- */

typedef enum { CH_CLOSED, CH_CONNECTING, CH_CONFIG, CH_OPEN } chan_state;

typedef struct {
    unsigned psm, scid, dcid;
    chan_state st;
    int cfg_ours_ok;        /* the pad accepted our configuration */
    int cfg_theirs_ok;      /* we accepted the pad's */
    int cfg_tries;
    long t_state, t_cfg;
    int refused;            /* connection responses refusing this channel */
    long retry_at;          /* no new request before this (refusal backoff) */
} chan;

/* A paired pad. Classic pads keep a link key; LE pads a long-term key
 * (with its EDIV and Rand under legacy pairing) and possibly an identity
 * resolving key, to recognise them behind private addresses. */
typedef struct {
    unsigned char addr[6], key[16], type;
    uint16_t vid, pid;
    uint8_t le, addr_type;
    uint16_t ediv;
    uint8_t rand[8], irk[16], has_irk;
} db_rec;

#define LE_MAX_CHARS 24

typedef struct {
    uint16_t decl, value, end, uuid;
    uint8_t props;
    uint8_t rid, rtype;             /* from its Report Reference */
    uint16_t ref, cccd;             /* descriptor handles, 0 if none */
} gatt_char;

/* GATT discovery cache: stored per-pad so reconnections skip the full
 * discovery (MTU→PnP→service→chars→descs→refs→map→CCCD) and go straight
 * to enabling notifications. */
#define GATT_CACHE_MAGIC  "GATTCH01"
#define GATT_CACHE_MAX    16

typedef struct {
    unsigned char addr[6];
    uint8_t nch;
    uint8_t pad;
    uint16_t svc_start, svc_end, map_handle;
    gatt_char ch[LE_MAX_CHARS];
} gatt_cache_rec;

typedef struct {
    int credits, max;
    long inflight[INFLIGHT_MAX];
    int head, count;
} acl_pool;



/* An LE pad's pairing (SMP) and GATT discovery. Crypto values are kept
 * most significant byte first, as the specification writes them. */
typedef struct {
    uint8_t addr_type;

    int smp;                        /* SMP_* */
    int sc;                         /* LE Secure Connections */
    uint8_t preq[7], pres[7];       /* the pairing commands, as sent */
    uint8_t na[16], nb[16], cb[16];
    uint8_t pkb[64];                /* the pad's public key, as sent */
    uint8_t dhkey[32];              /* least significant byte first */
    int have_pkb, have_cb, have_nb, have_dhkey, dhkey_asked;
    uint8_t ltk[16];                /* the key the link is encrypted with */
    uint8_t keys_expected, keys_got;
    uint8_t dist_ltk[16], dist_rand[8], dist_irk[16], id_addr[6], id_type;
    uint16_t dist_ediv;
    int enc_sent, bonded;
    long t_smp, t_enc;

    int gatt;                       /* G_* */
    uint16_t mtu;
    uint8_t req[32];
    int req_len, req_tries;
    long t_req;
    uint16_t svc_start, svc_end, cursor;
    gatt_char ch[LE_MAX_CHARS];
    int nch, idx;
    uint16_t map_handle;
    uint8_t map[1024];
    int map_len;

    /* Connection state machine. */
    int cstate;                     /* LC_* */
    long t_cstate, t_retry;
    int ctries;
    int st_scan, st_create, st_cancel;
    int cancel_done, cancel_needed, saw02;
    unsigned char addr02[6];
} le_link;

typedef struct {
    int remote_pending, cancel_sent; /* controller operation, not enqueue completion */
    int used;
    unsigned char addr[6];
    int connected;          /* the ACL link is up */
    unsigned handle;
    int pairing;            /* we found it in pairing mode */
    uint32_t cod;           /* class of device, as its inquiry record said */
    char name[LINK_NAME_MAX]; /* name from its inquiry response, "" if none */
    long t_create, t_conn;

    int auth_sent, auth_ok, enc_sent, enc_on;
    long t_auth_ok, t_secure;

    int ids_known;
    uint16_t vid, pid;
    const pad_profile *prof;

    int sdp_query;                  /* SDP_Q_* being asked, 0: none */
    int sdp_sent, sdp_tries;
    unsigned sdp_tid;
    long t_sdp;
    unsigned char sdp_cont[16];     /* continuation of a long answer */
    int sdp_cont_len;
    unsigned char sdp_buf[1024];    /* attribute lists gathered so far */
    int sdp_len;

    chan sdp, ctrl, intr;
    int ready, disconnecting;

    unsigned char rx[1024];
    int rx_len, rx_need;

    pad_state st;
    pad_output out;
    long t_out;

    /* A copy of the pad's HID report descriptor, kept so link_ready can hand
     * it to a host above after SDP/GATT has read it. */
    unsigned char desc[1024];
    int desc_len;

    /* The profile's own memory, and its wake-up sequence. */
    union { unsigned char bytes[PROFILE_CTX_MAX]; uint64_t align; } pctx;
    int init_step, init_rounds, init_done, full;
    long t_init;

    int le;                         /* a Bluetooth LE link */
    le_link le_s;
} link_t;

struct host {
    int closing; /* sticky: no admission or reconnect after any close attempt */
    hci_t hci;
    host_events ev;
    char db_path[256];
    char map_dir[256];
    db_rec db[DB_MAX];
    int ndb;

    link_t links[HOST_MAX_PADS];
    unsigned char sig_id;

    unsigned cc_op;
    unsigned char cc[HCI_PKT_MAX];
    int cc_len;

    unsigned acl_mtu;
    acl_pool br;                    /* the controller's ACL buffers */

    /* Bluetooth LE (le.c) */
    int le_ok;                      /* the controller does LE */
    int le_separate;                /* LE has its own buffers */
    unsigned le_mtu;
    acl_pool lep;
    unsigned char bdaddr[6];        /* our address */
    unsigned char le_pk[64];        /* our P-256 public key, as sent */
    int le_pk_ok;
    int le_scanning, le_scan_want;
    int le_scan_pending, le_scan_owned; /* ordered scan command acknowledgements */
    int le_scan_out;                /* outstanding scan enable commands */
    int le_scan_last;               /* last scan enable value sent */
    long t_scan;
    link_t *dhkey_owner;            /* the link a DHKey is being made for */

    /* LE address hold: no attempt on an address until a given time. */
    struct { unsigned char addr[6]; long until; } le_hold[4];
    int le_hold_next;

    /* GATT cache: survives restarts, keyed by pad address. */
    gatt_cache_rec gatt_cache[GATT_CACHE_MAX];
    int ngatt_cache;

    long pair_until;
    long purge_at;                  /* when stale links were last cleared */
    int inquiring;
    int dead;               /* the transport is gone */
    int diag_done;          /* the transport's self-report was logged */
    int reset_tried;        /* a USB port reset was attempted */
    int scan_orig, scan_set; /* page scan as found, and as we set it (-1: untouched) */
    long last_rx, probe_at; /* liveness: anything heard, last probe */
    int probes;

    /* Diagnostics: what the host itself saw (host_diag_get). */
    unsigned events_handled, acl_handled;
    unsigned acl_stale;     /* ACL for a handle that is not one of our links */
    unsigned acl_malformed; /* cut short, bad continuation or oversize */
    unsigned tx_no_credit;  /* frames dropped: no controller buffer free */
    unsigned tx_failed;     /* frames the transport refused */
    unsigned probes_sent, reset_attempts; /* attempts, whatever the transport did */
    long heard_ms;          /* last packet actually received; 0: none yet
                             * (last_rx is also seeded at open for liveness) */
};


/* ---- shared between host.c and le.c ---- */

const char *addr_str(const unsigned char *a);
int  send_cmd(host_t *h, unsigned op, const void *p, int len);
int  hci_sync(host_t *h, unsigned op, const void *p, int plen);
int  slot_of(host_t *h, const link_t *l);
link_t *link_by_addr(host_t *h, const unsigned char *a);
link_t *link_new(host_t *h, const unsigned char *addr);
void link_free(host_t *h, link_t *l);
void link_disconnect(host_t *h, link_t *l, const char *why);
void link_ready(host_t *h, link_t *l);
int link_origin_begin(host_t *h, link_t *l, unsigned handle);
void link_input(host_t *h, link_t *l, const unsigned char *rep, int len);
int  l2_send(host_t *h, link_t *l, unsigned cid, const unsigned char *d, int len);
void init_tick(host_t *h, link_t *l, long now);

db_rec *db_find(host_t *h, const unsigned char *addr);
db_rec *db_put(host_t *h, const unsigned char *addr);
void db_save(host_t *h);
void db_forget(host_t *h, const unsigned char *addr);

/* le.c */
void le_setup(host_t *h);
void le_on_meta(host_t *h, const unsigned char *ev, int len);
void le_on_create_status(host_t *h, const uint8_t *r, int len);
void le_on_cmd_complete(host_t *h, unsigned opcode, const uint8_t *p, int len);
void le_abandon(host_t *h, link_t *l);
void gatt_cache_save(host_t *h);
void gatt_cache_load(host_t *h);
gatt_cache_rec *gatt_cache_find(host_t *h, const unsigned char *addr);
void gatt_cache_put(host_t *h, link_t *l);
void le_on_frame(host_t *h, link_t *l, unsigned cid, const unsigned char *d, int len);
void le_on_encryption(host_t *h, link_t *l, int status, int on);
void le_step(host_t *h, link_t *l, long now);
void le_poll(host_t *h, long now);
void le_shutdown(host_t *h);
int  le_send_report(host_t *h, link_t *l, const unsigned char *rep, int len);

#endif
