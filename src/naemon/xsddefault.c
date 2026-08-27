#include "config.h"
#include "common.h"
#include "defaults.h"
#include "statusdata.h"
#include "comments.h"
#include "downtime.h"
#include "macros.h"
#include "xsddefault.h"
#include "utils.h"
#include "logging.h"
#include "globals.h"
#include "nm_alloc.h"
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

int buffer_stats[1][3];
int program_stats[MAX_CHECK_STATS_TYPES][3];

/******************************************************************/
/********************* INIT/CLEANUP FUNCTIONS *********************/
/******************************************************************/

/* initialize status data */
int xsddefault_initialize_status_data(const char *cfgfile)
{
	nagios_macros *mac;

	/* initialize locations if necessary */
	if (!status_file)
		status_file = nm_strdup(get_default_status_file());

	/* make sure we have what we need */
	if (status_file == NULL)
		return ERROR;

	mac = get_global_macros();
	/* save the status file macro */
	nm_free(mac->x[MACRO_STATUSDATAFILE]);
	mac->x[MACRO_STATUSDATAFILE] = nm_strdup(status_file);
	strip(mac->x[MACRO_STATUSDATAFILE]);

	/* delete the old status log (it might not exist) */
	if (status_file)
		unlink(status_file);

	return OK;
}


/* cleanup status data before terminating */
int xsddefault_cleanup_status_data(int delete_status_data)
{
	int return_code = OK;

	/* delete the status log */
	if (delete_status_data == TRUE && status_file) {
		if (unlink(status_file))
			return_code = ERROR;
	}

	nm_free(status_file);

	return return_code;
}


/******************************************************************/
/****************** STATUS DATA OUTPUT FUNCTIONS ******************/
/******************************************************************/

/* write all status data to file */
/*
 * Simple append buffer used for writing status data. Formatting the status
 * file with fprintf() showed up as ~50% of the event loop's CPU time on
 * installations with 100k services, almost all of it inside the printf
 * format-string interpreter. Emitting the (overwhelmingly integer and string)
 * fields directly into a large buffer avoids that entirely.
 */
struct statusbuf {
	char *buf;
	size_t len;
	size_t cap;
	int fd;
	int error;
};

#define STATUSBUF_SIZE (1024 * 1024)

static void sb_flush(struct statusbuf *sb)
{
	size_t off = 0;

	while (off < sb->len) {
		ssize_t n = write(sb->fd, sb->buf + off, sb->len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			sb->error = 1;
			break;
		}
		off += (size_t)n;
	}
	sb->len = 0;
}

/* make sure at least n bytes are available in the buffer */
static inline void sb_reserve(struct statusbuf *sb, size_t n)
{
	if (sb->len + n > sb->cap)
		sb_flush(sb);
	if (n > sb->cap) {
		/* single field larger than the buffer, grow to fit */
		sb->cap = n * 2;
		sb->buf = nm_realloc(sb->buf, sb->cap);
	}
}

static inline void sb_mem(struct statusbuf *sb, const char *s, size_t n)
{
	sb_reserve(sb, n);
	memcpy(sb->buf + sb->len, s, n);
	sb->len += n;
}

#define sb_lit(sb, s) sb_mem((sb), (s), sizeof(s) - 1)

static inline void sb_str(struct statusbuf *sb, const char *s)
{
	if (s != NULL)
		sb_mem(sb, s, strlen(s));
}

