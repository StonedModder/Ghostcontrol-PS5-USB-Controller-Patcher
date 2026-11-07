/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_ds4_game.h"

#include <elf.h>
#ifndef R_X86_64_JUMP_SLOT
#define R_X86_64_JUMP_SLOT 7
#endif
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "gc_ds4_policy.h"
#include "gc_remote.h"

#include "gc_ds4_game_stub.h"
#include <time.h>

#define GC_DS4_PAD_CLIENT_STRIDE 0x5c8u
#define GC_DS4_PAD_CONNECTED 0x18u
#define GC_DS4_PAD_HANDLE 0x1cu
#define GC_DS4_PAD_USER_ID 0x24u
#define GC_DS4_PAD_VALID 0x38u
#define GC_DS4_PAD_VENDOR 0x60u
#define GC_DS4_PAD_PRODUCT 0x62u
#define GC_GATEWAY_SIZE 32u
#define GC_STUB_OFFSET (GC_GATEWAY_SIZE * GC_DS4_GAME_LIBPAD_SYMBOLS)

static int make_code(intptr_t mapping, uint8_t code[GC_DS4_TARGET_PAGE_SIZE]);

static intptr_t owned_gateway(pid_t pid, intptr_t gateway, unsigned kind)
{
    intptr_t mapping = gateway - kind * GC_GATEWAY_SIZE;
    if (mapping <= 0 || (mapping & (GC_DS4_TARGET_PAGE_SIZE - 1))) return 0;
    uint8_t code[22];
    intptr_t args;
    if (gc_remote_read(pid, gateway, code, sizeof(code))) return 0;
    memcpy(&args, code + 2, sizeof(args));
    if (code[0] != 0x48 || code[1] != (kind == 2 || kind == 3 ? 0xb9 : 0xba) ||
        args != mapping + GC_DS4_TARGET_PAGE_SIZE || code[10] != 0x48 ||
        code[11] != 0xb8 || code[20] != 0xff || code[21] != 0xe0) return 0;
    return mapping;
}

extern void ghostpad_status_log(const char *format, ...);
#define game_log(...) ghostpad_status_log("[GC DS4 game] " __VA_ARGS__)

typedef struct {
    uint64_t next;
    uint64_t path;
    uint64_t unknown0[2];
    uint32_t refcount;
    uint32_t alignment0;
    uint64_t handle;
    uint64_t mapbase;
    uint64_t mapsize;
    uint64_t sections;
    uint64_t section_count;
    uint64_t unknown1;
    uint8_t reserved_to_dynsec[0x148u - 0x58u];
    uint64_t dynsec;
} GcDynlibObject;

typedef struct {
    uint64_t list_next;
    uint64_t list_prev;
    uint64_t sysvec;
    uint32_t refcount;
    uint32_t alignment0;
    uint64_t size;
    uint64_t symtab;
    uint64_t symtab_size;
    uint64_t strtab;
    uint64_t strtab_size;
    uint64_t plt_rela;
    uint64_t plt_rela_size;
} GcDynlibSection;

_Static_assert(offsetof(GcDynlibObject, handle) == 0x28,
               "rtld object handle offset changed");
_Static_assert(offsetof(GcDynlibObject, mapbase) == 0x30,
               "rtld object mapbase offset changed");
_Static_assert(offsetof(GcDynlibObject, sections) == 0x40,
               "rtld object sections offset changed");
_Static_assert(offsetof(GcDynlibObject, dynsec) == 0x148,
               "rtld dynamic section offset changed");
_Static_assert(offsetof(GcDynlibSection, symtab) == 0x28,
               "rtld symbol table offset changed");
_Static_assert(offsetof(GcDynlibSection, plt_rela) == 0x48,
               "rtld PLT relocation offset changed");

typedef struct {
    GcDs4ImportHook hooks[GC_DS4_MAX_IMPORT_HOOKS];
    uint32_t count;
    uint32_t kind_mask;
    int complete;
    int overflowed;
    intptr_t retained_mapping;
} GcImportPlan;

