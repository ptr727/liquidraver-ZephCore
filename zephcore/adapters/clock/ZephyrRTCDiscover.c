/*
 * SPDX-License-Identifier: MIT
 *
 * Compact raw-I2C hardware-RTC auto-discovery. See ZephyrRTCDiscover.h.
 *
 * Register layouts (sec/min/hour/.../month/year, all BCD) and the per-chip
 * power-loss flags are carried in devicetree via the "zephcore,rtc-i2c"
 * binding, so this reader is generic — adding a new chip is a DT node, not
 * code. Maps were taken from Zephyr's own drivers (rtc_pcf8563.c,
 * rtc_ds3231.c, rtc_rv3028.c, rtc_rx8130ce.c).
 */

#include "ZephyrRTCDiscover.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include <string.h>

LOG_MODULE_REGISTER(zephcore_rtc, CONFIG_ZEPHCORE_DATASTORE_LOG_LEVEL);

#define RTC_COMPAT zephcore_rtc_i2c

#if IS_ENABLED(CONFIG_ZEPHCORE_RTC_AUTODISCOVER) && DT_HAS_COMPAT_STATUS_OKAY(RTC_COMPAT)

/* Validity flag lives in the seconds byte itself (PCF8563 VL bit). */
#define RTC_STATUS_IN_SECONDS 0xFF

struct rtc_desc {
	const struct device *bus;
	uint16_t addr;
	uint8_t  time_reg;     /* register of the seconds byte */
	uint8_t  date_index;   /* day-of-month offset in the 7-byte block */
	uint8_t  status_reg;   /* power-loss flag register, or RTC_STATUS_IN_SECONDS */
	uint8_t  status_mask;  /* "time unreliable" bit within status_reg */
	const uint8_t *cfg;    /* rv3028-eeprom-config triplets, or NULL */
	uint8_t  cfg_len;
	const char *name;
};

#define RTC_CFG_NAME(node) _CONCAT(rtc_cfg_, DT_DEP_ORD(node))

#define RTC_CFG_ARRAY(node)                                           \
	IF_ENABLED(DT_NODE_HAS_PROP(node, rv3028_eeprom_config),      \
		   (static const uint8_t RTC_CFG_NAME(node)[] =       \
			    DT_PROP(node, rv3028_eeprom_config);      \
		    BUILD_ASSERT(sizeof(RTC_CFG_NAME(node)) % 3 == 0 &&  \
				 sizeof(RTC_CFG_NAME(node)) <= UINT8_MAX,  \
				 "rv3028-eeprom-config is (register, mask, value) triplets");))

DT_FOREACH_STATUS_OKAY(RTC_COMPAT, RTC_CFG_ARRAY)

#define RTC_DESC_ENTRY(node)                                          \
	{                                                             \
		.bus         = DEVICE_DT_GET(DT_BUS(node)),           \
		.addr        = (uint16_t)DT_REG_ADDR(node),           \
		.time_reg    = (uint8_t)DT_PROP(node, time_reg),      \
		.date_index  = (uint8_t)DT_PROP(node, date_index),    \
		.status_reg  = (uint8_t)DT_PROP(node, status_reg),    \
		.status_mask = (uint8_t)DT_PROP(node, status_mask),   \
		.cfg         = COND_CODE_1(                           \
			DT_NODE_HAS_PROP(node, rv3028_eeprom_config),  \
			(RTC_CFG_NAME(node)), (NULL)),                 \
		.cfg_len     = DT_PROP_LEN_OR(node, rv3028_eeprom_config, 0), \
		.name        = DT_NODE_FULL_NAME(node),               \
	},

static const struct rtc_desc rtc_descs[] = {
	DT_FOREACH_STATUS_OKAY(RTC_COMPAT, RTC_DESC_ENTRY)
};

/* Chip we'll read/write going forward (first one found present). */
static const struct rtc_desc *s_active;
static bool s_probed;
/* A candidate read all 0xFF and was skipped: a later zephcore_rtc_save()
 * runs the whole probe once more, once per boot. */
