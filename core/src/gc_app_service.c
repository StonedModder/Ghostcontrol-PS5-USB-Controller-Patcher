/* SPDX-License-Identifier: GPL-3.0-or-later
 * Explicit companion for the native app. Update execution stays refused.
 * Console identity, setup, Bluetooth, device inventory and motion are wired
 * on the payload build. No SystemService and no process attach. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "gc_config.h"
#include "gc_controller_commands.h"
#include "gc_ipc.h"
#include "gc_app_pad.h"
#include "gc_runtime_commands.h"
#include "gc_log_tail.h"
/* Diagnostic is additive: keep the platform SDK, exclude resident providers. */
#if defined(__PROSPERO__) && !defined(GC_APP_SERVICE_DIAGNOSTIC)
#define GC_APP_SERVICE_RESIDENT 1
#include "gc_service_features.h"
#endif
#include "gc_app_service_audio.h"
#include "gc_service_capture.h"
#include <json-c/json.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GC_APP_SERVICE_ROOT "/data/ghostcontrol-app-service"
#ifndef GC_APP_SERVICE_PORT
#define GC_APP_SERVICE_PORT 39073
#endif
#define GC_APP_SERVICE_VERSION "2.0.0-app-service-candidate"

typedef struct GcAppService {
    pthread_mutex_t mutex; /* Entire command/config/pad lifetime, one owner. */
    GcConfigStore *config;
    GcIpcServer *ipc;
    int exit_requested, closing, close_failed;
    int features_started;
    GcIpcDeferredReply *exit_reply;
    char exit_line[768];
    GcAppPad pad;
    GcServiceAudio *audio;
    GcServiceCapture *capture; /* owned before the audio owner, closed after it */
    GcRuntimeConfig runtime;
    int lock_fd,log_fd,running,instance_written;
    unsigned short port;
    uint64_t started_ms;
    char root[GC_CONFIG_PATH_MAX];
} GcAppService;
#if defined(GC_RESIDENT_EXIT_HOST_TEST) && !defined(__PROSPERO__)
static int app_release_host_stage(const char *stage);
#endif

static uint64_t app_now(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
static int app_json(struct json_object *o,char *out,size_t cap)
{
    if(!o) return -1;
    const char *text=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
    size_t n=strlen(text);
    if (n>32766 || !out || cap<=n+1) {
        struct json_object *small=json_object_new_object(),*v;
        const char *keys[]={"instance_contract_version","instance_storage_available","instance_live_available","instance_available","instance_policy_valid","capabilities"};
        for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++)
            if (json_object_object_get_ex(o,keys[i],&v)) json_object_object_add(small,keys[i],json_object_get(v));
        json_object_object_add(small,"ok",json_object_new_boolean(0));
        json_object_object_add(small,"error",json_object_new_string("reply_too_large"));
        text=json_object_to_json_string_ext(small,JSON_C_TO_STRING_PLAIN); n=strlen(text);
        if (out && cap>n+1) {memcpy(out,text,n); out[n]='\n'; out[n+1]=0;}
        else if (out && cap) out[0]=0;
        json_object_put(small); json_object_put(o); return -1;
    }
    memcpy(out,text,n); out[n]='\n'; out[n+1]=0;
    json_object_put(o); return 0;
}
#define BOOL(o,k,v) json_object_object_add(o,k,json_object_new_boolean(v))
#define NUM(o,k,v) json_object_object_add(o,k,json_object_new_int64((int64_t)(v)))
#define STR(o,k,v) json_object_object_add(o,k,json_object_new_string(v))
/* File-only runtime identity, never a PID-discovery or signalling channel.
 * Caller owns the service lock. Atomically replace a validated private regular
 * file via an exclusive 0600 temporary file under the validated private root.
 * Preserve identity after exit, marking only this instance closed after join. */
static int app_instance_write(GcAppService *s,const char *state)
{
    /* Console /data roots are often not 0700/euid-owned the way a host
     * private directory is. Keep symlink and multi-link rejection, but do
     * not refuse startup solely because the store is group-readable or the
     * loader runs under a different effective uid than the directory owner. */
    int dir=open(s->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    struct stat st;
    if(dir<0) return -1;
    if(fstat(dir,&st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        close(dir); return -1;
    }
    const char *target="runtime-instance.json";
    if(!fstatat(dir,target,&st,AT_SYMLINK_NOFOLLOW)) {
        if(!S_ISREG(st.st_mode) || st.st_nlink!=1) {close(dir); return -1;}
    } else if(errno!=ENOENT) {close(dir); return -1;}
    struct json_object *o=json_object_new_object();
    if(!o) {close(dir); return -1;}
    NUM(o,"pid",getpid()); STR(o,"version",GC_APP_SERVICE_VERSION);
    NUM(o,"port",s->port); STR(o,"root",s->root); STR(o,"service_kind","app-native");
    json_object_object_add(o,"boot",json_object_new_uint64(s->pad.boot));
    NUM(o,"started_ms",s->started_ms); STR(o,"state",state);
    const char *text=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
    size_t length=strlen(text),done=0;
    char temp[96];
    snprintf(temp,sizeof(temp),".runtime-instance.%ld.%llu.tmp",(long)getpid(),
             (unsigned long long)s->pad.boot);
    int fd=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600),rc=-1;
    if(fd<0) {json_object_put(o); close(dir); return -1;}
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1) goto done;
    (void)fchmod(fd,0600);
    while(done<length) {
        ssize_t n=write(fd,text+done,length-done);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) goto done;
        done+=(size_t)n;
    }
    if(fsync(fd)) goto done;
    if(close(fd)) {fd=-1; goto done;} fd=-1;
    if(renameat(dir,temp,dir,target)) goto done;
    s->instance_written=1; rc=0;
 done:
    if(fd>=0) close(fd);
    if(rc) unlinkat(dir,temp,0);
    json_object_put(o); close(dir); return rc;
}
static int app_error(const char *code,const char *detail,char *out,size_t cap)
{
    struct json_object *o=json_object_new_object();
    BOOL(o,"ok",0); BOOL(o,"applied",0); STR(o,"error",code);
    if(detail && *detail) {
        char hex[1025];
        size_t n=strlen(detail); if(n>512) n=512;
        if(!gc_ipc_hex_encode(detail,n,hex,sizeof(hex))) STR(o,"detail",hex);
    }
    app_json(o,out,cap); return -1;
}
static struct json_object *app_caps(void)
{
    struct json_object *o=json_object_new_object();
    const char *no[]={"usb","vda","kernel","game_bridge","remote_motion","motion",
      "bluetooth_pairing","boot_installer","output_after_app_exit","update_execution",
      "console_id","wireless_ds4","ghostpad_output","xmb","background_lifecycle"};
    for(size_t i=0;i<sizeof(no)/sizeof(no[0]);i++) BOOL(o,no[i],0);
    BOOL(o,"profiles",1); BOOL(o,"persistence",1); BOOL(o,"app_pad",1);
    BOOL(o,"native_pad",1); BOOL(o,"setup",0); BOOL(o,"bluetooth",0);
    BOOL(o,"controller_rules",1); BOOL(o,"update_check",0);
    BOOL(o,"controller_output_preferences",1); BOOL(o,"virtual_output_types",0);
    BOOL(o,"device_inventory",0);
    BOOL(o,"background_output",0); BOOL(o,"usb_vda",0);
    BOOL(o,"effects",1); STR(o,"effects_scope","connected_app_owned_pad_only");
#ifdef GC_APP_SERVICE_RESIDENT
    BOOL(o,"setup",1); BOOL(o,"bluetooth",1); BOOL(o,"console_id",1);
    BOOL(o,"device_inventory",1); BOOL(o,"motion",1);
    /* Compiled-in routing is not an API-only virtual identity provider.
     * Do not advertise console output or type support from a target macro. */
#endif
    return o;
}
static int app_log(GcAppService *s,const char *command)
{
    char line[160];
    int n=snprintf(line,sizeof(line),"%llu app-service %s\n",(unsigned long long)app_now(),command);
    if(n>0 && (size_t)n<sizeof(line)) {
        size_t done=0;
        while(done<(size_t)n) {
            ssize_t w=write(s->log_fd,line+done,(size_t)n-done);
            if(w<0 && errno==EINTR) continue;
            if(w<=0) return -1;
            done+=(size_t)w;
        }
        return 0;
    }
    return -1;
}
/* Checked under the exclusive root lease at startup and on every runtime tick.
 * ENOENT is the only benign inspection failure; never consume unsafe markers. */
