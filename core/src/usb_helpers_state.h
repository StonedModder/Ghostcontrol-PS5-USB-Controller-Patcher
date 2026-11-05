#pragma once

#include <stdint.h>

/* Pure completion validation shared by the target implementation and host
 * tests. USB_FS_COMPLETE dequeues the oldest completion for the whole ugen
 * session; its input ep_index is not a filter. */
enum UsbOutCompletionCheck {
    USB_OUT_COMPLETION_OTHER = 0,
    USB_OUT_COMPLETION_OK = 1,
    USB_OUT_COMPLETION_STATUS_ERROR = -1,
    USB_OUT_COMPLETION_FRAME_ERROR = -2,
    USB_OUT_COMPLETION_LENGTH_ERROR = -3
};

static inline int
usb_input_cache_should_release(uint64_t now_ms, uint64_t last_input_ms,
                               unsigned timeout_ms) {
    return timeout_ms != 0 && now_ms >= last_input_ms &&
           now_ms - last_input_ms >= timeout_ms;
}

static inline enum UsbOutCompletionCheck
usb_out_check_completion(uint8_t completed_index, uint8_t expected_index,
                         int status, uint32_t actual_frames,
                         uint32_t actual_length, uint32_t expected_length) {
    if (completed_index != expected_index)
        return USB_OUT_COMPLETION_OTHER;
    if (status != 0)
        return USB_OUT_COMPLETION_STATUS_ERROR;
    if (actual_frames != 1)
        return USB_OUT_COMPLETION_FRAME_ERROR;
    if (actual_length != expected_length)
        return USB_OUT_COMPLETION_LENGTH_ERROR;
    return USB_OUT_COMPLETION_OK;
}
