#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lib/libnaemon.h"
#include "broker.h"
#include "nebmods.h"
#include "nebmodules.h"
#include "nebstructs.h"
#include "nebstructs_json.h"

#ifdef HAVE_JSONC
#include <json-c/json.h>
#endif

/*
 * Convert nebstruct data into json_objects
 *
 * This file provides functions that can convert nebstruct data into
 * json oecjts. To goal of this is to help developers to faster create
 * Event Broker Modules.
 * 
 * Most of this code is stolen (with permission) from the
 * Statusengine Event Broker, which is also a good referenfe if you want to
 * add missing functions
 * https://github.com/statusengine/module/blob/master/src/statusengine.c
 */


// This function converts host_check_data into a json_object
// Remember to call json_object_put() an pass the json_object to free up the memory
json_object *nebstruct_encode_host_check_as_json(nebstruct_host_check_data *ds) {
    json_object *my_object;
    json_object *hostcheck_object;
    nebstruct_host_check_data *current_hostcheck;

    char *raw_command;
    host *current_host;

    my_object = json_object_new_object();
    hostcheck_object = json_object_new_object();

    json_object_object_add(my_object, "type",      json_object_new_int(ds->type));
    json_object_object_add(my_object, "flags",     json_object_new_int(ds->flags));
    json_object_object_add(my_object, "attr",      json_object_new_int(ds->attr));
    json_object_object_add(my_object, "timestamp", json_object_new_int(ds->timestamp.tv_sec));

    current_host = (host *)ds->object_ptr;
    current_hostcheck = ds;

    HOSTCHECKFIELD_STRING(host_name);

    raw_command = NULL;
    get_raw_command_line_r(get_global_macros(), current_host->check_command_ptr, current_host->check_command, &raw_command, 0);

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
    json_object_object_add(hostcheck_object, "start_time", json_object_new_int64(ds->start_time.tv_sec));
    json_object_object_add(hostcheck_object, "end_time", json_object_new_int64(ds->end_time.tv_sec));
    HOSTCHECKFIELD_INT(early_timeout);
    HOSTCHECKFIELD_DOUBLE(execution_time);
    HOSTCHECKFIELD_DOUBLE(latency);
    HOSTCHECKFIELD_INT(return_code);

    json_object_object_add(my_object, "hostcheck", hostcheck_object);

    free(raw_command);

    return my_object;
}


// This function converts service_check_data into a json_object
// Remember to call json_object_put() an pass the json_object to free up the memory
json_object *nebstruct_encode_service_check_as_json(nebstruct_service_check_data *ds) {
    json_object *my_object;
    json_object *servicecheck_object;
    nebstruct_service_check_data *current_servicecheck;

    char *raw_command;
    service *current_service;

    my_object = json_object_new_object();
    servicecheck_object = json_object_new_object();

    json_object_object_add(my_object, "type",      json_object_new_int(ds->type));
    json_object_object_add(my_object, "flags",     json_object_new_int(ds->flags));
    json_object_object_add(my_object, "attr",      json_object_new_int(ds->attr));
    json_object_object_add(my_object, "timestamp", json_object_new_int(ds->timestamp.tv_sec));

    current_service = (service *)ds->object_ptr;
    current_servicecheck = ds;

	SERVICECHECKFIELD_STRING(host_name);
	SERVICECHECKFIELD_STRING(service_description);

    raw_command = NULL;
    get_raw_command_line_r(get_global_macros(), current_service->check_command_ptr, current_service->check_command, &raw_command, 0);

    json_object_object_add(servicecheck_object, "command_line", (raw_command != NULL ? json_object_new_string(raw_command) : NULL));
    json_object_object_add(servicecheck_object, "command_name", (current_service->check_command != NULL ? json_object_new_string(current_service->check_command) : NULL));

    SERVICECHECKFIELD_STRING(output);
    SERVICECHECKFIELD_STRING(long_output);
    SERVICECHECKFIELD_STRING(perf_data);
    SERVICECHECKFIELD_INT(check_type);
    SERVICECHECKFIELD_INT(current_attempt);
    SERVICECHECKFIELD_INT(max_attempts);
    SERVICECHECKFIELD_INT(state_type);
    SERVICECHECKFIELD_INT(state);
    SERVICECHECKFIELD_INT(timeout);
    json_object_object_add(servicecheck_object, "start_time", json_object_new_int64(ds->start_time.tv_sec));
    json_object_object_add(servicecheck_object, "end_time", json_object_new_int64(ds->end_time.tv_sec));
    SERVICECHECKFIELD_INT(early_timeout);
    SERVICECHECKFIELD_DOUBLE(execution_time);
    SERVICECHECKFIELD_DOUBLE(latency);
    SERVICECHECKFIELD_INT(return_code);

    json_object_object_add(my_object, "servicecheck", servicecheck_object);

    free(raw_command);

    return my_object;
}
