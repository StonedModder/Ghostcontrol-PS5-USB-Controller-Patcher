/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gc_profile.h"
#include "gc_controller_instance.h"
#include "gc_controller_type.h"

#define GC_DATA_ROOT "/data/ghostcontrol"
#define GC_CONFIG_SCHEMA 1
#define GC_CONTROLLERS_SCHEMA_LEGACY 1
#define GC_CONTROLLERS_SCHEMA_INSTANCE 2
#define GC_CONFIG_MAX_PROFILES 128
#define GC_CONFIG_MAX_CONTROLLERS 32
#define GC_CONFIG_PATH_MAX 384
#define GC_IPC_PORT 39071

typedef struct {
    char id[GC_PROFILE_ID_MAX];
    char name[GC_PROFILE_NAME_MAX];
    int enabled;
    int ignore;
    uint16_t vid;
    uint16_t pid;
    char protocol[GC_PROFILE_PROTOCOL_MAX];
    uint8_t preferred_in_endpoint;
    uint8_t preferred_out_endpoint;
    /* Stable USB bInterfaceNumber; -1 selects the best matching interface. */
    int preferred_interface_number;
    /* Requested identity presented to games; independent of input protocol.
     * Persistence does not prove backend support or live application. */
    GcControllerOutputType output_type;
char instance_id[24];                 /* empty = model default */
char profile_id[GC_PROFILE_ID_MAX];    /* exclusive exact association */
} GcControllerRule;

typedef struct {
    int schema;
    int autostart;
    int run_in_background;
    int daemon_mode;
    int auto_restart;
    int wireless_ds4;
    int ghostpad_enabled;
    uint16_t ghostpad_port;
    unsigned ghostpad_stale_ms;
    int dualsense_fx_enabled;
    int dualsense_fx_effect;
    unsigned dualsense_fx_period_ms;
    unsigned dualsense_fx_step_ms;
    unsigned dualsense_fx_r;
    unsigned dualsense_fx_g;
    unsigned dualsense_fx_b;
    unsigned health_interval_ms;
    unsigned disconnect_restart_ms;
    unsigned input_stale_release_ms;
    int update_enabled;
    int include_prereleases;
    char active_profile[GC_PROFILE_ID_MAX];
    uint16_t ipc_port;
    int xmb_enabled;
    /* Which product draws the HomeUI top-nav tile: "auto", "ghostcontrol"
     * or "onionhen". Read by the ShellUI module, not by the core. */
    char homeui_tile_owner[16];
    /* Which product owns the Settings row the console exposes as
     * "Debug Settings": "auto", "ghostcontrol" or "onionhen". */
    char settings_entry_owner[16];
} GcRuntimeConfig;

typedef struct GcConfigStore GcConfigStore;

/* Opens one owned store and creates the complete directory/default-file schema
 * when it is absent. Pass NULL for the production /data/ghostcontrol root. */
GcConfigStore *gc_config_open(const char *root, char *error, size_t error_size);
void gc_config_close(GcConfigStore *store);

int gc_config_reload(GcConfigStore *store, char *error, size_t error_size);
int gc_config_snapshot(GcConfigStore *store, GcRuntimeConfig *runtime,
                       uint64_t *generation);
uint64_t gc_config_generation(const GcConfigStore *store);
size_t gc_config_profiles(GcConfigStore *store, GcProfile *profiles,
                          size_t capacity, uint64_t *generation);
size_t gc_config_controllers(GcConfigStore *store, GcControllerRule *rules,
                             size_t capacity, uint64_t *generation);
int gc_config_find_controller(GcConfigStore *store, uint16_t vid,
                              uint16_t pid, GcControllerRule *rule,
                              uint64_t *generation);
/* Finds a persisted rule by VID:PID even when it is disabled or ignored.
 * This is for configuration/editing paths that must update the existing
 * unique rule. Runtime admission should use gc_config_find_controller(). */
int gc_config_find_controller_including_disabled(
    GcConfigStore *store, uint16_t vid, uint16_t pid,
    GcControllerRule *rule, uint64_t *generation);
/* Returns an enabled profile that explicitly matches this VID:PID. Wildcard
 * profiles are deliberately excluded: an exact profile represents the
 * user's completed controller-setup choice and is therefore safe to admit. */
int gc_config_find_exact_profile(GcConfigStore *store, uint16_t vid,
                                 uint16_t pid, const char *protocol,
                                 GcProfile *profile, uint64_t *generation);
int gc_config_select_profile(GcConfigStore *store, uint16_t vid, uint16_t pid,
                             const char *protocol, GcProfile *profile,
                             uint64_t *generation);

int gc_config_save_runtime(GcConfigStore *store,
                           const GcRuntimeConfig *runtime,
                           char *error, size_t error_size);
int gc_config_save_profile(GcConfigStore *store, const GcProfile *profile,
                           char *error, size_t error_size);
