#ifndef _BROKER_H
#define _BROKER_H

#if !defined (_NAEMON_H_INSIDE) && !defined (NAEMON_COMPILATION)
#error "Only <naemon/naemon.h> can be included directly."
#endif

#include "checks.h"
#include "objects_host.h"
#include "objects_contact.h"
#include "objects_service.h"
#include "nebmods.h"
#include "macros.h"

/*************** EVENT BROKER OPTIONS *****************/

#define BROKER_NOTHING                  0
#define BROKER_EVERYTHING	 	1048575

#define BROKER_PROGRAM_STATE            1	/* DONE */
#define BROKER_TIMED_EVENTS             2	/* DONE, DEPRECATED */
#define BROKER_SERVICE_CHECKS           4	/* DONE */
#define BROKER_HOST_CHECKS              8	/* DONE */
#define BROKER_EVENT_HANDLERS   	16	/* DONE */
#define BROKER_LOGGED_DATA              32	/* DONE */
#define BROKER_NOTIFICATIONS    	64      /* DONE */
#define BROKER_FLAPPING_DATA   	        128	/* DONE */
#define BROKER_COMMENT_DATA         	256	/* DONE */
#define BROKER_DOWNTIME_DATA		512     /* DONE */
#define BROKER_SYSTEM_COMMANDS          1024	/* DONE */
#define BROKER_VAULT_MACROS             2048    /* DONE */
#define BROKER_STATUS_DATA              4096    /* DONE */
#define BROKER_ADAPTIVE_DATA            8192    /* DONE */
#define BROKER_EXTERNALCOMMAND_DATA     16384   /* DONE */
#define BROKER_RETENTION_DATA           32768   /* DONE */
#define BROKER_ACKNOWLEDGEMENT_DATA     65536
#define BROKER_STATECHANGE_DATA         131072
#define BROKER_RESERVED18               262144
#define BROKER_RESERVED19               524288


/****** EVENT TYPES ************************/

#define NEBTYPE_NONE                          0

#define NEBTYPE_HELLO                            1
#define NEBTYPE_GOODBYE                          2
#define NEBTYPE_INFO                             3

#define NEBTYPE_PROCESS_START                    100
#define NEBTYPE_PROCESS_DAEMONIZE                101
#define NEBTYPE_PROCESS_RESTART                  102
#define NEBTYPE_PROCESS_SHUTDOWN                 103
#define NEBTYPE_PROCESS_PRELAUNCH                104   /* before objects are read or verified */
#define NEBTYPE_PROCESS_EVENTLOOPSTART           105
#define NEBTYPE_PROCESS_EVENTLOOPEND             106

#define NEBTYPE_TIMEDEVENT_ADD                   200
#define NEBTYPE_TIMEDEVENT_REMOVE                201
#define NEBTYPE_TIMEDEVENT_EXECUTE               202
#define NEBTYPE_TIMEDEVENT_DELAY                 203   /* NOT IMPLEMENTED */
#define NEBTYPE_TIMEDEVENT_SKIP                  204   /* NOT IMPLEMENTED */
#define NEBTYPE_TIMEDEVENT_SLEEP                 205

#define NEBTYPE_LOG_DATA                         300
#define NEBTYPE_LOG_ROTATION                     301

#define NEBTYPE_SYSTEM_COMMAND_START             400
#define NEBTYPE_SYSTEM_COMMAND_END               401

#define NEBTYPE_EVENTHANDLER_START               500
#define NEBTYPE_EVENTHANDLER_END                 501

#define NEBTYPE_NOTIFICATION_START               600
#define NEBTYPE_NOTIFICATION_END                 601
#define NEBTYPE_CONTACTNOTIFICATION_START        602
#define NEBTYPE_CONTACTNOTIFICATION_END          603
#define NEBTYPE_CONTACTNOTIFICATIONMETHOD_START  604
#define NEBTYPE_CONTACTNOTIFICATIONMETHOD_END    605

#define NEBTYPE_SERVICECHECK_INITIATE            700
#define NEBTYPE_SERVICECHECK_PROCESSED           701
#define NEBTYPE_SERVICECHECK_RAW_START           702   /* NOT IMPLEMENTED */
#define NEBTYPE_SERVICECHECK_RAW_END             703   /* NOT IMPLEMENTED */
#define NEBTYPE_SERVICECHECK_ASYNC_PRECHECK      704

