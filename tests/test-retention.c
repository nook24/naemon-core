#include <check.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "naemon/objects_service.h"
#include "naemon/objects_command.h"
#include "naemon/objects_host.h"
#include "naemon/downtime.h"
#include "naemon/events.h"
#include "naemon/xrddefault.c"

#define TARGET_SERVICE_NAME "my_service"
#define TARGET_HOST_NAME "my_host"

static host *hst;
static command *cmd;
static service *svc;

/* This is separate due to it being required from inside the tests. */
void setup_objects(void)
{

	init_objects_command(1);
	cmd = create_command("my_command", "/bin/true");
	ck_assert(cmd != NULL);
	register_command(cmd);

	init_objects_host(1);
	hst = create_host(TARGET_HOST_NAME);
	ck_assert(hst != NULL);
	hst->check_command_ptr = cmd;
	hst->retain_status_information = TRUE;
	register_host(hst);

	init_objects_service(1);
	svc = create_service(hst, TARGET_SERVICE_NAME);
	ck_assert(svc != NULL);
	svc->check_command_ptr = cmd;
	svc->retain_status_information = TRUE;
	register_service(svc);

}

void teardown_objects(void)
{

	destroy_objects_command();
	destroy_objects_host();
	destroy_objects_service(TRUE);

}

void setup(void)
{

	init_event_queue();
	setup_objects();

	retain_state_information = TRUE;
	retention_file = nm_strdup("/tmp/retention.dat");
	temp_file = nm_strdup("/tmp/retention.tmp");

	initialize_retention_data();

}

void teardown(void)
{

	teardown_objects();
	cleanup_retention_data();
	destroy_event_queue();

}

START_TEST(retention_data_for_hosts_long_output)
{

	char *long_output = g_strescape("This is a long \n plugin output \n of some sort \n and such \n", "");

	hst->long_plugin_output = strdup(long_output);

	ck_assert(OK == save_state_information(0)); /* this parameter does nothing ... */

	teardown_objects();
	setup_objects();

	ck_assert(OK == read_initial_state_information());
	ck_assert_str_eq(hst->long_plugin_output, long_output);

	g_free(long_output);

}
END_TEST

START_TEST(retention_data_for_services_long_output)
{

	char *long_output = g_strescape("This is a long \n plugin output \n of some sort \n and such \n", "");

	svc->long_plugin_output = strdup(long_output);

	ck_assert(OK == save_state_information(0));

	teardown_objects();
	setup_objects();

	ck_assert(OK == read_initial_state_information());
	ck_assert_str_eq(svc->long_plugin_output, long_output);

	g_free(long_output);

}
END_TEST

/* what xrddefault_read_state_information() returns for a given file */
struct retention_case {
	const char *what;
	const char *content;   /* NULL = no file at all */
	int expect;
};

static const struct retention_case retention_cases[] = {
	{ "missing file", NULL, OK },
	{ "valid file", "# comment\n\ninfo {\ncreated=1\n}\n\nprogram {\nmodified_host_attributes=0\n}\n", OK },
	{ "line without '='", "program {\nmodified_host_attributes=0\ngarbage\n}\n", ERROR },
	{ "'}' outside of a block", "info {\ncreated=1\n}\n}\n", ERROR },
	{ "unknown block", "unknown {\nx=1\n}\n", ERROR },
	{ "line outside of a block", "info {\ncreated=1\n}\ngarbage=1\n", ERROR },
	{ "block inside a block", "program {\nhost {\nhost_name=my_host\n}\n", ERROR },
	{ "file ends inside a block", "info {\ncreated=1\n}\nprogram {\nmodified_host_attributes=0\n", ERROR },
};

START_TEST(retention_damage_detected)
{
	const struct retention_case *c = &retention_cases[_i];
	char path[] = "/tmp/naemon-retention-XXXXXX";
	int fd = mkstemp(path);

	ck_assert(fd >= 0);
	if (c->content) {
		ck_assert(write(fd, c->content, strlen(c->content)) == (ssize_t)strlen(c->content));
	} else {
		unlink(path);
	}
	close(fd);

	nm_free(retention_file);
	retention_file = nm_strdup(path);
	ck_assert_msg(read_initial_state_information() == c->expect, "%s: expected %s", c->what, c->expect == OK ? "OK" : "ERROR");
	unlink(path);
}
END_TEST

/* without retention_strict_loading the undamaged parts are still restored */
START_TEST(retention_damage_keeps_the_rest)
{
	FILE *fp;

	hst->long_plugin_output = strdup("restored");
	ck_assert(OK == save_state_information(0));
	fp = fopen(retention_file, "a");
	ck_assert(fp != NULL);
	fputs("garbage outside of any block\n", fp);
	fclose(fp);

	teardown_objects();
	setup_objects();

	ck_assert(ERROR == read_initial_state_information());
	ck_assert_str_eq(hst->long_plugin_output, "restored");
}
END_TEST

Suite *
retention_suite(void)
{
	Suite *s = suite_create("Retention data");

	TCase *tc_retention_data_for_hosts_long_output = tcase_create("Retention data for hosts");
	TCase *tc_retention_data_for_services_long_output = tcase_create("Retention data for services");

	tcase_add_checked_fixture(tc_retention_data_for_hosts_long_output, setup, teardown);
	tcase_add_checked_fixture(tc_retention_data_for_services_long_output, setup, teardown);

	tcase_add_test(tc_retention_data_for_hosts_long_output, retention_data_for_hosts_long_output);
	tcase_add_test(tc_retention_data_for_services_long_output, retention_data_for_services_long_output);

	suite_add_tcase(s, tc_retention_data_for_hosts_long_output);
	suite_add_tcase(s, tc_retention_data_for_services_long_output);

	{
		TCase *tc_damage = tcase_create("Damaged retention data");
		tcase_add_checked_fixture(tc_damage, setup, teardown);
		tcase_add_loop_test(tc_damage, retention_damage_detected, 0, sizeof(retention_cases) / sizeof(retention_cases[0]));
		tcase_add_test(tc_damage, retention_damage_keeps_the_rest);
		suite_add_tcase(s, tc_damage);
	}
	return s;
}

int main(void)
{
	int number_failed = 0;
	Suite *s = retention_suite();
	SRunner *sr = srunner_create(s);
	srunner_run_all(sr, CK_ENV);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