static int app_exit_marker(GcAppService *s,char *error,size_t cap)
{
    char path[512]; struct stat st;
    snprintf(path,sizeof(path),"%s/exit",s->root);
    if(lstat(path,&st)) {
        if(errno==ENOENT) return 0;
        snprintf(error,cap,"exit marker unavailable: %s",strerror(errno)); return -1;
    }
    if(!S_ISREG(st.st_mode) || st.st_nlink!=1) {
        snprintf(error,cap,"exit marker must be a regular single-link file"); return -1;
    }
    if(unlink(path)) {
        snprintf(error,cap,"exit marker cleanup failed: %s",strerror(errno)); return -1;
    }
    return 1;
}
/* Snapshot after every config write/reload. Only the effective app settings are
 * clamped (step=0 means fastest normal frame step); persisted readback is exact.
 * Do not bump pad revision for unrelated preferences/profile mutations. */
static int app_sync(GcAppService *s)
{
    GcRuntimeConfig r;
    if(gc_config_snapshot(s->config,&r,NULL)) return -1;
    unsigned enabled=s->running && r.dualsense_fx_enabled;
    unsigned step=r.dualsense_fx_step_ms<16?16:r.dualsense_fx_step_ms;
    if(s->pad.enabled!=enabled || s->pad.effect!=(unsigned)r.dualsense_fx_effect ||
       s->pad.period_ms!=r.dualsense_fx_period_ms || s->pad.step_ms!=step ||
       s->pad.r!=r.dualsense_fx_r || s->pad.g!=r.dualsense_fx_g || s->pad.b!=r.dualsense_fx_b) {
        if(gc_app_pad_configure(&s->pad,enabled,(unsigned)r.dualsense_fx_effect,
              r.dualsense_fx_period_ms,step,r.dualsense_fx_r,r.dualsense_fx_g,r.dualsense_fx_b)) return -1;
    }
    s->runtime=r; return 0;
}
/* Product configuration deliberately remains constructor-default disabled.
 * Schema support is not acceptance of the missing consumers/product gates. */
