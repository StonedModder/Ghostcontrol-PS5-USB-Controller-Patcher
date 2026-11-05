/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_controller_commands.h"
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Serialize shared command adapters; the config store owns the atomic merge,
 * durable save, persisted rule and generation. No backend calls live here. */
static pthread_mutex_t command_lock = PTHREAD_MUTEX_INITIALIZER;

static int write_json(struct json_object *o, char *out, size_t cap)
{
    if (!o) return -1;
    const char *text = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
    size_t n = strlen(text);
    int result = -1;
    if (n > 32766 || !out || cap <= n + 1) {
        struct json_object *small=json_object_new_object(), *v;
        const char *keys[]={"instance_contract_version","instance_storage_available","instance_live_available","instance_available","instance_policy_valid","saved","effect"};
        for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++)
            if (json_object_object_get_ex(o,keys[i],&v)) json_object_object_add(small,keys[i],json_object_get(v));
        json_object_object_add(small,"ok",json_object_new_boolean(0));
        json_object_object_add(small,"verified",json_object_new_boolean(0));
        json_object_object_add(small,"applied",json_object_new_boolean(0));
        json_object_object_add(small,"output_supported",json_object_new_boolean(0));
        json_object_object_add(small,"generation",NULL); json_object_object_add(small,"rule",NULL); json_object_object_add(small,"profile",NULL);
        json_object_object_add(small,"error",json_object_new_string("reply_too_large"));
        text=json_object_to_json_string_ext(small,JSON_C_TO_STRING_PLAIN); n=strlen(text);
        if (out && cap>n+1) {memcpy(out,text,n); out[n]='\n'; out[n+1]=0;}
        else if (out && cap) out[0]=0;
        json_object_put(small); json_object_put(o); return -1;
    }
    if (out && cap > n + 1) {
        memcpy(out, text, n); out[n] = '\n'; out[n+1] = 0; result = 0;
    } else if (out && cap) out[0] = 0;
    json_object_put(o);
    return result;
}
static int failure(const char *error, char *out, size_t cap)
{
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "saved", json_object_new_boolean(0));
    json_object_object_add(o, "applied", json_object_new_boolean(0));
    json_object_object_add(o, "output_supported", json_object_new_boolean(0));
    json_object_object_add(o, "error", json_object_new_string(error));
    (void)write_json(o, out, cap);
    return -1;
}
static void instance_caps(GcConfigStore *s, struct json_object *o)
{
    GcInstanceCapabilities c = {0};
    (void)gc_config_instance_capabilities(s, &c);
    json_object_object_add(o,"instance_contract_version",json_object_new_int(c.instance_contract_version));
    json_object_object_add(o,"instance_storage_available",json_object_new_boolean(c.instance_storage_available));
    json_object_object_add(o,"instance_live_available",json_object_new_boolean(c.instance_live_available));
    json_object_object_add(o,"instance_available",json_object_new_boolean(c.instance_available));
    json_object_object_add(o,"instance_policy_valid",json_object_new_boolean(c.instance_policy_valid));
}
static int instance_reply(GcConfigStore *s, const GcControllerInstanceMutationResult *r,
                          const char *error, char *out, size_t cap)
{
    int verified = r && r->committed && r->verified;
    struct json_object *o = json_object_new_object();
    instance_caps(s,o);
    json_object_object_add(o,"ok",json_object_new_boolean(verified && !error));
    json_object_object_add(o,"saved",json_object_new_boolean(r && r->committed));
    json_object_object_add(o,"verified",json_object_new_boolean(verified));
    json_object_object_add(o,"applied",json_object_new_boolean(0));
    json_object_object_add(o,"output_supported",json_object_new_boolean(0));
    json_object_object_add(o,"effect",json_object_new_string("instance_preference_only"));
    struct json_object *rule = NULL, *profile = NULL, *generation = NULL;
    if (verified) {
        char decimal[21]; snprintf(decimal,sizeof(decimal),"%llu",(unsigned long long)r->generation);
        generation = json_object_new_string(decimal);
        rule = json_object_new_object();
        json_object_object_add(rule,"id",json_object_new_string(r->rule.id));
        json_object_object_add(rule,"instance_id",json_object_new_string(r->rule.instance_id));
        json_object_object_add(rule,"profile_id",json_object_new_string(r->rule.profile_id));
        json_object_object_add(rule,"output_type",json_object_new_string(gc_controller_output_name(r->rule.output_type)));
        if (r->has_profile) {
            profile = json_object_new_object();
            json_object_object_add(profile,"id",json_object_new_string(r->profile.id));
            json_object_object_add(profile,"owner_controller_id",json_object_new_string(r->profile.owner_controller_id));
            json_object_object_add(profile,"owner_instance_id",json_object_new_string(r->profile.owner_instance_id));
        }
    }
    json_object_object_add(o,"generation",generation);
    json_object_object_add(o,"rule",rule); json_object_object_add(o,"profile",profile);
    json_object_object_add(o,"error",error ? json_object_new_string(error) : NULL);
    int rc = write_json(o,out,cap);
    return error || !verified ? -1 : rc;
}
static int digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Explicit radix avoids permissive strtoul signs/whitespace and overflow.
 * IDs/endpoints use hexadecimal, interface uses decimal or explicit 0x. */
