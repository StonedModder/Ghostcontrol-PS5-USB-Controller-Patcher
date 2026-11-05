/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Resident-only native pad/output owner. Called only after admission and ticks
 * stop; callers serialize all native pad operations through checked close.
 * A blocker names current uncertainty, not hypothetical future API errors.
 * Close returns 0 only when no owned pad handle remains. Borrowed handles must
 * not be closed. Failure retains ownership/error state and forbids replacement.
 * Neither function changes credentials or touches process/kernel memory. */
const char *gc_service_output_release_blocker(void);
int gc_service_output_close_checked(void);