static bool s_skipped_ff;
static bool s_reprobed;

#define BCD2BIN(x) ((((x) >> 4) & 0x0F) * 10 + ((x) & 0x0F))

/* RV3028 registers for reading and writing its configuration EEPROM. */
#define RV3028_REG_STATUS     0x0E
#define RV3028_STATUS_EEBUSY  BIT(7)
#define RV3028_REG_CONTROL1   0x0F
#define RV3028_CONTROL1_EERD  BIT(3)
#define RV3028_REG_EE_ADDR    0x25
#define RV3028_REG_EE_DATA    0x26
#define RV3028_REG_EE_COMMAND 0x27
#define RV3028_EE_CMD_REFRESH 0x12  /* all configuration EEPROM -> RAM */
#define RV3028_EE_CMD_WRITE   0x21  /* EEDATA -> one EEPROM byte */
#define RV3028_EE_CMD_READ    0x22  /* one EEPROM byte -> EEDATA */
#define RV3028_REG_CFG_FIRST  0x35  /* EEPROM Clkout */
#define RV3028_REG_CFG_LAST   0x37  /* EEPROM Backup */
#define RV3028_BACKUP_BSM     0x0C  /* 37h switchover mode; 00 = disabled */
#define BIN2BCD(x) ((((x) / 10) << 4) | ((x) % 10))

/* A byte is valid BCD if both nibbles are 0-9, and its decoded value fits the
 * field. Used to tell a real RTC apart from an unrelated I2C chip that happens
 * to share an address (e.g. an MPU-class IMU at 0x68, same as DS3231) — we must
 * never adopt and write time into such a device. */
static bool bcd_field_ok(uint8_t v, unsigned max)
{
	if ((v & 0x0F) > 9 || (v >> 4) > 9) {
		return false;
	}
	return BCD2BIN(v) <= max;
}

/* Howard Hinnant's civil<->days algorithms (proleptic Gregorian, UTC). */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
	y -= (m <= 2);
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int yy = (int)yoe + (int)(era * 400);
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = yy + (*m <= 2);
}

/* Read the chip's power-loss flag. true => held time is unreliable. */
static bool rtc_time_unreliable(const struct rtc_desc *d, const uint8_t blk[7])
{
	if (d->status_reg == RTC_STATUS_IN_SECONDS) {
		return (blk[0] & d->status_mask) != 0;
	}

	uint8_t st;
	if (i2c_reg_read_byte(d->bus, d->addr, d->status_reg, &st) != 0) {
		return true;  /* can't confirm => don't trust it */
	}
	return (st & d->status_mask) != 0;
}

/* Wait for EEbusy to clear: ~66 ms after power-on, ~16 ms for a byte write.
 * A failed read ends the wait, rather than polling a dead bus through each
 * transfer's timeout. */
static bool rv3028_eeprom_idle(const struct rtc_desc *d)
{
	for (int ms = 0; ms <= 100; ms++) {
		uint8_t st;

		if (i2c_reg_read_byte(d->bus, d->addr, RV3028_REG_STATUS, &st) != 0) {
			return false;
		}
		if (!(st & RV3028_STATUS_EEBUSY)) {
			return true;
		}
		k_msleep(1);
	}
	return false;
}

/* The manual (4.6.7): wait 10 ms after an EEPROM write, 1 ms after a read or
 * Refresh, before checking EEbusy. */
static bool rv3028_eeprom_cmd(const struct rtc_desc *d, uint8_t cmd, int32_t wait_ms)
{
	if (i2c_reg_write_byte(d->bus, d->addr, RV3028_REG_EE_COMMAND, 0x00) != 0 ||
	    i2c_reg_write_byte(d->bus, d->addr, RV3028_REG_EE_COMMAND, cmd) != 0) {
		return false;
	}
	k_msleep(wait_ms);
	return rv3028_eeprom_idle(d);
}