static int number(const char *s, unsigned base, unsigned max, unsigned *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (!*s) return -1;
    unsigned n = 0;
    for (; *s; ++s) {
        int d = digit((unsigned char)*s);
        if (d < 0 || (unsigned)d >= base || (unsigned)d > max ||
            n > (max - (unsigned)d) / base) return -1;
        n = n * base + (unsigned)d;
    }
    *out = n; return 0;
}
static int boolean(const char *s, int *out)
{
    if (!strcmp(s, "true") || !strcmp(s, "1")) { *out = 1; return 0; }
    if (!strcmp(s, "false") || !strcmp(s, "0")) { *out = 0; return 0; }
    return -1;
}
static int endpoint(const char *s, int input, uint8_t *out)
{
    unsigned n = 0;
    if (strcmp(s, "auto") && number(s, 16, 255, &n)) return -1;
    if (n && ((!!(n & 0x80) != !!input) || (n & 0x70) || !(n & 15))) return -1;
    *out = (uint8_t)n; return 0;
}
static int clean(const char *s)
{
    for (; *s; ++s) if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f) return 0;
    return 1;
}
static int split(char *text, char **fields, size_t max)
{
    size_t n = 0;
    for (;;) {
        if (n == max || !*text) return -1;
        fields[n++] = text;
        char *p = strchr(text, '|');
        if (!p) break;
        *p = 0; text = p + 1;
        if (!*fields[n-1]) return -1;
    }
    return (int)n;
}
static struct json_object *rule_json(const GcControllerRule *r)
{
    struct json_object *o = json_object_new_object();
    char name[GC_PROFILE_NAME_MAX*2+1];
    size_t n = strlen(r->name);
    static const char digits[] = "0123456789abcdef";
    for (size_t i=0; i<n; ++i) {
        unsigned char c = (unsigned char)r->name[i];
        name[2*i] = digits[c >> 4]; name[2*i+1] = digits[c & 15];
    }
    name[2*n] = 0;
#define STR(k,v) json_object_object_add(o, k, json_object_new_string(v))
#define INT(k,v) json_object_object_add(o, k, json_object_new_int(v))
#define BOOL(k,v) json_object_object_add(o, k, json_object_new_boolean(v))
    STR("id", r->id); STR("name_encoding", "hex"); STR("name", name);
    STR("scope",r->instance_id[0] ? "instance" : "model");
    STR("instance_id",r->instance_id); STR("profile_id",r->profile_id);
    STR("protocol", r->protocol); STR("output_type", gc_controller_output_name(r->output_type));
    INT("vid", r->vid); INT("pid", r->pid);
    INT("interface_number", r->preferred_interface_number);
    INT("in_endpoint", r->preferred_in_endpoint); INT("out_endpoint", r->preferred_out_endpoint);
    BOOL("enabled", r->enabled); BOOL("ignore", r->ignore); BOOL("output_supported", 0);
#undef STR
#undef INT
#undef BOOL
    return o;
}
static int persisted_reply(const GcControllerRule *found, uint64_t generation,
                           char *out, size_t cap)
{
    struct json_object *o = json_object_new_object();
    json_object_object_add(o,"ok",json_object_new_boolean(1));
    json_object_object_add(o,"saved",json_object_new_boolean(1));
    json_object_object_add(o,"applied",json_object_new_boolean(0));
    json_object_object_add(o,"output_supported",json_object_new_boolean(0));
    json_object_object_add(o,"effect",json_object_new_string("controller_rule_persisted"));
    json_object_object_add(o,"id",json_object_new_string(found->id));
    json_object_object_add(o,"output_type",json_object_new_string(gc_controller_output_name(found->output_type)));
    json_object_object_add(o,"controller",rule_json(found));
    json_object_object_add(o,"generation",json_object_new_uint64(generation));
    return write_json(o,out,cap);
}
/* The store determines latest scope and mutation under one writer lock.
 * Never prelookup scope or reconstruct commit truth from a later snapshot. */
