/*
 * Minimal pull-style JSON reader.
 *
 * The reader walks a read-only buffer without allocating and without
 * modifying it. The caller drives it with the expected schema, which keeps
 * the nesting depth bounded by the schema itself. Every error records the
 * line and column where it was detected.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_JSON_READER_H_
#define GW_JSON_READER_H_

#include <stdbool.h>
#include <stddef.h>

enum jr_kind {
	JR_NONE,
	JR_OBJECT,
	JR_ARRAY,
	JR_STRING,
	JR_NUMBER,
	JR_BOOL,
	JR_NULL,
};

struct jr {
	const char *buf;
	size_t len;
	size_t pos;
	bool first; /* next member/element is the first of its container */
	char err[96];
};

void jr_init(struct jr *jr, const char *buf, size_t len);

/** Kind of the next value, without consuming it. */
enum jr_kind jr_peek(struct jr *jr);

/** Human readable name of a kind, for error messages. */
const char *jr_kind_name(enum jr_kind kind);

int jr_object_begin(struct jr *jr);

/**
 * Advance to the next member of the current object.
 *
 * @retval 1 a key was read into @p key and the ':' consumed.
 * @retval 0 the closing '}' was consumed.
 * @retval -EINVAL on a syntax error (see jr->err).
 */
int jr_object_next(struct jr *jr, char *key, size_t key_size);

int jr_array_begin(struct jr *jr);

/**
 * @retval 1 another element follows, 0 the closing ']' was consumed,
 *         -EINVAL on a syntax error.
 */
int jr_array_next(struct jr *jr);

/** Read a string; fails if it does not fit in @p out (including the NUL). */
int jr_string(struct jr *jr, char *out, size_t out_size);

int jr_number(struct jr *jr, double *out);

int jr_bool(struct jr *jr, bool *out);

/** Require that only whitespace remains. */
int jr_finish(struct jr *jr);

/** Record an error at the current position. Always returns -EINVAL. */
int jr_fail(struct jr *jr, const char *msg);

#endif /* GW_JSON_READER_H_ */
