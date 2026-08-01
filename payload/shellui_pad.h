/* SPDX-License-Identifier: GPL-3.0-or-later
 * shellui_pad.h — System process pad injection interface
 *
 * Injects through the confirmed SceShellCore/SceShellUI path: SceShellCore
 * creates the virtual controller, and SceShellUI performs Mbus binding plus
 * VDI diagnostics.
 *
 * Key safety rule: PT_ATTACH is attempted FIRST.  Nothing is written to the
 * target process until attach succeeds, preventing the PS5 freeze caused by
 * leaving an INT3 in live code when attach fails.
 */
#pragma once
#include <stdint.h>

/* Persistent status log exported by main.c. It mirrors critical logs to
 * /data/ghostpad/ghostpad_status.log so diagnostics survive when /dev/klog
 * or the TCP klog bridge are unavailable. */
void ghostpad_status_log(const char *fmt, ...);
void ghostpad_status_log_reset(void);

/* Maximum size of ScePadData we forward to the stub (padded for alignment) */
#define SHELLUI_PAD_DATA_SIZE 256

/* Shared state between our process and the stub running in SceShellUI.
 * We write pad_data + increment seq; the stub polls seq and calls InsertData. */
typedef struct {
    /* ── function pointers resolved in the target process's address space ── */
    int32_t (*fp_gethandle)(int32_t userId, int32_t type, int32_t index);
    int32_t (*fp_gethandle_ext)(int32_t userId, int32_t type, int32_t index, uint64_t a4, uint64_t a5, uint64_t a6);
    int32_t (*fp_open)(int32_t userId, int32_t type, int32_t index, void *param);
    int32_t (*fp_open_ext)(int32_t userId, int32_t type, int32_t index, void *param, uint64_t a5, uint64_t a6);
    int32_t (*fp_open_ext2)(int32_t userId, int32_t type, int32_t index, void *param, uint64_t a5, uint64_t a6);
    int32_t (*fp_insert)(int32_t handle, const void *data); /* unused legacy slot */
    int32_t (*fp_vdi)(int32_t handle, const void *data);    /* scePadVirtualDeviceInsertData */
    int32_t (*fp_vda)(void *param, int32_t type);           /* scePadVirtualDeviceAddDevice  */
    int32_t (*fp_del)(int32_t handle);                     /* scePadVirtualDeviceDeleteDevice */
    int32_t (*fp_setpriv)(int32_t privilege);              /* scePadSetProcessPrivilege       */
    int32_t (*fp_setloginuser)(int32_t loginUserNumber);   /* scePadSetLoginUserNumber        */
    int32_t (*fp_setusernumber)(int32_t userNumber);       /* scePadSetUserNumber             */
    int32_t (*fp_setfocus)(int32_t focus, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6); /* scePadSetProcessFocus */
    void    (*fp_usleep)(unsigned int usec);

    /* ── parameters ── */
    int32_t  userId;
    int32_t  virtual_device_type;

    /* ── shared pad state (written by our process, read by stub) ── */
    volatile uint32_t seq;                       /* increment to trigger injection */
    uint8_t           pad_data[SHELLUI_PAD_DATA_SIZE]; /* ScePadData bytes         */

    /* ── stub status (written by stub, read by our process) ── */
    volatile int32_t  pad_handle;  /* set by stub after scePadGetHandle          */
    volatile int32_t  ready;       /* 1=running, -1=error                        */
    volatile int32_t  stop;        /* set to 1 to ask stub to exit               */

    /* ── diagnostic: first-iteration return codes from each probe ── */
    volatile int32_t  rc_log[16];
} ShellUiPadArgs;


/* Inject the pad stub into SceShellUI.
 *
 * On success returns 0 and sets:
 *   *out_shellui_pid  — PID of SceShellUI
 *   *out_args_kaddr   — address of ShellUiPadArgs inside SceShellUI's VA space
 *                       (use mdbg_copyin to update pad_data + seq each frame)
 *
 * Returns -1 on failure (see klog for details).
 */
int shellui_pad_inject(int32_t userId, int force_virtual_vda,
                       int32_t virtual_device_type, pid_t *out_shellui_pid,
                       intptr_t *out_args_kaddr);

/* Update pad data inside SceShellUI's stub (call at up to 60 Hz).
 * pad_data must point to a ScePadData struct (at most SHELLUI_PAD_DATA_SIZE bytes).
 * This uses mdbg_copyin — no ptrace attach needed.
 */
int shellui_pad_update(pid_t shellui_pid, intptr_t args_kaddr,
                       const void *pad_data, uint32_t pad_data_len);

