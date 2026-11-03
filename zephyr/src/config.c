/*
 * JSON configuration parser and validator (CFG-1).
 *
 * Schema: SPEC section 6. Unknown members are rejected so that typos are
 * reported instead of silently ignored. Every error message starts with the
 * JSON path of the offending value.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gw_config.h"
#include "json_reader.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

#define KEY_MAX 24

struct parser {
	struct jr jr;
	struct gw_config *cfg;
	char path[64];
	size_t path_len;
	char *err;
	size_t err_size;
};

/* ---- error and path helpers -------------------------------------------- */

static int fail(struct parser *p, const char *key, const char *fmt, ...)
{
	char msg[96];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (key != NULL) {
		snprintf(p->err, p->err_size, "%s%s%s: %s", p->path, p->path_len ? "." : "", key,
			 msg);
	} else {
		snprintf(p->err, p->err_size, "%s: %s", p->path_len ? p->path : "(root)", msg);
	}
	return -EINVAL;
}

/* Report a syntax error detected by the reader. */
static int fail_syntax(struct parser *p)
{
	return fail(p, NULL, "%s", p->jr.err);
}

static size_t path_push(struct parser *p, const char *fmt, ...)
{
	size_t saved = p->path_len;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(&p->path[p->path_len], sizeof(p->path) - p->path_len, fmt, ap);
	va_end(ap);
	if (n > 0) {
		p->path_len = MIN(p->path_len + (size_t)n, sizeof(p->path) - 1);
	}
	return saved;
}

static void path_pop(struct parser *p, size_t saved)
{
	p->path_len = saved;
	p->path[saved] = '\0';
}

/* ---- typed readers ----------------------------------------------------- */

static int expect_kind(struct parser *p, const char *key, enum jr_kind want)
{
	enum jr_kind got = jr_peek(&p->jr);

	if (got != want) {
		return fail(p, key, "expected %s, got %s", jr_kind_name(want), jr_kind_name(got));
	}
	return 0;
}

static int read_str(struct parser *p, const char *key, char *out, size_t size)
{
	if (expect_kind(p, key, JR_STRING) < 0) {
		return -EINVAL;
	}
	if (jr_string(&p->jr, out, size) < 0) {
		return fail(p, key, "%s", p->jr.err);
	}
	return 0;
}

static int read_double(struct parser *p, const char *key, double *out)
{
	if (expect_kind(p, key, JR_NUMBER) < 0) {
		return -EINVAL;
	}
	if (jr_number(&p->jr, out) < 0) {
		return fail(p, key, "%s", p->jr.err);
	}
	if (!isfinite(*out)) {
		return fail(p, key, "number out of range");
	}
	return 0;
}

static int read_uint(struct parser *p, const char *key, uint32_t min, uint32_t max,
		     uint32_t *out)
{
	double v;

	if (read_double(p, key, &v) < 0) {
		return -EINVAL;
	}
	if (v != floor(v)) {
		return fail(p, key, "expected an integer");
	}
	if (v < (double)min || v > (double)max) {
		return fail(p, key, "must be between %u and %u", (unsigned int)min,
			    (unsigned int)max);
	}
	*out = (uint32_t)v;
	return 0;
}

static int read_u8(struct parser *p, const char *key, uint32_t min, uint32_t max, uint8_t *out)
{
	uint32_t v;
	int ret = read_uint(p, key, min, max, &v);

	*out = (uint8_t)v;
	return ret;
}

static int read_u16(struct parser *p, const char *key, uint32_t min, uint32_t max,
		    uint16_t *out)
{
	uint32_t v;
	int ret = read_uint(p, key, min, max, &v);

	*out = (uint16_t)v;
	return ret;
}

static int read_bool(struct parser *p, const char *key, bool *out)
{
	if (expect_kind(p, key, JR_BOOL) < 0) {
		return -EINVAL;
	}
	if (jr_bool(&p->jr, out) < 0) {
		return fail(p, key, "%s", p->jr.err);
	}
	return 0;
}

