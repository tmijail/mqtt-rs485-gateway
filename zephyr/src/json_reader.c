/*
 * Minimal pull-style JSON reader (RFC 8259 subset: no duplicate-key or
 * depth bookkeeping, both are enforced by the schema-driven caller).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "json_reader.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JR_NUMBER_MAX 32

int jr_fail(struct jr *jr, const char *msg)
{
	unsigned int line = 1;
	unsigned int col = 1;

	for (size_t i = 0; i < jr->pos && i < jr->len; i++) {
		if (jr->buf[i] == '\n') {
			line++;
			col = 1;
		} else {
			col++;
		}
	}
	snprintf(jr->err, sizeof(jr->err), "line %u, col %u: %s", line, col, msg);
	return -EINVAL;
}

void jr_init(struct jr *jr, const char *buf, size_t len)
{
	jr->buf = buf;
	jr->len = len;
	jr->pos = 0;
	jr->first = false;
	jr->err[0] = '\0';
}

static void skip_ws(struct jr *jr)
{
	while (jr->pos < jr->len) {
		char c = jr->buf[jr->pos];

		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
			break;
		}
		jr->pos++;
	}
}

static int peek_char(struct jr *jr)
{
	skip_ws(jr);
	return (jr->pos < jr->len) ? (unsigned char)jr->buf[jr->pos] : -1;
}

enum jr_kind jr_peek(struct jr *jr)
{
	switch (peek_char(jr)) {
	case '{':
		return JR_OBJECT;
	case '[':
		return JR_ARRAY;
	case '"':
		return JR_STRING;
	case 't':
	case 'f':
		return JR_BOOL;
	case 'n':
		return JR_NULL;
	case '-':
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		return JR_NUMBER;
	default:
		return JR_NONE;
	}
}

const char *jr_kind_name(enum jr_kind kind)
{
	static const char *const names[] = {
		[JR_NONE] = "nothing", [JR_OBJECT] = "an object", [JR_ARRAY] = "an array",
		[JR_STRING] = "a string", [JR_NUMBER] = "a number", [JR_BOOL] = "a boolean",
		[JR_NULL] = "null",
	};

	return names[kind];
}

static int expect(struct jr *jr, char c, const char *msg)
{
	if (peek_char(jr) != (unsigned char)c) {
		return jr_fail(jr, msg);
	}
	jr->pos++;
	return 0;
}

int jr_object_begin(struct jr *jr)
{
	int ret = expect(jr, '{', "expected an object");

	jr->first = true;
	return ret;
}

int jr_array_begin(struct jr *jr)
{
	int ret = expect(jr, '[', "expected an array");

	jr->first = true;
	return ret;
}

/* Shared member/element separator logic. */
static int container_next(struct jr *jr, char close)
{
	int c = peek_char(jr);

	if (c == close) {
		jr->pos++;
		jr->first = false;
		return 0;
	}
	if (!jr->first) {
		if (c != ',') {
			return jr_fail(jr, close == '}' ? "expected ',' or '}'" : "expected ',' or ']'");
		}
		jr->pos++;
		if (peek_char(jr) == close) {
			return jr_fail(jr, "trailing comma");
		}
	}
	jr->first = false;
	return 1;
}

int jr_object_next(struct jr *jr, char *key, size_t key_size)
{
	int ret = container_next(jr, '}');

	if (ret <= 0) {
		return ret;
	}
	if (jr_peek(jr) != JR_STRING) {
		return jr_fail(jr, "expected a member name");
	}
	ret = jr_string(jr, key, key_size);
	if (ret < 0) {
		return ret;
	}
	ret = expect(jr, ':', "expected ':'");
	return ret < 0 ? ret : 1;
}

int jr_array_next(struct jr *jr)
{
	return container_next(jr, ']');
}

