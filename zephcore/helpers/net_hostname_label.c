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

	for (size_t i = 0; name != NULL && name[i] != '\0' && n < sizeof(host) - 1; i++) {
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
		/* Nothing survived. A name that could collide beats no name at
		 * all, and renaming the node fixes it. */
		strcpy(host, "zephcore");
		n = strlen(host);
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
