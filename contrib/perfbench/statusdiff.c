/*
 * statusdiff - a diagnostic event broker module
 *
 * Answers one question: when naemon sends a host or service status event,
 * which fields of the object have actually changed since the last event for
 * that same object?
 *
 * A scheduled check produces two status events, and the claim this module
 * exists to verify is that the first of the two -- the one
 * schedule_next_service_check() emits as NEBTYPE_SERVICESTATUS_SCHEDULE --
 * changes nothing a broker module would care about except next_check.
 *
 * The module keeps a snapshot of every field that the major broker modules
 * serialise, compares each incoming event against it, and counts how often
 * each field changed, broken down by NEBTYPE. On shutdown it writes a table
 * to the naemon log.
 *
 * This is a measurement tool, not something to run in production: it holds a
 * snapshot per object and its whole point is to do the redundant work that
 * the NEBTYPE split lets real modules avoid.
 *
 *   gcc -shared -fPIC -o statusdiff.so statusdiff.c \
 *       $(pkg-config --cflags naemon) -Wall -O2
 *
 *   broker_module=/path/to/statusdiff.so
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <naemon/naemon.h>

NEB_API_VERSION(CURRENT_NEB_API_VERSION)

static void *statusdiff_handle;

/*
 * The fields each of the two object types exposes to broker modules, in the
 * same selection Statusengine serialises. Listed once here and expanded into
 * a snapshot struct, a comparison and a counter table, so the three can never
 * drift apart.
 *
 * NUM covers everything comparable with !=, STR everything that needs strcmp
 * and a copy.
 */
#define COMMON_NUM_FIELDS(X)                                                 \
	/* Statusengine also emits should_be_scheduled, but that is a hardcoded  \
	 * Nagios-compatibility constant -- naemon has no such field */          \
	X(current_state) X(has_been_checked)                                     \
	X(current_attempt) X(max_attempts) X(last_check) X(next_check)           \
	X(check_type) X(last_state_change) X(last_hard_state_change)             \
	X(last_hard_state) X(state_type) X(last_notification)                    \
	X(next_notification) X(no_more_notifications)                            \
	X(notifications_enabled) X(problem_has_been_acknowledged)                \
	X(acknowledgement_type) X(current_notification_number)                   \
	X(accept_passive_checks) X(event_handler_enabled) X(checks_enabled)      \
	X(flap_detection_enabled) X(is_flapping) X(percent_state_change)         \
	X(latency) X(execution_time) X(scheduled_downtime_depth)                 \
	X(process_performance_data) X(obsess) X(modified_attributes)             \
	X(check_interval) X(retry_interval)                                      \
	/* not serialised by Statusengine, but written by                        \
	 * schedule_next_*_check(), so worth seeing in the table */              \
	X(check_options)

#define COMMON_STR_FIELDS(X)                                                 \
	X(plugin_output) X(long_plugin_output) X(event_handler) X(perf_data)     \
	X(check_command) X(check_period)

#define SVC_NUM_FIELDS(X)                                                    \
	COMMON_NUM_FIELDS(X)                                                     \
	X(last_time_ok) X(last_time_warning) X(last_time_critical)               \
	X(last_time_unknown)

#define HST_NUM_FIELDS(X)                                                    \
	COMMON_NUM_FIELDS(X)                                                     \
	X(last_time_up) X(last_time_down) X(last_time_unreachable)

#define SVC_STR_FIELDS(X) COMMON_STR_FIELDS(X)
#define HST_STR_FIELDS(X) COMMON_STR_FIELDS(X)

/*
 * One row per NEBTYPE we see. Types are sparse and few, so a short linear
 * array beats anything cleverer and keeps the output ordered by first sight.
 */
#define MAX_TYPES 8

#define DECLARE_TRACKER(name, NUM, STR, objtype)                             \
	struct name##_snapshot {                                                 \
		NUM(SNAP_NUM)                                                        \
		STR(SNAP_STR)                                                        \
		int seen;                                                            \
	};                                                                       \
	struct name##_counters {                                                 \
		int type;                                                            \
		unsigned long events;                                                \
		unsigned long nothing_changed;                                       \
		NUM(COUNTER)                                                         \
		STR(COUNTER)                                                         \
	};                                                                       \
	static struct name##_counters name##_counters[MAX_TYPES];                \
	static int name##_ntypes;

#define SNAP_NUM(f) double f;
#define SNAP_STR(f) char *f;
#define COUNTER(f) unsigned long f##_changed;

DECLARE_TRACKER(svc, SVC_NUM_FIELDS, SVC_STR_FIELDS, service)
DECLARE_TRACKER(hst, HST_NUM_FIELDS, HST_STR_FIELDS, host)

/*
 * Snapshots live on the object itself. Every host and service carries a
 * module-owned pointer slot for exactly this, which saves keeping a hash
 * table keyed on the object pointer.
 */
static struct svc_snapshot *svc_snapshots;
static struct hst_snapshot *hst_snapshots;
static unsigned int num_services, num_hosts;

static int str_differs(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a != b;
	return strcmp(a, b) != 0;
}

static void snap_str(char **slot, const char *val)
{
	free(*slot);
	*slot = val ? strdup(val) : NULL;
}

#define FIND_COUNTERS(name, type_)                                           \
	({                                                                       \
		struct name##_counters *c_ = NULL;                                   \
		int i_;                                                              \
		for (i_ = 0; i_ < name##_ntypes; i_++) {                             \
			if (name##_counters[i_].type == (type_)) {                       \
				c_ = &name##_counters[i_];                                   \
				break;                                                       \
			}                                                                \
		}                                                                    \
		if (c_ == NULL && name##_ntypes < MAX_TYPES) {                       \
			c_ = &name##_counters[name##_ntypes++];                          \
			c_->type = (type_);                                              \
		}                                                                    \
		c_;                                                                  \
	})

