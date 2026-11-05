#include "log.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

/* The log is kept under LOG_MAX bytes: past it, it becomes path.1 (the
 * one before is dropped) and a new one starts. */
#define LOG_MAX (1L << 20)

static FILE *g_log;
static char g_path[256];
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_close_uncertain,g_sync_uncertain;

static int lock_log(void)
{
    if(atomic_load(&g_sync_uncertain)) return -1;
    return pthread_mutex_lock(&g_lock)==0 ? 0 : -1;
}
static int unlock_log(void)
{
    if(!pthread_mutex_unlock(&g_lock)) return 0;
    atomic_store(&g_sync_uncertain,1);return -1;
}
/* The stream is invalid after fclose, even when fclose reports failure.
 * Never keep/retry that FILE* or reopen over an ambiguous underlying fd. */
static int close_held(void)
{
    if(atomic_load(&g_close_uncertain)) return -1;
    if(!g_log) return 0;
    FILE *owned=g_log;g_log=NULL;
    if(fclose(owned)) {atomic_store(&g_close_uncertain,1);return -1;}
    return 0;
}

int log_open(const char *path)
{
    if(lock_log()) return 0;
    int opened=0;
    if(!atomic_load(&g_close_uncertain)) {
        if(!path) opened=1;
        else if(!g_log && strlen(path)<sizeof g_path) {
            g_log=fopen(path,"a");
            if(g_log) {snprintf(g_path,sizeof g_path,"%s",path);opened=1;}
        }
    }
    return unlock_log() ? 0 : opened;
}

const char *log_release_blocker(void)
{
    if(atomic_load(&g_sync_uncertain)) return "bt_log_lock_uncertain";
    if(lock_log()) return "bt_log_lock_unavailable";
    const char *why=atomic_load(&g_close_uncertain) ? "bt_log_close_uncertain" : NULL;
    return unlock_log() ? "bt_log_lock_uncertain" : why;
}
int log_close_checked(void)
{
    if(lock_log()) return -1;
    int rc=close_held();
    return unlock_log() ? -1 : rc;
}
void log_close(void) { (void)log_close_checked(); }

static void rotate(void)
{
    char old[sizeof g_path + 2];

    if (!g_log || ftell(g_log) < LOG_MAX) return;
    if(close_held()) return;
    snprintf(old, sizeof old, "%s.1", g_path);
    remove(old);
    rename(g_path, old);
    g_log = fopen(g_path, "a");
}

void log_line(const char *fmt, ...)
{
    if(lock_log()) return;
    FILE *out;
    va_list ap;

    rotate();
    out = g_log ? g_log : stderr;
    fprintf(out, "[%8ld] ", now_ms());
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
    (void)unlock_log();
}
