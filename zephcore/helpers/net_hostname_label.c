/*
 * SPDX-License-Identifier: MIT
 * See net_hostname_label.h.
 */

#include "net_hostname_label.h"

#include <zephyr/logging/log.h>
#include <zephyr/net/hostname.h>
#include <zephyr/sys/util.h>

#include <string.h>

LOG_MODULE_REGISTER(zephcore_hostname, CONFIG_ZEPHCORE_MAIN_LOG_LEVEL);

/* Longer than any node name the protocol carries; only a bound, not a limit. */
#define NODE_NAME_SCAN_MAX      128
#define FALLBACK_HOSTNAME       "zephcore"

void zc_net_set_hostname(const char *name)
{
#if IS_ENABLED(CONFIG_NET_HOSTNAME_DYNAMIC)
	/*
	 * A node name is free text the user picks in the app; a DNS label is
	 * not. RFC 1123 allows only letters, digits and hyphens, with no
	 * leading or trailing hyphen, so anything else becomes a hyphen and
	 * runs of them collapse. The result is lowercased, because DNS
	 * comparison is case-insensitive and mixed case only makes one node
	 * look like two.
	 */
	char host[MIN(CONFIG_NET_HOSTNAME_MAX_LEN, 63) + 1];
	size_t n = 0;
	bool prev_hyphen = true;        /* drops leading hyphens */

	/* Bound the input as well as the output: a run of characters that all
	 * sanitise away advances neither n nor prev_hyphen, so a scan bounded
	 * only by the output index would walk forward until it happened to
	 * find a NUL. */
	const size_t name_len = (name != NULL) ? strnlen(name, NODE_NAME_SCAN_MAX) : 0;

	for (size_t i = 0; i < name_len && n < sizeof(host) - 1; i++) {
		unsigned char c = (unsigned char)name[i];

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			host[n++] = (char)c;
			prev_hyphen = false;
		} else if (c >= 'A' && c <= 'Z') {
			host[n++] = (char)(c - 'A' + 'a');
			prev_hyphen = false;
		} else if (!prev_hyphen) {
			host[n++] = '-';
			prev_hyphen = true;
		}
	}

	while (n > 0 && host[n - 1] == '-') {
		n--;                    /* and trailing ones */
	}
	host[n] = '\0';

	if (n == 0) {
		/* Nothing survived, which an all-emoji or non-Latin node name
		 * does routinely. A name that could collide beats no name at
		 * all, and renaming the node fixes it. Copied with a bound
		 * because the buffer is sized from Kconfig and can be shorter
		 * than this literal. */
		n = MIN(strlen(FALLBACK_HOSTNAME), sizeof(host) - 1);
		memcpy(host, FALLBACK_HOSTNAME, n);
		host[n] = '\0';
	}

	if (strcmp(host, net_hostname_get()) == 0) {
		return;         /* every CLI save comes here, rename or not */
	}

	int rc = net_hostname_set(host, n);

	if (rc < 0) {
		LOG_WRN("Hostname '%s' rejected (%d)", host, rc);
	} else {
		LOG_INF("DHCP hostname: %s", host);
	}
#else
	ARG_UNUSED(name);
#endif
}
