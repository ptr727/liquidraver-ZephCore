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
#include <stddef.h>
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

/*
 * Render the cause as space-prefixed labels, e.g. " PIN SOFTWARE". Returns
 * the characters written, excluding the NUL.
 *
 * 0 means nothing to render: no cause, a cause of 0, no labelled bits, or cap
 * too small for one label. Nothing at all is written when buf is NULL or cap
 * is 0; otherwise buf is always NUL-terminated. The first label that does not
 * fit ends the string.
 */
int zephcore_boot_reset_cause_str(char *buf, size_t cap);

/*
 * The part of the cause that zephcore_boot_reset_cause_str() would name. A
 * caller deciding whether a cause is worth reporting wants this rather than
 * the raw cause, so an unlabelled bit cannot attribute a report to whichever
 * labels happen to accompany it.
 */
uint32_t zephcore_boot_reset_cause_labelled(void);

#ifdef __cplusplus
}
#endif
