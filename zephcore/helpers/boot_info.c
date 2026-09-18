/*
 * SPDX-License-Identifier: MIT
 * Boot-time reset-cause capture. See boot_info.h.
 */

#include "boot_info.h"

#include <zephyr/init.h>
#include <zephyr/drivers/hwinfo.h>

static uint32_t s_reset_cause;
static bool s_reset_cause_valid;

/* POST_KERNEL: before main() on every role. */
static int boot_info_init(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) != 0) {
		/* Nothing to capture; the accessor reports false. */
		return 0;
	}

	s_reset_cause = cause;
	s_reset_cause_valid = true;

	/* Clear, so these flags are not reported again next boot. The return is
	 * discarded: a platform with no clear returns -ENOSYS and there is
	 * nothing to be done about it. */
	(void)hwinfo_clear_reset_cause();

	return 0;
}

SYS_INIT(boot_info_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

bool zephcore_boot_reset_cause(uint32_t *out)
{
	if (!s_reset_cause_valid) {
		return false;
	}

	*out = s_reset_cause;
	return true;
}