static int read_enum(struct parser *p, const char *key, const char *const *names, size_t count,
		     uint8_t *out)
{
	char buf[KEY_MAX];

	if (read_str(p, key, buf, sizeof(buf)) < 0) {
		return -EINVAL;
	}
	for (size_t i = 0; i < count; i++) {
		if (strcmp(buf, names[i]) == 0) {
			*out = (uint8_t)i;
			return 0;
		}
	}
	return fail(p, key, "invalid value '%s'", buf);
}

static int parse_ipv4(const char *s, uint8_t out[4])
{
	unsigned int part = 0;
	int digits = 0;
	int idx = 0;

	for (;; s++) {
		if (*s >= '0' && *s <= '9') {
			part = part * 10U + (unsigned int)(*s - '0');
			if (++digits > 3 || part > 255U) {
				return -EINVAL;
			}
		} else if ((*s == '.' || *s == '\0') && digits > 0 && idx < 4) {
			out[idx++] = (uint8_t)part;
			part = 0;
			digits = 0;
			if (*s == '\0') {
				break;
			}
		} else {
			return -EINVAL;
		}
	}
	return idx == 4 ? 0 : -EINVAL;
}

static int read_ipv4(struct parser *p, const char *key, uint8_t out[4])
{
	char buf[16];

	if (read_str(p, key, buf, sizeof(buf)) < 0) {
		return -EINVAL;
	}
	if (parse_ipv4(buf, out) < 0) {
		return fail(p, key, "invalid IPv4 address '%s'", buf);
	}
	return 0;
}

/* Sparkplug identifiers become MQTT topic levels (Sparkplug 3.0, 4.1). */
static int check_spb_id(struct parser *p, const char *key, const char *id, bool required)
{
	if (id[0] == '\0') {
		return required ? fail(p, key, "must not be empty") : 0;
	}
	if (strpbrk(id, "/+#") != NULL) {
		return fail(p, key, "must not contain '/', '+' or '#'");
	}
	return 0;
}

static int unknown(struct parser *p, const char *key)
{
	return fail(p, key, "unknown member");
}

static int next_member(struct parser *p, char *key)
{
	int ret = jr_object_next(&p->jr, key, KEY_MAX);

	if (ret < 0) {
		return fail_syntax(p);
	}
	return ret;
}

static int begin_object(struct parser *p, const char *key)
{
	if (expect_kind(p, key, JR_OBJECT) < 0) {
		return -EINVAL;
	}
	return jr_object_begin(&p->jr) < 0 ? fail_syntax(p) : 0;
}

/* ---- sections ---------------------------------------------------------- */

