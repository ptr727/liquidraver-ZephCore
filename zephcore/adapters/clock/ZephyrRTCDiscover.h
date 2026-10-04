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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Probe all declared RTC chips. If one is present and holds a sane time
 * (year >= 2025 and its power-loss flag is clear), store the Unix epoch in
 * *epoch_out and return true. The first chip that reads as an RTC (valid BCD
 * time, or its power-loss flag set or unreadable; never all 0xFF, nor, at a
 * descriptor with rv3028-eeprom-config, with an RV3028 always-zero bit set
 * in the same register on two reads) is remembered as the write-back target,
 * valid time or not. Adopting one with rv3028-eeprom-config first waits out
 * the chip's power-on refresh (up to ~66 ms), then turns its backup
 * switchover off while it reads the EEPROM and refreshes RAM from it, and
 * writes each byte that differs. Measured on a RAK4631: about 14 ms when
 * nothing is written, 52 ms for two bytes. Then switchover takes up to 2 ms
 * to react in DSM, 15.6 ms in LSM (4.2.2-3). If that store fails, the config
 * is set in RAM, which lasts until the chip's next daily refresh, and the
 * store is retried on the system work queue every 10 minutes, at most 3
 * times. Returns false if none present or no trustworthy time is held.
 */
bool zephcore_rtc_restore(uint32_t *epoch_out);

/*
 * Persist an authoritative epoch to the discovered RTC chip and clear its
 * power-loss flag. No-op if no RTC was discovered. Safe to call often, but
 * intended only for real syncs (GPS/app/CLI), not per-packet clock nudges.
 * If restore adopted nothing but skipped an all-0xFF candidate, the first
 * save probes again, once per boot, in the caller's context.
 */
void zephcore_rtc_save(uint32_t epoch);

#ifdef __cplusplus
}
#endif
