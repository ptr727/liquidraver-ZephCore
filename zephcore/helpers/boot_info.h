/*
 * SPDX-License-Identifier: MIT
 *
 * The MCU's reset cause, captured once at boot.
 *
 * hwinfo's flags accumulate between resets on some platforms; clearing after
 * reading is what isolates the most recent one. The companion did that in
 * main(), making it both the only reader and the one that cleared. The
 * repeater, room server and observer did neither.
 *
 * Doing it once at POST_KERNEL covers every role and keeps the value for
 * later readers.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The reset cause hwinfo reported at boot. out must be non-NULL.
 *
 * False means hwinfo could not report one, which is not the same as a cause
 * of 0. Call from main() or later: an initialiser at or before POST_KERNEL
 * CONFIG_KERNEL_INIT_PRIORITY_DEFAULT may get false.
 */
bool zephcore_boot_reset_cause(uint32_t *out);

#ifdef __cplusplus
}
#endif
