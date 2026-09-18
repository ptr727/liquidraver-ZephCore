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
 *   - Whoever never clears it reports stale bits, on the SoCs that latch
 *     them. The repeater and room server never touched the register, so on
 *     nRF, EFR32 and STM32 a board reset by the watchdog once reported
 *     WATCHDOG on every boot thereafter. Which bits a given SoC reports is
 *     its own business -- nRF52840 never reports BROWNOUT at all -- and
 *     ESP32 neither accumulates nor implements a clear, reporting one
 *     reason per boot, so there this is uniformity rather than a fix.
 *
 * This captures the cause once at POST_KERNEL, before main() runs on any
 * role, then clears the register so the next boot starts clean. Every reader
 * -- the companion's restart notice and any later diagnostic or CLI report
 * alike -- reads the captured copy, so they cannot disagree and neither can
 * consume it.
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
 *
 * Call this from main() or later. The capture runs as a SYS_INIT hook at
 * POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, so an earlier caller gets
 * false and cannot tell that apart from a platform that cannot report a cause
 * at all. Earlier means PRE_KERNEL_1 or _2, or POST_KERNEL at a numerically
 * lower priority value, which run before the capture deterministically; at the
 * same priority value link order decides and nothing enforces it.
 */
bool zephcore_boot_reset_cause(uint32_t *out);

/*
 * Render the cause as space-prefixed labels, e.g. " PIN SOFTWARE". Returns the
 * number of characters written, excluding the NUL.
 *
 * Writes "" and returns 0 in three cases the string alone cannot tell apart:
 * the platform has no reset-cause support, the chip reported a cause of 0, or
 * the cause carries only bits this build has no label for. A caller that needs
 * to distinguish the first from the other two asks zephcore_boot_reset_cause(),
 * which returns false only in that case.
 *
 * A label that would not fit in cap is dropped rather than cut, so the result
 * is always a whole number of labels and the return value counts only those.
 */
int zephcore_boot_reset_cause_str(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
