/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include <sys/types.h>

int wireless_ds4_remote_reader_start(int32_t user_id, pid_t *out_pid,
                                    intptr_t *out_args_address);
int wireless_ds4_remote_reader_read(pid_t pid, intptr_t args_address,
                                   void *pad_data, uint32_t pad_data_len,
                                   uint32_t *out_seq);
int wireless_ds4_remote_reader_stop(pid_t pid, intptr_t args_address);

int wireless_ds4_game_bridge_install(int32_t user_id, pid_t *out_game_pid,
                                    intptr_t *out_args_address);
int wireless_ds4_game_bridge_update(pid_t game_pid, intptr_t args_address,
                                   const void *pad_data,
                                   uint32_t pad_data_len);

typedef struct {
    uint32_t active;
    uint32_t seq;
    int32_t pad_handle;
    int32_t receiver_ready;
    int32_t receiver_last_result;
    uint32_t receiver_port;
    uint64_t receiver_packets;
    uint64_t read_state_calls;
    uint64_t read_state_ext_calls;
    uint64_t read_calls;
    uint64_t read_ext_calls;
    uint64_t data_internal_calls;
    uint64_t controller_info_calls;
    uint64_t controller_info_spoofs;
    uint32_t buttons;
    uint8_t connected;
} Ds4tod5GameBridgeStatus;

int wireless_ds4_game_bridge_status(pid_t game_pid, intptr_t args_address,
                                   Ds4tod5GameBridgeStatus *out_status);
int wireless_ds4_game_bridge_remove(pid_t game_pid,
                                   intptr_t args_address);
