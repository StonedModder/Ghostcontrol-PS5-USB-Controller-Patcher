/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_service_audio.h"
#include <json-c/json.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

struct GcServiceAudio {
    pthread_mutex_t lock;
    pthread_t worker;
    int stopping;
    int release_failed;
    GcAudio audio;
    GcServiceAudioOptions options;
    GcAudioInventory inventory;
    const char *inventory_error;
    uint64_t instance, revision, selection_generation, deadline;
    int recovering;
    char selected[GC_AUDIO_ADDRESS_TEXT], root[256];
};
static uint64_t now_ms(void) {
    struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
static void *deadline_worker(void *ctx) {
    GcServiceAudio *s=ctx;
    for(;;) {
        pthread_mutex_lock(&s->lock);
        if(s->stopping) {pthread_mutex_unlock(&s->lock); return NULL;}
        if(s->deadline) {
            GcAudioSnapshot a; gc_audio_snapshot(&s->audio,&a);
            if(a.state!=GC_AUDIO_STATE_PENDING) {s->deadline=0; s->recovering=0;}
            else if(now_ms()>=s->deadline) {
                (void)gc_audio_on_transport(&s->audio,a.session,GC_AUDIO_EVENT_LOST);
                s->deadline=0; s->recovering=0; ++s->revision;
            }
        }
        pthread_mutex_unlock(&s->lock);
        struct timespec delay={0,20000000}; nanosleep(&delay,NULL);
    }
}
GcServiceAudio *gc_service_audio_open(const char *root,uint64_t instance,const GcServiceAudioOptions *options) {
    if(!root || !instance || strlen(root)>=256) return NULL;
    GcServiceAudio *s=calloc(1,sizeof(*s)); if(!s) return NULL;
    if(options) s->options=*options;
    strcpy(s->root,root); s->instance=instance; s->revision=1;
    if(pthread_mutex_init(&s->lock,NULL)) {free(s); return NULL;}
    if(gc_audio_init(&s->audio,s->options.provider,s->options.transport,s->options.encoder,root)) {
        pthread_mutex_destroy(&s->lock); free(s); return NULL;
    }
    if(pthread_create(&s->worker,NULL,deadline_worker,s)) {
        gc_audio_destroy(&s->audio); pthread_mutex_destroy(&s->lock); free(s); return NULL;
    }
    return s;
}
void gc_service_audio_close(GcServiceAudio *s) {
    if(!s) return;
    pthread_mutex_lock(&s->lock); s->stopping=1; pthread_mutex_unlock(&s->lock);
    pthread_join(s->worker,NULL); gc_audio_destroy(&s->audio); pthread_mutex_destroy(&s->lock); free(s);
}
int gc_service_audio_diagnostic(const GcServiceAudio *s) {
    return s && !s->options.provider && !s->options.transport &&
        !s->options.encoder && !s->options.inventory;
}
int gc_service_audio_close_diagnostic(GcServiceAudio *s) {
    if(!s) return 0;
    if(!gc_service_audio_diagnostic(s) || s->release_failed) return -1;
    s->release_failed=1;
    if(pthread_mutex_lock(&s->lock)) return -1;
    s->stopping=1;
    if(pthread_mutex_unlock(&s->lock)) return -1;
    if(pthread_join(s->worker,NULL)) return -1;
    /* IPC is joined and this provider-less owner has no external callbacks.
     * Preserve the ordinary shutdown, but check every destruction result. */
    gc_audio_shutdown(&s->audio);
    if(pthread_cond_destroy(&s->audio.quiet)) return -1;
    if(pthread_mutex_destroy(&s->audio.mutex)) return -1;
    if(pthread_mutex_destroy(&s->lock)) return -1;
    free(s); return 0;
}
const char *gc_service_audio_release_blocker(const GcServiceAudio *s) {
    if (!s) return NULL;
    if (s->release_failed) return "audio_release_uncertain";
    if (s->options.provider) return "audio_provider_close_has_no_checked_release";
    if (s->options.transport) return "audio_transport_close_has_no_checked_release";
    if (s->options.encoder || s->options.inventory) return "audio_external_callback_quiescence_unavailable";
    return NULL;
}
int gc_service_audio_close_checked(GcServiceAudio *s) {
    if (gc_service_audio_release_blocker(s)) return -1;
    return gc_service_audio_close_diagnostic(s);
}
GcAudioError gc_service_audio_transport_event(GcServiceAudio *s,uint64_t session,GcAudioTransportEvent event) {
    if(!s) return GC_AUDIO_INVALID_ARGUMENT;
    /* Callback uses the approved owner's own lock, never shutdown here. */
    return gc_audio_on_transport(&s->audio,session,event);
}
#define B(o,k,v) json_object_object_add(o,k,json_object_new_boolean(v))
#define S(o,k,v) json_object_object_add(o,k,json_object_new_string(v))
#define I(o,k,v) json_object_object_add(o,k,json_object_new_int(v))
static void u64(struct json_object *o,const char *key,uint64_t value) {
    char text[21]; snprintf(text,sizeof(text),"%llu",(unsigned long long)value); S(o,key,text);
}
static int reply(struct json_object *o,char *out,size_t cap) {
    const char *text=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN); size_t n=strlen(text);
    int rc=0;
    if(n>8190 || !out || cap<n+2) {if(out && cap) out[0]=0; rc=-1;}
    else {memcpy(out,text,n); out[n]='\n'; out[n+1]=0;}
    json_object_put(o); return rc;
}
static int inventory(GcServiceAudio *s) {
    GcAudioInventory v={0};
    memset(&s->inventory,0,sizeof(s->inventory));
    s->inventory_error="unavailable";
    if(!s->options.inventory || s->options.inventory(s->options.inventory_context,&v) || !v.available) return 0;
    s->inventory_error="invalid_identity";
    if(v.available!=1 || !v.generation || v.count>GC_SERVICE_AUDIO_PEERS) return -1;
    for(size_t i=0;i<v.count;i++) {
        char canonical[18];
        if(!memchr(v.peers[i].address,0,18) || gc_audio_address_canonical(v.peers[i].address,canonical) ||
           strcmp(canonical,v.peers[i].address) || v.peers[i].kind<1 || v.peers[i].kind>3) return -1;
        for(size_t j=0;j<i;j++) if(!strcmp(v.peers[i].address,v.peers[j].address)) return -1;
    }
    s->inventory=v; s->inventory_error="none"; return 0;
}
static int headset(GcServiceAudio *s,const char *address) {
    if(!s->inventory.available) return 0;
    for(size_t i=0;i<s->inventory.count;i++) if(s->inventory.peers[i].kind==GC_AUDIO_PEER_HEADSET && !strcmp(s->inventory.peers[i].address,address)) return 1;
    return 0;
}
static const char *state(GcServiceAudio *s,const GcAudioSnapshot *a) {
    if(a->state==GC_AUDIO_STATE_PENDING) return s->recovering?"recovering":"pending";
    if(a->state==GC_AUDIO_STATE_STREAMING) return "streaming";
    if(a->state==GC_AUDIO_STATE_SUSPENDED) return "suspended";
    if(a->last_error!=GC_AUDIO_OK) return "error";
    return a->state==GC_AUDIO_STATE_UNAVAILABLE?"unavailable":"idle";
}
/* Saved preference inventory is file-only, bounded and independent of current
 * measured profile inventory. It can never authorize SELECT or START. */
