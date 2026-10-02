/*
 * SPDX-License-Identifier: MIT
 * Offer a node name to DHCP as the hostname, sanitised into a DNS label.
 *
 * Shared by the companion's network transports: a DHCP server that registers
 * its leases in DNS then resolves the node by name rather than by address.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sanitise `name` into a DNS label and set it as the hostname. Call before
 * DHCP starts, since the hostname rides in the DISCOVER; a later call (a
 * rename) is carried from the next lease renewal. A no-op unless
 * CONFIG_NET_HOSTNAME_DYNAMIC.
 */
void zc_net_set_hostname(const char *name);

#ifdef __cplusplus
}
#endif
