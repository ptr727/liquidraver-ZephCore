/*
 * SPDX-License-Identifier: MIT
 *
 * `hw`: what this firmware is and what hardware it found. Each line reports
 * what the code established: a driver names itself, and an unprobed part is
 * reported as unprobed, not absent.
 */

#pragma once

#include <stddef.h>

namespace mesh {
class MainBoard;
}

class CommonCLICallbacks;

namespace zephcore_hw {

/* Write the reply for `command`, the CLI line starting at "hw", into reply:
 * at most cap bytes, terminator included. */
void handle(const char *command, char *reply, size_t cap,
	    mesh::MainBoard *board, CommonCLICallbacks *callbacks);

} /* namespace zephcore_hw */
