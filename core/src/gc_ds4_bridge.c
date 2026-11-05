/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_ds4_bridge.h"
#include "gc_ds4_game.h"
#include "gc_remote.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int32_t sceUserServiceGetLoginUserIdList(int32_t users[4]);
extern void ghostpad_status_log(const char *, ...);
extern int sceKernelOpenEventFlag(intptr_t *, const char *);
extern int sceKernelPollEventFlag(intptr_t, uint64_t, unsigned, uint64_t *);
extern int sceKernelCloseEventFlag(intptr_t);

static pthread_mutex_t lifecycle = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static int started, cleanup_failed;
static atomic_int stopping;
static GcConfigStore *config_store;
static GcState *service_state;
static GcDs4SourceReader source;
static GcDs4GameBridge game;
static GcWirelessState status;

static uint64_t now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int suspend_pending(intptr_t info_flag, intptr_t status_flag)
{
    uint64_t pattern;
    if (info_flag >= 0 && !sceKernelPollEventFlag(info_flag, UINT64_MAX, 2, &pattern)) {
        unsigned state = (unsigned)(pattern & 0xffffu);
        if (state == 100 || state == 300 || state == 500) return 1;
    }
    return status_flag >= 0 && !sceKernelPollEventFlag(status_flag, UINT64_MAX, 2, &pattern) &&
        (pattern & UINT64_C(0x200000));
}

static void publish_status(GcWirelessState *value, const char *message)
{
    if (strcmp(value->status, message)) {
        snprintf(value->status, sizeof(value->status), "%s", message);
        ghostpad_status_log("[GC DS4] %s\n", message);
    }
    pthread_mutex_lock(&status_lock);
    status = *value;
    pthread_mutex_unlock(&status_lock);
    gc_state_set_wireless(service_state, value);
}

static int stop_targets(void)
{
    int game_result = gc_ds4_game_remove(&game);
    int source_result = gc_ds4_source_stop(&source);
    return game_result || source_result ? -1 : 0;
}

static int update_sources(const GcDs4SourceSnapshot *snapshot)
{
    int changed = snapshot->source_count != source.source_count;
    for (unsigned i = 0; i < snapshot->source_count && !changed; ++i)
        changed = memcmp(&source.sources[i], &snapshot->frames[i].source,
                          sizeof(source.sources[i])) != 0;
    if (changed) {
        if (gc_ds4_game_remove(&game)) return -1;
        source.source_count = snapshot->source_count;
        memset(source.sources, 0, sizeof(source.sources));
        for (unsigned i = 0; i < source.source_count; ++i)
            source.sources[i] = snapshot->frames[i].source;
    }
    return changed;
}

