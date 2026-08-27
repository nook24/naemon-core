/*
 * Stress test for the day cache in objects_timeperiod.c.
 *
 * The cache exists so that check_time_against_period() does not have to run
 * localtime_r()/mktime() for every dispatched check. It is only ever allowed
 * to return what the uncached computation would have returned, so this test
 * reimplements that uncached computation ("the reference") and compares the
 * two over a large number of timestamps.
 *
 * The interesting cases are days that are not 86400 seconds long or whose
 * local midnight is ambiguous or missing:
 *
 *   - DST spring forward (23h) and fall back (25h)
 *   - zones that shift by 30 or 45 minutes rather than an hour
 *   - zones whose DST transition happens *at* midnight, so local 00:00 does
 *     not exist that day (America/Havana, America/Santiago, Asia/Beirut)
 *   - leap seconds, which under a "right/" zone make a day 86401 seconds
 *   - a leap day, and the day after a zone skipped a calendar day entirely
 *     (Pacific/Apia 2011, Pacific/Kiritimati 1994) -- both of which are
 *     ordinary 86400 second days and *should* be cached
 *
 * Timestamps are visited in several orders on purpose. Sequential access is
 * the normal case, random access catches state left over from an unrelated
 * day, and walking backwards is what the cache sees when the system clock is
 * stepped back.
 */

#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* yes, include C file, we should access static functions */
#include "naemon/objects_timeperiod.c"

/* 1990-01-01 .. 2036-01-01: spans leap years, leap seconds and the historical
 * timezone oddities the tzdata database records */
#define RANGE_START 631152000L
#define RANGE_SPAN  (46LL * 365 * 86400)

static const char *tricky_timezones[] = {
	"UTC",
	"Europe/Berlin",           /* ordinary one hour DST */
	"Europe/London",           /* DST while sitting on UTC */
	"America/New_York",
	"America/Havana",          /* DST transition at midnight */
	"America/Santiago",        /* DST transition at midnight, southern hemisphere */
	"Asia/Beirut",             /* DST transition at midnight */
	"Africa/Cairo",            /* DST reintroduced in 2023 */
	"Australia/Lord_Howe",     /* 30 minute DST shift */
	"Pacific/Chatham",         /* 45 minute standard offset */
	"Pacific/Apia",            /* skipped 2011-12-30 entirely */
	"Pacific/Kiritimati",      /* skipped 1994-12-31 entirely */
	"Antarctica/Troll",        /* two hour DST shift */
	"Iran",
	"Asia/Kolkata",            /* 30 minute standard offset, no DST */
	"right/Europe/Berlin",     /* counts leap seconds: some days are 86401s */
};

/*
 * The computation as it was before the cache was introduced. Everything the
 * cache hands out has to match this.
 */
struct reference_day {
	time_t midnight;
	int year;
	int mon;
	int wday;
};

static struct reference_day reference_day_for(time_t when)
{
	struct reference_day r;
	struct tm *t, tm_s;

	t = localtime_r(&when, &tm_s);
	r.year = t->tm_year;
	r.mon = t->tm_mon;
	r.wday = t->tm_wday;
	t->tm_sec = 0;
	t->tm_min = 0;
	t->tm_hour = 0;
	r.midnight = mktime(t);
	return r;
}

/* Returns 0 when the cache agrees with the reference for this timestamp. */
static int disagrees_at(time_t when, const char **why)
{
	struct reference_day ref = reference_day_for(when);
	const struct day_cache *dc = get_day_cache(when);

	if (dc == NULL) {
		*why = "cache returned NULL";
		return 1;
	}
	if (dc->midnight != ref.midnight) {
		*why = "midnight differs";
		return 1;
	}
	if (dc->year != ref.year) {
		*why = "tm_year differs";
		return 1;
	}
	if (dc->mon != ref.mon) {
		*why = "tm_mon differs";
		return 1;
	}
	if (dc->wday != ref.wday) {
		*why = "tm_wday differs";
		return 1;
	}
	return 0;
}

static void forget_cached_day(void)
{
	memset(&day_cache, 0, sizeof(day_cache));
}

