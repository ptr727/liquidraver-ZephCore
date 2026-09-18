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
		DT_NODE_FULL_NAME(DT_PARENT(node_id)),           \
		DT_NODE_FULL_NAME(node_id),                      \
		DT_PROP_BY_IDX(node_id, compatible, 0),          \
		(uint16_t)DT_REG_ADDR(node_id),                  \
	},

/* Skip any child without both reg and compatible -- it is not an addressable
 * chip and has nothing to report. */
#define HW_I2C_CHILD(node_id)                                             \
	IF_ENABLED(UTIL_AND(DT_NODE_HAS_PROP(node_id, reg),               \
			    DT_NODE_HAS_PROP(node_id, compatible)),       \
		   (HW_I2C_ENTRY(node_id)))

const I2cDecl i2c_decls[] = {
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c0), okay)
	DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(i2c0), HW_I2C_CHILD)
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay)
	DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(i2c1), HW_I2C_CHILD)
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c2), okay)
	DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(i2c2), HW_I2C_CHILD)
#endif
	/* Sentinel. A board may declare no I2C children at all, and a
	 * zero-length array is not valid C++. Never reported -- see
	 * i2c_decl_count(). */
	{ nullptr, nullptr, nullptr, 0xFFFF },
};

constexpr size_t i2c_decl_count() { return ARRAY_SIZE(i2c_decls) - 1; }

/*
 * Bus handles, for the live scan only.
 *
 * Gated on CONFIG_I2C as well as node status: a devicetree node can be
 * status-okay while no I2C driver is compiled in (native_sim is exactly that),
 * and DEVICE_DT_GET on such a node references a device object nothing ever
 * defines, so the image fails to LINK rather than reporting a missing bus at
 * runtime. The declared inventory above deliberately takes no device handles
 * for the same reason.
 */
#if IS_ENABLED(CONFIG_I2C)
const struct device *const i2c_buses[] = {
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c0), okay)
	DEVICE_DT_GET(DT_NODELABEL(i2c0)),
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay)
	DEVICE_DT_GET(DT_NODELABEL(i2c1)),
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c2), okay)
	DEVICE_DT_GET(DT_NODELABEL(i2c2)),
#endif
	nullptr, /* sentinel, same reason as i2c_decls */
};

constexpr size_t i2c_bus_count() { return ARRAY_SIZE(i2c_buses) - 1; }
#else
constexpr size_t i2c_bus_count() { return 0; }
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
	if (i2c_bus_count() == 0) {
		sink_line(s, "i2c: no bus enabled");
		return;
	}

	for (size_t b = 0; b < i2c_bus_count(); b++) {
		const struct device *bus = i2c_buses[b];

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
	/* The driver's own compatible is the only honest model statement. A
	 * board overlay comment naming a part is documentation, not detection:
	 * the rak4631 overlay documents a u-blox MAX-7Q, binds
	 * gnss-nmea-generic, and drives whatever NMEA receiver is fitted. */
	sink_line(s, "  model: not identified (generic NMEA)");
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
	sink_line(s, "%s (%s)", CONFIG_BOARD_TARGET, CONFIG_SOC);
	if (cb != nullptr) {
		sink_line(s, "fw %s role %s", cb->getFirmwareVer(), cb->getRole());
	}

	struct zephcore_rtc_entry active;
	if (zephcore_rtc_active(&active)) {
		sink_line(s, "rtc %s@0x%02x", active.name, active.addr);
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

/* Parse an optional trailing page index. Unsigned throughout, and a value
 * beyond any plausible line count is clamped rather than wrapped. */
unsigned parse_start(const char *arg)
{
	if (arg == nullptr) {
		return 0;
	}
	while (*arg == ' ') {
		arg++;
	}
	if (*arg < '0' || *arg > '9') {
		return 0;
	}
	unsigned long v = strtoul(arg, nullptr, 10);
	return (v > 9999U) ? 9999U : (unsigned)v;
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

	if (*arg == '\0') {
		sink_init(&s, reply, cap, 0);
		section_summary(&s, board, callbacks);
	} else if (memcmp(arg, "board", 5) == 0) {
		sink_init(&s, reply, cap, parse_start(arg + 5));
		section_board(&s, board, callbacks);
	} else if (memcmp(arg, "rtc", 3) == 0) {
		sink_init(&s, reply, cap, parse_start(arg + 3));
		section_rtc(&s, rtc);
	} else if (memcmp(arg, "i2c scan", 8) == 0) {
		sink_init(&s, reply, cap, parse_start(arg + 8));
		section_i2c_scan(&s);
	} else if (memcmp(arg, "i2c", 3) == 0) {
		sink_init(&s, reply, cap, parse_start(arg + 3));
		section_i2c(&s);
	} else if (memcmp(arg, "gps", 3) == 0) {
		sink_init(&s, reply, cap, parse_start(arg + 3));
		section_gps(&s, callbacks);
	} else if (memcmp(arg, "sensors", 7) == 0) {
		sink_init(&s, reply, cap, parse_start(arg + 7));
		section_sensors(&s);
	} else if (memcmp(arg, "all", 3) == 0) {
		sink_init(&s, reply, cap, parse_start(arg + 3));
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