/* append an unsigned value, no padding */
static inline void sb_uint(struct statusbuf *sb, unsigned long long v)
{
	char tmp[24];
	int i = (int)sizeof(tmp);

	do {
		tmp[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v);
	sb_mem(sb, tmp + i, sizeof(tmp) - (size_t)i);
}

static inline void sb_int(struct statusbuf *sb, long long v)
{
	if (v < 0) {
		sb_lit(sb, "-");
		sb_uint(sb, (unsigned long long) - (v + 1) + 1ULL);
	} else {
		sb_uint(sb, (unsigned long long)v);
	}
}

/*
 * Doubles are still formatted by snprintf() -- reimplementing printf's
 * rounding would risk changing the file format. Instead the results are
 * memoized: the handful of double fields per object (check_interval,
 * retry_interval, execution time, latency, percent_state_change) repeat the
 * same few values across the whole object list, so a small direct-mapped
 * cache keyed on the bit pattern removes almost all of the printf calls
 * while producing exactly the bytes snprintf would have produced.
 */
#define SB_DBL_CACHE_SIZE 64
struct sb_dbl_cache_entry {
	uint64_t bits;
	const char *fmt;
	unsigned char len;
	char text[31];
};
static struct sb_dbl_cache_entry sb_dbl_cache[SB_DBL_CACHE_SIZE];

static void sb_dbl(struct statusbuf *sb, const char *fmt, double v)
{
	char tmp[64];
	uint64_t bits;
	unsigned idx;
	struct sb_dbl_cache_entry *e;
	int n;

	memcpy(&bits, &v, sizeof(bits));
	idx = (unsigned)((bits ^ (bits >> 32) ^ (uintptr_t)fmt) % SB_DBL_CACHE_SIZE);
	e = &sb_dbl_cache[idx];
	if (e->len && e->bits == bits && e->fmt == fmt) {
		sb_mem(sb, e->text, e->len);
		return;
	}

	n = snprintf(tmp, sizeof(tmp), fmt, v);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	sb_mem(sb, tmp, (size_t)n);

	/* only cache what fits, longer results are rare and not worth it */
	if ((size_t)n < sizeof(e->text)) {
		e->bits = bits;
		e->fmt = fmt;
		e->len = (unsigned char)n;
		memcpy(e->text, tmp, (size_t)n);
	}
}

/* "\tkey=value\n" helpers -- key is always a string literal */
#define sb_kv_str(sb, key, val)   do { sb_lit((sb), "\t" key "="); sb_str((sb), (val)); sb_lit((sb), "\n"); } while (0)
#define sb_kv_int(sb, key, val)   do { sb_lit((sb), "\t" key "="); sb_int((sb), (long long)(val)); sb_lit((sb), "\n"); } while (0)
#define sb_kv_uint(sb, key, val)  do { sb_lit((sb), "\t" key "="); sb_uint((sb), (unsigned long long)(val)); sb_lit((sb), "\n"); } while (0)
#define sb_kv_dbl(sb, key, fmt, val) do { sb_lit((sb), "\t" key "="); sb_dbl((sb), (fmt), (double)(val)); sb_lit((sb), "\n"); } while (0)

/* generic fallback for the few rare/complex lines */
static void sb_printf(struct statusbuf *sb, const char *fmt, ...)
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
		sb_mem(sb, tmp, (size_t)n);
		return;
	}
	{
		char *big = nm_malloc((size_t)n + 1);
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		sb_mem(sb, big, (size_t)n);
		free(big);
	}
}

/* "\t_NAME=MODIFIED;VALUE\n" */
static void sb_customvar(struct statusbuf *sb, const customvariablesmember *cv)
{
	sb_lit(sb, "\t_");
	sb_str(sb, cv->variable_name);
	sb_lit(sb, "=");
	sb_int(sb, cv->has_been_modified);
	sb_lit(sb, ";");
	sb_str(sb, cv->variable_value);
	sb_lit(sb, "\n");
}