#define CMP_NUM(f)                                                           \
	do {                                                                     \
		if (snap->seen && snap->f != (double)obj->f) {                       \
			c->f##_changed++;                                                \
			changed++;                                                       \
		}                                                                    \
		snap->f = (double)obj->f;                                            \
	} while (0);

#define CMP_STR(f)                                                           \
	do {                                                                     \
		if (snap->seen && str_differs(snap->f, obj->f)) {                    \
			c->f##_changed++;                                                \
			changed++;                                                       \
		}                                                                    \
		snap_str(&snap->f, obj->f);                                          \
	} while (0);

static int handle_service_status(int cb_type, void *data)
{
	nebstruct_service_status_data *ds = (nebstruct_service_status_data *)data;
	service *obj = (service *)ds->object_ptr;
	struct svc_snapshot *snap;
	struct svc_counters *c;
	int changed = 0;

	(void)cb_type;
	if (obj == NULL || obj->id >= num_services)
		return 0;

	snap = &svc_snapshots[obj->id];
	c = FIND_COUNTERS(svc, ds->type);
	if (c == NULL)
		return 0;

	SVC_NUM_FIELDS(CMP_NUM)
	SVC_STR_FIELDS(CMP_STR)

	c->events++;
	if (snap->seen && changed == 0)
		c->nothing_changed++;
	snap->seen = 1;
	return 0;
}

static int handle_host_status(int cb_type, void *data)
{
	nebstruct_host_status_data *ds = (nebstruct_host_status_data *)data;
	host *obj = (host *)ds->object_ptr;
	struct hst_snapshot *snap;
	struct hst_counters *c;
	int changed = 0;

	(void)cb_type;
	if (obj == NULL || obj->id >= num_hosts)
		return 0;

	snap = &hst_snapshots[obj->id];
	c = FIND_COUNTERS(hst, ds->type);
	if (c == NULL)
		return 0;

	HST_NUM_FIELDS(CMP_NUM)
	HST_STR_FIELDS(CMP_STR)

	c->events++;
	if (snap->seen && changed == 0)
		c->nothing_changed++;
	snap->seen = 1;
	return 0;
}

#define REPORT(f)                                                            \
	if (c->f##_changed)                                                      \
		nm_log(NSLOG_INFO_MESSAGE, "statusdiff:     %-28s %10lu  %5.1f%%\n", \
		       #f, c->f##_changed,                                           \
		       100.0 * (double)c->f##_changed / (double)c->events);

#define DUMP(name, label, NUM, STR)                                          \
	do {                                                                     \
		int i;                                                               \
		for (i = 0; i < name##_ntypes; i++) {                                \
			struct name##_counters *c = &name##_counters[i];                 \
			if (c->events == 0)                                              \
				continue;                                                    \
			nm_log(NSLOG_INFO_MESSAGE,                                       \
			       "statusdiff: %s NEBTYPE %d: %lu events, "                 \
			       "%lu (%.1f%%) changed nothing at all\n",                  \
			       label, c->type, c->events, c->nothing_changed,            \
			       100.0 * (double)c->nothing_changed / (double)c->events);  \
			NUM(REPORT)                                                      \
			STR(REPORT)                                                      \
		}                                                                    \
	} while (0)

static void dump_report(void)
{
	nm_log(NSLOG_INFO_MESSAGE, "statusdiff: ---- field change report ----\n");
	DUMP(hst, "host", HST_NUM_FIELDS, HST_STR_FIELDS);
	DUMP(svc, "service", SVC_NUM_FIELDS, SVC_STR_FIELDS);
	nm_log(NSLOG_INFO_MESSAGE, "statusdiff: ---- end of report ----\n");
}

int nebmodule_init(int flags, char *args, nebmodule *handle)
{
	host *h;
	service *s;

	(void)flags;
	(void)args;
	statusdiff_handle = handle;

	neb_set_module_info(statusdiff_handle, NEBMODULE_MODINFO_TITLE, "statusdiff");
	neb_set_module_info(statusdiff_handle, NEBMODULE_MODINFO_DESC,
	                    "Reports which object fields each status NEBTYPE actually changes");

	/*
	 * Object ids are assigned densely from 0 at config load, so the highest
	 * id plus one sizes an array we can index directly.
	 */
	for (h = host_list; h; h = h->next)
		if (h->id + 1 > num_hosts)
			num_hosts = h->id + 1;
	for (s = service_list; s; s = s->next)
		if (s->id + 1 > num_services)
			num_services = s->id + 1;

	hst_snapshots = calloc(num_hosts ? num_hosts : 1, sizeof(*hst_snapshots));
	svc_snapshots = calloc(num_services ? num_services : 1, sizeof(*svc_snapshots));
	if (hst_snapshots == NULL || svc_snapshots == NULL) {
		nm_log(NSLOG_RUNTIME_ERROR, "statusdiff: out of memory\n");
		return -1;
	}

	neb_register_callback(NEBCALLBACK_HOST_STATUS_DATA, statusdiff_handle, 0, handle_host_status);
	neb_register_callback(NEBCALLBACK_SERVICE_STATUS_DATA, statusdiff_handle, 0, handle_service_status);

	nm_log(NSLOG_INFO_MESSAGE, "statusdiff: watching %u hosts and %u services\n",
	       num_hosts, num_services);
	return 0;
}

int nebmodule_deinit(int flags, int reason)
{
	(void)flags;
	(void)reason;

	dump_report();
	neb_deregister_module_callbacks(statusdiff_handle);
	free(hst_snapshots);
	free(svc_snapshots);
	return 0;
}