static void app_instance_caps(GcConfigStore *store,struct json_object *o,struct json_object *aliases)
{
    GcInstanceCapabilities c={0}; (void)gc_config_instance_capabilities(store,&c);
    NUM(o,"instance_contract_version",c.instance_contract_version);
    BOOL(o,"instance_storage_available",c.instance_storage_available);
    BOOL(o,"instance_live_available",c.instance_live_available);
    BOOL(o,"instance_available",c.instance_available);
    BOOL(o,"instance_policy_valid",c.instance_policy_valid);
    BOOL(aliases,"controller_instance_storage",c.instance_storage_available);
    BOOL(aliases,"setup_controller_instance",c.instance_live_available);
}
/* Querying capability never destroys or closes a resource. */
static const char *app_release_blocker(GcAppService *s)
{
    const char *audio=gc_service_audio_release_blocker(s->audio);
    /* This service owns and destroys capture after audio stops using it. */
    if(audio && !(s->capture && !strcmp(audio,"audio_provider_close_has_no_checked_release"))) return audio;
#ifdef GC_APP_SERVICE_RESIDENT
    if(s->features_started) return gc_service_features_release_blocker();
#endif
    return NULL;
}
static int app_status_locked(GcAppService *s,char *out,size_t cap)
{
    GcAppPad p; gc_app_pad_status(&s->pad,app_now(),&p);
    struct json_object *o=json_object_new_object();
    BOOL(o,"ok",1); NUM(o,"schema",1); STR(o,"version",GC_APP_SERVICE_VERSION);
    NUM(o,"pid",getpid()); NUM(o,"started_ms",s->started_ms); STR(o,"service_kind","app-native");
#ifdef GC_APP_SERVICE_DIAGNOSTIC
    STR(o,"build_variant","diagnostic");
#endif
    NUM(o,"lifecycle_contract",1);
    const char *release_blocker=app_release_blocker(s);
    BOOL(o,"conditional_release",!s->closing && !release_blocker);
    STR(o,"lifecycle_blocker",s->closing?"shutdown_in_progress":(release_blocker?release_blocker:""));
    NUM(o,"listen_port",s->port);
    json_object_object_add(o,"boot",json_object_new_uint64(s->pad.boot));
    STR(o,"service",s->running?"running":"stopped"); STR(o,"error","");
#ifdef GC_APP_SERVICE_RESIDENT
    if(s->features_started && gc_service_features_retiring()) {
        STR(o,"service","release_uncertain"); STR(o,"error","input_release_uncertain");
    }
#endif
    NUM(o,"config_generation",gc_config_generation(s->config));
    struct json_object *slots=json_object_new_array();
#ifdef GC_APP_SERVICE_RESIDENT
    gc_service_features_slot(slots);
#endif
    json_object_object_add(o,"slots",slots); /* app pad when the payload can read it */
    json_object_object_add(o,"notifications",json_object_new_array());
    struct json_object *wireless=json_object_new_object();
    BOOL(wireless,"supported",0); BOOL(wireless,"connected",0);
    json_object_object_add(o,"wireless_ds4",wireless);
    struct json_object *pad=json_object_new_object();
    BOOL(pad,"connected",p.connected); NUM(pad,"caps",p.caps);
    STR(pad,"source","app_poll"); NUM(pad,"session",p.session); NUM(pad,"generation",p.generation);
    NUM(pad,"read_rc",p.read_rc); NUM(pad,"handle_rc",p.handle_rc);
    json_object_object_add(o,"native_pad",pad);
    struct json_object *aliases=app_caps();
    app_instance_caps(s->config,o,aliases);
    json_object_object_add(o,"capabilities",aliases);
    STR(o,"effect_route","app_owned_pad");
    return app_json(o,out,cap);
}
static int app_status(void *ctx,char *out,size_t cap)
{
    GcAppService *s=ctx; pthread_mutex_lock(&s->mutex);
    app_log(s,"STATUS");
    int rc=app_status_locked(s,out,cap); pthread_mutex_unlock(&s->mutex); return rc;
}
static void app_ipc_log(void *ctx,const char *message)
{
#if defined(GC_RESIDENT_EXIT_HOST_TEST) && !defined(__PROSPERO__)
    /* HOST readiness only: socket accepted before the logging owner lock. */
    if(!strcmp(message,"ipc: accepted client")) (void)app_release_host_stage("input-stop");
#endif
    GcAppService *s=ctx; pthread_mutex_lock(&s->mutex);
    app_log(s,message);
    pthread_mutex_unlock(&s->mutex);
}
static int app_fx(GcAppService *s,char *out,size_t cap)
{
    GcAppPad p; gc_app_pad_status(&s->pad,app_now(),&p);
    int light=s->running && p.connected && (p.caps&GC_APP_PAD_CAP_LIGHT) && p.enabled &&
      p.light_ack_revision==p.revision && p.light_rc==0;
    int motor=p.connected && (p.caps&GC_APP_PAD_CAP_RUMBLE) &&
      p.event_state==GC_APP_PAD_STARTED && p.mode_rc==0 && p.start_rc==0 && s->running;
    struct json_object *o=json_object_new_object();
    BOOL(o,"ok",1); STR(o,"route","app_owned_pad"); BOOL(o,"runtime_supported",1);
    BOOL(o,"configured_only",!light); BOOL(o,"applied",light || motor);
    BOOL(o,"light_applied",light); BOOL(o,"pending",p.event_state==GC_APP_PAD_PENDING ||
      (p.enabled && p.light_ack_revision!=p.revision));
    BOOL(o,"running",s->running); BOOL(o,"enabled",s->runtime.dualsense_fx_enabled);
    NUM(o,"effect",s->runtime.dualsense_fx_effect); NUM(o,"period_ms",s->runtime.dualsense_fx_period_ms);
    NUM(o,"step_ms",s->runtime.dualsense_fx_step_ms); NUM(o,"effective_step_ms",p.step_ms);
    NUM(o,"r",s->runtime.dualsense_fx_r); NUM(o,"g",s->runtime.dualsense_fx_g); NUM(o,"b",s->runtime.dualsense_fx_b);
    /* Animation colour is not on the frozen ACK wire: do not invent current RGB. */
    BOOL(o,"current_rgb_available",0); NUM(o,"controllers",p.connected?1:0);
    NUM(o,"boot",p.boot); NUM(o,"revision",p.revision); NUM(o,"event_id",p.event_id);
    NUM(o,"event_state",p.event_state); NUM(o,"light_ack_revision",p.light_ack_revision);
    NUM(o,"light_rc",p.light_rc); NUM(o,"mode_rc",p.mode_rc); NUM(o,"start_rc",p.start_rc);
    NUM(o,"stop_rc",p.stop_rc); NUM(o,"reset_rc",p.reset_rc);
    NUM(o,"read_rc",p.read_rc); NUM(o,"handle_rc",p.handle_rc);
    BOOL(o,"rumble_active",motor); NUM(o,"rumble_large",p.large); NUM(o,"rumble_small",p.small);
    NUM(o,"rumble_on_ms",p.on_ms); NUM(o,"rumble_off_ms",p.off_ms); NUM(o,"rumble_repeats",p.repeats);
    STR(o,"rumble_role","diagnostic"); BOOL(o,"rumble_is_game_forwarding",0);
    BOOL(o,"motion_valid",0); STR(o,"motion_error","motion_producer_absent");
    BOOL(o,"output_after_app_exit",0);
#ifdef GC_APP_SERVICE_RESIDENT
    {
        int motion=gc_service_features_motion(NULL,NULL);
        BOOL(o,"motion_valid",motion);
        STR(o,"motion_error",motion?"":"motion_producer_absent");
        BOOL(o,"output_after_app_exit",s->runtime.run_in_background);
    }
#endif
    return app_json(o,out,cap);
}
static int app_runtime(GcAppService *s,char *out,size_t cap)
{
    if(gc_runtime_command_get(s->config,NULL,out,cap)) return -1;
    struct json_object *o=json_tokener_parse(out);
    if(!o) return -1;
    STR(o,"effect_route","app_owned_pad");
    NUM(o,"listen_port",s->port);
    json_object_object_add(o,"capabilities",app_caps());
    return app_json(o,out,cap);
}
static int app_profiles(GcAppService *s,const char *id,char *out,size_t cap)
{
    GcProfile *p=calloc(GC_CONFIG_MAX_PROFILES,sizeof(*p));
    if(!p) return app_error("out_of_memory",NULL,out,cap);
    uint64_t generation; size_t count=gc_config_profiles(s->config,p,GC_CONFIG_MAX_PROFILES,&generation);
    struct json_object *o=json_object_new_object(); BOOL(o,"ok",1); NUM(o,"generation",generation);
    if(!id) {
        struct json_object *a=json_object_new_array();
        for(size_t i=0;i<count;i++) json_object_array_add(a,json_object_new_string(p[i].id));
        json_object_object_add(o,"profiles",a);
    } else {
        size_t i; for(i=0;i<count;i++) if(!strcmp(id,p[i].id)) break;
        if(i==count) { free(p); json_object_put(o); return app_error("profile_not_found",NULL,out,cap); }
        char *ini=malloc(GC_PROFILE_TEXT_MAX),*hex=malloc(GC_PROFILE_TEXT_MAX*2+1);
        int length=ini?gc_profile_serialize(&p[i],ini,GC_PROFILE_TEXT_MAX):-1;
        if(!hex || length<0 || gc_ipc_hex_encode(ini,(size_t)length,hex,GC_PROFILE_TEXT_MAX*2+1)) {
            free(ini); free(hex); free(p); json_object_put(o); return app_error("profile_serialization_failed",NULL,out,cap);
        }
        STR(o,"id",id); STR(o,"encoding","hex"); STR(o,"data",hex); free(ini); free(hex);
    }
    free(p); return app_json(o,out,cap);
}
static int app_profile_write(GcAppService *s,const char *cmd,const char *arg,char *out,size_t cap)
{
    char error[256]="",id[GC_PROFILE_ID_MAX],key[GC_PROFILE_EDIT_KEY_MAX+1],value[GC_PROFILE_EDIT_VALUE_MAX+1];
    int rc=-1;
    if(!strcmp(cmd,"PROFILE_CREATE")) rc=gc_config_create_profile(s->config,arg,error,sizeof(error));
    else if(!strcmp(cmd,"PROFILE_IMPORT")) rc=gc_config_import_profile(s->config,arg,error,sizeof(error));
    else if(!strcmp(cmd,"PROFILE_EXPORT")) rc=gc_config_export_profile(s->config,arg,error,sizeof(error));
    else if(!strcmp(cmd,"PROFILE_EDIT")) {
        if(!gc_profile_edit_split(arg,id,sizeof(id),key,sizeof(key),value,sizeof(value),error,sizeof(error)))
            rc=gc_config_edit_profile(s->config,id,key,value,error,sizeof(error));
    } else if(!strcmp(cmd,"PROFILE_RENAME")) {
        /* Production UI hex-encodes the whole old-ID|new-ID argument.
         * Decode once, reject embedded NUL/control bytes, and never trim,
         * case-fold, or otherwise repair identifiers. */
        char decoded[GC_PROFILE_ID_MAX*2]; size_t length=0;
        if(gc_ipc_hex_decode(arg,decoded,sizeof(decoded)-1,&length) ||
           !length || memchr(decoded,0,length))
            return app_error("invalid_profile_rename",NULL,out,cap);
        for(size_t i=0;i<length;i++)
            if((unsigned char)decoded[i]<32 || (unsigned char)decoded[i]==127)
                return app_error("invalid_profile_rename",NULL,out,cap);
        decoded[length]=0;
        const char *sep=strchr(decoded,'|');
        if(sep && sep!=decoded && (size_t)(sep-decoded)<sizeof(id) &&
           sep[1] && strlen(sep+1)<sizeof(id) && !strchr(sep+1,'|')) {
            memcpy(id,decoded,(size_t)(sep-decoded)); id[sep-decoded]=0;
            rc=gc_config_rename_profile(s->config,id,sep+1,error,sizeof(error));
        }
    }
    if(rc) return app_error("profile_operation_failed",error,out,cap);
    if(app_sync(s)) return app_error("config_sync_failed",NULL,out,cap);
    struct json_object *o=json_object_new_object(); BOOL(o,"ok",1); BOOL(o,"saved",1);
    BOOL(o,"applied",0); STR(o,"effect","profile_storage_only"); NUM(o,"generation",gc_config_generation(s->config));
    return app_json(o,out,cap);
}
static int app_tail(GcAppService *s,const char *arg,char *out,size_t cap)
{
    int lines; if(gc_log_tail_parse_lines(arg,&lines)) return app_error("invalid_log_lines",NULL,out,cap);
    struct stat st;
    if(fstat(s->log_fd,&st)) return app_error("log_read_failed",NULL,out,cap);
    size_t n=st.st_size>GC_LOG_TAIL_MAX_BYTES?GC_LOG_TAIL_MAX_BYTES:(size_t)st.st_size;
    off_t start=st.st_size-(off_t)n;
    char *text=malloc(n+1);
    if(!text) return app_error("out_of_memory",NULL,out,cap);
    size_t done=0;
    while(done<n) {
        ssize_t got=pread(s->log_fd,text+done,n-done,start+(off_t)done);
        if(got<0 && errno==EINTR) continue;
        if(got<=0) {free(text); return app_error("log_read_failed",NULL,out,cap);}
        done+=(size_t)got;
    }
    /* If the bounded window begins mid-line, omit that partial line. */
    size_t skip=0;
    if(start>0) { while(skip<n && text[skip]!='\n') ++skip; if(skip<n) ++skip; }
    int rc=gc_log_tail_format(text+skip,n-skip,lines,out,cap); free(text); return rc;
}
static int app_rumble(GcAppService *s,const char *arg,char *out,size_t cap)
{
    /* State module's tighter finite pattern limits are authoritative. */
    unsigned v[5]; const char *p=arg;
    for(unsigned i=0;i<5;i++) {
        unsigned n=0; if(*p<'0' || *p>'9') return app_error("invalid_rumble_settings",NULL,out,cap);
        while(*p>='0' && *p<='9') {
            if(n>60000u/10 || n*10u+(unsigned)(*p-'0')>60000u) return app_error("invalid_rumble_settings",NULL,out,cap);
            n=n*10u+(unsigned)(*p++-'0');
        }
        v[i]=n;
        if(i<4) { if(*p++!='|') return app_error("invalid_rumble_settings",NULL,out,cap); }
        else if(*p) return app_error("invalid_rumble_settings",NULL,out,cap);
    }
    if(!s->running) return app_error("service_stopped",NULL,out,cap);
    if(gc_app_pad_schedule(&s->pad,app_now(),v[0],v[1],v[2],v[3],v[4]))
        return app_error("rumble_rejected_invalid_or_busy",NULL,out,cap);
    struct json_object *o=json_object_new_object(); BOOL(o,"ok",1); BOOL(o,"accepted",1);
    BOOL(o,"pending",1); BOOL(o,"applied",0); NUM(o,"event_id",s->pad.event_id);
    return app_json(o,out,cap);
}
static int app_exit_decimal(const char *p,uint64_t *out)
{
    if(!*p || *p=='0') return -1;
    uint64_t n=0;
    for(;*p;p++) {
        if(*p<'0' || *p>'9' || n>(UINT64_MAX-(unsigned)(*p-'0'))/10) return -1;
        n=n*10+(unsigned)(*p-'0');
    }
    *out=n; return 0;
}
static int app_resident_exit(GcAppService *s,const char *arg,char *out,size_t cap)
{
    if(!arg || strlen(arg)>256) return app_error("invalid_resident_exit_request",NULL,out,cap);
    char copy[257]; strcpy(copy,arg);
    char *fields[8]={copy}; unsigned count=1;
    for(char *p=copy;*p;p++) {
        if((unsigned char)*p<33 || (unsigned char)*p>126) return app_error("invalid_resident_exit_request",NULL,out,cap);
        if(*p=='|') {
            if(count==8) return app_error("invalid_resident_exit_request",NULL,out,cap);
            *p=0; fields[count++]=p+1;
        }
    }
    uint64_t pid,start,boot,port;
    if(count!=8 || strcmp(fields[0],"1") || strcmp(fields[1],"app-native") ||
        strcmp(fields[2],GC_APP_SERVICE_VERSION) || app_exit_decimal(fields[3],&pid) ||
        pid>INT32_MAX || app_exit_decimal(fields[4],&start) || app_exit_decimal(fields[5],&boot) ||
        app_exit_decimal(fields[6],&port) || port<1024 || port>65535 || strlen(fields[7])!=32)
        return app_error("invalid_resident_exit_request",NULL,out,cap);
    for(const char *p=fields[7];*p;p++) if(!(*p>='0'&&*p<='9') && !(*p>='a'&&*p<='f'))
        return app_error("invalid_resident_exit_request",NULL,out,cap);
    const char *blocker=app_release_blocker(s);
    if(blocker) return app_error("resident_exit_unsupported",blocker,out,cap);
    if(pid!=(uint64_t)getpid() || start!=s->started_ms || boot!=s->pad.boot || port!=s->port)
        return app_error("resident_identity_mismatch",NULL,out,cap);
    if(s->closing || s->exit_requested || s->exit_reply) return app_error("resident_exit_busy",NULL,out,cap);
    int n=snprintf(s->exit_line,sizeof(s->exit_line),
        "{\"ok\":true,\"accepted\":true,\"released\":true,\"lifecycle_schema\":1,"
        "\"service_kind\":\"app-native\",\"version\":\"%s\",\"pid\":%llu,\"started_ms\":%llu,"
        "\"boot\":%llu,\"listen_port\":%llu,\"request_nonce\":\"%s\"}\n",
        GC_APP_SERVICE_VERSION,(unsigned long long)pid,(unsigned long long)start,
        (unsigned long long)boot,(unsigned long long)port,fields[7]);
    if(n<0 || (size_t)n>=sizeof(s->exit_line) || gc_ipc_defer_reply(s->ipc,&s->exit_reply))
        return app_error("resident_exit_reply_unavailable",NULL,out,cap);
    /* No allocation or failure after the real socket handoff. */
    s->exit_requested=1;
    return GC_IPC_REPLY_DEFERRED;
}

