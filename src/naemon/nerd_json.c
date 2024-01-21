#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lib/libnaemon.h"
#include "broker.h"
#include "nebmods.h"
#include "nebmodules.h"
#include "nebstructs.h"
#include "logging.h"
#include "nerd.h"
#include "nerd_json.h"
#ifdef HAVE_JSONC
#include <json-c/json.h>
#endif

static unsigned int chan_host_checks_json_id;

static int chan_host_checks_json(int cb, void *data)
{
	nebstruct_host_check_data *ds = (nebstruct_host_check_data *)data;

    char *raw_command = NULL;
    json_object *my_object;
    json_object *hostcheck_object;
    const char* json_string;

    nebstruct_host_check_data *current_hostcheck = ds;
    host *current_host = (host *)current_hostcheck->object_ptr;

	if (ds->type != NEBTYPE_HOSTCHECK_PROCESSED)
		return 0;

	if (nerd_channel_has_subscriptions(chan_host_checks_json_id) == 0)
		return 0;

    // Encode the current host check event as JSON string
    my_object = json_object_new_object();
    json_object_object_add(my_object, "type",      json_object_new_int(ds->type));
    json_object_object_add(my_object, "flags",     json_object_new_int(ds->flags));
    json_object_object_add(my_object, "attr",      json_object_new_int(ds->attr));
    json_object_object_add(my_object, "timestamp", json_object_new_int(ds->timestamp.tv_sec));


    hostcheck_object = json_object_new_object();

    HOSTCHECKFIELD_STRING(host_name);
    get_raw_command_line_r(get_global_macros(),current_host->check_command_ptr,current_host->check_command,&raw_command,0);
    json_object_object_add(hostcheck_object, "command_line", (raw_command != NULL ? json_object_new_string(raw_command) : NULL));
	json_object_object_add(hostcheck_object, "command_name", (current_host->check_command != NULL ? json_object_new_string(current_host->check_command) : NULL));

    HOSTCHECKFIELD_STRING(output);
    HOSTCHECKFIELD_STRING(long_output);
    HOSTCHECKFIELD_STRING(perf_data);
    HOSTCHECKFIELD_INT(check_type);
    HOSTCHECKFIELD_INT(current_attempt);
    HOSTCHECKFIELD_INT(max_attempts);
    HOSTCHECKFIELD_INT(state_type);
    HOSTCHECKFIELD_INT(state);
    HOSTCHECKFIELD_INT(timeout);
    json_object_object_add(hostcheck_object, "start_time", json_object_new_int64(current_hostcheck->start_time.tv_sec));
    json_object_object_add(hostcheck_object, "end_time", json_object_new_int64(current_hostcheck->end_time.tv_sec));
    HOSTCHECKFIELD_INT(early_timeout);
    HOSTCHECKFIELD_DOUBLE(execution_time);
    HOSTCHECKFIELD_DOUBLE(latency);
    HOSTCHECKFIELD_INT(return_code);


    json_object_object_add(my_object, "hostcheck", hostcheck_object);
    json_string = json_object_to_json_string(my_object);

    // Send JSON through the query handler socket
    nerd_broadcast(chan_host_checks_json_id, (void *)json_string, strlen(json_string));

	json_object_put(my_object);
    free(raw_command);
	return 0;
}

/* Add  JSON encoded channels into nerd if json-c lib is available */
int nerd_init_json(void)
{
    nm_log(NSLOG_INFO_MESSAGE, "nerd: This nerd has json Superpowers!\n");


	chan_host_checks_json_id = nerd_mkchan("hostchecks_json",
	                                  "Host check results encoded as JSON",
	                                  chan_host_checks_json, nebcallback_flag(NEBCALLBACK_HOST_CHECK_DATA));
	/*chan_service_checks_json_id = nerd_mkchan("servicechecks_json",
	                                     "Service check results encoded as JSON",
	                                     chan_service_checks, nebcallback_flag(NEBCALLBACK_SERVICE_CHECK_DATA));
*/
	return 0;
}