static void saved_inventory(GcServiceAudio *s,struct json_object *o) {
    struct json_object *list=json_object_new_array();
    int available=1,truncated=0;
    int fd=open(s->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0) {if(errno!=ENOENT) available=0;}
    else {
        DIR *dir=fdopendir(fd);
        if(!dir) {close(fd); available=0;}
        else {
            unsigned scanned=0,count=0; struct dirent *entry;
            while((entry=readdir(dir))!=NULL) {
                if(++scanned>64) {truncated=1; break;}
                const char *name=entry->d_name;
                if(strlen(name)!=sizeof("audio-000000000000.ini")-1 || strncmp(name,"audio-",6) || strcmp(name+18,".ini")) continue;
                char address[GC_AUDIO_ADDRESS_TEXT],canonical[GC_AUDIO_ADDRESS_TEXT];
                for(size_t i=0;i<6;i++) {address[i*3]=name[6+i*2]; address[i*3+1]=name[7+i*2]; address[i*3+2]=i==5?0:':';}
                if(gc_audio_address_canonical(address,canonical) || strcmp(address,canonical)) continue;
                if(count==GC_SERVICE_AUDIO_PEERS) {truncated=1; break;}
                struct stat st;
                if(fstatat(dirfd(dir),name,&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_nlink!=1) {available=0; continue;}
                struct json_object *v=json_object_new_object(); S(v,"address",address); S(v,"kind","saved_preference");
                B(v,"record_present",1); B(v,"readback_verified",0); json_object_array_add(list,v); ++count;
            }
            closedir(dir);
        }
    }
    B(o,"saved_inventory_available",available); B(o,"saved_inventory_truncated",truncated);
    json_object_object_add(o,"saved_preferences",list);
}

static struct json_object *status(GcServiceAudio *s,GcAudioError error,const GcAudioPrefsResult *write) {
    GcAudioSnapshot a; GcAudioCapabilities c; gc_audio_snapshot(&s->audio,&a); gc_audio_capabilities(&s->audio,&c);
    struct json_object *o=json_object_new_object(); B(o,"ok",error==GC_AUDIO_OK); I(o,"audio_schema",1);
    S(o,"error",gc_audio_error_name(error)); S(o,"last_error",gc_audio_error_name(a.last_error)); S(o,"state",state(s,&a));
    u64(o,"instance",s->instance); u64(o,"revision",s->revision); u64(o,"session",a.session);
    u64(o,"consent_generation",a.consent_generation); u64(o,"selection_generation",s->selection_generation);
    u64(o,"inventory_generation",s->inventory.generation); B(o,"inventory_available",s->inventory.available);
    S(o,"inventory_error",s->inventory_error);
    B(o,"accepted",error==GC_AUDIO_OK); B(o,"persisted",0); B(o,"readback_verified",0); B(o,"durable",0);
    B(o,"battery_available",0); S(o,"battery_reason","controls_adapter_unavailable");
    B(o,"service_wired",1); B(o,"provider_available",c.provider_available); B(o,"transport_authorized",c.transport_authorized);
    B(o,"encoder_available",c.encoder_available); B(o,"target_verified",0); B(o,"microphone",0); B(o,"game_capture",0); B(o,"le_audio",0);
    S(o,"source_id",c.source_id); I(o,"source_scope",c.source_scope); B(o,"consent_granted",a.consent_granted);
    S(o,"consent_source",a.consent_source); S(o,"selected",s->selected); S(o,"headset",a.headset);
    S(o,"blocking",gc_audio_error_name(c.blocking)); S(o,"seam_blocking",gc_audio_error_name(c.seam_blocking));
    S(o,"reason",c.seam_reason); B(o,"applied",0); B(o,"pending",a.state==GC_AUDIO_STATE_PENDING);
    struct json_object *peers=json_object_new_array(); const char *next=""; int found=0;
    for(size_t i=0;i<s->inventory.count;i++) {
        const GcAudioPeer *p=&s->inventory.peers[i]; struct json_object *v=json_object_new_object();
        S(v,"address",p->address); S(v,"kind",p->kind==GC_AUDIO_PEER_HEADSET?"headset":p->kind==GC_AUDIO_PEER_PAD?"pad":"unknown");
        json_object_array_add(peers,v);
        if(p->kind==GC_AUDIO_PEER_HEADSET) {if(!*next || found) {next=p->address; found=0;} if(!strcmp(s->selected,p->address)) found=1;}
    }
    json_object_object_add(o,"peers",peers); S(o,"next_headset",next);
    GcAudioSettingsLoad loaded={0};
    if(*s->selected) gc_audio_settings_load(s->root,s->selected,&loaded); else loaded.settings=gc_audio_settings_defaults();
    B(o,"settings_available",*s->selected && loaded.status!=GC_AUDIO_LOAD_ERROR);
    B(o,"saved_preference",loaded.status==GC_AUDIO_LOAD_OK); u64(o,"settings_generation",loaded.settings.generation);
    I(o,"volume",loaded.settings.volume); I(o,"latency_ms",loaded.settings.latency_ms);
    if(write) { B(o,"accepted",write->accepted); B(o,"persisted",write->persisted); B(o,"readback_verified",write->readback_verified); B(o,"durable",write->durable); }
    saved_inventory(s,o);
    return o;
}
static int decimal(const char *text,uint64_t *out) {
    if(!*text || (text[0]=='0' && text[1])) return -1;
    uint64_t v=0;
    for(const unsigned char *p=(const unsigned char *)text;*p;p++) {
        if(*p<'0'||*p>'9'||v>(UINT64_MAX-(uint64_t)(*p-'0'))/10) return -1;
        v=v*10+(uint64_t)(*p-'0');
    }
    *out=v; return 0;
}
int gc_service_audio_command(GcServiceAudio *s,const char *cmd,const char *arg,char *out,size_t cap) {
    if(!cmd || strncmp(cmd,"AUDIO_",6)) return 1;
    if(!s) return -1;
    pthread_mutex_lock(&s->lock);
    GcAudioError e=GC_AUDIO_OK; GcAudioPrefsResult write={0}; int has_write=0;
    if(!arg) arg="";
    /* ASCII-only grammar is a strict UTF-8 subset. No paths or free text. */
    size_t length=strnlen(arg,513); if(length>512) e=GC_AUDIO_INVALID_ARGUMENT;
    for(size_t i=0;i<length;i++) if((unsigned char)arg[i]<33 || (unsigned char)arg[i]>126) e=GC_AUDIO_INVALID_ARGUMENT;
    /* Passive observation is acquisition authority, not withdrawal authority.
     * Keep STATUS truthful even when the observer cannot supply valid peers. */
    const int withdrawal=!strcmp(cmd,"AUDIO_STOP") || !strcmp(cmd,"AUDIO_REVOKE");
    const int invalid_inventory=inventory(s);
    if(!strcmp(cmd,"AUDIO_STATUS")) {if(*arg) e=GC_AUDIO_INVALID_ARGUMENT; goto done;}
    if(invalid_inventory && !withdrawal && !e) e=GC_AUDIO_INVALID_IDENTITY;
    const char *commands[]={"AUDIO_SELECT","AUDIO_CONSENT","AUDIO_REVOKE","AUDIO_START","AUDIO_STOP","AUDIO_SUSPEND","AUDIO_RETRY","AUDIO_PREFS"};
    int known=0; for(size_t i=0;i<sizeof(commands)/sizeof(commands[0]);i++) if(!strcmp(cmd,commands[i])) known=1;
    if(!known) {e=GC_AUDIO_INVALID_ARGUMENT; goto done;}
    if(e) goto done;
    char copy[513]; memcpy(copy,arg,length+1); char *parts[10]; size_t count=0;
    parts[count++]=copy;
    for(char *p=copy;*p;p++) if(*p=='|') {if(count==10) {e=GC_AUDIO_INVALID_ARGUMENT; goto done;} *p=0; parts[count++]=p+1;}
    size_t wanted=!strcmp(cmd,"AUDIO_SELECT") || !strcmp(cmd,"AUDIO_CONSENT")?7:!strcmp(cmd,"AUDIO_PREFS")?9:6;
    if(count!=wanted) {e=GC_AUDIO_INVALID_ARGUMENT; goto done;}
    uint64_t fence[6]; for(size_t i=0;i<6;i++) if(decimal(parts[i],&fence[i])) {e=GC_AUDIO_INVALID_ARGUMENT; goto done;}
    GcAudioSnapshot a; gc_audio_snapshot(&s->audio,&a);
    if(fence[0]!=s->instance || fence[1]!=s->revision || fence[2]!=a.session || fence[3]!=a.consent_generation || fence[4]!=s->selection_generation || (!withdrawal && fence[5]!=s->inventory.generation)) {e=GC_AUDIO_STALE_GENERATION; goto done;}
    if(s->revision==UINT64_MAX || s->selection_generation==UINT64_MAX) {e=GC_AUDIO_BUSY; goto done;}
    if(!strcmp(cmd,"AUDIO_SELECT")) {
        char canonical[18]; if(gc_audio_address_canonical(parts[6],canonical) || strcmp(canonical,parts[6]) || !headset(s,parts[6])) {e=GC_AUDIO_INVALID_IDENTITY; goto done;}
        if(a.state>=GC_AUDIO_STATE_PENDING) {e=GC_AUDIO_BUSY; goto done;}
        strcpy(s->selected,parts[6]); ++s->selection_generation;
    } else if(!strcmp(cmd,"AUDIO_CONSENT")) {
        GcAudioCapabilities c; gc_audio_capabilities(&s->audio,&c);
        if(!c.source_valid || strcmp(parts[6],c.source_id)) {e=GC_AUDIO_INVALID_IDENTITY; goto done;}
        e=gc_audio_grant_consent(&s->audio,parts[6],NULL);
    } else if(!strcmp(cmd,"AUDIO_REVOKE")) e=gc_audio_revoke_consent(&s->audio);
    else if(!strcmp(cmd,"AUDIO_STOP")) {e=gc_audio_stop(&s->audio); s->deadline=0; s->recovering=0;}
    else if(!strcmp(cmd,"AUDIO_SUSPEND")) e=gc_audio_suspend(&s->audio);
    else if(!strcmp(cmd,"AUDIO_PREFS")) {
        uint64_t generation,volume,latency;
        if(decimal(parts[6],&generation)||decimal(parts[7],&volume)||decimal(parts[8],&latency)||volume>127||latency<40||latency>500||!headset(s,s->selected)) {e=GC_AUDIO_INVALID_ARGUMENT; goto done;}
        GcAudioSettingsLoad l; gc_audio_settings_load(s->root,s->selected,&l);
        if(l.status==GC_AUDIO_LOAD_ERROR) {e=GC_AUDIO_READBACK_FAILED; goto done;}
        l.settings.volume=(uint8_t)volume; l.settings.latency_ms=(uint16_t)latency;
        write=gc_audio_set_preferences(&s->audio,s->selected,generation,&l.settings); has_write=1; e=write.error;
    } else {
        int retry=!strcmp(cmd,"AUDIO_RETRY");
        if(!headset(s,s->selected)) {e=GC_AUDIO_INVALID_IDENTITY; goto done;}
        if(retry && (a.last_error==GC_AUDIO_OK || a.state>=GC_AUDIO_STATE_PENDING)) {e=GC_AUDIO_INVALID_ARGUMENT; goto done;}
        if(!retry && a.last_error!=GC_AUDIO_OK) {e=GC_AUDIO_INVALID_ARGUMENT; goto done;}
        GcAudioCapabilities source; gc_audio_capabilities(&s->audio,&source);
        if(!source.source_valid) {e=GC_AUDIO_SOURCE_UNAVAILABLE; goto done;}
        e=gc_audio_start(&s->audio,s->selected,source.source_id,a.consent_generation);
        if(!e) {s->recovering=retry; s->deadline=now_ms()+2000;}
    }
    if(!e || (has_write && write.persisted)) ++s->revision;
 done:;
    int rc=reply(status(s,e,has_write?&write:NULL),out,cap);
    pthread_mutex_unlock(&s->lock); return rc;
}