/* One EEPROM byte, read (4.6.6) or written (4.6.5). */
static bool rv3028_eeprom_read(const struct rtc_desc *d, uint8_t reg, uint8_t *val)
{
	return i2c_reg_write_byte(d->bus, d->addr, RV3028_REG_EE_ADDR, reg) == 0 &&
	       rv3028_eeprom_cmd(d, RV3028_EE_CMD_READ, 1) &&
	       i2c_reg_read_byte(d->bus, d->addr, RV3028_REG_EE_DATA, val) == 0;
}

static bool rv3028_eeprom_write(const struct rtc_desc *d, uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(d->bus, d->addr, RV3028_REG_EE_ADDR, reg) == 0 &&
	       i2c_reg_write_byte(d->bus, d->addr, RV3028_REG_EE_DATA, val) == 0 &&
	       rv3028_eeprom_cmd(d, RV3028_EE_CMD_WRITE, 10);
}

/* A config triplet is used only if its register is 35h-37h, its mask covers
 * implemented bits only (35h bits 5:4 are not; manual 3.15.4), and no
 * earlier triplet names the same register. */
static bool rv3028_cfg_triplet_ok(const struct rtc_desc *d, size_t i)
{
	static const uint8_t impl[] = { 0xCF, 0xFF, 0xFF };  /* 35h, 36h, 37h */
	uint8_t reg = d->cfg[i];

	if (reg < RV3028_REG_CFG_FIRST || reg > RV3028_REG_CFG_LAST ||
	    (d->cfg[i + 1] & ~impl[reg - RV3028_REG_CFG_FIRST]) != 0) {
		return false;
	}
	for (size_t k = 0; k < i; k += 3) {
		if (d->cfg[k] == reg) {
			return false;
		}
	}
	return true;
}

/* Bits the RV3028 always reads as 0 in its time block (manual 3.2). False if
 * one is set in the same register of both reads a and b: not an RV3028. */
static bool rv3028_time_bits_ok(const uint8_t a[7], const uint8_t b[7])
{
	static const uint8_t zero[7] = { 0x80, 0x80, 0xC0, 0xF8, 0xC0, 0xE0, 0x00 };

	for (size_t k = 0; k < 7; k++) {
		if ((a[k] & zero[k]) && (b[k] & zero[k])) {
			return false;
		}
	}
	return true;
}

/* RAM 37h with the configured 37h bits applied, or as it was if none are. */
static uint8_t rv3028_cfg_backup_ram(const struct rtc_desc *d, uint8_t ram)
{
	for (size_t i = 0; i + 2 < d->cfg_len; i += 3) {
		if (d->cfg[i] == RV3028_REG_CFG_LAST && rv3028_cfg_triplet_ok(d, i)) {
			return (ram & ~d->cfg[i + 1]) | (d->cfg[i + 2] & d->cfg[i + 1]);
		}
	}
	return ram;
}

/* Store the descriptor's rv3028-eeprom-config in the chip's EEPROM. The
 * EEPROM is only touched with automatic refresh held off (4.6.7) and backup
 * switchover disabled in RAM (3.15.6: BSM 00 for any EEPROM read or write).
 * Each byte is compared against the EEPROM itself (4.6.6) and written only if
 * it differs (4.6.5). A closing Refresh (4.6.4) reloads RAM from the EEPROM,
 * switching back to the stored BSM, and the config is read back on every run.
 * A triplet outside 35h-37h, masking an unimplemented bit, or repeating a
 * register is ignored, so a devicetree mistake cannot write on every boot.
 * Reports whether the config was confirmed, and if so whether EERD, which
 * also stops the daily refresh, was cleared afterwards. */
enum rv3028_store { RV3028_STORED, RV3028_EERD_SET, RV3028_NOT_STORED };

static enum rv3028_store rv3028_store_config(const struct rtc_desc *d)
{
	bool changed = false, held = false, refreshed = false, ok;
	uint8_t backup = 0;

	ok = i2c_reg_update_byte(d->bus, d->addr, RV3028_REG_CONTROL1,
				 RV3028_CONTROL1_EERD, RV3028_CONTROL1_EERD) == 0 &&
	     rv3028_eeprom_idle(d) &&
	     i2c_reg_read_byte(d->bus, d->addr, RV3028_REG_CFG_LAST, &backup) == 0;
	if (ok) {
		held = true;
		ok = i2c_reg_write_byte(d->bus, d->addr, RV3028_REG_CFG_LAST,
					backup & ~RV3028_BACKUP_BSM) == 0;
	}