/* Describes a timestamp in a way that is useful in a failure message. */
static const char *describe(time_t when)
{
	static char buf[80];
	struct tm tm_s;

	if (localtime_r(&when, &tm_s) == NULL)
		snprintf(buf, sizeof(buf), "%ld", (long)when);
	else
		strftime(buf, sizeof(buf), "%F %T %Z", &tm_s);
	return buf;
}

static void use_timezone(const char *tz)
{
	setenv("TZ", tz, 1);
	tzset();
	forget_cached_day();
}

START_TEST(daycache_matches_reference)
{
	const char *tz = tricky_timezones[_i];
	const char *why = NULL;
	time_t t;
	long i;

	use_timezone(tz);

	/* sequential, with a step that is not a divisor of a day so that every
	 * time of day gets visited as the walk progresses */
	for (t = RANGE_START; t < RANGE_START + RANGE_SPAN; t += 4001) {
		if (disagrees_at(t, &why))
			ck_abort_msg("TZ=%s sequential: %s at %s", tz, why, describe(t));
	}

	/* random access */
	srand(20260827);
	for (i = 0; i < 200000; i++) {
		long long r = ((long long)rand() << 31) ^ (long long)rand();
		if (r < 0)
			r = -r;
		t = RANGE_START + (time_t)(r % RANGE_SPAN);
		if (disagrees_at(t, &why))
			ck_abort_msg("TZ=%s random: %s at %s", tz, why, describe(t));
	}

	/* backwards, i.e. what a system clock stepped back looks like */
	for (t = RANGE_START + RANGE_SPAN; t > RANGE_START; t -= 4001) {
		if (disagrees_at(t, &why))
			ck_abort_msg("TZ=%s backwards: %s at %s", tz, why, describe(t));
	}
}
END_TEST

START_TEST(daycache_matches_reference_around_transitions)
{
	const char *tz = tricky_timezones[_i];
	const char *why = NULL;
	time_t day;

	use_timezone(tz);

	/*
	 * Sweep densely across every day whose midnight is not at offset 0 from
	 * the naive UTC day boundary -- that catches DST transitions wherever
	 * they sit -- plus all of February and March, which covers leap days.
	 */
	for (day = RANGE_START; day < RANGE_START + RANGE_SPAN; day += 86400) {
		struct tm tm_s;
		time_t t;

		if (localtime_r(&day, &tm_s) == NULL)
			continue;
		if (tm_s.tm_hour == 0 && tm_s.tm_mon != 1 && tm_s.tm_mon != 2)
			continue;

		for (t = day - 7200; t < day + 100000; t += 137) {
			if (disagrees_at(t, &why))
				ck_abort_msg("TZ=%s transition sweep: %s at %s", tz, why, describe(t));
		}
	}
}
END_TEST

/*
 * A timezone switch has to invalidate the cache: the same timestamp belongs to
 * a different local day afterwards. tzset() is what applies the switch, so the
 * cache watches the globals tzset() maintains.
 */
START_TEST(daycache_notices_timezone_change)
{
	static const char *zones[] = {
		"UTC", "Europe/Berlin", "America/New_York", "Pacific/Chatham",
		"Asia/Kolkata", "Europe/London", "Australia/Lord_Howe",
	};
	/* a fixed instant, deliberately queried again after each switch */
	const time_t when = 1750000000L;
	unsigned lap, i;

	use_timezone("UTC");

	for (lap = 0; lap < 3; lap++) {
		for (i = 0; i < ARRAY_SIZE(zones); i++) {
			struct reference_day ref;
			const struct day_cache *dc;

			/* note: no forget_cached_day() here, the cache has to
			 * work this out for itself */
			setenv("TZ", zones[i], 1);
			tzset();

			ref = reference_day_for(when);
			dc = get_day_cache(when);

			ck_assert_msg(dc != NULL, "cache returned NULL for TZ=%s", zones[i]);
			ck_assert_msg(dc->midnight == ref.midnight,
			              "TZ=%s: cache kept a stale midnight from the previous zone "
			              "(got %ld, expected %ld)",
			              zones[i], (long)dc->midnight, (long)ref.midnight);
			ck_assert_msg(dc->wday == ref.wday,
			              "TZ=%s: cache kept a stale weekday", zones[i]);
		}
	}
}
END_TEST

/*
 * Irregular days must fall through to the uncached path, ordinary ones must be
 * served from the cache. This pins down the classification itself, so that a
 * future change cannot quietly start caching a transition day.
 */
