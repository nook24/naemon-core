#include "nebstructs.h"
#ifdef HAVE_JSONC
#include <json-c/json.h>
#else
#error "Requires that the json-c library is loaded"
#endif

/* For processing nebstruct_host_check_data data */
#define HOSTCHECKFIELD_STRING(FIELD) \
	json_object_object_add(hostcheck_object, #FIELD, (current_hostcheck->FIELD != NULL ? json_object_new_string(current_hostcheck->FIELD) : NULL))

#define HOSTCHECKFIELD_INT(FIELD) \
	json_object_object_add(hostcheck_object, #FIELD, json_object_new_int64(current_hostcheck->FIELD))

#define HOSTCHECKFIELD_DOUBLE(FIELD) \
	json_object_object_add(hostcheck_object, #FIELD, json_object_new_double(current_hostcheck->FIELD))


json_object *nebstruct_encode_host_check_as_json(nebstruct_host_check_data *ds);

/* For processing nebstruct_servicecheck_data data */
#define SERVICECHECKFIELD_STRING(FIELD) \
	json_object_object_add(servicecheck_object, #FIELD, (current_servicecheck->FIELD != NULL ? json_object_new_string(current_servicecheck->FIELD) : NULL))

#define SERVICECHECKFIELD_INT(FIELD) \
	json_object_object_add(servicecheck_object, #FIELD, json_object_new_int64(current_servicecheck->FIELD))

#define SERVICECHECKFIELD_DOUBLE(FIELD) \
	json_object_object_add(servicecheck_object, #FIELD, json_object_new_double(current_servicecheck->FIELD))

json_object *nebstruct_encode_service_check_as_json(nebstruct_service_check_data *ds);
