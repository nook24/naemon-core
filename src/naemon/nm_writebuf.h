#ifndef _NM_WRITEBUF_H
#define _NM_WRITEBUF_H

#if !defined (_NAEMON_H_INSIDE) && !defined (NAEMON_COMPILATION)
#error "Only <naemon/naemon.h> can be included directly."
#endif

#include "lib/lnae-utils.h"
#include "objects_common.h"
#include <stddef.h>
#include <string.h>

NAGIOS_BEGIN_DECL

/*
 * Buffered writer for the state files naemon dumps periodically -- status.dat
 * and retention.dat. Both used to be written with one fprintf() call per
 * field, which on installations with 100k objects meant hundreds of thousands
 * of format string parses per second; profiling the event loop showed half of
 * its CPU time inside printf's format interpreter. Both files are
 * overwhelmingly integers and strings, so appending them into a large buffer
 * directly avoids that work entirely, and the buffer also removes the
 * thousands of small write() syscalls a stdio stream's default 4k buffer
 * caused.
 *
 * The appenders that run per field are inlined here on purpose; the cold paths
 * (flushing, doubles, the printf fallback) live in nm_writebuf.c.
 */

struct nm_writebuf {
	char *buf;
	size_t len;
	size_t cap;
	int fd;
	int error; /* sticky: set when a write() failed */
};

#define NM_WRITEBUF_SIZE (1024 * 1024)

/* Allocates the buffer. Call nm_writebuf_done() to flush and release it. */
void nm_writebuf_init(struct nm_writebuf *wb, int fd, size_t size);

/* Flushes the remaining bytes and frees the buffer. Returns 0 on success. */
int nm_writebuf_done(struct nm_writebuf *wb);

/* Writes the buffer out and empties it. */
void nm_writebuf_flush(struct nm_writebuf *wb);

/* Grows the buffer so a single oversized field fits. */
void nm_writebuf_grow(struct nm_writebuf *wb, size_t need);

static inline void nm_wb_reserve(struct nm_writebuf *wb, size_t n)
{
	if (wb->len + n > wb->cap)
		nm_writebuf_flush(wb);
	if (n > wb->cap)
		nm_writebuf_grow(wb, n);
}

static inline void nm_wb_mem(struct nm_writebuf *wb, const char *s, size_t n)
{
	nm_wb_reserve(wb, n);
	memcpy(wb->buf + wb->len, s, n);
	wb->len += n;
}

/* Appends a string literal, using its compile time length. */
#define nm_wb_lit(wb, s) nm_wb_mem((wb), (s), sizeof(s) - 1)

/* A NULL string appends nothing, which is what the old "" fallbacks did. */
static inline void nm_wb_str(struct nm_writebuf *wb, const char *s)
{
	if (s != NULL)
		nm_wb_mem(wb, s, strlen(s));
}

static inline void nm_wb_uint(struct nm_writebuf *wb, unsigned long long v)
{
	char tmp[24];
	int i = (int)sizeof(tmp);

	do {
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v);
	nm_wb_mem(wb, tmp + i, sizeof(tmp) - (size_t)i);
}

static inline void nm_wb_int(struct nm_writebuf *wb, long long v)
{
	if (v < 0) {
		nm_wb_lit(wb, "-");
		/* written this way so LLONG_MIN does not overflow when negated */
		nm_wb_uint(wb, (unsigned long long) - (v + 1) + 1ULL);
	} else {
		nm_wb_uint(wb, (unsigned long long)v);
	}
}

/*
 * Doubles are still formatted by snprintf() -- reimplementing printf's
 * rounding would risk changing the file formats. Instead the results are
 * memoized: the handful of double fields per object repeat the same few
 * values across the whole object list, so a small direct mapped cache keyed
 * on the bit pattern removes almost all of the printf calls while producing
 * exactly the bytes snprintf would have produced.
 */
void nm_wb_dbl(struct nm_writebuf *wb, const char *fmt, double v);

/* Fallback for the few lines that are neither a plain integer nor a string. */
void nm_wb_printf(struct nm_writebuf *wb, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));

/*
 * "key=value\n" helpers. key is always a string literal, and carries its own
 * indentation where the file format has any -- status.dat indents its fields
 * with a tab, retention.dat does not.
 */
#define nm_wb_kv_str(wb, key, val)   do { nm_wb_lit((wb), key "="); nm_wb_str((wb), (val)); nm_wb_lit((wb), "\n"); } while (0)
#define nm_wb_kv_int(wb, key, val)   do { nm_wb_lit((wb), key "="); nm_wb_int((wb), (long long)(val)); nm_wb_lit((wb), "\n"); } while (0)
#define nm_wb_kv_uint(wb, key, val)  do { nm_wb_lit((wb), key "="); nm_wb_uint((wb), (unsigned long long)(val)); nm_wb_lit((wb), "\n"); } while (0)
#define nm_wb_kv_dbl(wb, key, fmt, val) do { nm_wb_lit((wb), key "="); nm_wb_dbl((wb), (fmt), (double)(val)); nm_wb_lit((wb), "\n"); } while (0)

/* "<prefix>_NAME=MODIFIED;VALUE\n" -- both files write custom variables. */
void nm_wb_customvar(struct nm_writebuf *wb, const char *prefix,
                     const customvariablesmember *cv);

NAGIOS_END_DECL

#endif
