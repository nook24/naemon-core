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
#include "worker.h"
#ifdef HAVE_JSONC
#include <json-c/json.h>
#include "nebstructs_json.h"
#endif

static unsigned int chan_host_checks_json_id;
//static unsigned int chan_service_checks_json_id;

static int chan_host_checks_json(int cb, void *data)
{
    json_object *hostcheck_object;
    const char *json_string;
    char *json_string_dub;

    nebstruct_host_check_data *ds = (nebstruct_host_check_data *)data;

    if (ds->type != NEBTYPE_HOSTCHECK_PROCESSED)
        return 0;

    if (nerd_channel_has_subscriptions(chan_host_checks_json_id) == 0)
        return 0;

    // Encode the current host check event as JSON object
    hostcheck_object = nebstruct_encode_host_check_as_json(ds);

    // Convert the JSON object to a string
    json_string = json_object_to_json_string(hostcheck_object);

    // Duplicate so we can free everything
    json_string_dub = nm_strdup(json_string);

    // Release resources
    json_object_put(hostcheck_object);

    // Send JSON through the query handler socket strlen()+1 to include the null terminator 
    nerd_broadcast(chan_host_checks_json_id, (void *)json_string_dub, strlen(json_string_dub)+1);

    free(json_string_dub);

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