int xsddefault_save_status_data(void)
{
	char *tmp_log = NULL;
	customvariablesmember *temp_customvariablesmember = NULL;
	host *temp_host = NULL;
	service *temp_service = NULL;
	contact *temp_contact = NULL;
	comment *temp_comment = NULL;
	GHashTableIter iter;
	gpointer comment_;
	scheduled_downtime *temp_downtime = NULL;
	time_t current_time;
	int fd = 0;
	struct statusbuf sb;
	int result = OK;

	/* users may not want us to write status data */
	if (!status_file || !strcmp(status_file, "/dev/null"))
		return OK;

	nm_asprintf(&tmp_log, "%sXXXXXX", status_file);
	if (tmp_log == NULL)
		return ERROR;

	log_debug_info(DEBUGL_STATUSDATA, 2, "Writing status data to temp file '%s'\n", tmp_log);

	if ((fd = mkstemp(tmp_log)) == -1) {

		/* log an error */
		nm_log(NSLOG_RUNTIME_ERROR, "Error: Unable to create temp file '%s' for writing status data: %s\n", tmp_log, strerror(errno));

		nm_free(tmp_log);

		return ERROR;
	}

	sb.buf = nm_malloc(STATUSBUF_SIZE);
	sb.cap = STATUSBUF_SIZE;
	sb.len = 0;
	sb.fd = fd;
	sb.error = 0;

	/* generate check statistics */
	generate_check_stats();

	/* write version info to status file */
	sb_lit(&sb, "########################################\n");
	sb_lit(&sb, "#          NAGIOS STATUS FILE\n");
	sb_lit(&sb, "#\n");
	sb_lit(&sb, "# THIS FILE IS AUTOMATICALLY GENERATED\n");
	sb_lit(&sb, "# BY NAGIOS.  DO NOT MODIFY THIS FILE!\n");
	sb_lit(&sb, "########################################\n\n");

	time(&current_time);

	/* write file info */
	sb_lit(&sb, "info {\n");
	sb_kv_uint(&sb, "created", current_time);
	sb_lit(&sb, "\tversion=" VERSION "\n");
	sb_lit(&sb, "\t}\n\n");

	/* save program status data */
	sb_lit(&sb, "programstatus {\n");
	sb_kv_uint(&sb, "modified_host_attributes", modified_host_process_attributes);
	sb_kv_uint(&sb, "modified_service_attributes", modified_service_process_attributes);
	sb_kv_int(&sb, "nagios_pid", nagios_pid);
	sb_kv_int(&sb, "daemon_mode", daemon_mode);
	sb_kv_uint(&sb, "program_start", program_start);
	sb_kv_uint(&sb, "last_log_rotation", last_log_rotation);
	sb_kv_int(&sb, "enable_notifications", enable_notifications);
	sb_kv_int(&sb, "active_service_checks_enabled", execute_service_checks);
	sb_kv_int(&sb, "passive_service_checks_enabled", accept_passive_service_checks);
	sb_kv_int(&sb, "active_host_checks_enabled", execute_host_checks);
	sb_kv_int(&sb, "passive_host_checks_enabled", accept_passive_host_checks);
	sb_kv_int(&sb, "enable_event_handlers", enable_event_handlers);
	sb_kv_int(&sb, "obsess_over_services", obsess_over_services);
	sb_kv_int(&sb, "obsess_over_hosts", obsess_over_hosts);
	sb_kv_int(&sb, "check_service_freshness", check_service_freshness);
	sb_kv_int(&sb, "check_host_freshness", check_host_freshness);
	sb_kv_int(&sb, "enable_flap_detection", enable_flap_detection);
	sb_kv_int(&sb, "process_performance_data", process_performance_data);
	sb_kv_str(&sb, "global_host_event_handler", global_host_event_handler);
	sb_kv_str(&sb, "global_service_event_handler", global_service_event_handler);
	sb_kv_str(&sb, "global_host_notification_handler", global_host_notification_handler);
	sb_kv_str(&sb, "global_service_notification_handler", global_service_notification_handler);
	sb_kv_uint(&sb, "next_comment_id", next_comment_id);
	sb_kv_uint(&sb, "next_downtime_id", next_downtime_id);
	sb_kv_uint(&sb, "next_event_id", next_event_id);
	sb_printf(&sb, "\tactive_scheduled_host_check_stats=%d,%d,%d\n", check_statistics[ACTIVE_SCHEDULED_HOST_CHECK_STATS].minute_stats[0], check_statistics[ACTIVE_SCHEDULED_HOST_CHECK_STATS].minute_stats[1], check_statistics[ACTIVE_SCHEDULED_HOST_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\tactive_ondemand_host_check_stats=%d,%d,%d\n", check_statistics[ACTIVE_ONDEMAND_HOST_CHECK_STATS].minute_stats[0], check_statistics[ACTIVE_ONDEMAND_HOST_CHECK_STATS].minute_stats[1], check_statistics[ACTIVE_ONDEMAND_HOST_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\tpassive_host_check_stats=%d,%d,%d\n", check_statistics[PASSIVE_HOST_CHECK_STATS].minute_stats[0], check_statistics[PASSIVE_HOST_CHECK_STATS].minute_stats[1], check_statistics[PASSIVE_HOST_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\tactive_scheduled_service_check_stats=%d,%d,%d\n", check_statistics[ACTIVE_SCHEDULED_SERVICE_CHECK_STATS].minute_stats[0], check_statistics[ACTIVE_SCHEDULED_SERVICE_CHECK_STATS].minute_stats[1], check_statistics[ACTIVE_SCHEDULED_SERVICE_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\tactive_ondemand_service_check_stats=%d,%d,%d\n", check_statistics[ACTIVE_ONDEMAND_SERVICE_CHECK_STATS].minute_stats[0], check_statistics[ACTIVE_ONDEMAND_SERVICE_CHECK_STATS].minute_stats[1], check_statistics[ACTIVE_ONDEMAND_SERVICE_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\tpassive_service_check_stats=%d,%d,%d\n", check_statistics[PASSIVE_SERVICE_CHECK_STATS].minute_stats[0], check_statistics[PASSIVE_SERVICE_CHECK_STATS].minute_stats[1], check_statistics[PASSIVE_SERVICE_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\tcached_host_check_stats=%d,%d,%d\n", check_statistics[ACTIVE_CACHED_HOST_CHECK_STATS].minute_stats[0], check_statistics[ACTIVE_CACHED_HOST_CHECK_STATS].minute_stats[1], check_statistics[ACTIVE_CACHED_HOST_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\tcached_service_check_stats=%d,%d,%d\n", check_statistics[ACTIVE_CACHED_SERVICE_CHECK_STATS].minute_stats[0], check_statistics[ACTIVE_CACHED_SERVICE_CHECK_STATS].minute_stats[1], check_statistics[ACTIVE_CACHED_SERVICE_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\texternal_command_stats=%d,%d,%d\n", check_statistics[EXTERNAL_COMMAND_STATS].minute_stats[0], check_statistics[EXTERNAL_COMMAND_STATS].minute_stats[1], check_statistics[EXTERNAL_COMMAND_STATS].minute_stats[2]);

	sb_printf(&sb, "\tparallel_host_check_stats=%d,%d,%d\n", check_statistics[PARALLEL_HOST_CHECK_STATS].minute_stats[0], check_statistics[PARALLEL_HOST_CHECK_STATS].minute_stats[1], check_statistics[PARALLEL_HOST_CHECK_STATS].minute_stats[2]);
	sb_printf(&sb, "\tserial_host_check_stats=%d,%d,%d\n", check_statistics[SERIAL_HOST_CHECK_STATS].minute_stats[0], check_statistics[SERIAL_HOST_CHECK_STATS].minute_stats[1], check_statistics[SERIAL_HOST_CHECK_STATS].minute_stats[2]);
	sb_lit(&sb, "\t}\n\n");


	/* save host status data */
	for (temp_host = host_list; temp_host != NULL; temp_host = temp_host->next) {

		sb_lit(&sb, "hoststatus {\n");
		sb_kv_str(&sb, "host_name", temp_host->name);

		sb_kv_uint(&sb, "modified_attributes", temp_host->modified_attributes);
		sb_kv_str(&sb, "check_command", temp_host->check_command);
		sb_kv_str(&sb, "check_period", temp_host->check_period);
		sb_kv_str(&sb, "notification_period", temp_host->notification_period);
		sb_kv_dbl(&sb, "check_interval", "%f", temp_host->check_interval);
		sb_kv_dbl(&sb, "retry_interval", "%f", temp_host->retry_interval);
		sb_kv_str(&sb, "event_handler", temp_host->event_handler);

		sb_kv_int(&sb, "has_been_checked", temp_host->has_been_checked);
		sb_kv_dbl(&sb, "check_execution_time", "%.3f", temp_host->execution_time);
		sb_kv_dbl(&sb, "check_latency", "%.3f", temp_host->latency);
		sb_kv_int(&sb, "check_type", temp_host->check_type);
		sb_kv_int(&sb, "current_state", temp_host->current_state);
		sb_kv_int(&sb, "last_hard_state", temp_host->last_hard_state);
		sb_kv_uint(&sb, "last_event_id", temp_host->last_event_id);
		sb_kv_uint(&sb, "current_event_id", temp_host->current_event_id);
		sb_kv_str(&sb, "current_problem_id", temp_host->current_problem_id);
		sb_kv_str(&sb, "last_problem_id", temp_host->last_problem_id);
		sb_kv_uint(&sb, "problem_start", temp_host->problem_start);
		sb_kv_uint(&sb, "problem_end", temp_host->problem_end);
		sb_kv_str(&sb, "plugin_output", temp_host->plugin_output);
		sb_kv_str(&sb, "long_plugin_output", temp_host->long_plugin_output);
		sb_kv_str(&sb, "performance_data", temp_host->perf_data);
		sb_kv_uint(&sb, "last_check", temp_host->last_check);
		sb_kv_uint(&sb, "next_check", temp_host->next_check);
		sb_kv_int(&sb, "check_options", temp_host->check_options);
		sb_kv_int(&sb, "current_attempt", temp_host->current_attempt);
		sb_kv_int(&sb, "max_attempts", temp_host->max_attempts);
		sb_kv_int(&sb, "state_type", temp_host->state_type);
		sb_kv_uint(&sb, "last_state_change", temp_host->last_state_change);
		sb_kv_uint(&sb, "last_hard_state_change", temp_host->last_hard_state_change);
		sb_kv_uint(&sb, "last_time_up", temp_host->last_time_up);
		sb_kv_uint(&sb, "last_time_down", temp_host->last_time_down);
		sb_kv_uint(&sb, "last_time_unreachable", temp_host->last_time_unreachable);
		sb_kv_uint(&sb, "last_notification", temp_host->last_notification);
		sb_kv_uint(&sb, "next_notification", temp_host->next_notification);
		sb_kv_int(&sb, "no_more_notifications", temp_host->no_more_notifications);
		sb_kv_int(&sb, "current_notification_number", temp_host->current_notification_number);
		sb_kv_str(&sb, "current_notification_id", temp_host->current_notification_id);
		sb_kv_int(&sb, "notifications_enabled", temp_host->notifications_enabled);
		sb_kv_int(&sb, "problem_has_been_acknowledged", temp_host->problem_has_been_acknowledged);
		sb_kv_int(&sb, "acknowledgement_type", temp_host->acknowledgement_type);
		sb_kv_uint(&sb, "acknowledgement_end_time", temp_host->acknowledgement_end_time);
		sb_kv_int(&sb, "active_checks_enabled", temp_host->checks_enabled);
		sb_kv_int(&sb, "passive_checks_enabled", temp_host->accept_passive_checks);
		sb_kv_int(&sb, "event_handler_enabled", temp_host->event_handler_enabled);
		sb_kv_int(&sb, "flap_detection_enabled", temp_host->flap_detection_enabled);
		sb_kv_int(&sb, "process_performance_data", temp_host->process_performance_data);
		sb_kv_int(&sb, "obsess", temp_host->obsess);
		sb_kv_int(&sb, "is_flapping", temp_host->is_flapping);
		sb_kv_dbl(&sb, "percent_state_change", "%.2f", temp_host->percent_state_change);
		sb_kv_int(&sb, "scheduled_downtime_depth", temp_host->scheduled_downtime_depth);
		sb_kv_str(&sb, "last_update", tv_str(&temp_host->last_update));
		/* custom variables */
		for (temp_customvariablesmember = temp_host->custom_variables; temp_customvariablesmember != NULL; temp_customvariablesmember = temp_customvariablesmember->next) {
			if (temp_customvariablesmember->variable_name)
				sb_customvar(&sb, temp_customvariablesmember);
		}
		sb_lit(&sb, "\t}\n\n");
	}

	/* save service status data */
	for (temp_service = service_list; temp_service != NULL; temp_service = temp_service->next) {

		sb_lit(&sb, "servicestatus {\n");
		sb_kv_str(&sb, "host_name", temp_service->host_name);

		sb_kv_str(&sb, "service_description", temp_service->description);
		sb_kv_uint(&sb, "modified_attributes", temp_service->modified_attributes);
		sb_kv_str(&sb, "check_command", temp_service->check_command);
		sb_kv_str(&sb, "check_period", temp_service->check_period);
		sb_kv_str(&sb, "notification_period", temp_service->notification_period);
		sb_kv_dbl(&sb, "check_interval", "%f", temp_service->check_interval);
		sb_kv_dbl(&sb, "retry_interval", "%f", temp_service->retry_interval);
		sb_kv_str(&sb, "event_handler", temp_service->event_handler);

		sb_kv_int(&sb, "has_been_checked", temp_service->has_been_checked);
		sb_kv_dbl(&sb, "check_execution_time", "%.3f", temp_service->execution_time);
		sb_kv_dbl(&sb, "check_latency", "%.3f", temp_service->latency);
		sb_kv_int(&sb, "check_type", temp_service->check_type);
		sb_kv_int(&sb, "current_state", temp_service->current_state);
		sb_kv_int(&sb, "last_hard_state", temp_service->last_hard_state);
		sb_kv_uint(&sb, "last_event_id", temp_service->last_event_id);
		sb_kv_uint(&sb, "current_event_id", temp_service->current_event_id);
		sb_kv_str(&sb, "current_problem_id", temp_service->current_problem_id);
		sb_kv_str(&sb, "last_problem_id", temp_service->last_problem_id);
		sb_kv_uint(&sb, "problem_start", temp_service->problem_start);
		sb_kv_uint(&sb, "problem_end", temp_service->problem_end);
		sb_kv_int(&sb, "current_attempt", temp_service->current_attempt);
		sb_kv_int(&sb, "max_attempts", temp_service->max_attempts);
		sb_kv_int(&sb, "state_type", temp_service->state_type);
		sb_kv_uint(&sb, "last_state_change", temp_service->last_state_change);
		sb_kv_uint(&sb, "last_hard_state_change", temp_service->last_hard_state_change);
		sb_kv_uint(&sb, "last_time_ok", temp_service->last_time_ok);
		sb_kv_uint(&sb, "last_time_warning", temp_service->last_time_warning);
		sb_kv_uint(&sb, "last_time_unknown", temp_service->last_time_unknown);
		sb_kv_uint(&sb, "last_time_critical", temp_service->last_time_critical);
		sb_kv_str(&sb, "plugin_output", temp_service->plugin_output);
		sb_kv_str(&sb, "long_plugin_output", temp_service->long_plugin_output);
		sb_kv_str(&sb, "performance_data", temp_service->perf_data);
		sb_kv_uint(&sb, "last_check", temp_service->last_check);
		sb_kv_uint(&sb, "next_check", temp_service->next_check);
		sb_kv_int(&sb, "check_options", temp_service->check_options);
		sb_kv_int(&sb, "current_notification_number", temp_service->current_notification_number);
		sb_kv_str(&sb, "current_notification_id", temp_service->current_notification_id);
		sb_kv_uint(&sb, "last_notification", temp_service->last_notification);
		sb_kv_uint(&sb, "next_notification", temp_service->next_notification);
		sb_kv_int(&sb, "no_more_notifications", temp_service->no_more_notifications);
		sb_kv_int(&sb, "notifications_enabled", temp_service->notifications_enabled);
		sb_kv_int(&sb, "active_checks_enabled", temp_service->checks_enabled);
		sb_kv_int(&sb, "passive_checks_enabled", temp_service->accept_passive_checks);
		sb_kv_int(&sb, "event_handler_enabled", temp_service->event_handler_enabled);
		sb_kv_int(&sb, "problem_has_been_acknowledged", temp_service->problem_has_been_acknowledged);
		sb_kv_int(&sb, "acknowledgement_type", temp_service->acknowledgement_type);
		sb_kv_uint(&sb, "acknowledgement_end_time", temp_service->acknowledgement_end_time);
		sb_kv_int(&sb, "flap_detection_enabled", temp_service->flap_detection_enabled);
		sb_kv_int(&sb, "process_performance_data", temp_service->process_performance_data);
		sb_kv_int(&sb, "obsess", temp_service->obsess);
		sb_kv_int(&sb, "is_flapping", temp_service->is_flapping);
		sb_kv_dbl(&sb, "percent_state_change", "%.2f", temp_service->percent_state_change);
		sb_kv_int(&sb, "scheduled_downtime_depth", temp_service->scheduled_downtime_depth);
		sb_kv_str(&sb, "last_update", tv_str(&temp_service->last_update));
		/* custom variables */
		for (temp_customvariablesmember = temp_service->custom_variables; temp_customvariablesmember != NULL; temp_customvariablesmember = temp_customvariablesmember->next) {
			if (temp_customvariablesmember->variable_name)
				sb_customvar(&sb, temp_customvariablesmember);
		}
		sb_lit(&sb, "\t}\n\n");
	}

	/* save contact status data */
	for (temp_contact = contact_list; temp_contact != NULL; temp_contact = temp_contact->next) {

		sb_lit(&sb, "contactstatus {\n");
		sb_kv_str(&sb, "contact_name", temp_contact->name);

		sb_kv_uint(&sb, "modified_attributes", temp_contact->modified_attributes);
		sb_kv_uint(&sb, "modified_host_attributes", temp_contact->modified_host_attributes);
		sb_kv_uint(&sb, "modified_service_attributes", temp_contact->modified_service_attributes);
		sb_kv_str(&sb, "host_notification_period", temp_contact->host_notification_period);
		sb_kv_str(&sb, "service_notification_period", temp_contact->service_notification_period);

		sb_kv_uint(&sb, "last_host_notification", temp_contact->last_host_notification);
		sb_kv_uint(&sb, "last_service_notification", temp_contact->last_service_notification);
		sb_kv_int(&sb, "host_notifications_enabled", temp_contact->host_notifications_enabled);
		sb_kv_int(&sb, "service_notifications_enabled", temp_contact->service_notifications_enabled);
		/* custom variables */
		for (temp_customvariablesmember = temp_contact->custom_variables; temp_customvariablesmember != NULL; temp_customvariablesmember = temp_customvariablesmember->next) {
			if (temp_customvariablesmember->variable_name)
				sb_customvar(&sb, temp_customvariablesmember);
		}
		sb_lit(&sb, "\t}\n\n");
	}

	/* save all comments */
	if(comment_hashtable != NULL) {
		g_hash_table_iter_init(&iter, comment_hashtable);
		while (g_hash_table_iter_next(&iter, NULL, &comment_)) {
			temp_comment = comment_;
			if (temp_comment->comment_type == HOST_COMMENT)
				sb_lit(&sb, "hostcomment {\n");
			else
				sb_lit(&sb, "servicecomment {\n");
			sb_kv_str(&sb, "host_name", temp_comment->host_name);
			if (temp_comment->comment_type == SERVICE_COMMENT)
				sb_kv_str(&sb, "service_description", temp_comment->service_description);
			sb_kv_int(&sb, "entry_type", temp_comment->entry_type);
			sb_kv_uint(&sb, "comment_id", temp_comment->comment_id);
			sb_kv_int(&sb, "source", temp_comment->source);
			sb_kv_int(&sb, "persistent", temp_comment->persistent);
			sb_kv_uint(&sb, "entry_time", temp_comment->entry_time);
			sb_kv_int(&sb, "expires", temp_comment->expires);
			sb_kv_uint(&sb, "expire_time", temp_comment->expire_time);
			sb_kv_str(&sb, "author", temp_comment->author);
			sb_kv_str(&sb, "comment_data", temp_comment->comment_data);
			sb_lit(&sb, "\t}\n\n");
		}
	}

	/* save all downtime */
	for (temp_downtime = scheduled_downtime_list; temp_downtime != NULL; temp_downtime = temp_downtime->next) {

		if (temp_downtime->type == HOST_DOWNTIME)
			sb_lit(&sb, "hostdowntime {\n");
		else
			sb_lit(&sb, "servicedowntime {\n");
		sb_kv_str(&sb, "host_name", temp_downtime->host_name);
		if (temp_downtime->type == SERVICE_DOWNTIME)
			sb_kv_str(&sb, "service_description", temp_downtime->service_description);
		sb_kv_uint(&sb, "downtime_id", temp_downtime->downtime_id);
		sb_kv_uint(&sb, "comment_id", temp_downtime->comment_id);
		sb_kv_uint(&sb, "entry_time", temp_downtime->entry_time);
		sb_kv_uint(&sb, "start_time", temp_downtime->start_time);
		sb_kv_uint(&sb, "flex_downtime_start", temp_downtime->flex_downtime_start);
		sb_kv_uint(&sb, "end_time", temp_downtime->end_time);
		sb_kv_uint(&sb, "triggered_by", temp_downtime->triggered_by);
		sb_kv_int(&sb, "fixed", temp_downtime->fixed);
		sb_kv_uint(&sb, "duration", temp_downtime->duration);
		sb_kv_int(&sb, "is_in_effect", temp_downtime->is_in_effect);
		sb_kv_int(&sb, "start_notification_sent", temp_downtime->start_notification_sent);
		sb_kv_str(&sb, "author", temp_downtime->author);
		sb_kv_str(&sb, "comment", temp_downtime->comment);
		sb_lit(&sb, "\t}\n\n");
	}


	/* flush the remaining buffer to disk */
	sb_flush(&sb);
	nm_free(sb.buf);

	/* reset file permissions */
	fchmod(fd, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);

	/* fsync the file so that it is completely written out before moving it */
	fsync(fd);

	/* close the temp file */
	result = sb.error | close(fd);

	/* save/close was successful */
	if (result == 0) {

		result = OK;

		/* move the temp file to the status log (overwrite the old status log) */
		if (my_rename(tmp_log, status_file)) {
			unlink(tmp_log);
			nm_log(NSLOG_RUNTIME_ERROR, "Error: Unable to update status data file '%s': %s", status_file, strerror(errno));
			result = ERROR;
		}
	}

	/* a problem occurred saving the file */
	else {

		result = ERROR;

		/* remove temp file and log an error */
		unlink(tmp_log);
		nm_log(NSLOG_RUNTIME_ERROR, "Error: Unable to save status file: %s", strerror(errno));
	}

	nm_free(tmp_log);

	return result;
}