static int hex4(struct jr *jr, uint32_t *out)
{
	uint32_t v = 0;

	if (jr->len - jr->pos < 4) {
		return jr_fail(jr, "truncated \\u escape");
	}
	for (int i = 0; i < 4; i++) {
		char c = jr->buf[jr->pos++];

		v <<= 4;
		if (c >= '0' && c <= '9') {
			v |= (uint32_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			v |= (uint32_t)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			v |= (uint32_t)(c - 'A' + 10);
		} else {
			return jr_fail(jr, "invalid \\u escape");
		}
	}
	*out = v;
	return 0;
}

static size_t utf8_encode(uint32_t cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static int unicode_escape(struct jr *jr, char tmp[4], size_t *n)
{
	uint32_t cp;
	uint32_t lo;

	if (hex4(jr, &cp) < 0) {
		return -EINVAL;
	}
	if (cp >= 0xD800 && cp <= 0xDBFF) {
		if (jr->len - jr->pos < 2 || jr->buf[jr->pos] != '\\' ||
		    jr->buf[jr->pos + 1] != 'u') {
			return jr_fail(jr, "unpaired UTF-16 surrogate");
		}
		jr->pos += 2;
		if (hex4(jr, &lo) < 0) {
			return -EINVAL;
		}
		if (lo < 0xDC00 || lo > 0xDFFF) {
			return jr_fail(jr, "unpaired UTF-16 surrogate");
		}
		cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
	} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
		return jr_fail(jr, "unpaired UTF-16 surrogate");
	} else if (cp == 0) {
		return jr_fail(jr, "NUL character in string");
	}
	*n = utf8_encode(cp, tmp);
	return 0;
}

int jr_string(struct jr *jr, char *out, size_t out_size)
{
	size_t n = 0;

	if (expect(jr, '"', "expected a string") < 0) {
		return -EINVAL;
	}

	while (true) {
		char tmp[4];
		size_t tmp_len = 1;
		char c;

		if (jr->pos >= jr->len) {
			return jr_fail(jr, "unterminated string");
		}
		c = jr->buf[jr->pos++];
		if (c == '"') {
			break;
		}
		if ((unsigned char)c < 0x20) {
			return jr_fail(jr, "control character in string");
		}
		tmp[0] = c;
		if (c == '\\') {
			if (jr->pos >= jr->len) {
				return jr_fail(jr, "unterminated string");
			}
			c = jr->buf[jr->pos++];
			switch (c) {
			case '"':
			case '\\':
			case '/':
				tmp[0] = c;
				break;
			case 'b':
				tmp[0] = '\b';
				break;
			case 'f':
				tmp[0] = '\f';
				break;
			case 'n':
				tmp[0] = '\n';
				break;
			case 'r':
				tmp[0] = '\r';
				break;
			case 't':
				tmp[0] = '\t';
				break;
			case 'u':
				if (unicode_escape(jr, tmp, &tmp_len) < 0) {
					return -EINVAL;
				}
				break;
			default:
				return jr_fail(jr, "invalid escape sequence");
			}
		}
		if (n + tmp_len >= out_size) {
			char msg[48];

			snprintf(msg, sizeof(msg), "string too long (max %u bytes)",
				 (unsigned int)(out_size - 1));
			return jr_fail(jr, msg);
		}
		memcpy(&out[n], tmp, tmp_len);
		n += tmp_len;
	}
	out[n] = '\0';
	return 0;
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

int jr_number(struct jr *jr, double *out)
{
	char tmp[JR_NUMBER_MAX + 1];
	size_t start;
	size_t p;
	char *end;

	if (jr_peek(jr) != JR_NUMBER) {
		return jr_fail(jr, "expected a number");
	}
	start = jr->pos;
	p = start;

	/* Validate the strict JSON number grammar before handing it to strtod. */
	if (p < jr->len && jr->buf[p] == '-') {
		p++;
	}
	if (p < jr->len && jr->buf[p] == '0') {
		p++;
	} else if (p < jr->len && is_digit(jr->buf[p])) {
		while (p < jr->len && is_digit(jr->buf[p])) {
			p++;
		}
	} else {
		return jr_fail(jr, "invalid number");
	}
	if (p < jr->len && jr->buf[p] == '.') {
		p++;
		if (p >= jr->len || !is_digit(jr->buf[p])) {
			return jr_fail(jr, "invalid number");
		}
		while (p < jr->len && is_digit(jr->buf[p])) {
			p++;
		}
	}
	if (p < jr->len && (jr->buf[p] == 'e' || jr->buf[p] == 'E')) {
		p++;
		if (p < jr->len && (jr->buf[p] == '+' || jr->buf[p] == '-')) {
			p++;
		}
		if (p >= jr->len || !is_digit(jr->buf[p])) {
			return jr_fail(jr, "invalid number");
		}
		while (p < jr->len && is_digit(jr->buf[p])) {
			p++;
		}
	}
	if (p - start > JR_NUMBER_MAX) {
		return jr_fail(jr, "number too long");
	}

	memcpy(tmp, &jr->buf[start], p - start);
	tmp[p - start] = '\0';
	*out = strtod(tmp, &end);
	if (end != &tmp[p - start]) {
		return jr_fail(jr, "invalid number");
	}
	jr->pos = p;
	return 0;
}

static bool match_literal(struct jr *jr, const char *lit)
{
	size_t n = strlen(lit);

	if (jr->len - jr->pos < n || memcmp(&jr->buf[jr->pos], lit, n) != 0) {
		return false;
	}
	jr->pos += n;
	return true;
}

int jr_bool(struct jr *jr, bool *out)
{
	if (jr_peek(jr) == JR_BOOL) {
		if (match_literal(jr, "true")) {
			*out = true;
			return 0;
		}
		if (match_literal(jr, "false")) {
			*out = false;
			return 0;
		}
	}
	return jr_fail(jr, "expected true or false");
}

int jr_finish(struct jr *jr)
{
	if (peek_char(jr) != -1) {
		return jr_fail(jr, "unexpected data after the document");
	}
	return 0;
}
