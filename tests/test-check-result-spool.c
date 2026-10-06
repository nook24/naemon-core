#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "naemon/shared.h"
#include "naemon/nm_alloc.h"

static char spool[] = "/tmp/naemon-spool-XXXXXX";

static void setup(void)
{
	ck_assert(mkdtemp(spool) != NULL);
}

static void teardown(void)
{
	rmdir(spool);
}

static char *path_in(const char *dir, const char *name)
{
	char *p;
	nm_asprintf(&p, "%s/%s", dir, name);
	return p;
}

/* A spool file another program shrinks while naemon reads it used to raise SIGBUS. */
START_TEST(file_shrinking_while_read)
{
	char *f = path_in(spool, "c000001");
	mmapfile *mf;
	char *line;
	FILE *fp;
	int i;

	fp = fopen(f, "w");
	ck_assert(fp != NULL);
	for (i = 0; i < 100000; i++)
		fprintf(fp, "output=line %d\n", i);
	fclose(fp);

	mf = mmap_fopen(f);
	ck_assert(mf != NULL);
	line = mmap_fgets(mf);
	ck_assert_str_eq(line, "output=line 0\n");
	free(line);

	ck_assert(truncate(f, 0) == 0);

	for (i = 0; (line = mmap_fgets(mf)) != NULL; i++)
		free(line);
	ck_assert_int_lt(i, 100000);
	mmap_fclose(mf);
	unlink(f);
	free(f);
}
END_TEST

int main(void)
{
	int failed;
	Suite *s = suite_create("Check result spool");
	TCase *tc = tcase_create("spool");
	SRunner *sr;

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, file_shrinking_while_read);
	suite_add_tcase(s, tc);

	sr = srunner_create(s);
	srunner_run_all(sr, CK_ENV);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