static uint64_t
fnv1a64(const uint8_t *bytes, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int
find_libpad_object(pid_t pid, intptr_t base, GcDynlibObject *output)
{
    uint64_t process = (uint64_t)kernel_get_proc(pid);
    uint64_t shared = 0;
    uint64_t object_address = 0;
    if (!process || !output ||
        kernel_copyout(process + 0x3e8, &shared, sizeof(shared)) != 0 ||
        !shared ||
        kernel_copyout(shared, &object_address, sizeof(object_address)) != 0)
        return -1;
    for (unsigned index = 0; object_address && index < 512; ++index) {
        GcDynlibObject object;
        if (kernel_copyout((intptr_t)object_address,
                           &object, sizeof(object)) != 0)
            return -1;
        object_address = object.next;
        if (object.mapbase != (uint64_t)base)
            continue;
        if (!object.mapsize || object.mapsize > UINT64_C(0x02000000))
            return -1;
        *output = object;
        return 0;
    }
    return -1;
}

static int
symbol_kind(const char name[12], const char nids[GC_DS4_GAME_LIBPAD_SYMBOLS][12])
{
    for (unsigned kind = 0; kind < GC_DS4_GAME_LIBPAD_SYMBOLS; ++kind) {
        if (memcmp(name, nids[kind], 11) == 0)
            return (int)kind;
    }
    return -1;
}

static int
collect_imports(pid_t pid, intptr_t libpad_base,
                const intptr_t originals[GC_DS4_GAME_LIBPAD_SYMBOLS],
                GcImportPlan *plan)
{
    static const char *const names[GC_DS4_GAME_LIBPAD_SYMBOLS] = {
        "scePadReadState", "scePadReadStateExt", "scePadRead",
        "scePadReadExt", "scePadGetDataInternal",
        "scePadGetControllerInformation", "scePadSetVibration"
    };
    char nids[GC_DS4_GAME_LIBPAD_SYMBOLS][12];
    memset(plan, 0, sizeof(*plan));
    for (unsigned kind = 0; kind < GC_DS4_GAME_LIBPAD_SYMBOLS; ++kind) {
        memset(nids[kind], 0, sizeof(nids[kind]));
        nid_encode(names[kind], nids[kind]);
    }

    uint64_t process = (uint64_t)kernel_get_proc(pid);
    uint64_t shared = 0;
    uint64_t object_address = 0;
    if (!process ||
        kernel_copyout(process + 0x3e8, &shared, sizeof(shared)) != 0 ||
        !shared ||
        kernel_copyout(shared, &object_address, sizeof(object_address)) != 0 ||
        !object_address)
        return -1;

    unsigned object_count = 0;
    while (object_address && object_count < 512) {
        ++object_count;
        GcDynlibObject object;
        if (kernel_copyout((intptr_t)object_address,
                           &object, sizeof(object)) != 0)
            return -1;
        object_address = object.next;
        if (!object.mapbase || object.mapsize < sizeof(uint64_t) ||
            object.mapbase == (uint64_t)libpad_base || !object.dynsec)
            continue;

        GcDynlibSection section;
        if (kernel_copyout((intptr_t)object.dynsec,
                           &section, sizeof(section)) != 0)
            return -1;
        int no_plt = !section.symtab && !section.strtab &&
            !section.plt_rela && !section.symtab_size &&
            !section.strtab_size && !section.plt_rela_size;
        if (no_plt)
            continue;
        if (!section.plt_rela && !section.plt_rela_size) continue;
        if (!section.symtab || !section.strtab || !section.plt_rela ||
            object.mapbase > (uint64_t)INTPTR_MAX ||
            object.mapsize > (uint64_t)INTPTR_MAX - object.mapbase ||
            section.symtab_size == 0 || section.strtab_size == 0 ||
            section.plt_rela_size == 0 ||
            section.symtab_size > UINT64_C(0x02000000) ||
            section.strtab_size > UINT64_C(0x02000000) ||
            section.plt_rela_size > UINT64_C(0x02000000) ||
            section.symtab_size % sizeof(Elf64_Sym) != 0 ||
            section.plt_rela_size % sizeof(Elf64_Rela) != 0)
            return -1;

        uint64_t symbol_count = section.symtab_size / sizeof(Elf64_Sym);
        uint64_t relocation_count = section.plt_rela_size / sizeof(Elf64_Rela);
        for (uint64_t index = 0; index < relocation_count; ++index) {
            Elf64_Rela relocation;
            if (kernel_copyout(
                    (intptr_t)(section.plt_rela + index * sizeof(relocation)),
                    &relocation, sizeof(relocation)) != 0)
                return -1;
            uint64_t symbol_index = ELF64_R_SYM(relocation.r_info);
            if (ELF64_R_TYPE(relocation.r_info) != R_X86_64_JUMP_SLOT)
                continue;
            if (symbol_index >= symbol_count)
                return -1;
            Elf64_Sym symbol;
            if (kernel_copyout(
                    (intptr_t)(section.symtab + symbol_index * sizeof(symbol)),
                    &symbol, sizeof(symbol)) != 0)
                return -1;
            if ((uint64_t)symbol.st_name + 11u >= section.strtab_size)
                continue;
            char name[12];
            if (kernel_copyout(
                    (intptr_t)(section.strtab + symbol.st_name),
                    name, sizeof(name)) != 0)
                return -1;
            int kind = symbol_kind(name, nids);
            if (kind < 0)
                continue;
            if (originals[kind] <= 0 ||
                relocation.r_offset > object.mapsize - sizeof(uint64_t))
                return -1;
            intptr_t slot = (intptr_t)(object.mapbase + relocation.r_offset);
            intptr_t current = 0;
            if (slot <= 0 || (slot & 7) != 0 ||
                gc_remote_read(pid, slot, &current, sizeof(current)) != 0)
                return -1;
            if (current != originals[kind]) {
                intptr_t retained = owned_gateway(pid, current, (unsigned)kind);
                if (!retained || (plan->retained_mapping && plan->retained_mapping != retained))
                    return -1;
                plan->retained_mapping = retained;
            }
            int duplicate = 0;
            for (uint32_t prior = 0; prior < plan->count; ++prior)
                duplicate |= plan->hooks[prior].slot == slot;
            if (duplicate)
                continue;
            if (plan->count == GC_DS4_MAX_IMPORT_HOOKS) {
                plan->overflowed = 1;
                return -1;
            }
            int protection = kernel_get_vmem_protection(
                pid, slot, sizeof(uint64_t));
            if (protection < 0)
                return -1;
            GcDs4ImportHook *hook = &plan->hooks[plan->count++];
            hook->slot = slot;
            hook->original = originals[kind];
            hook->gateway = current;
            hook->protection = (uint32_t)protection;
            hook->kind = (uint32_t)kind;
            plan->kind_mask |= UINT32_C(1) << kind;
        }
    }
    if (object_address)
        return -1;
    plan->complete = 1;
    uint32_t read_kinds = plan->kind_mask & UINT32_C(0x1f);
    if (!read_kinds)
        return -1;
    uint32_t required = read_kinds & (~read_kinds + 1u);
    return gc_ds4_import_scan_complete(
        required, plan->kind_mask, plan->count,
        plan->complete, plan->overflowed) ? 0 : -1;
}

static int
verify_library_evidence(pid_t pid, intptr_t base,
                        const intptr_t originals[GC_DS4_GAME_LIBPAD_SYMBOLS],
                        const GcDs4LibpadEvidence *source)
{
    const uint32_t required_evidence = (UINT32_C(1) << 6) - 1u;
    if (!source || source->firmware != kernel_get_fw_version() ||
        (source->fingerprint_mask & required_evidence) != required_evidence ||
        !source->controller_info_abi)
        return -1;
    GcDynlibObject object;
    if (find_libpad_object(pid, base, &object) != 0)
        return -1;
    for (size_t kind = 0; kind < 6; ++kind) {
        if (originals[kind] < base ||
            object.mapsize < 256 ||
            (uint64_t)(originals[kind] - base) > object.mapsize - 256)
            return -1;
        uint8_t code[256];
        if (gc_remote_read(pid, originals[kind], code, sizeof(code)) != 0 ||
            fnv1a64(code, sizeof(code)) != source->fingerprints[kind])
            return -1;
    }
    if ((source->fingerprint_mask & (UINT32_C(1) << 6)) && originals[6] > 0) {
        uint8_t code[256];
        if (gc_remote_read(pid, originals[6], code, sizeof(code)) != 0 ||
            fnv1a64(code, sizeof(code)) != source->fingerprints[6]) return -1;
    }
    return 0;
}

static int inside(const GcDynlibObject *object, intptr_t address, size_t size)
{
    return address > 0 && (uint64_t)address >= object->mapbase &&
        size <= object->mapsize &&
        (uint64_t)address - object->mapbase <= object->mapsize - size;
}

/* Admit only the observed mode-selecting wrappers, with targets in this libpad. */
static int wrapper_target(pid_t pid, const GcDynlibObject *object,
                           intptr_t function, unsigned kind, intptr_t *target)
{
    uint8_t code[16];
    if (!inside(object, function, sizeof(code)) ||
        gc_remote_read(pid, function, code, sizeof(code)) != 0) return -1;
    int counted = kind == 2 || kind == 3;
    int extended = kind == 1 || kind == 3 || kind == 4;
    uint8_t reg = counted ? 0xc9 : 0xd2;
    uint8_t mov = counted ? 0xb9 : 0xba;
    size_t jump;
    if (!extended && (code[0] == 0x31 || code[0] == 0x29) && code[1] == reg)
        jump = 2;
    else if (code[0] == mov && code[1] == extended &&
             !code[2] && !code[3] && !code[4]) jump = 5;
    else return -1;
    int32_t relative;
    if (code[jump] == 0xe9) {
        memcpy(&relative, code + jump + 1, sizeof(relative));
        *target = function + (intptr_t)jump + 5 + relative;
    } else if (code[jump] == 0xff && code[jump + 1] == 0x25) {
        memcpy(&relative, code + jump + 2, sizeof(relative));
        intptr_t slot = function + (intptr_t)jump + 6 + relative;
        if (!inside(object, slot, sizeof(*target)) ||
            gc_remote_read(pid, slot, target, sizeof(*target)) != 0) return -1;
    } else return -1;
    if (!inside(object, *target, 0x300)) return -1;
    int protection = kernel_get_vmem_protection(pid, *target, 1);
    return protection >= 0 && (protection & PROT_EXEC) ? 0 : -1;
}

static int read_game_pads(pid_t pid, const GcDynlibObject *object,
                          const intptr_t originals[GC_DS4_GAME_LIBPAD_SYMBOLS],
                          GcDs4GamePad pads[24])
{
    intptr_t targets[5];
    for (unsigned kind = 0; kind < 5; ++kind)
        if (wrapper_target(pid, object, originals[kind], kind, &targets[kind]))
            return -1;
    if (targets[0] != targets[1] || targets[0] != targets[4] ||
        targets[2] != targets[3]) return -1;
    uint8_t code[0x300];
    if (gc_remote_read(pid, targets[0], code, sizeof(code))) return -1;
    intptr_t table = 0;
    for (size_t offset = 0; offset + 7 <= sizeof(code); ++offset) {
        if (memcmp(code + offset, "\x48\x8d\x0d", 3)) continue;
        int32_t relative;
        memcpy(&relative, code + offset + 3, sizeof(relative));
        intptr_t candidate = targets[0] + (intptr_t)offset + 7 + relative;
        if (!inside(object, candidate, 24 * GC_DS4_PAD_CLIENT_STRIDE)) continue;
        int stride = 0, limit = 0;
        size_t end = offset + 160 < sizeof(code) ? offset + 160 : sizeof(code);
        for (size_t scan = offset; scan + 6 <= end; ++scan) {
            stride |= !memcmp(code + scan, "\x48\x05\xc8\x05\x00\x00", 6);
            limit |= !memcmp(code + scan, "\x48\x3d\xc0\x8a\x00\x00", 6);
        }
        if (stride && limit) {
            if (table && table != candidate) return -1;
            table = candidate;
        }
    }
    if (!table) return -1;
    for (unsigned slot = 0; slot < 24; ++slot) {
        uint8_t prefix[0x64], verify[0x64];
        intptr_t entry = table + slot * GC_DS4_PAD_CLIENT_STRIDE;
        if (gc_remote_read(pid, entry, prefix, sizeof(prefix)) ||
            gc_remote_read(pid, entry, verify, sizeof(verify)) ||
            memcmp(prefix, verify, sizeof(prefix))) return -1;
        GcDs4GamePad *pad = &pads[slot];
        memset(pad, 0, sizeof(*pad));
        memcpy(&pad->pad_handle, prefix + GC_DS4_PAD_HANDLE, 4);
        memcpy(&pad->user_id, prefix + GC_DS4_PAD_USER_ID, 4);
        memcpy(&pad->vendor_id, prefix + GC_DS4_PAD_VENDOR, 2);
        memcpy(&pad->product_id, prefix + GC_DS4_PAD_PRODUCT, 2);
        int32_t connected, valid;
        memcpy(&connected, prefix + GC_DS4_PAD_CONNECTED, 4);
        memcpy(&valid, prefix + GC_DS4_PAD_VALID, 4);
        pad->connected = connected != 0;
        pad->valid = valid != 0;
        pad->pad_index = pad->pad_handle & 0xff;
    }
    return 0;
}

int gc_ds4_game_find_target(pid_t *pid)
{
    pid_t candidates[8];
    if (!pid) return -1;
    *pid = 0;
    size_t count = gc_remote_find_pids("eboot.bin", candidates, 8);
    for (size_t slot = 0; slot < count; ++slot) {
        uint32_t pad, libc, kernel;
        pid_t candidate = candidates[slot];
        if (gc_remote_get_library(candidate, "libScePad", &pad) ||
            gc_remote_get_library(candidate, "libSceLibcInternal", &libc) ||
            (gc_remote_get_library(candidate, "libkernel", &kernel) &&
             gc_remote_get_library(candidate, "libkernel_sys", &kernel))) continue;
        if (*pid) { *pid = 0; return -1; }
        *pid = candidate;
    }
    return *pid > 0 ? 0 : -1;
}

static int write_verified(pid_t pid, intptr_t address, const void *data, size_t size)
{
    uint8_t verify[GC_DS4_TARGET_PAGE_SIZE];
    return size <= sizeof(verify) &&
        gc_remote_write(pid, address, data, size) == 0 &&
        gc_remote_read(pid, address, verify, size) == 0 &&
        !memcmp(verify, data, size) ? 0 : -1;
}

/* A write can have succeeded even if its transport reports an error. The caller
 * records ownership before attempting it, and rollback verifies both states. */
static int replace_import(pid_t pid, const GcDs4ImportHook *hook,
                           intptr_t expected, intptr_t replacement)
{
    intptr_t current = 0;
    if (gc_remote_read(pid, hook->slot, &current, sizeof(current)) ||
        (current != expected && current != replacement)) return -1;
    intptr_t page = hook->slot & ~((intptr_t)GC_DS4_TARGET_PAGE_SIZE - 1);
    int result = 0;
    if (current != replacement) {
        if (gc_remote_protect(pid, page, GC_DS4_TARGET_PAGE_SIZE,
                              (int)hook->protection | PROT_WRITE)) return -1;
        result = write_verified(pid, hook->slot, &replacement, sizeof(replacement));
    }
    /* Retry protection restoration even when rollback sees the original value. */
    if (gc_remote_protect(pid, page, GC_DS4_TARGET_PAGE_SIZE,
                          (int)hook->protection)) result = -1;
    return result;
}

static int live_identity(const GcDs4GameBridge *bridge)
{
    return bridge->pid > 0 && bridge->process_identity != 0 &&
        kernel_get_proc(bridge->pid) == bridge->process_identity;
}

void gc_ds4_game_observe_exit(GcDs4GameBridge *bridge)
{
    if (bridge && bridge->pid > 0 && !live_identity(bridge))
    {
        memset(bridge, 0, sizeof(*bridge));
        gc_remote_record_clear(GC_REMOTE_RECORD_GAME);
    }
}

int gc_ds4_game_remove(GcDs4GameBridge *bridge)
{
    if (!bridge) return -1;
    gc_ds4_game_observe_exit(bridge);
    if (!bridge->mapping) return 0;
    uint32_t inactive = 0;
    int disabled = !write_verified(bridge->pid,
        bridge->args_address + offsetof(GcDs4GameArgs, active),
        &inactive, sizeof(inactive));
    int restored = 1;
    for (uint32_t index = bridge->hook_count; index > 0; --index) {
        GcDs4ImportHook *hook = &bridge->hooks[index - 1];
        if (replace_import(bridge->pid, hook, hook->gateway, hook->original))
            restored = 0;
    }
    /* Never unmap live game code after a gateway could have been loaded. */
    bridge->state = disabled && restored ? GC_DS4_GAME_RETAINED_REUSABLE
                                         : GC_DS4_GAME_RETAINED_QUARANTINED;
    return disabled && restored ? 0 : -1;
}

int gc_ds4_game_quiesce(GcDs4GameBridge *bridge)
{
    if (!bridge) return -1;
    gc_ds4_game_observe_exit(bridge);
    if (!bridge->mapping) return 0;
    uint32_t inactive = 0;
    int result = write_verified(bridge->pid,
        bridge->args_address + offsetof(GcDs4GameArgs, active), &inactive, sizeof(inactive));
    for (unsigned i = 0; i < bridge->hook_count; ++i) {
        const GcDs4ImportHook *hook = &bridge->hooks[i];
        intptr_t current;
        if (gc_remote_read(bridge->pid, hook->slot, &current, sizeof(current))) { result = -1; continue; }
        if (current == hook->original) continue;
        if (current != hook->gateway ||
            write_verified(bridge->pid, hook->slot, &hook->original, sizeof(hook->original))) result = -1;
    }
    /* No remote syscalls, ptrace, joins or unmaps at a suspend boundary. */
    bridge->state = result ? GC_DS4_GAME_RETAINED_QUARANTINED : GC_DS4_GAME_RETAINED_REUSABLE;
    return result;
}

int gc_ds4_game_validate(const GcDs4GameBridge *bridge, const GcDs4SourceReader *source)
{
    if (!bridge || !source || bridge->state != GC_DS4_GAME_LIVE || !live_identity(bridge)) return -1;
    uint32_t pad;
    if (gc_remote_get_library(bridge->pid, "libScePad", &pad)) return -1;
    intptr_t base = kernel_dynlib_mapbase_addr(bridge->pid, pad);
    GcDynlibObject object;
    GcDs4GameArgs args;
    GcDs4GamePad pads[24];
    GcDs4Route routes[GC_DS4_MAX_ROUTES];
    GcDs4RouteResult results[GC_DS4_MAX_ROUTES];
    GcImportPlan plan;
    if (find_libpad_object(bridge->pid, base, &object) ||
        gc_remote_read(bridge->pid, bridge->args_address, &args, sizeof(args)) ||
        read_game_pads(bridge->pid, &object, args.originals, pads) ||
        collect_imports(bridge->pid, base, args.originals, &plan) ||
        plan.count != bridge->hook_count || plan.retained_mapping != bridge->mapping) return -1;
    size_t count = gc_ds4_select_routes(source->sources, source->source_count, pads, 24, routes, results);
    if (count != bridge->route_count) return -1;
    for (size_t i = 0; i < count; ++i)
        if (routes[i].source_slot != bridge->routes[i].source_slot ||
            routes[i].game_slot != bridge->routes[i].game_slot ||
            routes[i].pad_handle != bridge->routes[i].pad_handle) return -1;
    for (unsigned i = 0; i < plan.count; ++i)
        if (plan.hooks[i].slot != bridge->hooks[i].slot ||
            plan.hooks[i].original != bridge->hooks[i].original ||
            plan.hooks[i].gateway != bridge->hooks[i].gateway) return -1;
    return 0;
}

static const uintptr_t stub_functions[GC_DS4_GAME_LIBPAD_SYMBOLS] = {
    (uintptr_t)gc_ds4_game_read_state_stub,
    (uintptr_t)gc_ds4_game_read_state_ext_stub,
    (uintptr_t)gc_ds4_game_read_stub,
    (uintptr_t)gc_ds4_game_read_ext_stub,
    (uintptr_t)gc_ds4_game_data_internal_stub,
    (uintptr_t)gc_ds4_game_controller_info_stub,
    (uintptr_t)gc_ds4_game_set_vibration_stub
};

static int make_code(intptr_t mapping, uint8_t code[GC_DS4_TARGET_PAGE_SIZE])
{
    uintptr_t first = stub_functions[0];
    uintptr_t end = (uintptr_t)gc_ds4_game_stub_end;
    if (end <= first || end - first > GC_DS4_TARGET_PAGE_SIZE - GC_STUB_OFFSET)
        return -1;
    memset(code, 0xcc, GC_DS4_TARGET_PAGE_SIZE);
    memcpy(code + GC_STUB_OFFSET, (const void *)first, end - first);
    intptr_t args = mapping + GC_DS4_TARGET_PAGE_SIZE;
    for (unsigned kind = 0; kind < GC_DS4_GAME_LIBPAD_SYMBOLS; ++kind) {
        if (stub_functions[kind] < first || stub_functions[kind] >= end) return -1;
        uint8_t *gateway = code + kind * GC_GATEWAY_SIZE;
        intptr_t target = mapping + GC_STUB_OFFSET + (intptr_t)(stub_functions[kind] - first);
        gateway[0] = 0x48; gateway[1] = kind == 2 || kind == 3 ? 0xb9 : 0xba;
        memcpy(gateway + 2, &args, 8);     /* movabs rcx/rdx, args */
        gateway[10] = 0x48; gateway[11] = 0xb8;
        memcpy(gateway + 12, &target, 8); /* movabs rax, target */
        gateway[20] = 0xff; gateway[21] = 0xe0; /* tail-call, preserves caller stack */
    }
    return 0;
}

/* Recover only our exact compiled code and a complete independently discovered
 * import plan. No PID or address from a persisted text file is trusted. */
static int adopt_retained(GcDs4GameBridge *bridge, pid_t pid, const GcImportPlan *plan,
                          const intptr_t originals[GC_DS4_GAME_LIBPAD_SYMBOLS])
{
    intptr_t mapping = plan->retained_mapping;
    GcDs4GameArgs args;
    uint8_t expected[GC_DS4_TARGET_PAGE_SIZE], actual[GC_DS4_TARGET_PAGE_SIZE];
    if (make_code(mapping, expected) || gc_remote_read(pid, mapping, actual, sizeof(actual)) ||
        memcmp(expected, actual, sizeof(actual)) ||
        gc_remote_read(pid, mapping + GC_DS4_TARGET_PAGE_SIZE, &args, sizeof(args)) ||
        args.magic != GC_DS4_GAME_MAGIC || args.layout != GC_DS4_GAME_LAYOUT ||
        args.mapping != mapping || args.mapping_size != GC_DS4_GAME_MAPPING_SIZE ||
        args.pad_size != sizeof(ScePadData) || args.hook_count != plan->count ||
        memcmp(args.originals, originals, sizeof(args.originals))) return -1;
    for (unsigned index = 0; index < plan->count; ++index) {
        const GcDs4ImportHook *hook = &plan->hooks[index];
        if (args.hook_slots[index] != hook->slot || args.hook_originals[index] != hook->original ||
            args.hook_gateways[index] != mapping + hook->kind * GC_GATEWAY_SIZE ||
            args.hook_kinds[index] != hook->kind || args.hook_protections[index] != hook->protection)
            return -1;
    }
    memset(bridge, 0, sizeof(*bridge));
    bridge->pid = pid; bridge->process_identity = kernel_get_proc(pid);
    bridge->mapping = mapping; bridge->mapping_size = GC_DS4_GAME_MAPPING_SIZE;
    bridge->args_address = mapping + GC_DS4_TARGET_PAGE_SIZE;
    bridge->hook_count = plan->count; bridge->routing_epoch = args.routing_epoch;
    bridge->publish_sequence = args.sequence;
    memcpy(bridge->hooks, plan->hooks, plan->count * sizeof(plan->hooks[0]));
    for (unsigned i = 0; i < bridge->hook_count; ++i)
        bridge->hooks[i].gateway = mapping + bridge->hooks[i].kind * GC_GATEWAY_SIZE;
    bridge->state = GC_DS4_GAME_RETAINED_QUARANTINED;
    return gc_ds4_game_remove(bridge);
}

int gc_ds4_game_install(GcDs4GameBridge *bridge, const GcDs4SourceReader *source)
{
    if (!bridge || !source || !source->source_count ||
        source->source_count > GC_DS4_MAX_ROUTES) return -1;
    gc_ds4_game_observe_exit(bridge);
    if (bridge->state == GC_DS4_GAME_LIVE ||
        bridge->state == GC_DS4_GAME_RETAINED_QUARANTINED) return -1;
    pid_t pid;
    if (gc_ds4_game_find_target(&pid) || (bridge->pid && bridge->pid != pid)) return -1;
    uint32_t pad, kernel;
    if (gc_remote_get_library(pid, "libScePad", &pad) ||
        (gc_remote_get_library(pid, "libkernel", &kernel) &&
         gc_remote_get_library(pid, "libkernel_sys", &kernel))) return -1;
    intptr_t originals[GC_DS4_GAME_LIBPAD_SYMBOLS];
    const char *names[] = {"scePadReadState", "scePadReadStateExt", "scePadRead",
        "scePadReadExt", "scePadGetDataInternal", "scePadGetControllerInformation",
        "scePadSetVibration"};
    for (unsigned kind = 0; kind < GC_DS4_GAME_LIBPAD_SYMBOLS; ++kind)
        originals[kind] = gc_remote_resolve(pid, pad, names[kind]);
    intptr_t clock_fn = gc_remote_resolve(pid, kernel, "clock_gettime");
    intptr_t base = kernel_dynlib_mapbase_addr(pid, pad);
    GcDynlibObject object;
    GcDs4GamePad pads[24];
    GcDs4Route routes[GC_DS4_MAX_ROUTES];
    GcDs4RouteResult results[GC_DS4_MAX_ROUTES];
    GcImportPlan plan;
    if (!clock_fn || verify_library_evidence(pid, base, originals, &source->evidence) ||
        find_libpad_object(pid, base, &object) ||
        read_game_pads(pid, &object, originals, pads)) return -1;
    uint32_t route_count = (uint32_t)gc_ds4_select_routes(source->sources,
        source->source_count, pads, 24, routes, results);
    if (!route_count || collect_imports(pid, base, originals, &plan)) return -1;
    if (!bridge->mapping) {
        GcRemoteRecord record;
        if (!gc_remote_record_load(GC_REMOTE_RECORD_GAME, &record)) {
            if (kernel_get_proc(record.pid) != record.process_identity)
                gc_remote_record_clear(GC_REMOTE_RECORD_GAME);
            else {
                if (record.pid != pid || (plan.retained_mapping && plan.retained_mapping != record.mapping))
                    return -1;
                plan.retained_mapping = record.mapping;
            }
        }
    }
    if (plan.retained_mapping) {
        if (bridge->mapping || adopt_retained(bridge, pid, &plan, originals)) return -1;
        if (collect_imports(pid, base, originals, &plan) || plan.retained_mapping) return -1;
    }
    intptr_t identity = kernel_get_proc(pid);
    if (!identity) return -1;
    int fresh = !bridge->mapping;
    intptr_t mapping = bridge->mapping;
    if (fresh && gc_remote_map(pid, GC_DS4_GAME_MAPPING_SIZE, &mapping)) return -1;
    uint8_t code[GC_DS4_TARGET_PAGE_SIZE], verify[GC_DS4_TARGET_PAGE_SIZE];
    int code_ok = !make_code(mapping, code);
    if (code_ok && fresh)
        code_ok = !write_verified(pid, mapping, code, sizeof(code)) &&
            !gc_remote_protect(pid, mapping, sizeof(code), PROT_READ | PROT_EXEC);
    else if (code_ok)
        code_ok = !gc_remote_read(pid, mapping, verify, sizeof(verify)) &&
            !memcmp(code, verify, sizeof(code));
    if (!code_ok) {
        if (fresh) (void)gc_remote_unmap(pid, mapping, GC_DS4_GAME_MAPPING_SIZE);
        return -1;
    }
    bridge->pid = pid;
    bridge->process_identity = identity;
    bridge->mapping = mapping;
    bridge->mapping_size = GC_DS4_GAME_MAPPING_SIZE;
    bridge->args_address = mapping + GC_DS4_TARGET_PAGE_SIZE;
    bridge->state = GC_DS4_GAME_RETAINED_QUARANTINED;
    uint32_t odd_epoch = (bridge->routing_epoch + 1u) | 1u;
    if (write_verified(pid, bridge->args_address + offsetof(GcDs4GameArgs, routing_epoch),
                        &odd_epoch, sizeof(odd_epoch))) goto fail;
    GcDs4GameArgs args;
    memset(&args, 0, sizeof(args));
    args.magic = GC_DS4_GAME_MAGIC; args.layout = GC_DS4_GAME_LAYOUT;
    args.routing_epoch = odd_epoch;
    args.sequence = bridge->publish_sequence;
    args.mapping = mapping; args.mapping_size = GC_DS4_GAME_MAPPING_SIZE;
    args.pad_size = sizeof(ScePadData); args.route_count = route_count;
    args.clock_gettime_fn = clock_fn;
    memcpy(args.originals, originals, sizeof(originals));
    for (unsigned index = 0; index < route_count; ++index) {
        const GcDs4Source *identity_source = &source->sources[routes[index].source_slot];
        args.routes[index] = (GcDs4TargetRoute){routes[index].pad_handle,
            identity_source->pad_index, identity_source->user_id, routes[index].source_slot};
    }
    args.hook_count = plan.count;
    for (unsigned index = 0; index < plan.count; ++index) {
        GcDs4ImportHook *hook = &plan.hooks[index];
        hook->gateway = mapping + hook->kind * GC_GATEWAY_SIZE;
        args.hook_slots[index] = hook->slot;
        args.hook_originals[index] = hook->original;
        args.hook_gateways[index] = hook->gateway;
        args.hook_protections[index] = hook->protection;
        args.hook_kinds[index] = hook->kind;
    }
    if (write_verified(pid, bridge->args_address, &args, sizeof(args))) goto fail;
    if (gc_remote_record_save(GC_REMOTE_RECORD_GAME, pid, identity, mapping,
                              GC_DS4_GAME_MAPPING_SIZE)) goto fail;
    bridge->routing_epoch = odd_epoch + 1;
    if (write_verified(pid, bridge->args_address + offsetof(GcDs4GameArgs, routing_epoch),
                        &bridge->routing_epoch, sizeof(bridge->routing_epoch))) goto fail;
    bridge->route_count = route_count;
    memcpy(bridge->routes, routes, route_count * sizeof(routes[0]));
    memset(bridge->rumble_sequences, 0, sizeof(bridge->rumble_sequences));
    memcpy(bridge->sources, source->sources, sizeof(bridge->sources));
    memcpy(bridge->hooks, plan.hooks, plan.count * sizeof(plan.hooks[0]));
    bridge->hook_count = 0;
    for (unsigned index = 0; index < plan.count; ++index) {
        bridge->hook_count = index + 1; /* includes uncertain writes */
        if (replace_import(pid, &plan.hooks[index], plan.hooks[index].original,
                            plan.hooks[index].gateway)) goto fail;
    }
    bridge->state = GC_DS4_GAME_LIVE;
    game_log("installed pid=%d routes=%u imports=%u\n", pid, route_count, plan.count);
    return 0;
fail:
    (void)gc_ds4_game_remove(bridge);
    return -1;
}

int gc_ds4_game_publish(GcDs4GameBridge *bridge, const GcDs4SourceSnapshot *snapshot)
{
    if (!bridge || !snapshot || bridge->state != GC_DS4_GAME_LIVE ||
        !live_identity(bridge) ||
        !snapshot->sequence || snapshot->source_count > GC_DS4_MAX_ROUTES) return -1;
    GcDs4GameBank bank;
    memset(&bank, 0, sizeof(bank));
    for (unsigned route = 0; route < bridge->route_count; ++route) {
        unsigned slot = bridge->routes[route].source_slot;
        if (slot >= snapshot->source_count) return -1;
        const GcDs4SourceFrame *frame = &snapshot->frames[slot];
        const GcDs4Source *expected = &bridge->sources[slot];
        if (frame->source.user_id != expected->user_id ||
            frame->source.pad_index != expected->pad_index ||
            frame->source.pad_handle != expected->pad_handle ||
            !frame->source.identity_verified) return -1;
        if (!frame->read_result && frame->data.connected) bank.pads[route] = frame->data;
    }
    uint32_t sequence = gc_ds4_next_sequence(bridge->publish_sequence);
    intptr_t address = bridge->args_address + offsetof(GcDs4GameArgs, banks) +
        (sequence & 1u) * sizeof(bank);
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    uint64_t deadline = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec + UINT64_C(250000000);
    uint32_t active = 1;
    if (gc_remote_write(bridge->pid, address, &bank, sizeof(bank)) ||
        gc_remote_write(bridge->pid, bridge->args_address + offsetof(GcDs4GameArgs, sequence),
                        &sequence, sizeof(sequence)) ||
        gc_remote_write(bridge->pid, bridge->args_address + offsetof(GcDs4GameArgs, deadline_ns),
                        &deadline, sizeof(deadline)) ||
        gc_remote_write(bridge->pid, bridge->args_address + offsetof(GcDs4GameArgs, active),
                        &active, sizeof(active))) return -1;
    bridge->publish_sequence = sequence;
    return 0;
}

int gc_ds4_game_status(const GcDs4GameBridge *bridge, GcDs4GameStatus *status)
{
    if (!bridge || !status) return -1;
    memset(status, 0, sizeof(*status));
    status->state = bridge->state; status->pid = bridge->pid;
    status->route_count = bridge->route_count; status->hook_count = bridge->hook_count;
    if (!bridge->mapping) return 0;
    GcDs4GameArgs args;
    if (gc_remote_read(bridge->pid, bridge->args_address, &args, sizeof(args)) ||
        args.magic != GC_DS4_GAME_MAGIC || args.layout != GC_DS4_GAME_LAYOUT) return -1;
    status->sequence = args.sequence; status->deadline_ns = args.deadline_ns;
    status->publications = args.publications;
    for (unsigned kind = 0; kind < GC_DS4_GAME_LIBPAD_SYMBOLS; ++kind) status->calls[kind] = args.calls[kind];
    status->lease_expirations = args.lease_expirations;
    status->snapshot_fallbacks = args.snapshot_fallbacks;
    return 0;
}

int gc_ds4_game_rumble(const GcDs4GameBridge *bridge, unsigned route,
                       uint32_t *sequence, uint8_t *large, uint8_t *small)
{
    if (!bridge || !sequence || !large || !small ||
        bridge->state != GC_DS4_GAME_LIVE || route >= bridge->route_count ||
        !live_identity(bridge)) return -1;
    const intptr_t address = bridge->args_address + offsetof(GcDs4GameArgs, rumble) +
        route * sizeof(GcDs4GameRumble);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        GcDs4GameRumble first, second;
        if (gc_remote_read(bridge->pid, address, &first, sizeof(first)) ||
            gc_remote_read(bridge->pid, address, &second, sizeof(second))) return -1;
        if (first.sequence == second.sequence && !(second.sequence & 1u)) {
            *sequence = second.sequence;
            *large = second.large;
            *small = second.small;
            return 0;
        }
    }
    return -1;
}
