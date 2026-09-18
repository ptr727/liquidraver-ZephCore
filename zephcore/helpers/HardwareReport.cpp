/*
 * SPDX-License-Identifier: MIT
 * `hw` hardware report. See HardwareReport.h.
 */

#include "HardwareReport.h"

#include "CommonCLI.h"
#include "boot_info.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/version.h>

#include <mesh/Board.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../adapters/clock/ZephyrRTCDiscover.h"
}

#if __has_include("../adapters/sensors/ZephyrEnvSensors.h")
#include "../adapters/sensors/ZephyrEnvSensors.h"
#define HW_HAS_SENSOR_HDR 1
#endif

#if __has_include("../adapters/gps/ZephyrGPSManager.h")
#include "../adapters/gps/ZephyrGPSManager.h"
#define HW_HAS_GPS_HDR 1
#endif

namespace zephcore_hw {
namespace {

/* Room reserved for the " next:NNNNN" resume marker, so a truncated page can
 * always say where to resume. Without the reservation the marker is exactly
 * what gets cut off, leaving an unresumable page. */
#define HW_NEXT_RESERVE 12

/*
 * Paged line sink.
 *
 * Every section writes through this, so `hw all` and a single subcommand page
 * identically and there is one place that can overflow. Line indices are
 * unsigned end to end: a resume index parsed as unsigned and then rendered
 * signed is how a page emits "next:-1" and becomes unresumable.
 */
struct Sink {
	char *buf;
	size_t cap;     /* usable capacity, already excluding HW_NEXT_RESERVE */
	size_t len;
	unsigned first; /* first line index to emit */
	unsigned line;  /* index of the line being considered */
	unsigned next;  /* line index that did not fit */
	bool full;
};

void sink_init(Sink *s, char *buf, size_t cap, unsigned first)
{
	s->buf = buf;
	s->cap = (cap > HW_NEXT_RESERVE) ? cap - HW_NEXT_RESERVE : 1;
	s->len = 0;
	s->first = first;
	s->line = 0;
	s->next = 0;
	s->full = false;
	buf[0] = '\0';
}

void sink_line(Sink *s, const char *fmt, ...)
{
	unsigned idx = s->line++;

	if (s->full || idx < s->first) {
		return;
	}

	char line[160];
	va_list ap;
	va_start(ap, fmt);
	int w = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (w < 0) {
		return;
	}

	size_t need = strlen(line) + (s->len ? 1 : 0);
	if (s->len + need >= s->cap) {
		s->full = true;
		s->next = idx;
		return;
	}

	if (s->len) {
		s->buf[s->len++] = '\n';
	}
	memcpy(s->buf + s->len, line, strlen(line));
	s->len += strlen(line);
	s->buf[s->len] = '\0';
}

/* Append the resume marker if the page was cut short. */
void sink_finish(Sink *s)
{
	if (!s->full) {
		return;
	}
	/* cap excluded HW_NEXT_RESERVE, so this always fits. */
	snprintf(s->buf + s->len, HW_NEXT_RESERVE, " next:%u", s->next);
}

/* ================= devicetree I2C inventory =================
 *
 * Declared children only -- no bus traffic. Deliberately does not try to
 * resolve a struct device for each node: DEVICE_DT_GET_OR_NULL still expands
 * to DEVICE_DT_GET for any status-okay node, so a node with no driver bound
 * (every zephcore,rtc-i2c descriptor is exactly that -- data-only, probed by
 * raw I2C) would fail to link. Binding is reported by the adapters that
 * actually know it: `hw rtc` and `hw sensors`.
 */

struct I2cDecl {
	const char *bus;
	const char *node;
	const char *compat;
	uint16_t addr;
};

#define HW_I2C_ENTRY(node_id)                                    \
	{                                                        \
		DT_NODE_FULL_NAME(DT_BUS(node_id)),              \
		DT_NODE_FULL_NAME(node_id),                      \
		DT_PROP_BY_IDX(node_id, compatible, 0),          \
		(uint16_t)DT_REG_ADDR(node_id),                  \
	},

/*
 * Every enabled node that sits on an I2C bus and is an addressable chip.
 *
 * Walks the whole devicetree via DT_FOREACH_STATUS_OKAY_NODE and filters with
 * DT_ON_BUS rather than naming bus nodelabels: this tree already has boards on
 * i2c22 and i2c30 (nrf54l, seeed_lr2021_evk), so any hardcoded i2c0/1/2 list
 * silently omits their devices. Silently omitting a declared chip is the one
 * thing this report must never do.
 *
 * Nodes without both reg and compatible are skipped -- not addressable chips.
 */
#define HW_I2C_NODE(node_id)                                              \
	IF_ENABLED(UTIL_AND(DT_ON_BUS(node_id, i2c),                      \
		   UTIL_AND(DT_NODE_HAS_PROP(node_id, reg),                \
			    DT_NODE_HAS_PROP(node_id, compatible))),      \
		   (HW_I2C_ENTRY(node_id)))

const I2cDecl i2c_decls[] = {
	DT_FOREACH_STATUS_OKAY_NODE(HW_I2C_NODE)
	/* Sentinel. A board may declare no I2C children at all, and a
	 * zero-length array is not valid C++. Never reported -- see
	 * i2c_decl_count(). */
	{ nullptr, nullptr, nullptr, 0xFFFF },
};

constexpr size_t i2c_decl_count() { return ARRAY_SIZE(i2c_decls) - 1; }

#if IS_ENABLED(CONFIG_I2C)
/*
 * The controller behind each declared device, for the live scan. Derived from
 * the same devicetree walk, so it follows whatever nodelabels a board uses
 * rather than a hardcoded i2c0/1/2 list. Entries repeat once per child and are
 * de-duplicated at scan time.
 *
 * DEVICE_DT_GET is safe here in a way it is not for the child nodes: the parent
 * of an I2C child is an I2C controller, and this table only exists when
 * CONFIG_I2C is on, so a driver is instantiated for it.
 *
 * Limitation, stated rather than hidden: a bus with no declared device at all
 * is not scanned, because nothing in the devicetree walk names it.
 */
#define HW_I2C_BUS_ENTRY(node_id) DEVICE_DT_GET(DT_BUS(node_id)),

#define HW_I2C_BUS_NODE(node_id)                                          \
	IF_ENABLED(UTIL_AND(DT_ON_BUS(node_id, i2c),                      \
		   UTIL_AND(DT_NODE_HAS_PROP(node_id, reg),                \
			    DT_NODE_HAS_PROP(node_id, compatible))),      \
		   (HW_I2C_BUS_ENTRY(node_id)))

const struct device *const i2c_bus_refs[] = {
	DT_FOREACH_STATUS_OKAY_NODE(HW_I2C_BUS_NODE)
	nullptr, /* sentinel, same reason as i2c_decls */
};

constexpr size_t i2c_bus_ref_count() { return ARRAY_SIZE(i2c_bus_refs) - 1; }
#endif /* CONFIG_I2C */

/* Name a scanned address from the devicetree, when the board declared it. */
const char *declared_name_at(const char *bus, uint16_t addr)
{
	for (size_t i = 0; i < i2c_decl_count(); i++) {
		if (i2c_decls[i].addr == addr &&
		    strcmp(i2c_decls[i].bus, bus) == 0) {
			return i2c_decls[i].compat;
		}
	}
	return nullptr;
}

const char *rtc_state_str(enum zephcore_rtc_state st)
{
	switch (st) {
	case ZEPHCORE_RTC_PRESENT:  return "present";
	case ZEPHCORE_RTC_ABSENT:   return "absent";
	default:                    return "unprobed";
	}
}

/* ================= sections ================= */

void section_board(Sink *s, mesh::MainBoard *board, CommonCLICallbacks *cb)
{
	sink_line(s, "board: %s", CONFIG_BOARD_TARGET);
	if (board != nullptr) {
		sink_line(s, "name: %s", board->getManufacturerName());
	}
	sink_line(s, "soc: %s", CONFIG_SOC);
	sink_line(s, "zephyr: %s", KERNEL_VERSION_STRING);

	if (cb != nullptr) {
		sink_line(s, "fw: %s (%s)", cb->getFirmwareVer(), cb->getBuildDate());
		sink_line(s, "role: %s", cb->getRole());
	}

	char bl[48];
	if (board != nullptr && board->getBootloaderVersion(bl, sizeof(bl))) {
		sink_line(s, "bootloader: %s", bl);
	}

	uint32_t cause;
	if (zephcore_boot_reset_cause(&cause)) {
		char causes[96];
		zephcore_boot_reset_cause_str(causes, sizeof(causes));
		/* A cause of 0 is a real answer -- the chip reported no known
		 * cause -- and is not the same as the platform being unable to
		 * tell us, which is the branch below. */
		sink_line(s, "reset: 0x%08x%s", cause,
			  causes[0] ? causes : " (none reported)");
	} else {
		sink_line(s, "reset: not supported");
	}

	uint8_t devid[16];
	ssize_t n = hwinfo_get_device_id(devid, sizeof(devid));
	if (n > 0) {
		char hex[sizeof(devid) * 2 + 1];
		for (ssize_t i = 0; i < n && (size_t)i < sizeof(devid); i++) {
			snprintf(hex + i * 2, 3, "%02x", devid[i]);
		}
		sink_line(s, "devid: %s", hex);
	} else {
		sink_line(s, "devid: not supported");
	}
}

void section_rtc(Sink *s, mesh::RTCClock *rtc)
{
	(void)rtc;

	size_t declared = zephcore_rtc_declared();

	if (declared == 0) {
		/* Not a failure: rtc-i2c.dtsi is opt-in per board, because its
		 * fixed addresses collide with common parts (0x68 is both a
		 * DS3231 and an MPU-class IMU). Say what is true -- the board
		 * declares none -- rather than "no RTC fitted", which this
		 * firmware has no way to know. */
		sink_line(s, "rtc: none declared in devicetree");
		return;
	}

	if (!zephcore_rtc_probed()) {
		sink_line(s, "rtc: %u declared, not yet probed", (unsigned)declared);
		return;
	}

	struct zephcore_rtc_entry active;
	if (zephcore_rtc_active(&active)) {
		sink_line(s, "rtc: %s at 0x%02x on %s", active.name,
			  active.addr, active.bus);
	} else {
		sink_line(s, "rtc: none present (%u declared)", (unsigned)declared);
	}

	for (size_t i = 0; i < declared; i++) {
		struct zephcore_rtc_entry e;
		if (!zephcore_rtc_get(i, &e)) {
			continue;
		}
		sink_line(s, "  0x%02x %s %s%s", e.addr, e.name,
			  rtc_state_str(e.state), e.active ? " *" : "");
	}
}

void section_i2c(Sink *s)
{
	sink_line(s, "i2c: %u declared (devicetree, not a bus scan)",
		  (unsigned)i2c_decl_count());
	for (size_t i = 0; i < i2c_decl_count(); i++) {
		sink_line(s, "  %s 0x%02x %s", i2c_decls[i].bus,
			  i2c_decls[i].addr, i2c_decls[i].compat);
	}
}

void section_i2c_scan(Sink *s)
{
#if !IS_ENABLED(CONFIG_I2C)
	/* Different from "scanned and found nothing": this build cannot scan. */
	sink_line(s, "i2c: no I2C support compiled in");
#else
	unsigned buses = 0;

	for (size_t bi = 0; bi < i2c_bus_ref_count(); bi++) {
		const struct device *bus = i2c_bus_refs[bi];

		/* Skip a controller already scanned: the table carries one
		 * entry per declared child, so a bus with four devices on it
		 * appears four times. */
		bool seen = false;
		for (size_t j = 0; j < bi; j++) {
			if (i2c_bus_refs[j] == bus) {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		buses++;

		if (!device_is_ready(bus)) {
			sink_line(s, "%s: not ready", bus->name);
			continue;
		}

		unsigned found = 0;
		char line[128];
		int used = snprintf(line, sizeof(line), "%s:", bus->name);

		/* 0x08-0x77: the 7-bit range excluding the reserved low and
		 * high blocks. A zero-length write is the standard probe -- it
		 * addresses the device and stops, so a chip that would react to
		 * a read of register 0 is not disturbed. */
		for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
			if (i2c_write(bus, nullptr, 0, addr) != 0) {
				continue;
			}
			found++;
			const char *nm = declared_name_at(bus->name, addr);
			int w = snprintf(line + used, sizeof(line) - used,
					 nm ? " 0x%02x(%s)" : " 0x%02x", addr, nm);
			if (w < 0 || (size_t)(used + w) >= sizeof(line)) {
				/* This address did not fit. Flush the line so far,
				 * start a continuation, and re-render the address
				 * into it. Dropping it would silently omit a chip
				 * that is physically present, which is the one
				 * thing a bus scan must never do. */
				line[used] = '\0';
				sink_line(s, "%s", line);
				used = snprintf(line, sizeof(line), "%s:", bus->name);
				w = snprintf(line + used, sizeof(line) - used,
					     nm ? " 0x%02x(%s)" : " 0x%02x", addr, nm);
				if (w > 0 && (size_t)(used + w) < sizeof(line)) {
					used += w;
				}
			} else {
				used += w;
			}
		}

		if (found == 0) {
			sink_line(s, "%s: none found", bus->name);
		} else {
			sink_line(s, "%s", line);
		}
	}

	if (buses == 0) {
		/* Not "no bus enabled": buses are discovered by walking declared
		 * devices, so the SoC may well have an enabled controller with
		 * nothing declared on it. Say only what that walk established. */
		sink_line(s, "i2c: no bus carries a declared device, none scanned");
	}
#endif /* CONFIG_I2C */
}

void section_gps(Sink *s, CommonCLICallbacks *cb)
{
#if DT_NODE_EXISTS(DT_NODELABEL(gnss))
	sink_line(s, "gnss: %s", DT_PROP_BY_IDX(DT_NODELABEL(gnss), compatible, 0));
	sink_line(s, "  on %s", DT_NODE_FULL_NAME(DT_PARENT(DT_NODELABEL(gnss))));
#if DT_NODE_HAS_PROP(DT_PARENT(DT_NODELABEL(gnss)), current_speed)
	sink_line(s, "  baud %u",
		  (unsigned)DT_PROP(DT_PARENT(DT_NODELABEL(gnss)), current_speed));
#endif
	/* The bound driver's compatible, printed above, is the only honest model
	 * statement. A board overlay comment naming a part is documentation, not
	 * detection: the rak4631 overlay documents a u-blox MAX-7Q, binds
	 * gnss-nmea-generic, and drives whatever NMEA receiver is fitted.
	 *
	 * Only the generic NMEA driver leaves the model genuinely unknown -- it
	 * parses any NMEA talker and names no part. A specific driver IS the
	 * model, so adding an "unidentified" line there would contradict the
	 * compatible printed directly above it. */
#if DT_NODE_HAS_COMPAT(DT_NODELABEL(gnss), gnss_nmea_generic)
	sink_line(s, "  model: not identified (generic NMEA driver)");
#endif
#else
	sink_line(s, "gnss: none declared in devicetree");
#endif

#ifdef HW_HAS_GPS_HDR
	sink_line(s, "  available: %s", gps_is_available() ? "yes" : "no");
	sink_line(s, "  enabled: %s", gps_is_enabled() ? "yes" : "no");
#else
	if (cb != nullptr) {
		sink_line(s, "  enabled: %s", cb->isGpsEnabled() ? "yes" : "no");
	}
#endif
	(void)cb;
}

void section_sensors(Sink *s)
{
#ifdef HW_HAS_SENSOR_HDR
	bool env = env_sensors_available();
	bool pwr = power_sensors_available();

	sink_line(s, "sensors: env %s, power %s",
		  env ? "yes" : "no", pwr ? "yes" : "no");

	if (env) {
		struct env_data d;
		if (env_sensors_read(&d) == 0) {
			sink_line(s, "  channels:%s%s%s%s%s",
				  d.has_temperature ? " temp" : "",
				  d.has_humidity ? " hum" : "",
				  d.has_pressure ? " press" : "",
				  d.has_mcu_temperature ? " mcutemp" : "",
				  d.has_luminosity ? " lux" : "");
		}
	}
#else
	/* No sensor support compiled in -- which is a different statement from
	 * a sensor manager that looked and found nothing. */
	sink_line(s, "sensors: not compiled in");
#endif
}

void section_summary(Sink *s, mesh::MainBoard *board, CommonCLICallbacks *cb)
{
	/* CONFIG_BOARD, not CONFIG_BOARD_TARGET: the target already embeds the
	 * SoC ("rak4631/nrf52840"), which read as "rak4631/nrf52840 (nrf52840)".
	 * `hw board` still reports the full target. */
	sink_line(s, "%s (%s)", CONFIG_BOARD, CONFIG_SOC);
	if (cb != nullptr) {
		sink_line(s, "fw %s role %s", cb->getFirmwareVer(), cb->getRole());
	}

	struct zephcore_rtc_entry active;
	if (zephcore_rtc_active(&active)) {
		/* A devicetree node's full name already ends in "@<addr>", so the
		 * address is not appended here -- doing so rendered
		 * "rtc-rv3028@52@0x52" on real hardware. */
		sink_line(s, "rtc %s", active.name);
	} else if (zephcore_rtc_declared() == 0) {
		sink_line(s, "rtc none declared");
	} else {
		sink_line(s, "rtc none present");
	}

#if DT_NODE_EXISTS(DT_NODELABEL(gnss))
	sink_line(s, "gnss %s", DT_PROP_BY_IDX(DT_NODELABEL(gnss), compatible, 0));
#else
	sink_line(s, "gnss none");
#endif

	sink_line(s, "i2c %u declared", (unsigned)i2c_decl_count());

	uint32_t cause;
	if (zephcore_boot_reset_cause(&cause)) {
		char causes[96];
		zephcore_boot_reset_cause_str(causes, sizeof(causes));
		sink_line(s, "reset%s", causes[0] ? causes : " none");
	}
	(void)board;
}

/*
 * Match `name` as a whole word at the start of arg, returning the remainder or
 * nullptr.
 *
 * A bare prefix test is the surrounding CommonCLI house style, but this command
 * documents a usage string for anything it does not recognise, and a prefix
 * test breaks that promise: `hw boardwalk` would run `hw board` and quietly
 * discard the rest.
 */
const char *match_word(const char *arg, const char *name)
{
	size_t n = strlen(name);

	if (strncmp(arg, name, n) != 0) {
		return nullptr;
	}
	if (arg[n] != '\0' && arg[n] != ' ') {
		return nullptr;
	}
	return arg + n;
}

/*
 * Parse the optional trailing page index. False when the tail is neither empty
 * nor a plain number, so `hw board xyz` reaches the usage string rather than
 * being silently treated as `hw board`.
 *
 * Unsigned end to end, and clamped rather than wrapped: an index parsed
 * unsigned and then rendered signed is what emits an unresumable "next:-1".
 */
bool parse_tail(const char *rest, unsigned *start)
{
	*start = 0;

	while (*rest == ' ') {
		rest++;
	}
	if (*rest == '\0') {
		return true;
	}
	if (*rest < '0' || *rest > '9') {
		return false;
	}

	char *end = nullptr;
	unsigned long v = strtoul(rest, &end, 10);

	while (end != nullptr && *end == ' ') {
		end++;
	}
	if (end != nullptr && *end != '\0') {
		return false;
	}

	*start = (v > 9999UL) ? 9999U : (unsigned)v;
	return true;
}

} /* namespace */

void handle(const char *command, char *reply, size_t cap, bool local,
	    mesh::MainBoard *board, mesh::RTCClock *rtc,
	    CommonCLICallbacks *callbacks)
{
	/* command starts at "hw"; step past it and any spaces. */
	const char *arg = command + 2;
	while (*arg == ' ') {
		arg++;
	}

	Sink s;
	const char *rest;
	unsigned start = 0;

	/* Each arm must match a whole word AND carry a valid tail; a branch that
	 * matches the word but not the tail falls through to the usage string. */
	if (*arg == '\0') {
		sink_init(&s, reply, cap, 0);
		section_summary(&s, board, callbacks);
	} else if ((rest = match_word(arg, "board")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_board(&s, board, callbacks);
	} else if ((rest = match_word(arg, "rtc")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_rtc(&s, rtc);
	} else if ((rest = match_word(arg, "i2c scan")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_i2c_scan(&s);
	} else if ((rest = match_word(arg, "i2c")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_i2c(&s);
	} else if ((rest = match_word(arg, "gps")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_gps(&s, callbacks);
	} else if ((rest = match_word(arg, "sensors")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_sensors(&s);
	} else if ((rest = match_word(arg, "all")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_board(&s, board, callbacks);
		section_rtc(&s, rtc);
		section_i2c(&s);
		section_gps(&s, callbacks);
		section_sensors(&s);
	} else {
		snprintf(reply, cap,
			 "usage: hw [board|rtc|i2c [scan]|gps|sensors|all] [start]");
		return;
	}

	sink_finish(&s);
	(void)local;
}

} /* namespace zephcore_hw */