static int app_command_locked(GcAppService *s,const char *cmd,const char *arg,char *out,size_t cap)
{
    if(!arg) arg="";
    if(!strcmp(cmd,"RESIDENT_EXIT_IF")) return app_resident_exit(s,arg,out,cap);
    if(s->exit_requested || s->closing) return app_error("service_shutting_down",NULL,out,cap);
    if(!strncmp(cmd,"AUDIO_",6)) return gc_service_audio_command(s->audio,cmd,arg,out,cap);
    for(const unsigned char *p=(const unsigned char *)arg;*p;p++)
        if(*p<32 || *p==127) return app_error("invalid_argument",NULL,out,cap);
    const char *noarg[]={"PING","START","STOP","RESTART","RELOAD","RUNTIME_GET","DUALSENSE_FX_STATUS","DUALSENSE_RUMBLE_STOP","PROFILES","PROFILE_FILES","DEVICES","CONTROLLERS"};
    for(size_t i=0;i<sizeof(noarg)/sizeof(noarg[0]);i++)
        if(!strcmp(cmd,noarg[i]) && *arg) return app_error("unexpected_argument",NULL,out,cap);
    {
        int handled=gc_controller_command(s->config,cmd,arg,out,cap);
        if(handled<=0) return handled;
    }
#ifdef GC_APP_SERVICE_RESIDENT
    {
        int handled=s->features_started ? gc_service_features_command(cmd,arg,out,cap) : 1;
        if(handled<=0) return handled;
        if(!s->features_started && (!strncmp(cmd,"SETUP_",6) || !strncmp(cmd,"BLUETOOTH_",10) ||
            !strcmp(cmd,"DEVICES") || !strcmp(cmd,"CONSOLE_ID")))
            return app_error("input_stopped","Start input before using resident resources",out,cap);
    }
#endif
    if(!strcmp(cmd,"EXIT")) {
        if(*arg) return app_error("unexpected_argument",NULL,out,cap);
        s->exit_requested=1;
        return snprintf(out,cap,"{\"ok\":true,\"accepted\":true,\"pending\":true,\"applied\":false,\"effect\":\"companion_exiting\"}\n")<0?-1:0;
    }
    if(!strcmp(cmd,"PING")) return snprintf(out,cap,"{\"ok\":true}\n")<0?-1:0;
    if(!strcmp(cmd,"RUNTIME_GET")) return app_runtime(s,out,cap);
    if(!strcmp(cmd,"RUNTIME_SET")) {
        int rc=gc_runtime_command_set(s->config,arg,out,cap); if(rc) return rc;
        if(app_sync(s)) return app_error("config_saved_but_sync_failed",NULL,out,cap);
        struct json_object *o=json_tokener_parse(out);
        int fx=!strncmp(arg,"dualsense_fx_",13);
        BOOL(o,"runtime_supported",fx); BOOL(o,"applied",0);
        BOOL(o,"pending",fx && s->running); STR(o,"effect",fx?"app_pad_configuration_pending":"persisted_preference_only");
        return app_json(o,out,cap);
    }
    if(!strcmp(cmd,"APP_PAD_POLL") || !strcmp(cmd,"APP_PAD_ACK"))
        return gc_app_pad_command(&s->pad,app_now(),cmd,arg,out,cap);
    if(!strcmp(cmd,"DUALSENSE_FX_STATUS")) return app_fx(s,out,cap);
    if(!strcmp(cmd,"DUALSENSE_RUMBLE")) return app_rumble(s,arg,out,cap);
    if(!strcmp(cmd,"DUALSENSE_RUMBLE_STOP")) {
        gc_app_pad_stop(&s->pad);
        return snprintf(out,cap,"{\"ok\":true,\"accepted\":true,\"pending\":true,\"applied\":false}\n")<0?-1:0;
    }
    if(!strcmp(cmd,"START") || !strcmp(cmd,"STOP") || !strcmp(cmd,"RESTART") || !strcmp(cmd,"RELOAD")) {
        char error[256]="";
#ifdef GC_APP_SERVICE_RESIDENT
        if((!strcmp(cmd,"STOP") || !strcmp(cmd,"RESTART")) && s->features_started) {
            const char *blocker=gc_service_features_release_blocker();
            if(blocker) return app_error("input_stop_unsupported",blocker,out,cap);
            if(gc_service_features_close_checked()) return app_error("input_release_uncertain",NULL,out,cap);
            s->features_started=0;
        }
#endif
        if(!strcmp(cmd,"STOP") || !strcmp(cmd,"RESTART")) {s->running=0; gc_app_pad_stop(&s->pad);}
        if(!strcmp(cmd,"RELOAD") && gc_config_reload(s->config,error,sizeof(error)))
            return app_error("reload_failed",error,out,cap);
        if(!strcmp(cmd,"START") || !strcmp(cmd,"RESTART")) {
#ifdef GC_APP_SERVICE_RESIDENT
            if(gc_service_features_retiring()) return app_error("input_release_uncertain",NULL,out,cap);
            if(!s->features_started) {gc_service_features_init(s->config); s->features_started=1;}
#endif
            s->running=1;
        }
        if(app_sync(s)) return app_error("config_sync_failed",NULL,out,cap);
        struct json_object *o=json_object_new_object(); BOOL(o,"ok",1); STR(o,"command",cmd);
        STR(o,"service",s->running?"running":"stopped"); BOOL(o,"applied",0); BOOL(o,"pending",1);
        STR(o,"effect","logical_lifecycle_app_output_requires_ack"); NUM(o,"generation",gc_config_generation(s->config));
        return app_json(o,out,cap);
    }
    if(!strcmp(cmd,"PROFILES")) return app_profiles(s,NULL,out,cap);
    if(!strcmp(cmd,"PROFILE_GET")) return app_profiles(s,arg,out,cap);
    if(!strcmp(cmd,"PROFILE_FILES")) return gc_config_profile_files_json(s->config,out,cap);
    if(!strcmp(cmd,"PROFILE_CREATE") || !strcmp(cmd,"PROFILE_EDIT") || !strcmp(cmd,"PROFILE_RENAME") ||
       !strcmp(cmd,"PROFILE_IMPORT") || !strcmp(cmd,"PROFILE_EXPORT")) return app_profile_write(s,cmd,arg,out,cap);
    if(!strcmp(cmd,"LOG_TAIL")) return app_tail(s,arg,out,cap);
    if(!strcmp(cmd,"DEVICES") || !strcmp(cmd,"CONTROLLERS"))
        return app_error("unsupported_usb_vda_inventory","App native pad observations are in STATUS.native_pad",out,cap);
    if(!strncmp(cmd,"BLUETOOTH_",10)) return app_error("unsupported_bluetooth_host_unavailable",NULL,out,cap);
    if(!strncmp(cmd,"UPDATE_",7)) return app_error("unsupported_update_execution",NULL,out,cap);
    if(!strcmp(cmd,"CONSOLE_ID")) return app_error("unsupported_documented_console_id_api_unavailable",NULL,out,cap);
    if(!strcmp(cmd,"REBIND") || !strcmp(cmd,"FORCE_VDA")) return app_error("unsupported_kernel_game_bridge",NULL,out,cap);
    if(!strncmp(cmd,"SETUP_",6) || !strcmp(cmd,"CONTROLLER_SAVE")) return app_error("unsupported_usb_game_bridge",NULL,out,cap);
    if(strstr(cmd,"MOTION") || !strncmp(cmd,"GHOSTPAD_",9)) return app_error("unsupported_remote_motion",NULL,out,cap);
    if(!strncmp(cmd,"BOOT_",5) || !strncmp(cmd,"INSTALL",7)) return app_error("unsupported_boot_installer",NULL,out,cap);
    return app_error("unsupported_command",NULL,out,cap);
}
static int app_command(void *ctx,const char *cmd,const char *arg,char *out,size_t cap)
{
    GcAppService *s=ctx; pthread_mutex_lock(&s->mutex);
    fprintf(stderr,"app-service: command '%s' arg_len=%zu\n",cmd,arg?strlen(arg):0);
    int rc=app_command_locked(s,cmd,arg,out,cap); app_log(s,cmd);
    pthread_mutex_unlock(&s->mutex); return rc;
}
#if defined(GC_RESIDENT_EXIT_HOST_TEST) && !defined(__PROSPERO__)
/* Explicit HOST-only seam holding/failing actual teardown, never a response
 * provider. Default and target objects have no hook or environment imports. */