#define NEBTYPE_HOSTCHECK_INITIATE               800   /* a check of the route to the host has been initiated */
#define NEBTYPE_HOSTCHECK_PROCESSED              801   /* the processed/final result of a host check */
#define NEBTYPE_HOSTCHECK_RAW_START              802   /* the start of a "raw" host check */
#define NEBTYPE_HOSTCHECK_RAW_END                803   /* a finished "raw" host check */
#define NEBTYPE_HOSTCHECK_ASYNC_PRECHECK         804
#define NEBTYPE_HOSTCHECK_SYNC_PRECHECK          805

#define NEBTYPE_COMMENT_ADD                      900
#define NEBTYPE_COMMENT_DELETE                   901
#define NEBTYPE_COMMENT_LOAD                     902

#define NEBTYPE_FLAPPING_START                   1000
#define NEBTYPE_FLAPPING_STOP                    1001

#define NEBTYPE_DOWNTIME_ADD                     1100
#define NEBTYPE_DOWNTIME_DELETE                  1101
#define NEBTYPE_DOWNTIME_LOAD                    1102
#define NEBTYPE_DOWNTIME_START                   1103
#define NEBTYPE_DOWNTIME_STOP                    1104

#define NEBTYPE_PROGRAMSTATUS_UPDATE             1200
#define NEBTYPE_HOSTSTATUS_UPDATE                1201
#define NEBTYPE_SERVICESTATUS_UPDATE             1202
#define NEBTYPE_CONTACTSTATUS_UPDATE             1203

/*
 * Host and service status events come in two flavours. Both arrive through
 * NEBCALLBACK_HOST_STATUS_DATA / NEBCALLBACK_SERVICE_STATUS_DATA and both
 * hand you the same nebstruct, so the type field is what tells them apart.
 *
 *   NEBTYPE_*STATUS_UPDATE    something about the object actually changed:
 *                             a check result was processed, a downtime or
 *                             acknowledgement was added, the object started
 *                             or stopped flapping, an external command
 *                             modified it, a notification went out.
 *
 *   NEBTYPE_*STATUS_SCHEDULE  nothing changed except when the object is due
 *                             to be checked next. Emitted by
 *                             schedule_next_host_check() and
 *                             schedule_next_service_check().
 *
 * Why this exists: a scheduled check emits a status event twice. Once when
 * the check is dispatched and the next run is put on the queue, and once when
 * its result comes back. The first of the two carries no new information
 * apart from next_check, so modules used to receive, decode and store a
 * duplicate of the previous event for every check they saw. Before these two
 * types existed there was no way to tell that duplicate apart from a genuine
 * update -- see https://github.com/naemon/naemon-core/issues/162.
 *
 * Usually the only fields a NEBTYPE_*STATUS_SCHEDULE event has changed are
 * next_check, check_options and last_update, with everything else repeating
 * the previous event. That is not guaranteed, though: schedule_next_*_check()
 * also runs from inside handle_async_*_check_result(), after the result has
 * already been written to the object, so those events carry the fresh check
 * result with them. How often that happens depends on the installation -- the
 * reschedule at dispatch is skipped while a check is still executing, so the
 * share grows with how often a check is still outstanding when its next one
 * falls due. Measured here at 0 % with sub-millisecond local checks and 8.4 %
 * with checks distributed over a network.
 *
 * This does not make the event unsafe to drop: a full NEBTYPE_*STATUS_UPDATE
 * always follows the result, and every event carries a complete snapshot
 * rather than a delta.
 *
 * Two things to keep in mind before ignoring NEBTYPE_*STATUS_SCHEDULE
 * outright:
 *
 *   - When a check is scheduled but then not run -- host is down, check
 *     period closed, dependencies or parents failed, checks disabled, the
 *     result was still within the cache horizon, or max_parallel_*_checks
 *     was reached -- the schedule event is the only one that fires. Drop it
 *     and next_check goes stale for exactly the objects that are not being
 *     checked.
 *
 *   - Both events fire for every object on every check, so whatever you do
 *     here runs inside the single-threaded event loop. Returning early on
 *     NEBTYPE_*STATUS_SCHEDULE is the cheap option; a small next_check-only
 *     update is the accurate one.
 *
 * Modules that do not look at the type keep seeing both events and behave
 * exactly as they did before.
 */
#define NEBTYPE_HOSTSTATUS_SCHEDULE              1204
#define NEBTYPE_SERVICESTATUS_SCHEDULE           1205

#define NEBTYPE_ADAPTIVEPROGRAM_UPDATE           1300
#define NEBTYPE_ADAPTIVEHOST_UPDATE              1301
#define NEBTYPE_ADAPTIVESERVICE_UPDATE           1302
#define NEBTYPE_ADAPTIVECONTACT_UPDATE           1303

#define NEBTYPE_EXTERNALCOMMAND_START            1400
#define NEBTYPE_EXTERNALCOMMAND_END              1401