int shellui_pad_direct_usable(pid_t shellui_pid, intptr_t args_kaddr);
int shellui_pad_direct_mode(pid_t shellui_pid, intptr_t args_kaddr);
int shellui_pad_direct_adopt_vdi_handle(pid_t shellui_pid, intptr_t args_kaddr,
                                        int32_t vdi_handle);
int shellui_pad_direct_recover(pid_t shellui_pid, intptr_t args_kaddr, int32_t userId, int32_t altUserId);
void shellui_pad_direct_get_last_status(int32_t *stage, int64_t *value);
/* Re-launch stub thread with pre-set VDI handle — one-time pt_call, then
 * use shellui_pad_update() (mdbg_copyin) for all packets. No per-packet
 * ptrace freeze. Requires ptrace authid set permanently in caller. */
int shellui_pad_relaunch_stub_with_handle(int32_t handle);
int shellui_pad_direct_begin(pid_t shellui_pid, intptr_t args_kaddr);
int shellui_pad_direct_send(pid_t shellui_pid, intptr_t args_kaddr,
                            const void *pad_data, uint32_t pad_data_len);
void shellui_pad_direct_end(pid_t shellui_pid, intptr_t args_kaddr);

/* PT_ATTACH SceShellUI, find virtual device handle via pt_call GetHandle,
 * then send Cross × 30 frames + release × 18 frames via pt_call VDI to
 * dismiss the assignment screen.  Call this ~3s after SceShellCore VDA
 * injection so the assignment screen has time to render. */
/* virtualDeviceId: pass the MBUS deviceId (from DEVICE_ADDED klog) so
 * dismiss can try GetHandle(deviceId, type, idx) in addition to userId-based
 * lookups.  SceShellUI's "Open Pad [deviceId,0,0]" suggests GetHandle accepts
 * deviceId as first arg when it is small (deviceId << userId range). */
int shellui_pad_dismiss_assignment_screen(int32_t userId, uint64_t virtualDeviceId);

/* PT_ATTACH SceShellUI, resolve sceMbusBindDeviceWithUserId from libSceMbus,
 * and call it to directly assign virtualDeviceId to userId without going
 * through the assignment screen UI.  Returns 0 on success. */
int shellui_pad_force_bind(uint64_t virtualDeviceId, int32_t userId);

/* PT_ATTACH SceShellUI, call sceMbusDisconnectDevice(physicalDeviceId) to evict the
 * current physical controller from the user's slot.  After eviction the virtual device
 * becomes the sole device at slot 0.  Returns 0 on success. */
int shellui_pad_disconnect_device(uint64_t physicalDeviceId);

/* Fallback when klog did not expose the physical Open Pad id. */
int shellui_pad_disconnect_first_physical_candidate(uint64_t skipDeviceId, uint64_t *outDeviceId);

/* Check whether a klog Open Pad handle belongs to a specific user in SceShellUI. */
int shellui_pad_user_has_handle(int32_t userId, int32_t observedHandle);

/* PT_ATTACH SceShellUI and call scePadVirtualDeviceInsertData(pad_handle, cross_data)
 * directly, bypassing scePadGetHandle.  padHandle comes from the CIM log. */
int shellui_pad_test_vdi_cross(int32_t pad_handle);
int shellcore_pad_test_vdi_cross(int32_t pad_handle);
int shellcore_pad_test_vdi_neutral(int32_t pad_handle); /* buttons=0, no UI input */

/* PT_ATTACH SceShellCore and call VDA(userId, type=3) via pt_call after
 * assignment.  Returns VDA return value; if >= 0 it is the VDI write handle. */
int32_t shellui_pad_retry_vda_shellcore(int32_t userId);

/* Manifest-verified SceShellCore VDA patch.
 *
 * PS4 firmware fingerprint from vda_probe_report:
 *   libScePad:scePadVirtualDeviceAddDevice hash256=0xbb22d8acd843d81e
 *   hash4k=0x346f2b8071895f89, VDA offset +0x5b40
 *
 * The patch only applies if prologue/hash/callsite/cave all match. It detours
 * the dispatcher call at VDA+0xc0 into the verified NOP cave at VDA+0xdd2,
 * calls the original dispatcher, forces eax=0, and returns to the original
 * canary/epilogue path. Early validation errors are left intact.
 * Returns 1 when applicable/applied/already applied, 0 on safe non-match. */
int shellui_pad_patch_vda(int dump_only);
int shellui_pad_patch_vda_self(int dump_only);
int shellui_pad_hook_setpriv(void);
int shellui_pad_unpatch(void);

