#pragma once

#include <stdint.h>
#include <sys/types.h>

void gc_fx_probe_shellui(pid_t pid, intptr_t libpad, intptr_t motion,
                         const int32_t *users, unsigned user_count);
