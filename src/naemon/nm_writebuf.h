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
 * ============================================================================
 * Buffered writer for status.dat and retention.dat
 * ============================================================================
 *
 * WHY THIS EXISTS
 *
 * Both state files used to be written with one fprintf() call per field:
 *
 *     fprintf(fp, "\tcurrent_state=%d\n", svc->current_state);
 *     fprintf(fp, "\tlast_check=%lu\n",   svc->last_check);
 *     ... about 57 more of these, per object ...
 *
 * That is comfortable to read but expensive to run. Every fprintf() call has
 * to walk its format string character by character at *runtime*, work out that
 * "%d" means "take an int off the varargs stack and render it as decimal",
 * take a lock on the FILE stream, and copy the result in. None of that work
 * can be done ahead of time, and none of it is shared between calls.
 *
 * With 100k services and the default status_update_interval of 10 seconds
 * that adds up to roughly 570,000 format string parses per second. Profiling
 * naemon's event loop under that load showed *half* of its CPU time inside
 * glibc's printf implementation. Since the event loop is a single thread,
 * that is time during which no check is scheduled and no result is processed.
 *
 * The observation this module is built on: both files are almost entirely
 * integers and strings. Rendering those does not need a format interpreter.
 * If we know at the call site that a field is an integer, we can just convert
 * it and append the bytes.
 *
 *
 * HOW IT WORKS
 *
 * A nm_writebuf is a plain byte buffer plus a file descriptor:
 *
 *     buf  -> [ "hoststatus {\n\thost_name=localhost\n\t..." ......... ]
 *              |<---------------- len ---------------->|
 *              |<------------------------- cap ---------------------->|
 *
 * Appending means "copy these bytes to buf + len and advance len". When the
 * next field would not fit, the buffer is written out with a single write(2)
 * and len goes back to 0. So instead of thousands of small stdio writes we
 * get one syscall per megabyte.
 *
 * The two things that make this fast are:
 *
 *   1. No format string is ever parsed. nm_wb_kv_int(wb, "current_state", v)
 *      knows at compile time that the key is a literal (so its length is
 *      sizeof(...) - 1, computed by the compiler) and that the value is an
 *      integer (so it goes through a small division loop, not vfprintf).
 *
 *   2. The per-field appenders are static inline in this header, so the
 *      compiler pastes them straight into the writer loops in xsddefault.c
 *      and xrddefault.c. There is no function call per field. Only the cold
 *      paths -- flushing, doubles, and the printf fallback -- are real
 *      functions in nm_writebuf.c.
 *
 *
 * USING IT
 *
 *     int fd = mkstemp(tmpfile);
 *     struct nm_writebuf wb;
 *
 *     nm_writebuf_init(&wb, fd, NM_WRITEBUF_SIZE);
 *
 *     nm_wb_lit(&wb, "hoststatus {\n");         // a literal, length known
 *     nm_wb_kv_str(&wb, "\thost_name", hst->name);
 *     nm_wb_kv_int(&wb, "\tcurrent_state", hst->current_state);
 *     nm_wb_kv_uint(&wb, "\tlast_check", hst->last_check);
 *     nm_wb_kv_dbl(&wb, "\tcheck_latency", "%.3f", hst->latency);
 *     nm_wb_lit(&wb, "\t}\n\n");
 *
 *     if (nm_writebuf_done(&wb) != 0)           // flushes and frees
 *             ... a write() failed somewhere ...
 *
 * The key passed to the nm_wb_kv_* macros MUST be a string literal, because
 * the macro pastes it together with "=" at compile time. It carries its own
 * indentation: status.dat indents fields inside a block with a tab,
 * retention.dat does not, so status.dat passes "\thost_name" and
 * retention.dat passes "host_name".
 *
 *
 * ERROR HANDLING
 *
 * Write errors are not reported per call. Checking a return value on every
 * one of a hundred thousand appends would be noise. Instead a failed write()
 * sets a sticky error flag, appending continues harmlessly, and
 * nm_writebuf_done() reports it once at the end -- which is exactly where the
 * old code checked ferror() on the stream. The caller must still not rename
 * the temp file over the real one when that comes back non-zero.
 * ============================================================================
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

/*
 * Make room for n more bytes. If they do not fit, the buffer is written out
 * and emptied first. The second check only fires for a single field that is
 * larger than the whole buffer (a very long plugin output, say), in which case
 * the buffer is grown to fit it.
 */
static inline void nm_wb_reserve(struct nm_writebuf *wb, size_t n)
{
	if (wb->len + n > wb->cap)
		nm_writebuf_flush(wb);
	if (n > wb->cap)
		nm_writebuf_grow(wb, n);
}

/* Appends n bytes. This is what every other appender ends up calling. */
static inline void nm_wb_mem(struct nm_writebuf *wb, const char *s, size_t n)
{
	nm_wb_reserve(wb, n);
	memcpy(wb->buf + wb->len, s, n);
	wb->len += n;
}

/*
 * Appends a string literal. sizeof(s) - 1 is its length minus the terminating
 * NUL, computed by the compiler, so this costs no strlen() at runtime. Only
 * works for literals -- for a char* use nm_wb_str().
 */
#define nm_wb_lit(wb, s) nm_wb_mem((wb), (s), sizeof(s) - 1)

/*
 * Appends a C string. A NULL string appends nothing, which is what the
 * (x == NULL) ? "" : x fallbacks in the old fprintf() calls did.
 */
static inline void nm_wb_str(struct nm_writebuf *wb, const char *s)
{
	if (s != NULL)
		nm_wb_mem(wb, s, strlen(s));
}

/*
 * Appends an unsigned integer in decimal, no padding.
 *
 * Digits come out least significant first, so they are written backwards into
 * a scratch buffer starting at the end, and the filled tail is what gets
 * appended:
 *
 *     v = 4711    tmp[24] = [ . . . . . . . . . . . . . . . . . . . 4 7 1 1 ]
 *                                                                  ^ i
 *                           append from tmp + i, length 24 - i
 *
 * The do/while runs at least once, so v == 0 correctly produces "0".
 * 24 bytes is enough for any 64 bit value (20 digits at most).
 */
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

/* Appends a signed integer in decimal. */
static inline void nm_wb_int(struct nm_writebuf *wb, long long v)
{
	if (v < 0) {
		nm_wb_lit(wb, "-");
		/*
		 * Negating LLONG_MIN directly is undefined behaviour, because its
		 * absolute value does not fit in a long long. Adding 1 first, then
		 * negating, then adding the 1 back in unsigned arithmetic gets the
		 * right magnitude without ever overflowing a signed type.
		 */
		nm_wb_uint(wb, (unsigned long long) - (v + 1) + 1ULL);
	} else {
		nm_wb_uint(wb, (unsigned long long)v);
	}
}

/*
 * Appends a double, formatted with `fmt` (one of "%f", "%.3f", "%.2f").
 *
 * Unlike the integers, doubles still go through snprintf(). Rounding a
 * binary double to a fixed number of decimal places the way printf does it
 * is genuinely fiddly -- printf rounds the exact value of the double, and
 * getting a hand written version to agree in every last-digit tie case is
 * not worth the risk of silently changing what lands in the file.
 *
 * Instead the *results* are memoized. There are only about five double
 * fields per object, and across a whole object list they repeat constantly:
 * every service in a config usually shares the same check_interval, and
 * execution times and latencies are 0.000 for anything not yet checked. A
 * small cache keyed on the double's bit pattern therefore hits most of the
 * time, and when it does not, snprintf produces the bytes and they get
 * remembered. The output is byte for byte what snprintf would have written
 * either way.
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
