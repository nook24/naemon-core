#include "nm_writebuf.h"
#include "nm_alloc.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

void nm_writebuf_init(struct nm_writebuf *wb, int fd, size_t size)
{
	if (size < 1)
		size = NM_WRITEBUF_SIZE;
	wb->buf = nm_malloc(size);
	wb->cap = size;
	wb->len = 0;
	wb->fd = fd;
	wb->error = 0;
}

void nm_writebuf_flush(struct nm_writebuf *wb)
{
	size_t off = 0;

	while (off < wb->len) {
		ssize_t n = write(wb->fd, wb->buf + off, wb->len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			wb->error = 1;
			break;
		}
		off += (size_t)n;
	}
	wb->len = 0;
}

void nm_writebuf_grow(struct nm_writebuf *wb, size_t need)
{
	/* only reached for a single field larger than the whole buffer */
	wb->cap = need * 2;
	wb->buf = nm_realloc(wb->buf, wb->cap);
}

int nm_writebuf_done(struct nm_writebuf *wb)
{
	int error;

	nm_writebuf_flush(wb);
	error = wb->error;
	nm_free(wb->buf);
	wb->cap = 0;
	wb->len = 0;
	return error;
}

#define NM_WB_DBL_CACHE_SIZE 512
struct nm_wb_dbl_cache_entry {
	uint64_t bits;
	const char *fmt;
	unsigned char len;
	char text[31];
};
static struct nm_wb_dbl_cache_entry nm_wb_dbl_cache[NM_WB_DBL_CACHE_SIZE];

void nm_wb_dbl(struct nm_writebuf *wb, const char *fmt, double v)
{
	char tmp[64];
	uint64_t bits;
	unsigned idx;
	struct nm_wb_dbl_cache_entry *e;
	int n;

	memcpy(&bits, &v, sizeof(bits));
	idx = (unsigned)((bits ^ (bits >> 32) ^ (uintptr_t)fmt) % NM_WB_DBL_CACHE_SIZE);
	e = &nm_wb_dbl_cache[idx];
	if (e->len && e->bits == bits && e->fmt == fmt) {
		nm_wb_mem(wb, e->text, e->len);
		return;
	}

	n = snprintf(tmp, sizeof(tmp), fmt, v);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	nm_wb_mem(wb, tmp, (size_t)n);

	/* only cache what fits, longer results are rare and not worth it */
	if ((size_t)n < sizeof(e->text)) {
		e->bits = bits;
		e->fmt = fmt;
		e->len = (unsigned char)n;
		memcpy(e->text, tmp, (size_t)n);
	}
}

void nm_wb_printf(struct nm_writebuf *wb, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(tmp)) {
		nm_wb_mem(wb, tmp, (size_t)n);
		return;
	}
	{
		char *big = nm_malloc((size_t)n + 1);
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		nm_wb_mem(wb, big, (size_t)n);
		nm_free(big);
	}
}

void nm_wb_customvar(struct nm_writebuf *wb, const char *prefix,
                     const customvariablesmember *cv)
{
	nm_wb_str(wb, prefix);
	nm_wb_lit(wb, "_");
	nm_wb_str(wb, cv->variable_name);
	nm_wb_lit(wb, "=");
	nm_wb_int(wb, cv->has_been_modified);
	nm_wb_lit(wb, ";");
	nm_wb_str(wb, cv->variable_value);
	nm_wb_lit(wb, "\n");
}