int gc_config_create_profile(GcConfigStore *store, const char *id,
                             char *error, size_t error_size);
int gc_config_rename_profile(GcConfigStore *store, const char *old_id,
                             const char *new_id, char *error,
                             size_t error_size);
int gc_config_export_profile(GcConfigStore *store, const char *id,
                             char *error, size_t error_size);
int gc_config_import_profile(GcConfigStore *store, const char *id,
                             char *error, size_t error_size);
/* Applies one key/value to an existing profile and saves it atomically, as
 * one mutation transaction: read the profile, run the value through the same
 * per-key parser the loader uses, then write and reload. 'id' and 'schema'
 * are rejected by gc_profile_apply_key(). */
int gc_config_edit_profile(GcConfigStore *store, const char *id,
                           const char *key, const char *value,
                           char *error, size_t error_size);
/* Writes the PROFILE_FILES reply listing the exchange files the UI can act
 * on, as {"ok":true,"imports":[...],"exports":[...]}\n. Entries are the .ini
 * basenames without extension, sorted, so each is directly usable as the id
 * PROFILE_IMPORT / PROFILE_EXPORT take. imports/ holds files to bring in;
 * exports/ holds profiles already written out. */
int gc_config_profile_files_json(GcConfigStore *store, char *response,
                                 size_t response_size);
int gc_config_save_controller(GcConfigStore *store,
                              const GcControllerRule *rule,
                              char *error, size_t error_size);
/* Atomic controller mutations return the persisted rule and its publication
 * generation while holding the same writer transaction. Legacy full saves may
 * preserve output_type when their wire format did not explicitly supply it. */
int gc_config_save_controller_verified(GcConfigStore *store,
    const GcControllerRule *rule, int preserve_output_type,
    GcControllerRule *saved, uint64_t *generation, char *error, size_t error_size);
int gc_config_set_controller_output(GcConfigStore *store, const char *id,
    GcControllerOutputType output_type, GcControllerRule *saved,
    uint64_t *generation, char *error, size_t error_size);
/* Return 0 with the writer transaction held only if generation still matches.
 * Use solely for bounded backend dispatch; do not mutate this store or wait for
 * writer callbacks while holding it. Release on every success path. */
int gc_config_policy_acquire(GcConfigStore *store, uint64_t generation);
void gc_config_policy_release(GcConfigStore *store);
/* Optional setup connection lease, acquired inside the config writer lock.
 * Provide both callbacks. Failed acquire owns nothing; successful acquire is
 * released exactly once after commit/readback or rollback. Callbacks must not
 * acquire config/feature locks, mutate config or join source/mutator workers. */
typedef struct {
    int (*acquire)(void *context);
    void (*release)(void *context);
    void *context;
} GcConfigSetupGuard;

typedef enum { GC_RESOLVE_NONE=0, GC_RESOLVE_MODEL=1, GC_RESOLVE_INSTANCE=2,
    GC_RESOLVE_VETO=3, GC_RESOLVE_IDENTITY_UNAVAILABLE=4,
    GC_RESOLVE_IDENTITY_COLLISION=5, GC_RESOLVE_BINDING_MISMATCH=6,
    GC_RESOLVE_PROFILE_UNAVAILABLE=7,
    GC_RESOLVE_POLICY_UNAVAILABLE=8 } GcControllerResolutionKind;
typedef struct {
    GcControllerResolutionKind kind;
    int has_rule, has_profile;
    GcControllerRule rule;
    GcProfile profile;
    uint64_t generation;
} GcControllerResolution;
int gc_config_resolve_controller(GcConfigStore *store,
    const GcControllerMatch *match, GcControllerResolution *out);
int gc_config_find_controller_by_id(GcConfigStore *store, const char *id,
    GcControllerRule *out, uint64_t *generation);
/* 0 = quarantined/invalid store; 1 = complete validated policy publication. */
int gc_config_instance_policy_valid(GcConfigStore *store);
typedef enum {
    GC_INSTANCE_MUTATION_FULL=0,
    GC_INSTANCE_MUTATION_LEGACY9=1,
    GC_INSTANCE_MUTATION_LEGACY10=2,
    GC_INSTANCE_MUTATION_LEGACY11=3,
    GC_INSTANCE_MUTATION_OUTPUT_SET=4
} GcControllerInstanceMutationMode;
typedef struct {
    GcControllerInstanceMutationMode mode;
    int output_present;
    GcControllerRule rule;
} GcControllerInstanceMutation;
typedef struct {
    int committed, verified, has_profile;
    GcControllerRule rule;
    GcProfile profile;
    uint64_t generation;
} GcControllerInstanceMutationResult;
int gc_config_mutate_controller_instance(GcConfigStore *store,
    const GcControllerInstanceMutation *intent,
    GcControllerInstanceMutationResult *published,
    char *error, size_t error_size);
