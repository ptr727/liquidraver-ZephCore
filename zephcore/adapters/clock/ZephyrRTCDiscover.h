/*
 * SPDX-License-Identifier: MIT
 *
 * Boot-time hardware-RTC auto-discovery (compact raw-I2C reader).
 *
 * Probes every I2C RTC chip declared with the "zephcore,rtc-i2c" binding
 * (boards/common/rtc-i2c.dtsi, or a board's own overlay). Chips that aren't
 * physically present fail the probe and are skipped — like the environment
 * sensors.
 *
 * If a present chip holds a valid time, zephcore_rtc_restore() returns it so
 * the soft clock can be seeded at boot (shown tagged "L" — local). Every
 * ZephyrRTCClock::setCurrentTime() within 2000-2099 writes it back via
 * zephcore_rtc_save(), so time survives the next power-off.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Probe the declared RTC chips in order, stopping at the first that holds a
 * sane time (year >= 2025 and its power-loss flag clear): store its Unix
 * epoch in *epoch_out and return true. The first chip found (valid time or
 * not) is remembered as the write-back target. Returns false if none is
 * present or none holds a trustworthy time. On a clean identification, a set
 * twelve-hour-bit is cleared before the time is taken. No time is taken if its
 * register cannot be read or reads FFh, or if the clear or the re-read fails.
 *
 * A descriptor with rv3028-eeprom-config also has that config stored in the
 * chip's EEPROM, retried on the system work queue if it fails.
 */
bool zephcore_rtc_restore(uint32_t *epoch_out);

/*
 * Persist an authoritative epoch to the discovered RTC chip and clear its
 * power-loss flag. No-op if no RTC was discovered. Intended for real syncs
 * (GPS/app/CLI), not per-packet clock nudges. Call only from the system work
 * queue: an unconfirmed write is repeated there and shares state with this
 * call. A time outside 2000-2099 is not written.
 */
void zephcore_rtc_save(uint32_t epoch);

/*
 * ---- Reporting accessors -------------------------------------------------
 *
 * Discovery knows which chips the board declares and which one it adopted, but
 * until now kept both to itself, so a release build could not say what RTC it
 * is driving (every MESH_DEBUG_* call is compiled away). These expose that.
 * The one change to probing: each probe run starts from no adopted chip, so a
 * second run reports its own outcome rather than the first run's, and a run
 * publishes its outcome only when it ends.
 *
 * They report only what discovery actually established. The probe loop stops
 * at the first chip holding a valid time, so candidates after it are never
 * reached -- those report UNPROBED rather than being reported as absent. A
 * candidate whose I2C bus was not ready reports UNPROBED for the same reason:
 * a probe that could not run is not evidence that the chip is missing.
 */

enum zephcore_rtc_state {
	ZEPHCORE_RTC_UNPROBED = 0, /* not probed: discovery stopped before
				    * reaching it, its bus was not ready, or
				    * the read failed other than with -EIO */
	ZEPHCORE_RTC_ABSENT,       /* the read ended in -EIO (no ACK; on nRF
				    * also any bus error), or two reads each
				    * showed a device that is not this RTC */
	ZEPHCORE_RTC_PRESENT,      /* identified as this RTC, including one
				    * whose time could not be read */
	ZEPHCORE_RTC_ALL_FF,       /* the first read was all 0xFF: an erased
				    * EEPROM, or an RTC that has not started.
				    * Skipped; if no chip was adopted, the
				    * first save probes again */
};

/* A devicetree-declared "zephcore,rtc-i2c" candidate, plus its probe outcome. */
struct zephcore_rtc_entry {
	const char *name;              /* DT node full name, e.g. "rtc-rv3028@52" */
	const char *bus;               /* I2C bus device name */
	uint16_t addr;                 /* I2C address */
	enum zephcore_rtc_state state; /* what the boot probe found */
	bool active;                   /* adopted as the write-back target */
};

/* Number of RTC candidates discovery knows of: those the board declares in
 * devicetree, or 0 when there are none or autodiscovery is compiled out. */
size_t zephcore_rtc_declared(void);

/* Copy up to max candidates into out[], in declaration order, and return how
 * many were copied. The states and the adopted marker come from one probe
 * run, even while a re-probe runs on another thread. No entry is active when
 * none was adopted, which includes discovery not having run yet -- check
 * zephcore_rtc_probed(). */
size_t zephcore_rtc_snapshot(struct zephcore_rtc_entry *out, size_t max);

/* Has boot-time discovery run? If false, every state above is UNPROBED. */
bool zephcore_rtc_probed(void);

#ifdef __cplusplus
}
#endif