static int persist_compat(GcConfigStore *s,
                          const GcControllerInstanceMutation *intent,
                          char *out, size_t cap)
{
    char error[256] = "";
    GcControllerMutationScope scope = GC_CONTROLLER_MUTATION_SCOPE_NONE;
    GcControllerInstanceMutationResult published = {0};
    int rc = gc_config_mutate_controller_compat(s, intent, &scope, &published,
                                               error, sizeof error);
    if (scope == GC_CONTROLLER_MUTATION_SCOPE_INSTANCE)
        return instance_reply(s, &published,
                              rc ? (*error ? error : "publication_failed") : NULL,
                              out, cap);
    /* Preserve the legacy MODEL reply shape and error projection. Typed
     * copies still come from the same successful writer publication. */
    if (rc || !published.verified) return failure("controller_rejected",out,cap);
    return persisted_reply(&published.rule,published.generation,out,cap);
}
static int list(GcConfigStore *s, char *out, size_t cap)
{
    GcControllerRule rules[GC_CONFIG_MAX_CONTROLLERS]; uint64_t gen = 0;
    size_t n = gc_config_controllers(s,rules,GC_CONFIG_MAX_CONTROLLERS,&gen);
    struct json_object *o = json_object_new_object(), *items = json_object_new_array(), *types = json_object_new_array();
    instance_caps(s,o);
    json_object_object_add(o,"ok",json_object_new_boolean(1));
    json_object_object_add(o,"generation",json_object_new_int64((int64_t)gen));
    json_object_object_add(o,"controllers",items); json_object_object_add(o,"output_types",types);
    for (size_t i=0; i<n; ++i) json_object_array_add(items,rule_json(&rules[i]));
    static const char *labels[] = {"DualSense", "DualShock 4", "PSVR2 Sense Left", "PSVR2 Sense Right"};
    for (int i=0; i<GC_CONTROLLER_OUTPUT_COUNT; ++i) {
        struct json_object *type = json_object_new_object();
        json_object_object_add(type,"id",json_object_new_string(gc_controller_output_name((GcControllerOutputType)i)));
        json_object_object_add(type,"label",json_object_new_string(labels[i]));
        json_object_object_add(type,"supported",json_object_new_boolean(0));
        json_object_array_add(types,type);
    }
    return write_json(o,out,cap);
}
/* Flat exact-key envelope: scan decoded key tokens before json-c can collapse
 * duplicate names, including escaped-equivalent keys. No nested values belong
 * to this protocol. json-c subsequently validates syntax and UTF-8 strictly. */