	for (size_t i = 0; ok && i + 2 < d->cfg_len; i += 3) {
		uint8_t old, reg = d->cfg[i], mask = d->cfg[i + 1];
		uint8_t val = d->cfg[i + 2] & mask;

		if (!rv3028_cfg_triplet_ok(d, i)) {
			LOG_WRN("%s: config triplet for 0x%02x ignored", d->name, reg);
			continue;
		}
		ok = rv3028_eeprom_read(d, reg, &old);
		if (ok && (old & mask) != val) {
			ok = rv3028_eeprom_write(d, reg, (old & ~mask) | val);
			changed = true;
		}
	}
	ok = refreshed = ok && rv3028_eeprom_cmd(d, RV3028_EE_CMD_REFRESH, 1);
	for (size_t i = 0; ok && i + 2 < d->cfg_len; i += 3) {
		uint8_t now;

		if (rv3028_cfg_triplet_ok(d, i)) {
			ok = i2c_reg_read_byte(d->bus, d->addr, d->cfg[i], &now) == 0 &&
			     ((now ^ d->cfg[i + 2]) & d->cfg[i + 1]) == 0;
		}
	}

	/* The Refresh did not run or did not finish, so RAM may still hold BSM
	 * 00. Once an EEPROM operation still running has had the chance to
	 * finish, write 37h as configured: RAM only, never an older value. */
	if (held && !refreshed) {
		(void)rv3028_eeprom_idle(d);
		(void)i2c_reg_write_byte(d->bus, d->addr, RV3028_REG_CFG_LAST,
					 rv3028_cfg_backup_ram(d, backup));
	}
	if (ok && changed) {
		LOG_INF("%s: config committed to EEPROM", d->name);
	}
	if (i2c_reg_update_byte(d->bus, d->addr, RV3028_REG_CONTROL1,
				RV3028_CONTROL1_EERD, 0) != 0) {
		return ok ? RV3028_EERD_SET : RV3028_NOT_STORED;
	}
	return ok ? RV3028_STORED : RV3028_NOT_STORED;
}

/* Set the config in the RAM mirror only, which lasts until the next refresh. */
static bool rv3028_set_ram(const struct rtc_desc *d)
{
	bool ok = true;

	for (size_t i = 0; i + 2 < d->cfg_len; i += 3) {
		uint8_t old, reg = d->cfg[i], mask = d->cfg[i + 1];

		if (rv3028_cfg_triplet_ok(d, i)) {
			ok = i2c_reg_read_byte(d->bus, d->addr, reg, &old) == 0 &&
			     i2c_reg_write_byte(d->bus, d->addr, reg,
						(old & ~mask) | (d->cfg[i + 2] & mask)) == 0 &&
			     ok;
		}
	}
	return ok;
}

/* A failed store is retried on the system work queue, a few times per boot
 * only: a password-locked chip, or a part at the address that is not an
 * RV3028, never succeeds, and every attempt writes to it again. */
#define RTC_CFG_RETRY K_MINUTES(10)
#define RTC_CFG_RETRIES 3

static uint8_t s_cfg_tries;
static void rv3028_cfg_retry_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(s_cfg_retry, rv3028_cfg_retry_fn);

/* Store the config, falling back to the RAM mirror if that fails: without
 * the EEPROM store the RAM config lasts only until the next refresh, which on
 * a chip still holding its delivery EEPROM turns switchover back off. */