static void *bridge_worker(void *unused)
{
    (void)unused;
    GcWirelessState health = {0};
    GcProfile profiles[GC_DS4_MAX_ROUTES];
    GcRuntimeConfig runtime = {.wireless_ds4 = 1};
    int32_t reader_users[4] = {-1, -1, -1, -1};
    uint64_t config_generation = 0, profile_generation = UINT64_MAX;
    uint64_t next_lifecycle = 0, next_discovery = 0, next_status = 0;
    uint64_t last_frame = 0, last_tick = now_ns();
    uint32_t last_sequence = 0;
    int source_live = 0;
    int available = gc_remote_available();
    int suspended = 0;
    intptr_t info_flag = -1, system_flag = -1;
    if (sceKernelOpenEventFlag(&info_flag, "SceSystemStateMgrInfo")) info_flag = -1;
    if (sceKernelOpenEventFlag(&system_flag, "SceSystemStateMgrStatus")) system_flag = -1;
    const char *message = "Waiting for a wireless DS4";
    while (!atomic_load(&stopping)) {
        uint64_t tick = now_ns();
        if (!suspended && tick > last_tick + UINT64_C(1000000000)) {
            /* A suspend or scheduling gap invalidates both sides before resuming. */
            (void)stop_targets();
            source_live = 0;
            next_discovery = tick + UINT64_C(1000000000);
            message = "Recovering after a pause";
        }
        last_tick = tick;
        if (tick >= next_lifecycle) {
            next_lifecycle = tick + UINT64_C(100000000);
            int sleeping = suspend_pending(info_flag, system_flag);
            if (sleeping && !suspended) {
                (void)gc_ds4_game_quiesce(&game);
                (void)gc_ds4_source_request_stop(&source);
                suspended = 1; source_live = 0;
                message = "Suspended";
            } else if (!sleeping && suspended) {
                (void)stop_targets(); suspended = 0;
                next_discovery = now_ns() + UINT64_C(1000000000);
                last_tick = now_ns();
                message = "Recovering after resume";
            }
            if (config_store)
                (void)gc_config_snapshot(config_store, &runtime, &config_generation);
            gc_ds4_game_observe_exit(&game);
            if (source_live && gc_ds4_source_heartbeat(&source)) {
                (void)stop_targets(); source_live = 0;
                message = "Wireless reader heartbeat failed; recovering";
                last_tick = now_ns();
            }
        }
        if (suspended) {
            /* Only poll local lifecycle flags until a resume boundary. */
        } else if (!runtime.wireless_ds4 || !available) {
            if (source.pid || game.state == GC_DS4_GAME_LIVE) (void)stop_targets();
            source_live = 0;
            message = available ? "Disabled" : "Waiting for remote syscall support";
            if (tick >= next_discovery) {
                next_discovery = tick + UINT64_C(3000000000);
                if (!available) available = gc_remote_available();
            }
        } else if (tick >= next_discovery) {
            next_discovery = tick + UINT64_C(1000000000);
            int32_t users[4] = {-1, -1, -1, -1};
            if (sceUserServiceGetLoginUserIdList(users) != 0)
                memset(users, 0xff, sizeof(users));
            if (source_live && memcmp(users, reader_users, sizeof(users))) {
                (void)stop_targets(); source_live = 0;
            }
            if (!source_live) {
                if (source.pid && gc_ds4_source_stop(&source))
                    message = "Reader cleanup pending";
                else if (users[0] >= 0 && !gc_ds4_source_start(users, 4, &source)) {
                    source_live = 1;
                    memcpy(reader_users, users, sizeof(users));
                    profile_generation = UINT64_MAX;
                    last_sequence = 0; last_frame = now_ns();
                } else message = "Waiting for a wireless DS4 in RemotePlay";
                /* Discovery can take time; do not mistake our own call for suspend. */
                last_tick = now_ns();
            }
            if (source_live && source.source_count && game.state != GC_DS4_GAME_LIVE) {
                if (game.state == GC_DS4_GAME_RETAINED_QUARANTINED)
                    (void)gc_ds4_game_remove(&game);
                if (!gc_ds4_game_install(&game, &source)) message = "Bridge installed; waiting for game input";
                else message = game.state == GC_DS4_GAME_RETAINED_QUARANTINED
                    ? "Game cleanup pending; restart the game" : "Waiting for a compatible game and exact pad route";
            }
        }
        if (runtime.wireless_ds4 && source_live) {
            GcDs4SourceSnapshot snapshot;
            if (!gc_ds4_source_snapshot(&source, &snapshot) && snapshot.sequence != last_sequence) {
                last_sequence = snapshot.sequence; last_frame = now_ns(); ++health.frames;
                int changed = update_sources(&snapshot);
                if (changed < 0) message = "Game cleanup pending; restart the game";
                else {
                    if (changed || profile_generation != config_generation) {
                        for (unsigned slot = 0; slot < snapshot.source_count; ++slot) {
                            gc_profile_defaults(&profiles[slot]);
                            if (config_store) (void)gc_config_select_profile(config_store,
                                snapshot.frames[slot].source.vendor_id,
                                snapshot.frames[slot].source.product_id, "ds4_wireless",
                                &profiles[slot], NULL);
                        }
                        profile_generation = config_generation;
                    }
                    for (unsigned slot = 0; slot < snapshot.source_count; ++slot)
                        gc_profile_apply(&profiles[slot], &snapshot.frames[slot].data);
                    if (game.state == GC_DS4_GAME_LIVE) {
                        if (gc_ds4_game_publish(&game, &snapshot)) {
                            (void)gc_ds4_game_remove(&game);
                            message = "Game bridge publication failed; retrying";
                        } else ++health.publications;
                    }
                }
            } else if (now_ns() > last_frame + UINT64_C(250000000)) {
                (void)stop_targets(); source_live = 0;
                message = "Wireless reader stalled; recovering";
                last_tick = now_ns();
            }
        }
        if (tick >= next_status) {
            next_status = tick + UINT64_C(1000000000);
            health.connected = source_live ? source.source_count : 0;
            health.routes = game.state == GC_DS4_GAME_LIVE ? game.route_count : 0;
            health.imports = game.state == GC_DS4_GAME_LIVE ? game.hook_count : 0;
            health.game_pid = game.pid;
            GcDs4GameStatus target;
            if (game.state == GC_DS4_GAME_LIVE && gc_ds4_game_validate(&game, &source)) {
                (void)gc_ds4_game_remove(&game);
                message = "Game pad ownership changed; refreshing routes";
            }
            if (game.state == GC_DS4_GAME_LIVE && !gc_ds4_game_status(&game, &target)) {
                uint64_t calls = 0;
                for (unsigned kind = 0; kind < GC_DS4_LIBPAD_SYMBOLS; ++kind) calls += target.calls[kind];
                if (calls > health.gateway_calls) message = "Wireless DS4 input active";
                health.gateway_calls = calls;
                health.lease_expirations = target.lease_expirations;
            }
            publish_status(&health, message);
        }
        uint64_t end = now_ns();
        if (end < tick + UINT64_C(8333333))
            usleep((useconds_t)((tick + UINT64_C(8333333) - end) / 1000));
        else ++health.missed_deadlines;
    }
    if (suspended) {
        int game_result = gc_ds4_game_quiesce(&game);
        int source_result = gc_ds4_source_request_stop(&source);
        cleanup_failed = game_result || source_result || source.pid ? -1 : 0;
    } else cleanup_failed = stop_targets();
    if (info_flag >= 0) (void)sceKernelCloseEventFlag(info_flag);
    if (system_flag >= 0) (void)sceKernelCloseEventFlag(system_flag);
    health.connected = health.routes = health.imports = 0;
    publish_status(&health, cleanup_failed ? "Stopped; target cleanup pending" : "Stopped");
    return NULL;
}

int gc_ds4_bridge_start(GcConfigStore *config, GcState *state)
{
    pthread_mutex_lock(&lifecycle);
    int result = 0;
    if (!started) {
        config_store = config; service_state = state;
        atomic_store(&stopping, 0);
        result = pthread_create(&worker, NULL, bridge_worker, NULL);
        started = result == 0;
    }
    pthread_mutex_unlock(&lifecycle);
    return result ? -1 : 0;
}

int gc_ds4_bridge_stop(void)
{
    pthread_mutex_lock(&lifecycle);
    if (started) {
        atomic_store(&stopping, 1);
        pthread_join(worker, NULL);
        started = 0;
    }
    int result = cleanup_failed;
    pthread_mutex_unlock(&lifecycle);
    return result;
}

void gc_ds4_bridge_status(GcWirelessState *out)
{
    if (!out) return;
    pthread_mutex_lock(&status_lock);
    *out = status;
    pthread_mutex_unlock(&status_lock);
}
