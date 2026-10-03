/*
 * SPDX-License-Identifier: MIT
 *
 * `hw` — what this firmware is, and what hardware it actually found.
 *
 * A release build compiles every MESH_DEBUG_* call away, so the firmware knows
 * its board, MCU, clock source, GNSS wiring and sensor inventory and has no
 * way to tell anyone. This formats those facts for the CLI.
 *
 * Radio parameters are deliberately NOT reported here: `get freq`, `get sf`
 * and the stats-radio family already cover them, and duplicating a live
 * setting in a second place is how the two drift apart.
 *
 * The rule every formatter here follows: report what the code KNOWS, never
 * what could be inferred. A devicetree comment naming a GNSS part is not
 * evidence the part is fitted (the rak4631 overlay documents a u-blox MAX-7Q
 * while binding gnss-nmea-generic, which drives whatever NMEA receiver is
 * fitted). So a driver names itself, and an unprobed address reports as
 * unprobed rather than absent.
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
 * `cap` is the caller's reply capacity: CommonCLI::replyCap(), which allows for
 * a remote reply's "xx|" prefix, and on a companion at most one app CLI frame
 * (MAX_FRAME_SIZE - 1).
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