static int parse_network(struct parser *p)
{
	struct gw_config *c = p->cfg;
	bool has_ip = false, has_mask = false, has_gw = false;
	char key[KEY_MAX];
	int ret;

	while ((ret = next_member(p, key)) > 0) {
		if (strcmp(key, "dhcp") == 0) {
			ret = read_bool(p, key, &c->network.dhcp);
		} else if (strcmp(key, "ip") == 0) {
			ret = read_ipv4(p, key, c->network.ip);
			has_ip = true;
		} else if (strcmp(key, "netmask") == 0) {
			ret = read_ipv4(p, key, c->network.netmask);
			has_mask = true;
		} else if (strcmp(key, "gateway") == 0) {
			ret = read_ipv4(p, key, c->network.gateway);
			has_gw = true;
		} else if (strcmp(key, "dns") == 0) {
			ret = read_ipv4(p, key, c->network.dns);
			c->network.has_dns = true;
		} else {
			ret = unknown(p, key);
		}
		if (ret < 0) {
			return ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	if (!c->network.dhcp && !(has_ip && has_mask && has_gw)) {
		return fail(p, NULL, "'ip', 'netmask' and 'gateway' are required when dhcp is false");
	}
	return 0;
}

static int parse_ntp(struct parser *p)
{
	char key[KEY_MAX];
	int ret;

	while ((ret = next_member(p, key)) > 0) {
		if (strcmp(key, "server") == 0) {
			ret = read_str(p, key, p->cfg->ntp.server, sizeof(p->cfg->ntp.server));
		} else {
			ret = unknown(p, key);
		}
		if (ret < 0) {
			return ret;
		}
	}
	return ret;
}

static int parse_mqtt(struct parser *p)
{
	struct gw_config *c = p->cfg;
	char key[KEY_MAX];
	int ret;

	while ((ret = next_member(p, key)) > 0) {
		if (strcmp(key, "host") == 0) {
			ret = read_str(p, key, c->mqtt.host, sizeof(c->mqtt.host));
		} else if (strcmp(key, "port") == 0) {
			ret = read_u16(p, key, 1, 65535, &c->mqtt.port);
		} else if (strcmp(key, "client_id") == 0) {
			ret = read_str(p, key, c->mqtt.client_id, sizeof(c->mqtt.client_id));
		} else if (strcmp(key, "username") == 0) {
			ret = read_str(p, key, c->mqtt.username, sizeof(c->mqtt.username));
		} else if (strcmp(key, "password") == 0) {
			ret = read_str(p, key, c->mqtt.password, sizeof(c->mqtt.password));
		} else if (strcmp(key, "keepalive_s") == 0) {
			ret = read_u16(p, key, 5, 3600, &c->mqtt.keepalive_s);
		} else {
			ret = unknown(p, key);
		}
		if (ret < 0) {
			return ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	if (c->mqtt.host[0] == '\0') {
		return fail(p, "host", "must not be empty");
	}
	return 0;
}

static int parse_sparkplug(struct parser *p)
{
	struct gw_config *c = p->cfg;
	char key[KEY_MAX];
	int ret;

	while ((ret = next_member(p, key)) > 0) {
		if (strcmp(key, "group_id") == 0) {
			ret = read_str(p, key, c->sparkplug.group_id, sizeof(c->sparkplug.group_id));
		} else if (strcmp(key, "edge_node_id") == 0) {
			ret = read_str(p, key, c->sparkplug.edge_node_id,
				       sizeof(c->sparkplug.edge_node_id));
		} else if (strcmp(key, "primary_host_id") == 0) {
			ret = read_str(p, key, c->sparkplug.primary_host_id,
				       sizeof(c->sparkplug.primary_host_id));
		} else {
			ret = unknown(p, key);
		}
		if (ret < 0) {
			return ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	ret = check_spb_id(p, "group_id", c->sparkplug.group_id, true);
	if (ret == 0) {
		ret = check_spb_id(p, "edge_node_id", c->sparkplug.edge_node_id, true);
	}
	if (ret == 0) {
		ret = check_spb_id(p, "primary_host_id", c->sparkplug.primary_host_id, false);
	}
	return ret;
}

static int parse_rs485(struct parser *p)
{
	static const char *const parities[] = {"none", "even", "odd"};
	struct gw_config *c = p->cfg;
	char key[KEY_MAX];
	int ret;

	while ((ret = next_member(p, key)) > 0) {
		if (strcmp(key, "baud") == 0) {
			ret = read_uint(p, key, 1200, 115200, &c->rs485.baud);
		} else if (strcmp(key, "parity") == 0) {
			ret = read_enum(p, key, parities, 3, &c->rs485.parity);
		} else if (strcmp(key, "stop_bits") == 0) {
			ret = read_u8(p, key, 1, 2, &c->rs485.stop_bits);
		} else if (strcmp(key, "response_timeout_ms") == 0) {
			ret = read_u16(p, key, 10, 10000, &c->rs485.response_timeout_ms);
		} else if (strcmp(key, "retries") == 0) {
			ret = read_u8(p, key, 0, 10, &c->rs485.retries);
		} else if (strcmp(key, "inter_frame_delay_ms") == 0) {
			ret = read_u16(p, key, 0, 1000, &c->rs485.inter_frame_delay_ms);
		} else {
			ret = unknown(p, key);
		}
		if (ret < 0) {
			return ret;
		}
	}
	return ret;
}

static int parse_local_io(struct parser *p)
{
	char key[KEY_MAX];
	int ret;

	while ((ret = next_member(p, key)) > 0) {
		if (strcmp(key, "di_debounce_ms") == 0) {
			ret = read_u16(p, key, 0, 1000, &p->cfg->local_io.di_debounce_ms);
		} else {
			ret = unknown(p, key);
		}
		if (ret < 0) {
			return ret;
		}
	}
	return ret;
}

static void tag_defaults(struct gw_tag_cfg *t)
{
	memset(t, 0, sizeof(*t));
	t->word_order = GW_WORD_BIG;
	t->scale = 1.0;
}

static int validate_tag(struct parser *p, const struct gw_device_cfg *dev, struct gw_tag_cfg *t,
			bool has_type)
{
	const struct gw_tag_cfg *tags = p->cfg->tags;
	bool bit = gw_area_is_bit((enum gw_area)t->area);

	if (t->name[0] == '\0') {
		return fail(p, "name", "is required");
	}
	if (!has_type) {
		t->type = bit ? GW_TYPE_BOOL : GW_TYPE_UINT16;
	}
	if (bit != (t->type == GW_TYPE_BOOL)) {
		return fail(p, "type", "'bool' is the only type for coil/discrete and is only "
				       "valid there");
	}
	if (t->writable && (t->area == GW_AREA_DISCRETE || t->area == GW_AREA_INPUT)) {
		return fail(p, "writable", "only coil and holding tags can be writable");
	}
	if (bit && (t->scale != 1.0 || t->offset != 0.0)) {
		return fail(p, "scale", "scale/offset are not allowed on bool tags");
	}
	if (t->scale == 0.0) {
		return fail(p, "scale", "must not be zero");
	}
	if (t->deadband < 0.0) {
		return fail(p, "deadband", "must not be negative");
	}
	if ((uint32_t)t->address + gw_type_regs((enum gw_type)t->type) > 65536U) {
		return fail(p, "address", "register range exceeds 65535");
	}
	for (uint16_t i = dev->first_tag; i < p->cfg->tag_count; i++) {
		if (strcmp(tags[i].name, t->name) == 0) {
			return fail(p, "name", "duplicate tag name '%s'", t->name);
		}
	}
	return 0;
}

static int parse_tag(struct parser *p, struct gw_device_cfg *dev, struct gw_tag_cfg *t)
{
	static const char *const areas[] = {"coil", "discrete", "holding", "input"};
	static const char *const types[] = {"bool",  "uint16", "int16",
					    "uint32", "int32",  "float32"};
	static const char *const orders[] = {"big", "little"};
	bool has_area = false, has_type = false;
	char units[GW_NAME_MAX + 1];
	char key[KEY_MAX];
	int ret;

	tag_defaults(t);
	if (begin_object(p, NULL) < 0) {
		return -EINVAL;
	}
	while ((ret = next_member(p, key)) > 0) {
		if (strcmp(key, "name") == 0) {
			ret = read_str(p, key, t->name, sizeof(t->name));
		} else if (strcmp(key, "area") == 0) {
			ret = read_enum(p, key, areas, 4, &t->area);
			has_area = true;
		} else if (strcmp(key, "address") == 0) {
			ret = read_u16(p, key, 0, 65535, &t->address);
		} else if (strcmp(key, "type") == 0) {
			ret = read_enum(p, key, types, 6, &t->type);
			has_type = true;
		} else if (strcmp(key, "word_order") == 0) {
			ret = read_enum(p, key, orders, 2, &t->word_order);
		} else if (strcmp(key, "scale") == 0) {
			ret = read_double(p, key, &t->scale);
		} else if (strcmp(key, "offset") == 0) {
			ret = read_double(p, key, &t->offset);
		} else if (strcmp(key, "deadband") == 0) {
			ret = read_double(p, key, &t->deadband);
		} else if (strcmp(key, "writable") == 0) {
			ret = read_bool(p, key, &t->writable);
		} else if (strcmp(key, "units") == 0) {
			/* Accepted by the common schema; engUnit (SPB-16) is not in the MVP. */
			ret = read_str(p, key, units, sizeof(units));
		} else {
			ret = unknown(p, key);
		}
		if (ret < 0) {
			return ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	if (!has_area) {
		return fail(p, "area", "is required");
	}
	return validate_tag(p, dev, t, has_type);
}

static int parse_tags(struct parser *p, struct gw_device_cfg *dev)
{
	struct gw_config *c = p->cfg;
	int ret;

	if (expect_kind(p, "tags", JR_ARRAY) < 0) {
		return -EINVAL;
	}
	if (jr_array_begin(&p->jr) < 0) {
		return fail_syntax(p);
	}
	dev->first_tag = c->tag_count;
	while ((ret = jr_array_next(&p->jr)) > 0) {
		size_t saved;

		if (dev->tag_count >= CONFIG_GW_MAX_TAGS_PER_DEVICE) {
			return fail(p, "tags", "too many tags (max %d per device)",
				    CONFIG_GW_MAX_TAGS_PER_DEVICE);
		}
		if (c->tag_count >= CONFIG_GW_MAX_TAGS) {
			return fail(p, "tags", "too many tags (max %d in total)",
				    CONFIG_GW_MAX_TAGS);
		}
		saved = path_push(p, ".tags[%u]", dev->tag_count);
		ret = parse_tag(p, dev, &c->tags[c->tag_count]);
		if (ret < 0) {
			return ret;
		}
		path_pop(p, saved);
		c->tag_count++;
		dev->tag_count++;
	}
	return ret < 0 ? fail_syntax(p) : 0;
}

static int parse_device(struct parser *p, struct gw_device_cfg *dev)
{
	const struct gw_config *c = p->cfg;
	bool has_tags = false, has_unit = false;
	char key[KEY_MAX];
	uint32_t poll_ms;
	int ret;

	memset(dev, 0, sizeof(*dev));
	dev->poll_ms = 1000;
	dev->offline_after = 3;

	if (begin_object(p, NULL) < 0) {
		return -EINVAL;
	}
	while ((ret = next_member(p, key)) > 0) {
		if (strcmp(key, "name") == 0) {
			ret = read_str(p, key, dev->name, sizeof(dev->name));
		} else if (strcmp(key, "unit_id") == 0) {
			ret = read_u8(p, key, 1, 247, &dev->unit_id);
			has_unit = true;
		} else if (strcmp(key, "poll_ms") == 0) {
			ret = read_uint(p, key, 50, 3600000, &poll_ms);
			dev->poll_ms = poll_ms;
		} else if (strcmp(key, "offline_after") == 0) {
			ret = read_u16(p, key, 1, 1000, &dev->offline_after);
		} else if (strcmp(key, "tags") == 0) {
			ret = parse_tags(p, dev);
			has_tags = true;
		} else {
			ret = unknown(p, key);
		}
		if (ret < 0) {
			return ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	if (check_spb_id(p, "name", dev->name, true) < 0) {
		return -EINVAL;
	}
	if (!has_unit) {
		return fail(p, "unit_id", "is required");
	}
	if (!has_tags || dev->tag_count == 0) {
		return fail(p, "tags", "at least one tag is required");
	}
	for (uint8_t i = 0; i < c->device_count; i++) {
		if (strcmp(c->devices[i].name, dev->name) == 0) {
			return fail(p, "name", "duplicate device name '%s'", dev->name);
		}
		if (c->devices[i].unit_id == dev->unit_id) {
			return fail(p, "unit_id", "unit_id %u already used by '%s'", dev->unit_id,
				    c->devices[i].name);
		}
	}
	return 0;
}

static int parse_devices(struct parser *p)
{
	struct gw_config *c = p->cfg;
	int ret;

	if (jr_array_begin(&p->jr) < 0) {
		return fail_syntax(p);
	}
	while ((ret = jr_array_next(&p->jr)) > 0) {
		size_t saved;

		if (c->device_count >= CONFIG_GW_MAX_DEVICES) {
			return fail(p, NULL, "too many devices (max %d)", CONFIG_GW_MAX_DEVICES);
		}
		saved = path_push(p, "devices[%u]", c->device_count);
		ret = parse_device(p, &c->devices[c->device_count]);
		if (ret < 0) {
			return ret;
		}
		path_pop(p, saved);
		c->device_count++;
	}
	return ret < 0 ? fail_syntax(p) : 0;
}

static void config_defaults(struct gw_config *c)
{
	memset(c, 0, sizeof(*c));
	c->network.dhcp = true;
	strcpy(c->ntp.server, "pool.ntp.org");
	c->mqtt.port = 1883;
	c->mqtt.keepalive_s = 30;
	c->rs485.baud = 9600;
	c->rs485.parity = GW_PARITY_NONE;
	c->rs485.stop_bits = 1;
	c->rs485.response_timeout_ms = 200;
	c->rs485.retries = 2;
	c->rs485.inter_frame_delay_ms = 5;
	c->local_io.di_debounce_ms = 20;
}

int gw_config_parse(const char *json, size_t len, struct gw_config *cfg, char *err,
		    size_t err_size)
{
	struct parser p = {.cfg = cfg, .err = err, .err_size = err_size};
	bool has_mqtt = false, has_spb = false;
	char key[KEY_MAX];
	int ret;

	config_defaults(cfg);
	err[0] = '\0';
	jr_init(&p.jr, json, len);

	if (begin_object(&p, NULL) < 0) {
		return -EINVAL;
	}
	while ((ret = next_member(&p, key)) > 0) {
		size_t saved = path_push(&p, "%s", key);

		if (strcmp(key, "devices") == 0) {
			path_pop(&p, saved);
			if (expect_kind(&p, key, JR_ARRAY) < 0) {
				return -EINVAL;
			}
			ret = parse_devices(&p);
		} else {
			int (*section)(struct parser *p) = NULL;

			if (strcmp(key, "network") == 0) {
				section = parse_network;
			} else if (strcmp(key, "ntp") == 0) {
				section = parse_ntp;
			} else if (strcmp(key, "mqtt") == 0) {
				section = parse_mqtt;
				has_mqtt = true;
			} else if (strcmp(key, "sparkplug") == 0) {
				section = parse_sparkplug;
				has_spb = true;
			} else if (strcmp(key, "rs485") == 0) {
				section = parse_rs485;
			} else if (strcmp(key, "local_io") == 0) {
				section = parse_local_io;
			}
			path_pop(&p, saved);
			if (section == NULL) {
				return unknown(&p, key);
			}
			if (begin_object(&p, key) < 0) {
				return -EINVAL;
			}
			saved = path_push(&p, "%s", key);
			ret = section(&p);
			path_pop(&p, saved);
		}
		if (ret < 0) {
			return ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	if (jr_finish(&p.jr) < 0) {
		return fail_syntax(&p);
	}
	if (!has_mqtt) {
		return fail(&p, "mqtt", "section is required");
	}
	if (!has_spb) {
		return fail(&p, "sparkplug", "section is required");
	}
	if (cfg->mqtt.client_id[0] == '\0') {
		strcpy(cfg->mqtt.client_id, cfg->sparkplug.edge_node_id);
	}
	return 0;
}
