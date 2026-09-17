/*
 * SPDX-License-Identifier: MIT
 *
 * Boot-time hardware-RTC auto-discovery (compact raw-I2C reader).
 *
 * Probes every I2C RTC chip declared with the "zephcore,rtc-i2c" binding
 * (boards/common/rtc-i2c.dtsi, opt-in per board). Chips that aren't physically
 * present fail the probe and are skipped — like the environment sensors.
 *
 * If a present chip holds a valid time, zephcore_rtc_restore() returns it so
 * the soft clock can be seeded at boot (shown tagged "L" — local). On every
 * authoritative sync (GPS/app/CLI) the caller writes it back via
 * zephcore_rtc_save() so time survives the next power-off.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Probe all declared RTC chips. If one is present and holds a sane time
 * (year >= 2025 and its power-loss flag is clear), store the Unix epoch in
 * *epoch_out and return true. The present chip (valid time or not) is
 * remembered as the write-back target. Returns false if none present or no
 * trustworthy time is held.
 */
bool zephcore_rtc_restore(uint32_t *epoch_out);

/*
 * Persist an authoritative epoch to the discovered RTC chip and clear its
 * power-loss flag. No-op if no RTC was discovered. Safe to call often, but
 * intended only for real syncs (GPS/app/CLI), not per-packet clock nudges.
 */
void zephcore_rtc_save(uint32_t epoch);

/*
 * ---- Reporting accessors -------------------------------------------------
 *
 * Discovery knows which chips the board declares and which one it adopted, but
 * until now kept both to itself, so a release build could not say what RTC it
 * is driving (every MESH_DEBUG_* call is compiled away). These expose that
 * without changing any probing behaviour.
 *
 * They report only what discovery actually established. The probe loop stops
 * at the first chip holding a valid time, so candidates after it are never
 * reached -- those report UNPROBED rather than being reported as absent.
 */

enum zephcore_rtc_state {
	ZEPHCORE_RTC_UNPROBED = 0, /* discovery stopped before reaching it */
	ZEPHCORE_RTC_ABSENT,       /* no ACK, or a non-RTC chip sharing the address */
	ZEPHCORE_RTC_PRESENT,      /* an RTC answered and was accepted as one */
};

/* A devicetree-declared "zephcore,rtc-i2c" candidate, plus its probe outcome. */
struct zephcore_rtc_entry {
	const char *name;              /* DT node full name, e.g. "rtc-rv3028@52" */
	const char *bus;               /* I2C bus device name */
	uint16_t addr;                 /* I2C address */
	enum zephcore_rtc_state state; /* what the boot probe found */
	bool active;                   /* adopted as the write-back target */
};

/* Number of RTC candidates this board declares in devicetree. 0 if none. */
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
