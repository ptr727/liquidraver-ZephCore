/*
 * SPDX-License-Identifier: MIT
 * Ethernet companion — see CompanionEthernet.h.
 *
 * Far smaller than the WiFi companion: the Ethernet L2 detects carrier itself,
 * so there is no association state machine here. All this does is give the
 * interface a stable address and start DHCP; the TCP transport is already
 * listening by the time it runs, and it serves whatever address DHCP lands on.
 */

#include "CompanionEthernet.h"

#include "../helpers/pm_sleep_guard.h"

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(zephcore_eth_companion, CONFIG_ZEPHCORE_MAIN_LOG_LEVEL);

static struct net_mgmt_event_callback s_iface_cb;
static struct net_mgmt_event_callback s_ipv4_cb;

static struct net_if *s_iface;
static volatile bool s_dhcp_bound;

static struct net_if *ethernet_iface(void)
{
	return net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
}

#if IS_ENABLED(CONFIG_ZEPHCORE_COMPANION_ETHERNET_STABLE_MAC)
/*
 * A DHCP reservation is only worth making against an address that survives a
 * reboot, so derive one from the factory efuse rather than leaving whatever the
 * controller powered up with. On ESP32 hwinfo_get_device_id() is the efuse base
 * MAC, and ESP-IDF derives its Ethernet address from the same base by adding 3
 * to the last octet, so this should also match what a vendor firmware on this
 * board uses.
 */
static void set_stable_mac(struct net_if *iface)
{
	struct ethernet_req_params params = {};
	uint8_t base[6];
	ssize_t len = hwinfo_get_device_id(base, sizeof(base));

	if (len != (ssize_t)sizeof(base)) {
		LOG_WRN("No device ID (%d) — keeping the controller's address, which "
			"is not stable across a power cycle", (int)len);
		return;
	}

	memcpy(params.mac_address.addr, base, sizeof(base));
	params.mac_address.addr[5] += 3;

	/* The request is refused with -EACCES while the interface is admin-up,
	 * and net_if_post_init() brings every interface up at POST_KERNEL, long
	 * before main() reaches here, so it has to go down for the duration.
	 * Nothing is lost by that: DHCP has not started yet. */
	bool was_up = net_if_is_admin_up(iface);

	if (was_up) {
		net_if_down(iface);
	}

	/* Sets the controller's address filter and the L2 link address together;
	 * setting only the link address would leave unicast RX filtered out. */
	int rc = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &params,
			  sizeof(params));
	if (rc < 0) {
		LOG_WRN("MAC set failed (%d) — a DHCP reservation may not hold", rc);
	}

	if (was_up) {
		rc = net_if_up(iface);
		if (rc < 0) {
			LOG_ERR("Interface did not come back up (%d)", rc);
		}
	}
}
#endif

static bool ethernet_ip(char *buf, size_t len)
{
	if (!s_iface) {
		return false;
	}

	bool found = false;

	/* The CLI reads this from the mesh event loop while DHCP adds and removes
	 * the address on its own thread, so the walk is locked. */
	net_if_lock(s_iface);

	struct net_if_ipv4 *ipv4 = s_iface->config.ip.ipv4;

	for (int i = 0; ipv4 && i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *a = &ipv4->unicast[i].ipv4;

		if (a->is_used && a->addr_state == NET_ADDR_PREFERRED) {
			found = net_addr_ntop(NET_AF_INET, &a->address.in_addr, buf,
					      len) != NULL;
			break;
		}
	}

	net_if_unlock(s_iface);
	return found;
}

static void iface_event(struct net_mgmt_event_callback *cb, uint64_t event,
			struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (iface != s_iface) {
		return;
	}

	if (event == NET_EVENT_IF_UP) {
		/* No restart here: NET_DHCPV4_RESTART_ON_IF_UP has dhcpv4 doing it
		 * already, and doing it again would stop the client, discard the
		 * state its INIT-REBOOT fast path reuses, and run the whole
		 * stop-start chain nested on the small net_mgmt stack. */
		LOG_INF("Ethernet link up");
	} else if (event == NET_EVENT_IF_DOWN) {
		/* Nothing to tear down: the TCP listener stays bound and the app
		 * reconnects by itself once the link returns. */
		LOG_INF("Ethernet link down");
		s_dhcp_bound = false;
	}
}

static void ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_IPV4_DHCP_STOP) {
		/* A NAK or an expiry ends the lease with the carrier still up, so
		 * without this the status stays "bound" with no address. */
		s_dhcp_bound = false;
		LOG_INF("DHCP lease ended");
		return;
	}

	if (event != NET_EVENT_IPV4_DHCP_BOUND) {
		return;
	}

	char addr[NET_IPV4_ADDR_LEN];

	s_dhcp_bound = true;
	if (ethernet_ip(addr, sizeof(addr))) {
		LOG_INF("DHCP bound: %s", addr);
	}
}

void companion_ethernet_start(const NodePrefs &prefs)
{
	ARG_UNUSED(prefs);

	s_iface = ethernet_iface();
	if (!s_iface) {
		LOG_ERR("No Ethernet interface");
		return;
	}

	/* Permanent, as the WiFi companion's: a wired node is mains or PoE
	 * powered, and the controller's RX path has to stay live. A no-op unless
	 * the build enables PM, which no board does with this transport today. */
	zc_pm_block_sleep();

#if IS_ENABLED(CONFIG_ZEPHCORE_COMPANION_ETHERNET_STABLE_MAC)
	/* Before DHCP starts, or the first DISCOVER goes out under the old
	 * address and a reservation misses on the boot after a reflash. */
	set_stable_mac(s_iface);
#endif

	net_mgmt_init_event_callback(&s_iface_cb, iface_event,
				     NET_EVENT_IF_UP | NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&s_iface_cb);

	net_mgmt_init_event_callback(&s_ipv4_cb, ipv4_event,
				     NET_EVENT_IPV4_DHCP_BOUND | NET_EVENT_IPV4_DHCP_STOP);
	net_mgmt_add_event_callback(&s_ipv4_cb);

	net_dhcpv4_start(s_iface);
	LOG_INF("Ethernet companion: waiting for a lease");
}

bool companion_ethernet_cli(const char *command, char *reply)
{
	if (strcmp(command, "get eth.status") == 0) {
		bool up = s_iface && net_if_is_up(s_iface);

		sprintf(reply, "> %s", !up            ? "link down" :
				       s_dhcp_bound   ? "link up (dhcp bound)"
						      : "link up (no lease)");
		return true;
	}
	if (strcmp(command, "get eth.ip") == 0) {
		char addr[NET_IPV4_ADDR_LEN];

		if (ethernet_ip(addr, sizeof(addr))) {
			sprintf(reply, "> %s", addr);
		} else {
			strcpy(reply, "> (no lease)");
		}
		return true;
	}
	if (strcmp(command, "get eth.mac") == 0) {
		struct net_linkaddr *ll = s_iface ? net_if_get_link_addr(s_iface) : NULL;

		if (ll && ll->len == 6) {
			sprintf(reply, "> %02x:%02x:%02x:%02x:%02x:%02x",
				ll->addr[0], ll->addr[1], ll->addr[2],
				ll->addr[3], ll->addr[4], ll->addr[5]);
		} else {
			strcpy(reply, "> (no address)");
		}
		return true;
	}
	return false;
}
