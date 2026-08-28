#include <check.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include "naemon/nm_writebuf.h"

/*
 * nm_wb_dbl() memoizes formatted doubles in a small direct-mapped cache, so
 * that repeated values such as 0.000000 do not go through snprintf() every
 * time. That cache is the risky part of this file: a wrong hash, a stale
 * entry, or a truncated copy would silently corrupt whatever landed in
 * status.dat and retention.dat, and nothing else in the tree would notice.
 *
 * Everything here therefore checks the same invariant from different angles:
 * whatever nm_wb_dbl() appends must be byte-for-byte what snprintf() would
 * have produced for the same value and format. That has to hold no matter how
 * large the cache is or how the values are interleaved.
 *
 * Verified by mutation: dropping the stored-value comparison fails five of
 * these tests, and truncating a cached string by one byte fails two.
 *
 * One gap worth knowing about. The cache also compares the format pointer,
 * and that check only matters when two different (value, format) pairs land
 * in the same slot. Since the format is mixed into the slot index, such a
 * collision is rare, and removing that comparison still passes this suite at
 * the default cache size -- it only fails when the cache is shrunk to four
 * entries or fewer, where everything collides. Shrinking
 * NM_WB_DBL_CACHE_SIZE to 1 and re-running is therefore worth doing by hand
 * after touching the lookup.
 */

#define BUFSZ (1 << 20)

static struct nm_writebuf wb;
static char backing[BUFSZ];

static void wb_reset(void)
{
	/* fd -1: nothing is ever flushed, the buffer is inspected directly */
	wb.buf = backing;
	wb.cap = sizeof(backing);
	wb.len = 0;
	wb.fd = -1;
	wb.error = 0;
}

static void ck_dbl_matches_snprintf(const char *fmt, double v)
{
	char expected[64];
	int n = snprintf(expected, sizeof(expected), fmt, v);

	wb_reset();
	nm_wb_dbl(&wb, fmt, v);
	ck_assert_int_eq((int)wb.len, n);
	ck_assert(memcmp(wb.buf, expected, (size_t)n) == 0);
}

START_TEST(dbl_matches_snprintf_over_many_values)
{
	static const char *fmts[] = {"%f", "%.2f", "%.3f"};
	unsigned f, i;

	for (f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
		for (i = 0; i < 5000; i++) {
			ck_dbl_matches_snprintf(fmts[f], (double)i);
			ck_dbl_matches_snprintf(fmts[f], i / 1000.0);
			ck_dbl_matches_snprintf(fmts[f], -(i / 7.0));
		}
	}
}
END_TEST

/*
 * The values the state files really write: whole numbers that repeat
 * endlessly, mixed with measured values that almost never repeat. This is the
 * pattern that decides whether the cache helps or thrashes, and the mix that
 * would expose an entry being handed out for the wrong value.
 */
START_TEST(dbl_survives_realistic_interleaving)
{
	int i;

	for (i = 0; i < 20000; i++) {
		ck_dbl_matches_snprintf("%f", 60.0);
		ck_dbl_matches_snprintf("%f", 300.0);
		ck_dbl_matches_snprintf("%.2f", 0.0);
		ck_dbl_matches_snprintf("%.3f", (i % 350) / 1000.0);
		ck_dbl_matches_snprintf("%.3f", (i % 749) / 10000.0);
	}
}
END_TEST

/*
 * The cache is keyed on the value's bit pattern and the format pointer. Two
 * different formats for the same value must not be allowed to share an entry,
 * and neither must two different values.
 */
START_TEST(dbl_does_not_confuse_formats_or_values)
{
	int i;

	for (i = 0; i < 2000; i++) {
		ck_dbl_matches_snprintf("%f", 1.0);
		ck_dbl_matches_snprintf("%.2f", 1.0);
		ck_dbl_matches_snprintf("%.3f", 1.0);
		ck_dbl_matches_snprintf("%f", 1.5);
		ck_dbl_matches_snprintf("%.2f", 1.5);
		ck_dbl_matches_snprintf("%.3f", 1.5);
	}
}
END_TEST

/* More distinct values than any sane cache holds, each seen twice. */
START_TEST(dbl_handles_more_values_than_the_cache_holds)
{
	int pass, i;

	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < 20000; i++)
			ck_dbl_matches_snprintf("%.3f", i * 1.001);
}
END_TEST

START_TEST(dbl_handles_edge_values)
{
	ck_dbl_matches_snprintf("%f", 0.0);
	ck_dbl_matches_snprintf("%f", -0.0);
	ck_dbl_matches_snprintf("%f", -1.0);
	ck_dbl_matches_snprintf("%.2f", 0.005);
	ck_dbl_matches_snprintf("%.3f", 1e12);
	ck_dbl_matches_snprintf("%.3f", -1e12);
	/* wider than the cache entry can store: must still print correctly */
	ck_dbl_matches_snprintf("%f", 1e30);
	ck_dbl_matches_snprintf("%f", -1e30);
}
END_TEST

START_TEST(int_writers_match_printf)
{
	char expected[32];
	long long vals[] = {0, 1, -1, 9, 10, -10, 99999, -99999,
	                    LLONG_MAX, LLONG_MIN};
	unsigned i;
	int n;

	for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
		n = snprintf(expected, sizeof(expected), "%lld", vals[i]);
		wb_reset();
		nm_wb_int(&wb, vals[i]);
		ck_assert_int_eq((int)wb.len, n);
		ck_assert(memcmp(wb.buf, expected, (size_t)n) == 0);
	}

	n = snprintf(expected, sizeof(expected), "%llu", ULLONG_MAX);
	wb_reset();
	nm_wb_uint(&wb, ULLONG_MAX);
	ck_assert_int_eq((int)wb.len, n);
	ck_assert(memcmp(wb.buf, expected, (size_t)n) == 0);
}
END_TEST

START_TEST(kv_macros_produce_key_value_lines)
{
	wb_reset();
	nm_wb_kv_str(&wb, "name", "value");
	nm_wb_kv_int(&wb, "count", -7);
	nm_wb_kv_uint(&wb, "size", 42u);
	nm_wb_kv_dbl(&wb, "ratio", "%.2f", 0.5);
	ck_assert(wb.len < sizeof(backing));
	wb.buf[wb.len] = '\0';
	ck_assert_str_eq(wb.buf, "name=value\ncount=-7\nsize=42\nratio=0.50\n");
}
END_TEST

/* nm_wb_str() must not walk off the end when handed NULL. */
START_TEST(str_accepts_null)
{
	wb_reset();
	nm_wb_kv_str(&wb, "empty", NULL);
	wb.buf[wb.len] = '\0';
	ck_assert_str_eq(wb.buf, "empty=\n");
}
END_TEST

int main(void)
{
	int number_failed;
	Suite *s = suite_create("nm_writebuf");
	TCase *tc = tcase_create("formatting");
	SRunner *sr;

	tcase_set_timeout(tc, 60);
	tcase_add_test(tc, dbl_matches_snprintf_over_many_values);
	tcase_add_test(tc, dbl_survives_realistic_interleaving);
	tcase_add_test(tc, dbl_does_not_confuse_formats_or_values);
	tcase_add_test(tc, dbl_handles_more_values_than_the_cache_holds);
	tcase_add_test(tc, dbl_handles_edge_values);
	tcase_add_test(tc, int_writers_match_printf);
	tcase_add_test(tc, kv_macros_produce_key_value_lines);
	tcase_add_test(tc, str_accepts_null);
	suite_add_tcase(s, tc);

	sr = srunner_create(s);
	srunner_run_all(sr, CK_ENV);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return number_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
