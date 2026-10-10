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

#include <mesh/MeshCore.h>

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

#define HW_GNSS_NODE DT_NODELABEL(gnss)

/* Candidates declared but compiled out: the discovery stubs report none. */
#define HW_RTC_DISABLED (!IS_ENABLED(CONFIG_ZEPHCORE_RTC_AUTODISCOVER) && \
			 DT_NUM_INST_STATUS_OKAY(zephcore_rtc_i2c) > 0)

namespace zephcore_hw {
namespace {

/* Room kept free for the "\n... next:NNNNN" resume marker. */
#define HW_NEXT_RESERVE 16
/* Largest resume index the marker holds and the parser accepts. */
#define HW_PAGE_INDEX_MAX 99999U

/* Paged line sink: every section writes through it, so all page alike. */
struct Sink {
	char *buf;
	size_t cap;     /* write limit, excluding HW_NEXT_RESERVE */
	size_t len;
	unsigned first; /* first line index to emit */
	unsigned line;  /* index of the line being considered */
	unsigned next;  /* line index that did not fit */
	bool full;
};

void sink_init(Sink *s, char *buf, size_t cap, unsigned first)
{
	s->buf = buf;
	s->cap = cap - HW_NEXT_RESERVE;
	s->len = 0;
	s->first = first;
	s->line = 0;
	s->next = 0;
	s->full = false;
	buf[0] = '\0';
}

/* True if the next line will not be emitted; it is counted as skipped. */
bool sink_skip(Sink *s)
{
	if (s->full || s->line < s->first) {
		s->line++;
		return true;
	}
	return false;
}

void sink_line(Sink *s, const char *fmt, ...)
{
	if (sink_skip(s)) {
		return;
	}
	unsigned idx = s->line++;

	char line[160];
	va_list ap;
	va_start(ap, fmt);
	int w = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (w < 0) {
		return;
	}
	if ((size_t)w >= sizeof(line)) {
		memcpy(line + sizeof(line) - 4, "...", 3);
	}

	size_t need = strlen(line) + (s->len ? 1 : 0);
	if (s->len == 0 && need >= s->cap) {
		/* A line longer than a page is cut, so a resume moves past it. */
		size_t keep = s->cap - 1;
		line[keep] = '\0';
		memcpy(line + keep - 3, "...", 3);
		need = keep;
	}
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

void sink_finish(Sink *s)
{
	if (s->len == 0 && !s->full) {
		/* An empty reply reads as an unknown command on a companion. */
		snprintf(s->buf, s->cap + HW_NEXT_RESERVE, "no line %u, the report has %u",
			 s->first, s->line);
	} else if (s->full) {
		snprintf(s->buf + s->len, HW_NEXT_RESERVE, "\n... next:%u", s->next);
	}
}

/* ================= devicetree I2C inventory ================= */

struct I2cDecl {
	const char *bus;
	const char *node;
	const char *compat;
	uint16_t addr;
};

/* The bus is named as Zephyr names the device (DEVICE_DT_NAME), so it matches
 * the dev->name the scan reports. */
#define HW_I2C_ENTRY(node_id)                                    \
	{                                                        \
		DEVICE_DT_NAME(DT_BUS(node_id)),                 \
		DT_NODE_FULL_NAME(node_id),                      \
		DT_PROP_BY_IDX(node_id, compatible, 0),          \
		(uint16_t)DT_REG_ADDR(node_id),                  \
	},

/* Every enabled, addressable node on any I2C bus. */
#define HW_I2C_NODE(node_id)                                              \
	IF_ENABLED(UTIL_AND(DT_ON_BUS(node_id, i2c),                      \
		   UTIL_AND(DT_NODE_HAS_PROP(node_id, reg),                \
			    DT_NODE_HAS_PROP(node_id, compatible))),      \
		   (HW_I2C_ENTRY(node_id)))

const I2cDecl i2c_decls[] = {
	DT_FOREACH_STATUS_OKAY_NODE(HW_I2C_NODE)
	{ nullptr, nullptr, nullptr, 0xFFFF }, /* sentinel: never reported */
};

constexpr size_t i2c_decl_count() { return ARRAY_SIZE(i2c_decls) - 1; }

#if IS_ENABLED(CONFIG_I2C)
/* The controller of each declared device, one entry per device. A bus with no
 * declared device is not listed, so it is not scanned. */
#define HW_I2C_BUS_ENTRY(node_id) DEVICE_DT_GET(DT_BUS(node_id)),

#define HW_I2C_BUS_NODE(node_id)                                          \
	IF_ENABLED(UTIL_AND(DT_ON_BUS(node_id, i2c),                      \
		   UTIL_AND(DT_NODE_HAS_PROP(node_id, reg),                \
			    DT_NODE_HAS_PROP(node_id, compatible))),      \
		   (HW_I2C_BUS_ENTRY(node_id)))

const struct device *const i2c_bus_refs[] = {
	DT_FOREACH_STATUS_OKAY_NODE(HW_I2C_BUS_NODE)
	nullptr, /* sentinel */
};

constexpr size_t i2c_bus_ref_count() { return ARRAY_SIZE(i2c_bus_refs) - 1; }

/* The compatibles declared at addr on bus, '|'-separated. False if none, or
 * if they do not fit. */
bool declared_names_at(const char *bus, uint16_t addr, char *out, size_t cap)
{
	size_t used = 0;

	out[0] = '\0';
	for (size_t i = 0; i < i2c_decl_count(); i++) {
		if (i2c_decls[i].addr != addr || strcmp(i2c_decls[i].bus, bus) != 0) {
			continue;
		}
		int w = snprintf(out + used, cap - used, "%s%s", used ? "|" : "",
				 i2c_decls[i].compat);
		if (w < 0 || (size_t)w >= cap - used) {
			out[0] = '\0';
			return false;
		}
		used += (size_t)w;
	}
	return used > 0;
}
#endif /* CONFIG_I2C */

/* One snapshot of discovery's outcome, so every line agrees with the others. */
constexpr size_t kRtcMax = DT_NUM_INST_STATUS_OKAY(zephcore_rtc_i2c) > 0
				   ? DT_NUM_INST_STATUS_OKAY(zephcore_rtc_i2c) : 1;

struct RtcView {
	struct zephcore_rtc_entry e[kRtcMax];
	size_t n;
	int active;         /* index into e, or -1 */
	unsigned unsettled; /* unprobed or all 0xFF */
};

void rtc_view(RtcView *v)
{
	v->n = zephcore_rtc_snapshot(v->e, kRtcMax);
	v->active = -1;
	v->unsettled = 0;
	for (size_t i = 0; i < v->n; i++) {
		if (v->e[i].active) {
			v->active = (int)i;
		}
		if (v->e[i].state == ZEPHCORE_RTC_UNPROBED ||
		    v->e[i].state == ZEPHCORE_RTC_ALL_FF) {
			v->unsettled++;
		}
	}
}

const char *rtc_state_str(enum zephcore_rtc_state st)
{
	switch (st) {
	case ZEPHCORE_RTC_PRESENT:  return "present";
	case ZEPHCORE_RTC_ABSENT:   return "absent";
	case ZEPHCORE_RTC_ALL_FF:   return "all 0xff";
	default:                    return "unprobed";
	}
}

/* The boot reset cause labels, without hints. False if the platform cannot
 * report a cause. */
bool reset_causes(uint32_t *cause, char *out, size_t cap)
{
	if (!zephcore_boot_reset_cause(cause)) {
		return false;
	}
	zephcore_boot_reset_cause_str(out, cap, false);
	return true;
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
	char causes[128]; /* every label without hints, per boot_info.h */
	if (reset_causes(&cause, causes, sizeof(causes))) {
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

void section_rtc(Sink *s)
{
	if (HW_RTC_DISABLED) {
		sink_line(s, "rtc: %u declared, autodiscovery disabled",
			  (unsigned)DT_NUM_INST_STATUS_OKAY(zephcore_rtc_i2c));
		return;
	}

	size_t declared = zephcore_rtc_declared();

	if (declared == 0) {
		sink_line(s, "rtc: none declared in devicetree");
		return;
	}
	if (!zephcore_rtc_probed()) {
		sink_line(s, "rtc: %u declared, not yet probed", (unsigned)declared);
		return;
	}

	RtcView v;

	rtc_view(&v);
	if (v.active >= 0) {
		const struct zephcore_rtc_entry *a = &v.e[v.active];

		sink_line(s, "rtc: %s at 0x%02x on %s", a->name, a->addr, a->bus);
	} else if (v.unsettled > 0) {
		sink_line(s, "rtc: none found, %u of %u declared unsettled",
			  v.unsettled, (unsigned)declared);
	} else {
		sink_line(s, "rtc: none present (%u declared)", (unsigned)declared);
	}

	for (size_t i = 0; i < v.n; i++) {
		const struct zephcore_rtc_entry *e = &v.e[i];

		sink_line(s, "  0x%02x %s %s%s", e->addr, e->name,
			  rtc_state_str(e->state), e->active ? " *" : "");
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
	sink_line(s, "i2c: no I2C support compiled in");
#else
	unsigned buses = 0;

	for (size_t bi = 0; bi < i2c_bus_ref_count(); bi++) {
		const struct device *bus = i2c_bus_refs[bi];

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
		bool stuck = false;
		char line[128];
		const int header = snprintf(line, sizeof(line), "%s:", bus->name);
		int used = header;

		/* A one-byte read probes each address: nothing is written to a
		 * device that has not been identified. */
		for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
			uint8_t probe;
			int rc = i2c_read(bus, &probe, 1, addr);

			if (rc == -ETIMEDOUT || rc == -EBUSY) {
				/* A dead or held bus would take the driver's timeout
				 * on every address. */
				if (used > header) {
					sink_line(s, "%s", line);
					used = header;
				}
				sink_line(s, "%s: bus not responding at 0x%02x (err %d), scan stopped",
					  bus->name, addr, rc);
				stuck = true;
				break;
			}
			if (rc != 0) {
				continue;
			}
			found++;

			char nm[64];
			char tok[72];
			int tw = declared_names_at(bus->name, addr, nm, sizeof(nm))
				 ? snprintf(tok, sizeof(tok), " 0x%02x(%s)", addr, nm)
				 : snprintf(tok, sizeof(tok), " 0x%02x", addr);

			if ((size_t)(used + tw) >= sizeof(line) && used > header) {
				sink_line(s, "%s", line);
				used = header;
			}
			memcpy(line + used, tok, (size_t)tw + 1);
			used += tw;
		}

		if (found == 0) {
			if (!stuck) {
				sink_line(s, "%s: none found", bus->name);
			}
		} else if (used > header) {
			sink_line(s, "%s", line);
		}
	}

	if (buses == 0) {
		sink_line(s, "i2c: no bus carries a declared device, none scanned");
	}
#endif /* CONFIG_I2C */
}

void section_gps(Sink *s)
{
#if DT_NODE_EXISTS(HW_GNSS_NODE) && !DT_NODE_HAS_STATUS(HW_GNSS_NODE, okay)
	sink_line(s, "gnss: %s declared but disabled in devicetree",
		  DT_PROP_BY_IDX(HW_GNSS_NODE, compatible, 0));
#elif DT_NODE_EXISTS(HW_GNSS_NODE)
	sink_line(s, "gnss: %s", DT_PROP_BY_IDX(HW_GNSS_NODE, compatible, 0));
	sink_line(s, "  on %s", DT_NODE_FULL_NAME(DT_PARENT(HW_GNSS_NODE)));
#if DT_NODE_HAS_PROP(DT_PARENT(HW_GNSS_NODE), current_speed)
	sink_line(s, "  baud %u", (unsigned)DT_PROP(DT_PARENT(HW_GNSS_NODE), current_speed));
#endif
	/* The generic NMEA driver drives any receiver and names no part. */
#if DT_NODE_HAS_COMPAT(HW_GNSS_NODE, gnss_nmea_generic)
	sink_line(s, "  model: not identified (generic NMEA driver)");
#endif
#else
	sink_line(s, "gnss: none declared in devicetree");
#endif

#ifdef HW_HAS_GPS_HDR
	sink_line(s, "  available: %s", gps_is_available() ? "yes" : "no");
	sink_line(s, "  enabled: %s", gps_is_enabled() ? "yes" : "no");
#endif
}

void section_sensors(Sink *s)
{
#if defined(HW_HAS_SENSOR_HDR) && IS_ENABLED(CONFIG_SENSOR)
	int n = env_sensor_count();

	if (n <= 0) {
		sink_line(s, "sensors: none found");
		return;
	}
	sink_line(s, "sensors: %d found", n);

	for (int i = 0; i < n; i++) {
		struct env_sensor_reading r;

		/* Read only a sensor whose line this page emits. */
		if (sink_skip(s)) {
			continue;
		}
		if (env_sensor_read(i, &r) != 0) {
			sink_line(s, "  %d: read failed", i);
			continue;
		}
		sink_line(s, "  %d:%s%s%s%s%s%s%s%s%s", i,
			  (r.fields & ENV_F_TEMPERATURE) ? " temp" : "",
			  (r.fields & ENV_F_HUMIDITY) ? " hum" : "",
			  (r.fields & ENV_F_PRESSURE) ? " press" : "",
			  (r.fields & ENV_F_ALTITUDE) ? " alt" : "",
			  (r.fields & ENV_F_LUMINOSITY) ? " light" : "",
			  (r.fields & ENV_F_VOLTAGE) ? " volt" : "",
			  (r.fields & ENV_F_CURRENT) ? " curr" : "",
			  (r.fields & ENV_F_POWER) ? " power" : "",
			  env_sensor_is_board_local(i) ? " (board)" : "");
	}
#else
	sink_line(s, "sensors: not compiled in");
#endif
}

void section_summary(Sink *s, CommonCLICallbacks *cb)
{
	sink_line(s, "%s (%s)", CONFIG_BOARD, CONFIG_SOC);
	if (cb != nullptr) {
		sink_line(s, "fw %s role %s", cb->getFirmwareVer(), cb->getRole());
	}

	RtcView v;

	rtc_view(&v);
	if (v.active >= 0) {
		/* The node's full name already ends in "@<addr>". */
		sink_line(s, "rtc %s", v.e[v.active].name);
	} else if (HW_RTC_DISABLED) {
		sink_line(s, "rtc autodiscovery disabled");
	} else if (zephcore_rtc_declared() == 0) {
		sink_line(s, "rtc none declared");
	} else if (!zephcore_rtc_probed()) {
		sink_line(s, "rtc not yet probed");
	} else if (v.unsettled > 0) {
		sink_line(s, "rtc none found, %u unsettled", v.unsettled);
	} else {
		sink_line(s, "rtc none present");
	}

#if DT_NODE_EXISTS(HW_GNSS_NODE) && !DT_NODE_HAS_STATUS(HW_GNSS_NODE, okay)
	sink_line(s, "gnss %s disabled", DT_PROP_BY_IDX(HW_GNSS_NODE, compatible, 0));
#elif DT_NODE_EXISTS(HW_GNSS_NODE)
	sink_line(s, "gnss %s", DT_PROP_BY_IDX(HW_GNSS_NODE, compatible, 0));
#else
	sink_line(s, "gnss none");
#endif

	sink_line(s, "i2c %u declared", (unsigned)i2c_decl_count());

	uint32_t cause;
	char causes[128];
	if (reset_causes(&cause, causes, sizeof(causes))) {
		sink_line(s, "reset%s", causes[0] ? causes : " none");
	} else {
		sink_line(s, "reset not supported");
	}
}

/* The rest of arg after `name` as a whole word, or nullptr. */
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

/* Parse the optional trailing page index. False unless the tail is empty or
 * a plain number. */
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

	*start = (v > HW_PAGE_INDEX_MAX) ? HW_PAGE_INDEX_MAX : (unsigned)v;
	return true;
}

} /* namespace */

void handle(const char *command, char *reply, size_t cap,
	    mesh::MainBoard *board, CommonCLICallbacks *callbacks)
{
	const char *arg = command + 2; /* past "hw" */
	while (*arg == ' ') {
		arg++;
	}

	Sink s;
	const char *rest;
	unsigned start = 0;

	if (*arg == '\0' || ((*arg >= '0' && *arg <= '9') && parse_tail(arg, &start))) {
		sink_init(&s, reply, cap, start);
		section_summary(&s, callbacks);
	} else if ((rest = match_word(arg, "board")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_board(&s, board, callbacks);
	} else if ((rest = match_word(arg, "rtc")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_rtc(&s);
	} else if ((rest = match_word(arg, "i2c scan")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_i2c_scan(&s);
	} else if ((rest = match_word(arg, "i2c")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_i2c(&s);
	} else if ((rest = match_word(arg, "gps")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_gps(&s);
	} else if ((rest = match_word(arg, "sensors")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_sensors(&s);
	} else if ((rest = match_word(arg, "all")) && parse_tail(rest, &start)) {
		sink_init(&s, reply, cap, start);
		section_board(&s, board, callbacks);
		section_rtc(&s);
		section_i2c(&s);
		section_gps(&s);
		section_sensors(&s);
	} else {
		snprintf(reply, cap,
			 "usage: hw [board|rtc|i2c [scan]|gps|sensors|all] [start]");
		return;
	}

	sink_finish(&s);
}

} /* namespace zephcore_hw */
