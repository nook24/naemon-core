
#define HOSTCHECKFIELD_STRING(FIELD) \
	json_object_object_add(hostcheck_object, #FIELD, (current_hostcheck->FIELD != NULL ? json_object_new_string(current_hostcheck->FIELD) : NULL))

#define HOSTCHECKFIELD_INT(FIELD) \
	json_object_object_add(hostcheck_object, #FIELD, json_object_new_int64(current_hostcheck->FIELD))

#define HOSTCHECKFIELD_DOUBLE(FIELD) \
	json_object_object_add(hostcheck_object, #FIELD, json_object_new_double(current_hostcheck->FIELD))


/*** Naemon Event Radio Dispatcher JSON functions ***/
int nerd_init_json(void);
