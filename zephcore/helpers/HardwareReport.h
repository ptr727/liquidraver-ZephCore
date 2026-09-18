/*
 * SPDX-License-Identifier: MIT
 *
 * `hw` — what this firmware is, and what hardware it actually found.
 *
 * A release build compiles every MESH_DEBUG_* call away, so the firmware knows
 * its board, MCU, radio, clock source, GNSS wiring and sensor inventory and
 * has no way to tell anyone. This formats those facts for the CLI.
 *
 * The rule every formatter here follows: report what the code KNOWS, never
 * what could be inferred. A devicetree comment naming a GNSS part is not
 * evidence the part is fitted (the rak4631 overlay documents a u-blox MAX-7Q
 * while binding gnss-nmea-generic, which happily drives the CASIC receiver
 * actually present). So a driver names itself, an unprobed address reports as
 * unprobed rather than absent, and an enable pin of -1 prints as -1.
 *
 * Named `hw`, not `hwinfo`, because <zephyr/drivers/hwinfo.h> is a Zephyr
 * subsystem this tree already uses; a CLI command sharing that name would read
 * ambiguously in the source.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mesh {
class MainBoard;
class RTCClock;
}

class CommonCLICallbacks;

namespace zephcore_hw {

/*
 * Write the reply for `command`, which is the full CLI line starting at "hw".
 * `cap` is the caller's reply capacity -- CLI_REPLY_SIZE locally, or
 * CLI_REMOTE_REPLY_SIZE when the request came from remote mesh admin, since a
 * remote reply rides the caller's LoRa packet buffer.
 *
 * `local` is true only when the request did not arrive over the air. Note it
 * is NOT a test for "this is a serial console": on companion firmware the USB
 * sideband, the app's CMD_RUN_CLI_COMMAND and the V-Contact loopback all reach
 * the CLI indistinguishably. It means "not remote mesh admin" and nothing more.
 */
void handle(const char *command, char *reply, size_t cap, bool local,
	    mesh::MainBoard *board, mesh::RTCClock *rtc,
	    CommonCLICallbacks *callbacks);

} /* namespace zephcore_hw */
