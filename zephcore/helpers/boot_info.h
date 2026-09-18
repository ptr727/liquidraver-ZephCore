/*
 * SPDX-License-Identifier: MIT
 *
 * Boot-time facts that are readable exactly once, captured before anything
 * consumes them.
 *
 * The MCU latches its reset cause in a register that survives the reset and
 * must be cleared, or the bits accumulate and the next boot reports the union
 * of every reset since the last clear. Both halves of that are a problem for
 * reporting:
 *
 *   - Whoever clears it destroys the fact for every later reader. The
 *     companion used to read and clear it in main(), so by the time a CLI
 *     command asked, the cause was gone and the honest answer was 0.
 *   - Whoever never clears it reports stale bits. The repeater and room
 *     server never touched the register, so a board that was once
 *     brownout-reset reported BROWNOUT on every boot thereafter.
 *
 * This captures the cause once at POST_KERNEL, before main() runs on any
 * role, then clears the register so the next boot starts clean. Every reader
 * -- the companion's restart notice and the `hw` CLI report alike -- reads the
 * captured copy, so they cannot disagree and neither can consume it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The reset cause latched for THIS boot. Returns false if the platform has no
 * hwinfo reset-cause support, which is different from a cause of 0 (the chip
 * reported no known cause) and is reported differently.
 */
bool zephcore_boot_reset_cause(uint32_t *out);

/*
 * Render the cause as space-prefixed labels, e.g. " PIN SOFTWARE". Writes ""
 * when the cause is 0 or carries only bits this build has no label for.
 * Returns the number of characters written, excluding the NUL.
 */
int zephcore_boot_reset_cause_str(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