static int app_release_host_stage(const char *stage)
{
    const char *root=getenv("GC_RESIDENT_EXIT_TEST_ROOT");
    if(!root) return 0;
    char path[1024];
    if(snprintf(path,sizeof(path),"%s/%s.entered",root,stage)>=(int)sizeof(path)) return -1;
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    /* Readiness markers are observational and can survive a HOST restart.
     * Their prior existence is not a cleanup fault or release witness. */
    if(fd<0) {if(errno!=EEXIST) return -1;}
    else if(close(fd)) return -1;
    if(snprintf(path,sizeof(path),"%s/%s.hold",root,stage)>=(int)sizeof(path)) return -1;
    while(access(path,F_OK)==0) {struct timespec wait={0,1000000}; nanosleep(&wait,NULL);}
    if(errno!=ENOENT) return -1;
    if(snprintf(path,sizeof(path),"%s/%s.fail",root,stage)>=(int)sizeof(path)) return -1;
    if(access(path,F_OK)==0 || errno!=ENOENT) return -1;
    return 0;
}
#define APP_RELEASE_STAGE(stage) app_release_host_stage(stage)
#else
#define APP_RELEASE_STAGE(stage) 0
#endif
static int app_release_unlock(int fd)
{
    if(APP_RELEASE_STAGE("unlock")) {errno=EIO; return -1;}
    return flock(fd,LOCK_UN);
}
static int app_release_close(int fd)
{
    int rc=close(fd); /* ambiguous close is never retried */
    if(APP_RELEASE_STAGE("close")) {errno=EIO; return -1;}
    return rc;
}
static int app_close_checked(GcAppService *s)
{
    if(!s) return 0;
    if(s->close_failed) return -1;
    if(pthread_mutex_lock(&s->mutex)) return -1;
    s->closing=1;
    if(pthread_mutex_unlock(&s->mutex)) return -1;
    /* IPC admission is retired before borrowed callbacks or resources. */
    GcIpcDeferredReply *reply=s->exit_reply;
    s->exit_reply=NULL;
    char line[sizeof(s->exit_line)]; memcpy(line,s->exit_line,sizeof(line));
    if(s->ipc) {
        if(gc_ipc_stop_checked(s->ipc)) goto retained;
        s->ipc=NULL;
    }
    if(reply && APP_RELEASE_STAGE("cleanup")) goto retained;
    /* Unsupported external teardown is a refusal, never a best-effort close.
     * Preserve borrowed config/audio/capture and the ORIGINAL lease. */
    if(app_release_blocker(s)) goto retained;
    if(s->capture) gc_service_audio_close(s->audio);
    else if(gc_service_audio_close_checked(s->audio)) goto retained;
    s->audio=NULL;
    /* Its seam can no longer be called, so release it before the root lease. */
    if(s->capture) {gc_service_capture_close(s->capture); s->capture=NULL;}
    if(pthread_mutex_lock(&s->mutex)) goto retained;
    if(reply && APP_RELEASE_STAGE("resources")) {
        pthread_mutex_unlock(&s->mutex); goto retained;
    }
    s->running=0; gc_app_pad_stop(&s->pad);
    if(s->config) {
        if(app_sync(s)) {pthread_mutex_unlock(&s->mutex); goto retained;}
#ifdef GC_APP_SERVICE_RESIDENT
        if(s->features_started && gc_service_features_close_checked()) {
            pthread_mutex_unlock(&s->mutex); goto retained;
        }
        s->features_started=0;
#endif
        gc_config_close(s->config); s->config=NULL;
    }
    if(s->log_fd>=0) {
        if(app_log(s,"close")) {pthread_mutex_unlock(&s->mutex); goto retained;}
        int fd=s->log_fd; s->log_fd=-1;
        if(close(fd)) {pthread_mutex_unlock(&s->mutex); goto retained;}
    }
    if(pthread_mutex_unlock(&s->mutex)) goto retained;
    /* The record describes this instance, not a future intention. A failed
     * resource close must leave it unclosed while original ownership remains. */
    if(s->instance_written && app_instance_write(s,"closed")) goto retained;
    if(pthread_mutex_destroy(&s->mutex)) goto retained;
    if(reply && APP_RELEASE_STAGE("lease")) goto retained;
    if(s->lock_fd>=0) {
        if(reply ? app_release_unlock(s->lock_fd) : flock(s->lock_fd,LOCK_UN)) goto retained;
        int fd=s->lock_fd; s->lock_fd=-1;
        if(reply ? app_release_close(fd) : close(fd)) goto retained;
    }
    free(s); /* deferred fd alone survives; no callbacks can reach this owner */
    if(reply) (void)gc_ipc_complete_reply(&reply,line,strlen(line),1000);
    return 0;
retained:
    s->close_failed=1;
    gc_ipc_discard_reply(&reply);
    return -1;
}
void gc_app_service_close(GcAppService *s) { (void)app_close_checked(s); }
GcAppService *gc_app_service_open_audio(const char *root,unsigned short port,char *error,size_t error_size,const GcServiceAudioOptions *audio_options)
{
    if(!root || !*root || strlen(root)>=GC_CONFIG_PATH_MAX || error_size==0) return NULL;
    struct stat st;
    if(lstat(root,&st)) {
        /* Prefer 0700 when the filesystem honors it; fall back to 0755 so a
         * console /data mount that rejects exclusive modes still starts. */
        if(errno!=ENOENT || (mkdir(root,0700) && mkdir(root,0755))) {
            snprintf(error,error_size,"root unavailable: %s",strerror(errno)); return NULL;
        }
    } else if(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        snprintf(error,error_size,"root must be a real private directory"); return NULL;
    }
    GcAppService *s=calloc(1,sizeof(*s)); if(!s) return NULL;
    s->lock_fd=s->log_fd=-1; snprintf(s->root,sizeof(s->root),"%s",root);
    if(pthread_mutex_init(&s->mutex,NULL)) {free(s); return NULL;}
    char path[512]; snprintf(path,sizeof(path),"%s/app-service.lock",root);
    s->lock_fd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(s->lock_fd<0 || fstat(s->lock_fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1) {
        snprintf(error,error_size,"ownership lock unavailable: %s",strerror(errno)); goto fail;
    }
    /* A successful exclusive lease is mandatory, even if a target filesystem
     * does not implement flock. An open descriptor or eventual port bind is
     * not ownership of this root. Fail before config/token/log/identity writes. */
    if(flock(s->lock_fd,LOCK_EX|LOCK_NB)) {
        int lock_error=errno;
        if(lock_error==EWOULDBLOCK || lock_error==EAGAIN)
            snprintf(error,error_size,"ownership unavailable; refusing without takeover");
        else
            snprintf(error,error_size,"ownership unavailable: exclusive lock failed: %s",
                     strerror(lock_error));
        goto fail;
    }
    /* Only a successful lease owner may discard a stale request. Do this
     * BEFORE config/native initialization, live listen or listening identity:
     * a request made after readiness belongs to this run and must survive. */
    if(app_exit_marker(s,error,error_size)<0) goto fail;
    /* Claim the lock BEFORE config/token/log mutation. Never probe another PID. */
    s->config=gc_config_open(root,error,error_size); if(!s->config) goto fail;
    snprintf(path,sizeof(path),"%s/app-service.log",root);
    s->log_fd=open(path,O_RDWR|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
    if(s->log_fd<0 || fstat(s->log_fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1) {
        if(s->log_fd>=0) { close(s->log_fd); s->log_fd=-1; }
        snprintf(error,error_size,"normal log unavailable"); goto fail;
    }
    uint64_t boot=0; int random_fd=open("/dev/urandom",O_RDONLY);
    ssize_t n=random_fd<0?-1:read(random_fd,&boot,sizeof(boot));
    if(random_fd>=0) close(random_fd);
    if(n!=(ssize_t)sizeof(boot) || !boot || !(s->started_ms=app_now())) {
        snprintf(error,error_size,"normal entropy/clock unavailable"); goto fail;
    }
    gc_app_pad_init(&s->pad,boot);
    char audio_root[256];
    if(snprintf(audio_root,sizeof(audio_root),"%s/audio",root)>=(int)sizeof(audio_root)) goto fail;
    /* AUDIO-07: the PCM provider seam is the console capture adapter. Created
     * before the audio owner, so the module always sees the same seam for the
     * life of the instance; with no capture backend it reports unavailable. */
    GcServiceAudioOptions audio_with_capture;
#if defined(GC_APP_SERVICE_NO_AUDIO_CAPTURE) || defined(GC_APP_SERVICE_DIAGNOSTIC)
    /* Input-only resident builds leave the optional console capture adapter
     * detached; AUDIO_STATUS remains wired and honestly reports unavailable. */
    s->capture=NULL;
#else
    s->capture=gc_service_capture_attach(audio_options,&audio_with_capture);
#endif
    s->audio=gc_service_audio_open(audio_root,boot,s->capture?&audio_with_capture:audio_options);
    if(!s->audio) {snprintf(error,error_size,"audio owner unavailable"); goto fail;}
    if(gc_config_snapshot(s->config,&s->runtime,NULL)) goto fail;
    s->running=s->runtime.autostart!=0;
    if(app_sync(s)) {snprintf(error,error_size,"app-pad config rejected"); goto fail;}
#ifdef GC_APP_SERVICE_RESIDENT
    if(s->running) {gc_service_features_init(s->config); s->features_started=1;}
#endif
    s->port=port?port:s->runtime.ipc_port;
    GcIpcOps ops={s,app_status,app_command,app_ipc_log};
    s->ipc=gc_ipc_start(s->port,root,&ops,error,error_size); if(!s->ipc) goto fail;
    /* No runtime identity is emitted until this instance owns a live IPC server. */
    if(app_instance_write(s,"listening")) {
        snprintf(error,error_size,"private runtime-instance readback unavailable"); goto fail;
    }
    app_log(s,"open"); return s;
fail:
    if(app_close_checked(s)) {
        /* A failed startup can already own callbacks/logger/native resources.
         * Returning NULL to main would CRT-exit and drop its original lease
         * without a checked release. Retain this actual owner quiescent. */
        fprintf(stderr,"app-service: startup cleanup uncertain; original ownership retained; replacement forbidden\n");
        for(;;) {struct timespec pause={1,0}; nanosleep(&pause,NULL);}
    }
    return NULL;
}

GcAppService *gc_app_service_open(const char *root,unsigned short port,char *error,size_t error_size)
{
    return gc_app_service_open_audio(root,port,error,error_size,NULL);
}
GcAudioError gc_app_service_audio_event(GcAppService *s,uint64_t session,GcAudioTransportEvent event)
{
    return s?gc_service_audio_transport_event(s->audio,session,event):GC_AUDIO_INVALID_ARGUMENT;
}

/* Caller holds the owner mutex through any native output using this decision. */
static int app_resident_colour_held(GcAppService *s, unsigned char rgb[3])
{
    uint64_t now=app_now();
    int app_owns=s->pad.last_poll_ms && now>=s->pad.last_poll_ms &&
        now-s->pad.last_poll_ms<1500;
    int due=!s->closing && !s->exit_requested && s->running && s->runtime.run_in_background && s->pad.enabled && !app_owns;
#ifdef GC_APP_SERVICE_RESIDENT
    due=due && !gc_service_features_retiring();
#endif
    unsigned effect=s->pad.effect, period=s->pad.period_ms?s->pad.period_ms:1000;
    unsigned step=s->pad.step_ms?s->pad.step_ms:32;
    unsigned r=s->pad.r, g=s->pad.g, b=s->pad.b;
    if(!due) return 0;
    unsigned phase=(unsigned)((now-now%step)%period);
    if(effect==1) {
        unsigned half=period/2; if(!half) half=1;
        unsigned tri=phase<half?phase:period-phase;
        unsigned x=tri*1000/half;
        unsigned sm=x*x*(3000-2*x)/1000000;
        sm=sm*sm/1000;
        r=r*sm/1000; g=g*sm/1000; b=b*sm/1000;
    } else if(effect==2) {
        unsigned hue=phase*1536/period, x=hue%256;
        switch(hue/256) {
        case 0: r=255; g=x; b=0; break;
        case 1: r=255-x; g=255; b=0; break;
        case 2: r=0; g=255; b=x; break;
        case 3: r=0; g=255-x; b=255; break;
        case 4: r=x; g=0; b=255; break;
        default: r=255; g=0; b=255-x; break;
        }
    } else if(effect==3) {
        int red=phase<period/2;
        unsigned u=red?phase:phase-period/2;
        int on=u<55 || (u>=110 && u<165);
        r=on&&red?255:0; g=0; b=on&&!red?255:0;
    } else if(effect==4 && phase>=period/8) {
        r=g=b=0;
    }
    rgb[0]=(unsigned char)r; rgb[1]=(unsigned char)g; rgb[2]=(unsigned char)b;
    return 1;
}
/* Calculation-only public helper; a returned colour is not an ownership grant. */
int gc_app_service_resident_colour(GcAppService *s, unsigned char rgb[3])
{
    if(!s || !rgb || pthread_mutex_lock(&s->mutex)) return 0;
    int due=app_resident_colour_held(s,rgb);
    if(pthread_mutex_unlock(&s->mutex)) return 0;
    return due;
}

#ifndef GC_APP_SERVICE_NO_MAIN
int gc_app_service_resident_colour(GcAppService *s, unsigned char rgb[3]);
void gc_service_output_apply(const unsigned char rgb[3]);
static volatile sig_atomic_t app_exit_requested;
static void app_signal(int signo) { (void)signo; app_exit_requested=1; }
int main(int argc,char **argv)
{
    const char *root=GC_APP_SERVICE_ROOT; unsigned short port=GC_APP_SERVICE_PORT;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--root") && i+1<argc) root=argv[++i];
        else if(!strcmp(argv[i],"--port") && i+1<argc) {
            const char *p=argv[++i]; unsigned n=0;
            if(!*p) return 2;
            for(;*p;p++) {if(*p<'0'||*p>'9'||n>6553) return 2; n=n*10+(unsigned)(*p-'0'); if(n>65535) return 2;}
            if(n<1024) return 2; port=(unsigned short)n;
        } else {fprintf(stderr,"usage: %s [--root private-directory] [--port 1024..65535]\n",argv[0]); return 2;}
    }
    signal(SIGINT,app_signal); signal(SIGTERM,app_signal);
    char error[256]=""; GcAppService *s=gc_app_service_open(root,port,error,sizeof(error));
    if(!s) {fprintf(stderr,"app-service: %s\n",error); return 1;}
    printf("app-service: local console port %u; normal app-owned output only\n",s->port); fflush(stdout);
    int status=0;
    while(!app_exit_requested) {
        pthread_mutex_lock(&s->mutex);
        int exiting=s->exit_requested;
        pthread_mutex_unlock(&s->mutex);
        if(exiting) break;
        /* Check before any guarded feature/output callback. On failure stop
         * naturally through the same join/closed/lease-held teardown as EXIT. */
        int marker=app_exit_marker(s,error,sizeof(error));
        if(marker) {
            if(marker<0) {
                fprintf(stderr,"app-service: %s\n",error);
                pthread_mutex_lock(&s->mutex); app_log(s,error); pthread_mutex_unlock(&s->mutex);
                status=1;
            }
            break;
        }
#ifdef GC_APP_SERVICE_RESIDENT
        unsigned char rgb[3];
        pthread_mutex_lock(&s->mutex);
        /* Admission, decision and actual native I/O share the cleanup lock.
         * STOP cannot retire an owner between permission and scePadOpen. */
        if(!s->closing && !s->exit_requested && s->running && s->features_started &&
           !gc_service_features_retiring()) {
            gc_service_features_tick(app_now());
            if(app_resident_colour_held(s,rgb)) {
                (void)APP_RELEASE_STAGE("output");
                gc_service_output_apply(rgb);
            }
        }
        pthread_mutex_unlock(&s->mutex);
#endif
        struct timespec pause={0,32000000}; nanosleep(&pause,NULL);
    }
    if(app_close_checked(s)) {
        /* Returning from main would release the lease via CRT/process exit.
         * On uncertainty stay quiescent and retain the original live owner.
         * No arbitrary join/close operation is promised a hard deadline. */
        fprintf(stderr,"app-service: checked release failed; original ownership retained where still held; replacement forbidden\n");
        for(;;) {struct timespec pause={1,0}; nanosleep(&pause,NULL);}
    }
    return status;
}
#endif
