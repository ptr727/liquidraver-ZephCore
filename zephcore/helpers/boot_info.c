/*
 * SPDX-License-Identifier: MIT
 * Boot-time fact capture. See boot_info.h.
 */

#include "boot_info.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/util.h>

#include <stdio.h>

static uint32_t s_reset_cause;
static bool s_reset_cause_valid;

/*
 * Captured at POST_KERNEL so it runs before main() on every role, which is
 * what makes this the single reader of the register. The hwinfo reset-cause
 * path is a plain register read on every SoC that implements it, so it needs
 * no device to be ready first.
 */
static int boot_info_init(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) != 0) {
		/* Not supported on this platform -- distinct from "no cause". */
		return 0;
	}

	s_reset_cause = cause;
	s_reset_cause_valid = true;

	/* Clear so the next boot reports its own cause rather than the union of
	 * every reset since the last clear. Failure is not fatal: the captured
	 * value for this boot is already correct, and a stale-bit report next
	 * boot is better than refusing to boot. */
	(void)hwinfo_clear_reset_cause();

	return 0;
}

SYS_INIT(boot_info_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

bool zephcore_boot_reset_cause(uint32_t *out)
{
	if (!s_reset_cause_valid) {
		return false;
	}
	if (out != NULL) {
		*out = s_reset_cause;
	}
	return true;
}

int zephcore_boot_reset_cause_str(char *buf, size_t cap)
{
	static const struct {
		uint32_t bit;
		const char *label;
	} labels[] = {
		{ RESET_PIN,        "PIN" },
		{ RESET_SOFTWARE,   "SOFTWARE" },
		{ RESET_BROWNOUT,   "BROWNOUT" },
		{ RESET_POR,        "POR" },
		{ RESET_WATCHDOG,   "WATCHDOG" },
		{ RESET_DEBUG,      "DEBUG" },
		{ RESET_SECURITY,   "SECURITY" },
		{ RESET_LOW_POWER_WAKE, "LOWPOWER" },
		{ RESET_CPU_LOCKUP, "LOCKUP" },
		{ RESET_PARITY,     "PARITY" },
		{ RESET_PLL,        "PLL" },
		{ RESET_CLOCK,      "CLOCK" },
		{ RESET_HARDWARE,   "HARDWARE" },
		{ RESET_USER,       "USER" },
		{ RESET_TEMPERATURE, "TEMPERATURE" },
	};

	if (buf == NULL || cap == 0) {
		return 0;
	}

	buf[0] = '\0';

	if (!s_reset_cause_valid) {
		return 0;
	}

	size_t n = 0;
	for (size_t i = 0; i < ARRAY_SIZE(labels); i++) {
		if ((s_reset_cause & labels[i].bit) == 0) {
			continue;
		}
		int w = snprintf(buf + n, cap - n, " %s", labels[i].label);
		if (w < 0 || (size_t)w >= cap - n) {
			/* Truncated -- stop cleanly rather than half a label. */
			buf[n] = '\0';
			break;
		}
		n += (size_t)w;
	}

	return (int)n;
}