static void rv3028_configure(const struct rtc_desc *d)
{
	if (d->cfg == NULL) {
		return;
	}
	s_cfg_tries++;
	switch (rv3028_store_config(d)) {
	case RV3028_STORED:
		return;
	case RV3028_EERD_SET:
		LOG_WRN("%s: config stored, but EERD still set, so no daily refresh "
			"(attempt %u)", d->name, s_cfg_tries);
		break;
	case RV3028_NOT_STORED:
		(void)rv3028_eeprom_idle(d);
		LOG_WRN("%s: config not stored in EEPROM, %s in RAM (attempt %u)", d->name,
			rv3028_set_ram(d) ? "set" : "NOT set", s_cfg_tries);
		break;
	}
	if (s_cfg_tries <= RTC_CFG_RETRIES) {
		k_work_schedule(&s_cfg_retry, RTC_CFG_RETRY);
	}
}

static void rv3028_cfg_retry_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	rv3028_configure(s_active);
}

/* Probe all chips once; cache the first present one in s_active. If a present
 * chip holds a sane time, return it via epoch_out. */
static bool rtc_probe(uint32_t *epoch_out)
{
	s_skipped_ff = false;

	for (size_t i = 0; i < ARRAY_SIZE(rtc_descs); i++) {
		const struct rtc_desc *d = &rtc_descs[i];
		uint8_t blk[7];

		if (!device_is_ready(d->bus)) {
			continue;
		}
		if (i2c_burst_read(d->bus, d->addr, d->time_reg, blk, sizeof(blk)) != 0) {
			continue;  /* no ACK => chip absent */
		}

		/* An erased EEPROM sharing the address reads 0xFF everywhere, its
		 * "status" too, which would otherwise pass as a set power-loss flag.
		 * A real RTC can read all 0xFF too (some leave their time undefined
		 * at power-on), so a later save probes again. */
		bool all_ff = true;

		for (size_t k = 0; k < sizeof(blk); k++) {
			all_ff = all_ff && (blk[k] == 0xFF);
		}
		if (all_ff) {
			s_skipped_ff = true;
			continue;
		}

		/* At an RV3028 descriptor, a bit an RV3028 always reads as 0 marks
		 * some other part. Only the same register showing one on two reads
		 * rules out an RV3028; a failed second read does not. Without a
		 * clean read the chip is adopted but no time is taken from it. */
		bool garbled = false;

		if (d->cfg != NULL && !rv3028_time_bits_ok(blk, blk)) {
			uint8_t again[sizeof(blk)];

			if (i2c_burst_read(d->bus, d->addr, d->time_reg, again, sizeof(again)) == 0) {
				if (!rv3028_time_bits_ok(blk, again)) {
					LOG_WRN("%s: not an RV3028, skipped", d->name);
					continue;
				}
				memcpy(blk, again, sizeof(blk));
			}
			garbled = !rv3028_time_bits_ok(blk, blk);
		}

		/* Mask off flag/century bits. We trust this is a real RTC (vs. an
		 * unrelated chip sharing the address) if EITHER the block is valid
		 * BCD, OR the chip's power-loss flag is set — the latter is itself
		 * proof it's an RTC that lost power and whose time registers may be
		 * garbage. Adopting in the power-loss case is essential: otherwise a
		 * battery-depleted RTC (garbage registers, VL/OSF/PORF latched) would
		 * never become the write-back target, so a sync could never
		 * re-initialise it and the clock would stay blank forever. */
		uint8_t sb = blk[0] & 0x7F, mb = blk[1] & 0x7F, hb = blk[2] & 0x3F;
		uint8_t db = blk[d->date_index] & 0x3F, ob = blk[5] & 0x1F, yb = blk[6];

		bool bcd_ok = bcd_field_ok(sb, 59) && bcd_field_ok(mb, 59) &&
			      bcd_field_ok(hb, 23) && bcd_field_ok(db, 31) &&
			      bcd_field_ok(ob, 12) && bcd_field_ok(yb, 99) &&
			      BCD2BIN(db) >= 1 && BCD2BIN(ob) >= 1;
		bool unreliable = rtc_time_unreliable(d, blk);

		if (!bcd_ok && !unreliable && !garbled) {
			continue;  /* neither valid time nor a lost-power RTC => skip */
		}

		if (s_active == NULL) {
			s_active = d;  /* RTC => our write-back target */
			rv3028_configure(d);
		}

		if (garbled) {
			LOG_WRN("%s present, time unreadable — clock will be set on the "
				"next GPS/app/CLI sync", d->name);
			continue;
		}
		if (unreliable) {
			LOG_WRN("%s present, power-loss flag set — clock will be set "
				"on the next GPS/app/CLI sync", d->name);
			continue;
		}

		unsigned sec   = BCD2BIN(sb);
		unsigned min   = BCD2BIN(mb);
		unsigned hour  = BCD2BIN(hb);
		unsigned day   = BCD2BIN(db);
		unsigned month = BCD2BIN(ob);
		unsigned year  = 2000 + BCD2BIN(yb);

		if (year < 2025) {
			LOG_INF("%s present, time not yet set (%04u-%02u-%02u)",
				d->name, year, month, day);
			continue;  /* RTC adopted for write-back, but no valid time */
		}

		int64_t e = days_from_civil((int)year, month, day) * 86400LL +
			    hour * 3600 + min * 60 + sec;
		if (epoch_out) {
			*epoch_out = (uint32_t)e;
			LOG_INF("RTC %s: restored %04u-%02u-%02u %02u:%02u:%02u UTC",
				d->name, year, month, day, hour, min, sec);
		}
		return true;
	}
	return false;
}