#define NEBTYPE_AGGREGATEDSTATUS_STARTDUMP       1500
#define NEBTYPE_AGGREGATEDSTATUS_ENDDUMP         1501

#define NEBTYPE_RETENTIONDATA_STARTLOAD          1600
#define NEBTYPE_RETENTIONDATA_ENDLOAD            1601
#define NEBTYPE_RETENTIONDATA_STARTSAVE          1602
#define NEBTYPE_RETENTIONDATA_ENDSAVE            1603

#define NEBTYPE_ACKNOWLEDGEMENT_ADD              1700
#define NEBTYPE_ACKNOWLEDGEMENT_REMOVE           1701   /* NOT IMPLEMENTED */
#define NEBTYPE_ACKNOWLEDGEMENT_LOAD             1702   /* NOT IMPLEMENTED */

#define NEBTYPE_STATECHANGE_START                1800   /* NOT IMPLEMENTED */
#define NEBTYPE_STATECHANGE_END                  1801



/****** EVENT FLAGS ************************/

#define NEBFLAG_NONE                          0
#define NEBFLAG_PROCESS_INITIATED             1         /* event was initiated by Naemon process */
#define NEBFLAG_USER_INITIATED                2         /* event was initiated by a user request */
#define NEBFLAG_MODULE_INITIATED              3         /* event was initiated by an event broker module */




/****** EVENT ATTRIBUTES *******************/

#define NEBATTR_NONE                          0

#define NEBATTR_SHUTDOWN_NORMAL               1
#define NEBATTR_SHUTDOWN_ABNORMAL             2
#define NEBATTR_RESTART_NORMAL                4
#define NEBATTR_RESTART_ABNORMAL              8

#define NEBATTR_FLAPPING_STOP_NORMAL          1
#define NEBATTR_FLAPPING_STOP_DISABLED        2         /* flapping stopped because flap detection was disabled */

#define NEBATTR_DOWNTIME_STOP_NORMAL          1
#define NEBATTR_DOWNTIME_STOP_CANCELLED       2

#define NEBATTR_CHECK_ALERT                   1
#define NEBATTR_CHECK_FIRST                   2


/****** EVENT BROKER FUNCTIONS *************/

NAGIOS_BEGIN_DECL

struct kvvec *get_global_store(void);
void broker_program_state(int, int, int);
void broker_log_data(int, int, int, char *, unsigned long, time_t);
int broker_event_handler(int, int, int, int, void *, int, int, struct timeval, struct timeval, double, int, int, int, char *, char *, char *);
void broker_system_command(int, int, int, struct timeval, struct timeval, double, int, int, int, char *, char *);
int broker_host_check(int, int, int, host *, int, int, int, struct timeval, struct timeval, char *, double, double, int, int, int, char *, char *, char *, char *, check_result *);
int broker_service_check(int, int, int, service *, int, struct timeval, struct timeval, char *, double, double, int, int, int, char *, check_result *);
void broker_comment_data(int, int, int, int, int, char *, char *, time_t, char *, char *, int, int, int, time_t, unsigned long);
void broker_downtime_data(int, int, int, int, char *, char *, time_t, char *, char *, time_t, time_t, int, unsigned long, unsigned long, unsigned long);
void broker_flapping_data(int, int, int, int, void *, double, double, double);
void broker_program_status(int, int, int);
void broker_host_status(int, int, int, host *);
void broker_service_status(int, int, int, service *);
void broker_contact_status(int, int, int, contact *);
neb_cb_resultset * broker_notification_data(int, int, int, int, int, struct timeval, struct timeval, void *, char *, char *, int, int);
int broker_contact_notification_data(int, int, int, int, int, struct timeval, struct timeval, void *, contact *, char *, char *, int);
int broker_contact_notification_method_data(int, int, int, int, int, struct timeval, struct timeval, void *, contact *, char *, char *, char *, int);
void broker_adaptive_program_data(int, int, int, int, unsigned long, unsigned long, unsigned long, unsigned long);
void broker_adaptive_host_data(int, int, int, host *, int, unsigned long, unsigned long);
void broker_adaptive_service_data(int, int, int, service *, int, unsigned long, unsigned long);
void broker_adaptive_contact_data(int, int, int, contact *, int, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
int broker_external_command(int, int, int, int, time_t, char *, char *);
void broker_aggregated_status_data(int, int, int);
void broker_retention_data(int, int, int);
void broker_acknowledgement_data(int, int, int, int, void *, char *, char *, int, int, int, time_t);
void broker_statechange_data(int, int, int, int, void *, int, int, int, int);
int broker_vault_macro(char *, char **, int *, nagios_macros *);

NAGIOS_END_DECL
#endif