struct classification_case {
	const char *tz;
	int year, mon, mday;
	int expect_cached;
	const char *what;
};

static const struct classification_case classification_cases[] = {
	/* ordinary days, including ones that only look exotic */
	{ "Europe/Berlin",      2024,  2, 29, 1, "leap day" },
	{ "Europe/Berlin",      2023,  2, 28, 1, "non leap year February" },
	{ "Europe/Berlin",      2025,  7,  1, 1, "plain summer day" },
	{ "Pacific/Apia",       2011, 12, 31, 1, "day after the zone skipped 2011-12-30" },
	{ "Pacific/Kiritimati", 1995,  1,  1, 1, "day after the zone skipped 1994-12-31" },
	{ "Asia/Kolkata",       2025,  3, 30, 1, "half hour offset, no DST" },

	/* irregular days, must not be cached */
	{ "Europe/Berlin",      2025,  3, 30, 0, "DST spring forward, 23h" },
	{ "Europe/Berlin",      2025, 10, 26, 0, "DST fall back, 25h" },
	{ "America/Havana",     2025,  3,  9, 0, "DST transition at midnight" },
	{ "America/Santiago",   2025,  9,  7, 0, "DST transition at midnight" },
	{ "Asia/Beirut",        2025,  3, 30, 0, "DST transition at midnight" },
	{ "Australia/Lord_Howe", 2025, 10, 5, 0, "30 minute DST shift" },
	{ "right/Europe/Berlin", 2017, 1,  1, 0, "day carrying a leap second" },
};

START_TEST(daycache_classifies_irregular_days)
{
	const struct classification_case *c = &classification_cases[_i];
	struct tm tm_s;
	time_t noon;
	const struct day_cache *dc;

	use_timezone(c->tz);

	memset(&tm_s, 0, sizeof(tm_s));
	tm_s.tm_year = c->year - 1900;
	tm_s.tm_mon = c->mon - 1;
	tm_s.tm_mday = c->mday;
	tm_s.tm_hour = 12;
	tm_s.tm_isdst = -1;
	noon = mktime(&tm_s);
	ck_assert_msg(noon != (time_t) -1, "could not build %s %04d-%02d-%02d",
	              c->tz, c->year, c->mon, c->mday);

	dc = get_day_cache(noon);
	ck_assert_msg(dc != NULL, "cache returned NULL");

	if (c->expect_cached) {
		ck_assert_msg(dc->valid_until != 0,
		              "TZ=%s %04d-%02d-%02d (%s) should be cacheable but was not",
		              c->tz, c->year, c->mon, c->mday, c->what);
	} else {
		ck_assert_msg(dc->valid_until == 0,
		              "TZ=%s %04d-%02d-%02d (%s) must not be cached",
		              c->tz, c->year, c->mon, c->mday, c->what);
	}

	/* whatever the classification, the answer still has to be right */
	{
		struct reference_day ref = reference_day_for(noon);
		ck_assert_msg(dc->midnight == ref.midnight,
		              "TZ=%s %04d-%02d-%02d (%s): wrong midnight",
		              c->tz, c->year, c->mon, c->mday, c->what);
	}
}
END_TEST

Suite *daycache_suite(void)
{
	Suite *s = suite_create("Timeperiod day cache");
	TCase *tc_sweep = tcase_create("Agreement with the uncached computation");
	TCase *tc_class = tcase_create("Classification of irregular days");

	/* the sweeps are deliberately large; the default 4s timeout is not enough */
	tcase_set_timeout(tc_sweep, 300);
	tcase_add_loop_test(tc_sweep, daycache_matches_reference,
	                    0, ARRAY_SIZE(tricky_timezones));
	tcase_add_loop_test(tc_sweep, daycache_matches_reference_around_transitions,
	                    0, ARRAY_SIZE(tricky_timezones));
	tcase_add_test(tc_sweep, daycache_notices_timezone_change);
	suite_add_tcase(s, tc_sweep);

	tcase_add_loop_test(tc_class, daycache_classifies_irregular_days,
	                    0, ARRAY_SIZE(classification_cases));
	suite_add_tcase(s, tc_class);

	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(daycache_suite());

	srunner_run_all(sr, CK_ENV);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