static struct json_object *strict_json(const char *text, size_t n)
{
    struct json_tokener *t = json_tokener_new();
    if (!t) return NULL;
    json_tokener_set_flags(t,JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *o = json_tokener_parse_ex(t,text,(int)n+1);
    if (json_tokener_get_error(t)!=json_tokener_success ||
        json_tokener_get_parse_end(t)!=n) { json_object_put(o); o=NULL; }
    json_tokener_free(t); return o;
}
/* json-c's UTF-8 flag is not a shortest-form/codepoint validator on every
 * supported host. Validate scalar values explicitly, before any mutation. */
static int utf8_scalars(const unsigned char *p)
{
    while (*p) {
        unsigned c=*p++, value, need, minimum;
        if (c<128) continue;
        if (c>=0xc2 && c<=0xdf) {value=c&31; need=1; minimum=128;}
        else if (c>=0xe0 && c<=0xef) {value=c&15; need=2; minimum=2048;}
        else if (c>=0xf0 && c<=0xf4) {value=c&7; need=3; minimum=65536;}
        else return 0;
        for (unsigned i=0;i<need;i++) {if ((*p&0xc0)!=0x80) return 0; value=(value<<6)|(*p++&63);}
        if (value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return 0;
    }
    return 1;
}
static int unicode_escape(const char *p,unsigned *value)
{
    *value=0;
    for (int i=0;i<4;i++) {if (!p[i]) return 0; int d=digit((unsigned char)p[i]); if (d<0) return 0; *value=(*value<<4)|(unsigned)d;}
    return 1;
}
static const char *string_end(const char *p)
{
    if (*p++!='"') return NULL;
    while (*p) {
        if (*p=='"') return p+1;
        if (*p=='\\') {
            if (!p[1]) return NULL;
            if (p[1]=='u') {
                unsigned value;
                if (!unicode_escape(p+2,&value)) return NULL;
                p+=6;
                if (value>=0xd800 && value<=0xdbff) {
                    if (p[0]!='\\' || p[1]!='u' || !unicode_escape(p+2,&value) || value<0xdc00 || value>0xdfff) return NULL;
                    p+=6;
                } else if (value>=0xdc00 && value<=0xdfff) return NULL;
                continue;
            }
            p++;
        }
        p++;
    }
    return NULL;
}
static int unique_flat_keys(const char *text)
{
    char keys[14][64]; size_t count=0;
    const char *p=text; while (*p==' ') p++;
    if (*p++!='{') return 0;
    for (;;) {
        while (*p==' ') p++;
        if (*p=='}') return count==14;
        const char *end=string_end(p);
        if (!end || count==14 || (size_t)(end-p)>=GC_INSTANCE_JSON_MAX) return 0;
        char token[GC_INSTANCE_JSON_MAX]; memcpy(token,p,(size_t)(end-p)); token[end-p]=0;
        struct json_object *key=strict_json(token,strlen(token));
        if (!key || json_object_get_type(key)!=json_type_string) {json_object_put(key); return 0;}
        const char *k=json_object_get_string(key); size_t len=(size_t)json_object_get_string_len(key);
        if (len>=sizeof(keys[0]) || strlen(k)!=len) {json_object_put(key); return 0;}
        for (size_t i=0;i<count;i++) if (!strcmp(keys[i],k)) {json_object_put(key); return 0;}
        memcpy(keys[count++],k,len+1); json_object_put(key);
        p=end; while (*p==' ') p++; if (*p++!=':') return 0;
        while (*p==' ') p++;
        if (*p=='"') {p=string_end(p); if (!p) return 0;}
        else {while (*p && *p!=',' && *p!='}') {if (*p=='{' || *p=='[' || *p=='"') return 0; p++;}}
        while (*p==' ') p++;
        if (*p=='}') return count==14;
        if (*p++!=',') return 0;
    }
}
static int field_string(struct json_object *o,const char *key,char *out,size_t cap,int empty)
{
    struct json_object *v;
    if (!json_object_object_get_ex(o,key,&v) || json_object_get_type(v)!=json_type_string) return -1;
    const char *p=json_object_get_string(v); size_t n=(size_t)json_object_get_string_len(v);
    if ((!empty && !n) || n>=cap || strlen(p)!=n || !clean(p)) return -1;
    memcpy(out,p,n+1); return 0;
}
static int field_int(struct json_object *o,const char *key,int min,int max,int *out)
{
    struct json_object *v;
    if (!json_object_object_get_ex(o,key,&v) || json_object_get_type(v)!=json_type_int) return -1;
    int64_t n=json_object_get_int64(v);
    if (n<min || n>max) return -1;
    *out=(int)n; return 0;
}
static int field_bool(struct json_object *o,const char *key,int *out)
{
    struct json_object *v;
    if (!json_object_object_get_ex(o,key,&v) || json_object_get_type(v)!=json_type_boolean) return -1;
    *out=json_object_get_boolean(v); return 0;
}
static int literal_id(const char *p,int empty)
{
    if (!*p) return empty;
    if (!strcmp(p,".") || !strcmp(p,"..")) return 0;
    for (;*p;p++) if (!((*p>='A' && *p<='Z') || (*p>='a' && *p<='z') ||
        (*p>='0' && *p<='9') || *p=='_' || *p=='.' || *p=='-')) return 0;
    return 1;
}
static int instance_save(GcConfigStore *s,const char *arg,char *out,size_t cap)
{
    char text[GC_INSTANCE_JSON_MAX]; size_t n=strnlen(arg,GC_INSTANCE_JSON_MAX*2);
    const char *error="invalid_instance_request";
    if (!n || (n&1) || n>2*(GC_INSTANCE_JSON_MAX-1)) return instance_reply(s,NULL,error,out,cap);
    for (size_t i=0;i<n/2;i++) {
        int hi=digit((unsigned char)arg[2*i]),lo=digit((unsigned char)arg[2*i+1]);
        if (hi<0 || lo<0) return instance_reply(s,NULL,error,out,cap);
        unsigned c=(unsigned)((hi<<4)|lo);
        if (c<32 || c==127) return instance_reply(s,NULL,error,out,cap);
        text[i]=(char)c;
    }
    text[n/2]=0;
    if (!utf8_scalars((const unsigned char *)text) || !unique_flat_keys(text)) return instance_reply(s,NULL,error,out,cap);
    struct json_object *o=strict_json(text,n/2),*v;
    if (!o || json_object_get_type(o)!=json_type_object) {json_object_put(o); return instance_reply(s,NULL,error,out,cap);}
    if (!json_object_object_get_ex(o,"v",&v) || json_object_get_type(v)!=json_type_int) goto done;
    if (json_object_get_int64(v)!=GC_INSTANCE_CONTRACT_VERSION) {error="unsupported_instance_contract"; goto done;}
    GcControllerInstanceMutation intent={.mode=GC_INSTANCE_MUTATION_FULL,.output_present=1};
    GcControllerRule *r=&intent.rule; int vid,pid,in,out_ep,iface; char output[32];
    if (json_object_object_length(o)!=14 ||
        field_string(o,"id",r->id,sizeof(r->id),0) || !literal_id(r->id,0) ||
        field_string(o,"name",r->name,sizeof(r->name),0) ||
        field_string(o,"protocol",r->protocol,sizeof(r->protocol),0) ||
        field_string(o,"instance_id",r->instance_id,sizeof(r->instance_id),0) ||
        field_string(o,"profile_id",r->profile_id,sizeof(r->profile_id),1) || !literal_id(r->profile_id,1) ||
        field_string(o,"output_type",output,sizeof(output),0) || gc_controller_output_parse(output,&r->output_type) ||
        field_int(o,"vid",0,65535,&vid) || field_int(o,"pid",0,65535,&pid) ||
        field_int(o,"in_endpoint",0,255,&in) || field_int(o,"out_endpoint",0,255,&out_ep) ||
        field_int(o,"interface_number",-1,255,&iface) ||
        field_bool(o,"enabled",&r->enabled) || field_bool(o,"ignore",&r->ignore)) goto done;
    if (strncmp(r->instance_id,"bt-classic:",11)) {error="unsupported_instance_namespace"; goto done;}
    if (strlen(r->instance_id)!=23 || !strcmp(r->instance_id,"bt-classic:000000000000") ||
        !strcmp(r->instance_id,"bt-classic:FFFFFFFFFFFF")) {error="invalid_instance_selector"; goto done;}
    for (size_t i=11;i<23;i++) if (!((r->instance_id[i]>='0' && r->instance_id[i]<='9') ||
        (r->instance_id[i]>='A' && r->instance_id[i]<='F'))) {error="invalid_instance_selector"; goto done;}
    r->vid=(uint16_t)vid; r->pid=(uint16_t)pid;
    r->preferred_in_endpoint=(uint8_t)in; r->preferred_out_endpoint=(uint8_t)out_ep;
    r->preferred_interface_number=iface;
    GcControllerInstanceMutationResult published={0}; char why[256]="";
    int rc=gc_config_mutate_controller_instance(s,&intent,&published,why,sizeof(why));
    json_object_put(o);
    return instance_reply(s,&published,rc ? (*why ? why : "publication_failed") : NULL,out,cap);
done:
    json_object_put(o); return instance_reply(s,NULL,error,out,cap);
}
static int save(GcConfigStore *s, const char *arg, char *out, size_t cap)
{
    char text[512], *f[11]; size_t n = strlen(arg);
    if (!n || (n & 1) || n/2 >= sizeof(text)) return failure("invalid_controller_encoding",out,cap);
    for (size_t i=0; i<n/2; ++i) {
        int hi = digit((unsigned char)arg[2*i]), lo = digit((unsigned char)arg[2*i+1]);
        if (hi < 0 || lo < 0) return failure("invalid_controller_encoding",out,cap);
        unsigned c = (unsigned)((hi << 4) | lo);
        if (c < 0x20 || c == 0x7f) return failure("invalid_controller_rule",out,cap);
        text[i] = (char)c;
    }
    text[n/2] = 0;
    int count = split(text,f,11);
    if (count < 9 || count > 11) return failure("invalid_controller_rule",out,cap);
    GcControllerRule r = {.preferred_interface_number = -1};
    unsigned vid, pid, iface;
    if (strlen(f[0]) >= sizeof(r.id) || strlen(f[1]) >= sizeof(r.name) ||
        strlen(f[4]) >= sizeof(r.protocol) || number(f[2],16,65535,&vid) ||
        number(f[3],16,65535,&pid) || endpoint(f[5],1,&r.preferred_in_endpoint) ||
        endpoint(f[6],0,&r.preferred_out_endpoint) || boolean(f[7],&r.enabled) || boolean(f[8],&r.ignore))
        return failure("invalid_controller_rule",out,cap);
    if (count >= 10 && strcmp(f[9],"auto")) {
        if (number(f[9],10,255,&iface)) return failure("invalid_controller_rule",out,cap);
        r.preferred_interface_number = (int)iface;
    }
    strcpy(r.id,f[0]); strcpy(r.name,f[1]); strcpy(r.protocol,f[4]); r.vid=(uint16_t)vid; r.pid=(uint16_t)pid;
    if (count == 11 && gc_controller_output_parse(f[10],&r.output_type))
        return failure("invalid_controller_rule",out,cap);
    GcControllerInstanceMutation intent = {
        .mode = count == 9 ? GC_INSTANCE_MUTATION_LEGACY9 :
                count == 10 ? GC_INSTANCE_MUTATION_LEGACY10 : GC_INSTANCE_MUTATION_LEGACY11,
        .output_present = count == 11,
        .rule = r
    };
    /* LEGACY9 omits this field; zero is the contract's unused intent, while
     * the writer applies the legacy -1 interface meaning during its merge. */
    if (count == 9) intent.rule.preferred_interface_number = 0;
    return persist_compat(s,&intent,out,cap);
}
int gc_controller_command(GcConfigStore *s, const char *cmd, const char *arg, char *out, size_t cap)
{
    if (!cmd || (strcmp(cmd,"CONTROLLERS") && strcmp(cmd,"CONTROLLER_SAVE") && strcmp(cmd,"CONTROLLER_OUTPUT_SET") && strcmp(cmd,"CONTROLLER_INSTANCE_SAVE"))) return 1;
    if (!out || !cap) return -1;
    if (!s) return !strcmp(cmd,"CONTROLLER_INSTANCE_SAVE") ? instance_reply(s,NULL,"instance_capability_unavailable",out,cap) : failure("config_unavailable",out,cap);
    if (!arg) arg = "";
    pthread_mutex_lock(&command_lock);
    int result;
    if (!strcmp(cmd,"CONTROLLER_INSTANCE_SAVE")) {
        result = instance_save(s,arg,out,cap);
    } else if (!strcmp(cmd,"CONTROLLERS")) {
        result = *arg ? failure("invalid_controller_argument",out,cap) : list(s,out,cap);
    } else if (!strcmp(cmd,"CONTROLLER_SAVE")) result = save(s,arg,out,cap);
    else {
        char text[GC_PROFILE_ID_MAX+32], *fields[2];
        GcControllerOutputType output_type;
        if (strlen(arg) >= sizeof(text) || !clean(arg)) result = failure("invalid_controller_rule",out,cap);
        else {
            strcpy(text,arg);
            if (split(text,fields,2) != 2 || gc_controller_output_parse(fields[1],&output_type))
                result = failure("invalid_controller_rule",out,cap);
            else if (strlen(fields[0]) >= GC_PROFILE_ID_MAX)
                result = failure("controller_rejected",out,cap);
            else {
                GcControllerInstanceMutation intent = {
                    .mode = GC_INSTANCE_MUTATION_OUTPUT_SET, .output_present = 1
                };
                strcpy(intent.rule.id,fields[0]);
                intent.rule.output_type = output_type;
                result = persist_compat(s,&intent,out,cap);
            }
        }
    }
    pthread_mutex_unlock(&command_lock);
    return result;
}
