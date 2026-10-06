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
 * ZephyrRTCClock::setCurrentTime() writes it back via zephcore_rtc_save(), so
 * time survives the next power-off.
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
 * not) is remembered as the write-back target. A failed first read is skipped
 * (nothing is there, or nothing could be read), and so is a first read of all
 * 0xFF (an erased EEPROM). Otherwise a device is passed over only if two
 * reads each show it is not that RTC: a bit set that the data sheet shows as
 * 0 (the descriptor's zero-mask), seconds, minutes, date or month out of
 * range while the power-loss flag does not read as set (an unreadable flag
 * counts as not set), or, on the second read, all 0xFF. A failed second read
 * never rules a device out. A time is taken only from a clean read whose
 * fields, hours and year are each in range (12-hour mode is not decoded).
 * Returns false if none present or no trustworthy time is held.
 *
 * At a descriptor with rv3028-eeprom-config, the time is read once more
 * with the chip's backup switch flag (BSF) cleared before and checked
 * after, and is not taken if the read failed or BSF was set. A chip adopted
 * on a read that identified it (not on a failed second read), with BSF
 * clear, has that config stored at once: after the power-on refresh (up to
 * ~66 ms), backup switchover is turned off while the EEPROM is read, each
 * byte that differs is written, and a refresh from EEPROM restores it.
 * Measured on a RAK4631: about 14 ms when nothing is written, 52 ms for two
 * bytes, then up to 2 ms for DSM to react (4.2.2). If the store fails, the
 * config is set in RAM, which lasts until the chip's next refresh from
 * EEPROM (daily while EERD is clear), and only once EEbusy reads 0;
 * otherwise switchover stays as the store left it, off once the store has
 * disabled it, until a retry or that refresh. A failed store, or one
 * skipped at boot, is retried on the system work queue every 10 minutes, at
 * most 3 times, each retry identifying the chip again first.
 */
bool zephcore_rtc_restore(uint32_t *epoch_out);

/*
 * Persist an authoritative epoch to the discovered RTC chip and clear its
 * power-loss flag. No-op if no RTC was discovered. Safe to call often, but
 * intended only for real syncs (GPS/app/CLI), not per-packet clock nudges.
 * If restore adopted nothing but skipped a candidate whose first read was all
 * 0xFF, the first save probes again, once per boot, in the caller's context.
 * A device ruled out on its second read is not probed again for that reason.
 *
 * At a descriptor with rv3028-eeprom-config the time is written with BSF
 * cleared before and checked after, year 00h first and the real year last,
 * so a write cut by a power loss reads at boot as "time not yet set". A
 * write not confirmed is repeated every 5 s on the system work queue, with
 * the time run on, until one is confirmed, a newer save replaces it, or 12
 * repeats (13 attempts in all) have failed. Call only from the system work
 * queue: the repeat shares state with this call.
 */
void zephcore_rtc_save(uint32_t epoch);

/*
 * ---- Reporting accessors -------------------------------------------------
 *
 * Discovery knows which chips the board declares and which one it adopted, but
 * until now kept both to itself, so a release build could not say what RTC it
 * is driving (every MESH_DEBUG_* call is compiled away). These expose that.
 * The one change to probing: each probe run starts from no adopted chip, so a
 * second run reports its own outcome rather than the first run's.
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

/* Fill *out for declared candidate i. False if i is out of range. */
bool zephcore_rtc_get(size_t i, struct zephcore_rtc_entry *out);

/* Fill *out with the adopted chip. False if none was adopted, which includes
 * the case where discovery has not run yet -- check zephcore_rtc_probed(). */
bool zephcore_rtc_active(struct zephcore_rtc_entry *out);

/* Has boot-time discovery run? If false, every state above is UNPROBED. */
bool zephcore_rtc_probed(void);

#ifdef __cplusplus
}
#endif