/* Additive compatibility transaction. The four non-diagnostic pointers are
 * required; error is optional. Initializes scope=NONE and all result bytes=0.
 * Accepts LEGACY9/10/11 and OUTPUT_SET, never FULL. Owns the writer once, routes
 * the latest exact ID, and retains that scope through subsequent failures.
 * Quarantined retained rows classify error families only, never authorize.
 * Scope is not commit/authorization truth. MODEL needs policy, INSTANCE also
 * needs held STORAGE. committed survives a fallible postpublication readback;
 * copies/generation are valid only when verified; MODEL has no profile.
 * Legacy APIs/types/projections are unchanged. No caller may hold the writer
 * or perform a prelookup/instance-to-model fallback to authorize this call. */
typedef enum {
    GC_CONTROLLER_MUTATION_SCOPE_NONE = 0,
    GC_CONTROLLER_MUTATION_SCOPE_MODEL = 1,
    GC_CONTROLLER_MUTATION_SCOPE_INSTANCE = 2
} GcControllerMutationScope;
int gc_config_mutate_controller_compat(GcConfigStore *store,
    const GcControllerInstanceMutation *intent,
    GcControllerMutationScope *scope,
    GcControllerInstanceMutationResult *published,
    char *error, size_t error_size);

int gc_config_save_setup_for_controller_guarded(GcConfigStore *store,
    const GcProfile *draft, const GcControllerMatch *captured,
    const char *controller_id, const char *profile_id,
    GcControllerResolution *published, const GcConfigSetupGuard *guard,
    int *committed, char *error, size_t error_size);

#define GC_INSTANCE_CONTRACT_VERSION 1
typedef enum {
    GC_INSTANCE_PRODUCT_DISABLED=0,
    GC_INSTANCE_PRODUCT_COMPANION_STORAGE=1,
    GC_INSTANCE_PRODUCT_RESIDENT_STORAGE_LIVE=2
} GcInstanceProductMode;
typedef enum {
    GC_INSTANCE_REQUIRE_STORAGE=1,
    GC_INSTANCE_REQUIRE_LIVE=2
} GcInstanceRequirement;
typedef struct {
    int instance_contract_version;
    int instance_storage_available;
    int instance_live_available;
    int instance_available;
    int instance_policy_valid;
} GcInstanceCapabilities;
int gc_config_instance_product_configure(GcConfigStore *store,
    GcInstanceProductMode mode, char *error, size_t error_size);
int gc_config_instance_capabilities(GcConfigStore *store,
    GcInstanceCapabilities *out);
/* Caller already owns this store's writer; never acquires it again. */
int gc_config_instance_check_held(GcConfigStore *store,
    GcInstanceRequirement required, char *error, size_t error_size);
/* 0 holds writer; -1 holds nothing. No expected generation needed for Begin. */
int gc_config_instance_acquire(GcConfigStore *store,
    GcInstanceRequirement required, char *error, size_t error_size);
void gc_config_instance_release(GcConfigStore *store);

typedef enum {
    GC_INSTANCE_INVALIDATE_EXTERNAL_RELOAD_FAILURE=1,
    GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK=2
} GcInstanceInvalidationReason;
typedef void (*GcInstancePolicyInvalidationHook)(void *context,
    GcInstanceInvalidationReason reason, uint64_t invalidation_generation);
int gc_config_instance_invalidation_register(GcConfigStore *store,
    GcInstancePolicyInvalidationHook hook, void *context,
    char *error, size_t error_size);
int gc_config_instance_invalidation_unregister(GcConfigStore *store,
    GcInstancePolicyInvalidationHook hook, void *context,
    char *error, size_t error_size);

/* Like the preserving save, with an observed connection held through the
 * transaction. *committed distinguishes committed-but-readback-failed from a
 * rejected/rolled-back save; it is set even when the function returns failure. */
int gc_config_save_setup_preserving_rule_guarded(GcConfigStore *store,
    const GcProfile *profile, GcControllerRule *saved, uint64_t *generation,
    const GcConfigSetupGuard *guard, int *committed, char *error, size_t error_size);
/* Merge setup with the latest unique VID:PID rule inside the transaction.
 * Preserve every existing rule field; construct defaults only for a new rule. */
int gc_config_save_setup_preserving_rule(GcConfigStore *store,
    const GcProfile *profile, GcControllerRule *saved, uint64_t *generation,
    char *error, size_t error_size);
/* Durably publishes a completed guided setup as one in-process transaction.
 * The in-memory generation changes only after profile, admission rule, and
 * automatic matching are all written. Detected write/reload failures roll all
 * touched files back before returning. */
int gc_config_save_setup(GcConfigStore *store, const GcProfile *profile,
                         const GcControllerRule *rule,
                         char *error, size_t error_size);

const char *gc_config_root(const GcConfigStore *store);
