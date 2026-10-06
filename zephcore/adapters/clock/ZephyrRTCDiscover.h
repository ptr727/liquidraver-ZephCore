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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Probe the declared RTC chips in order, stopping at the first that holds a
 * sane time (year >= 2025 and its power-loss flag clear): store its Unix
 * epoch in *epoch_out and return true. The first chip found (valid time or
 * not) is remembered as the write-back target. A failed first read means
 * nothing is there, and a first read of all 0xFF (an erased EEPROM) is
 * skipped. Otherwise a device is passed over only if two reads each show it
 * is not that RTC: a bit set that the data sheet shows as 0 (the
 * descriptor's zero-mask), seconds, minutes, date or month out of range
 * while the power-loss flag does not read as set (an unreadable flag counts
 * as not set), or, on the second read, all 0xFF. A failed second read never
 * rules a device out. A time is taken only from a clean read whose fields,
 * hours and year are each in range (12-hour mode is not decoded). Returns
 * false if none present or no trustworthy time is held.
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
 * most 3 times, each retry identifying the chip again first. At such a
 * descriptor no time is taken from a year of A0h-FEh, the range that holds
 * the A0h mark and what a New Year counts it on to. FFh is a failed read.
 */
bool zephcore_rtc_restore(uint32_t *epoch_out);

/*
 * Persist an authoritative epoch within 2000-2099 to the discovered RTC chip
 * and clear its power-loss flag. No-op if no RTC was discovered. Safe to
 * call often, but intended only for real syncs (GPS/app/CLI), not
 * per-packet clock nudges.
 * If restore adopted nothing but skipped a candidate whose first read was all
 * 0xFF, the first save probes again, once per boot, in the caller's context.
 * A device ruled out on its second read is not probed again for that reason.
 *
 * At a descriptor with rv3028-eeprom-config the time is written with BSF
 * cleared before and checked after: the year marked A0h, then all seven
 * registers in one burst, so a write cut between the mark and the year byte
 * gives no time at the next boot. A write not confirmed is repeated every
 * 5 s on the system work queue, with the time run on, until one is
 * confirmed, a newer save replaces it, or 12 repeats (13 attempts in all)
 * have failed. Call only from the system work queue: the repeat shares
 * state with this call.
 *
 * A time outside 2000-2099 is not written, since the year is kept as two
 * BCD digits with no century, and the chip keeps its older time. At an
 * rv3028-eeprom-config descriptor the year is marked A0h instead, so the
 * next boot takes no time; the mark is confirmed and repeated like a time
 * write, and a repeat whose run-on time leaves 2000-2099 marks instead.
 */
void zephcore_rtc_save(uint32_t epoch);

#ifdef __cplusplus
}
#endif
