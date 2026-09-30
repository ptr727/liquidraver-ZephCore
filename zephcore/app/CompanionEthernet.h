/*
 * SPDX-License-Identifier: MIT
 * Ethernet companion (CONFIG_ZEPHCORE_COMPANION_ETHERNET): bring up the wired
 * interface so the app can reach the node over TCP (TcpCompanionTransport, one
 * more interface beside BLE and USB), and the read-only eth.* CLI.
 */

#pragma once

#include <NodePrefs.h>

/* Set the interface address, take a DHCP lease, block sleep. Boot only. */
void companion_ethernet_start(const NodePrefs &prefs);

/* The eth.* commands. True when `command` was one of them (reply filled). */
bool companion_ethernet_cli(const char *command, char *reply);