bool zephcore_rtc_restore(uint32_t *epoch_out)
{
	s_probed = true;
	return rtc_probe(epoch_out);
}

void zephcore_rtc_save(uint32_t epoch)
{
	if (!s_probed) {
		/* Restore wasn't run (unexpected) — discover now. */
		(void)rtc_probe(NULL);
		s_probed = true;
	} else if (s_active == NULL && s_skipped_ff && !s_reprobed) {
		/* A running RTC may have counted off all 0xFF since boot; an
		 * erased EEPROM has not, and is skipped again. */
		s_reprobed = true;
		(void)rtc_probe(NULL);
	}
	if (s_active == NULL) {
		return;
	}

	const struct rtc_desc *d = s_active;
	int y;
	unsigned m, day;
	civil_from_days((int64_t)epoch / 86400, &y, &m, &day);
	unsigned rem  = epoch % 86400;
	unsigned hour = rem / 3600;
	unsigned min  = (rem % 3600) / 60;
	unsigned sec  = rem % 60;
	unsigned dow  = (unsigned)(((epoch / 86400) + 4) % 7);  /* 1970-01-01 = Thu */

	uint8_t blk[7];
	blk[0] = BIN2BCD(sec);
	blk[1] = BIN2BCD(min);
	blk[2] = BIN2BCD(hour);
	/* weekday occupies whichever of index 3/4 the date doesn't. */
	blk[d->date_index] = BIN2BCD(day);
	blk[d->date_index == 4 ? 3 : 4] = (uint8_t)dow;
	blk[5] = BIN2BCD(m);
	blk[6] = BIN2BCD((unsigned)(y % 100));

	if (i2c_burst_write(d->bus, d->addr, d->time_reg, blk, sizeof(blk)) != 0) {
		LOG_WRN("RTC %s: time write failed", d->name);
		return;
	}

	/* Clear the power-loss flag (for chips whose flag is a separate reg;
	 * the seconds-bit chips clear it implicitly when we wrote sec above). */
	if (d->status_reg != RTC_STATUS_IN_SECONDS) {
		uint8_t st;
		if (i2c_reg_read_byte(d->bus, d->addr, d->status_reg, &st) == 0) {
			(void)i2c_reg_write_byte(d->bus, d->addr, d->status_reg,
						 st & (uint8_t)~d->status_mask);
		}
	}
	LOG_DBG("RTC %s: persisted time", d->name);
}

#else  /* no zephcore,rtc-i2c node in DT — link-compatible stubs */

bool zephcore_rtc_restore(uint32_t *epoch_out)
{
	ARG_UNUSED(epoch_out);
	return false;
}

void zephcore_rtc_save(uint32_t epoch)
{
	ARG_UNUSED(epoch);
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(zephcore_rtc_i2c) */
