#!/usr/bin/env python3
"""Generate a synthetic naemon config for event loop benchmarking.

Writes <outdir>/etc/conf.d/objects.cfg plus the surrounding directory
structure and a naemon.cfg that points at it.

The check command is deliberately trivial (/bin/echo) so that the workers
finish almost immediately and what we end up measuring is naemon's own
scheduling and result handling rather than plugin runtime.

  ./gen-config.py /var/lib/naemon-bench --hosts 5000 --services 20
"""

import argparse
import os

OBJECTS_HEADER = """
define timeperiod {
    timeperiod_name 24x7
    alias           24x7
    sunday          00:00-24:00
    monday          00:00-24:00
    tuesday         00:00-24:00
    wednesday       00:00-24:00
    thursday        00:00-24:00
    friday          00:00-24:00
    saturday        00:00-24:00
}
define command {
    command_name    bench_check
    command_line    %(check_command)s
}
define command {
    command_name    notify
    command_line    /bin/true
}
define contact {
    contact_name                    bench
    alias                           bench
    host_notification_period        24x7
    service_notification_period     24x7
    host_notification_options       d,u,r
    service_notification_options    w,u,c,r
    host_notification_commands      notify
    service_notification_commands   notify
}
define contactgroup {
    contactgroup_name   benchgroup
    alias               benchgroup
    members             bench
}
define host {
    name                    bench-host-tpl
    register                0
    check_period            24x7
    notification_period     24x7
    contact_groups          benchgroup
    max_check_attempts      3
    check_interval          %(host_interval)d
    retry_interval          %(host_interval)d
    notification_interval   0
    check_command           bench_check!host
%(host_customvars)s}
define service {
    name                    bench-svc-tpl
    register                0
    check_period            24x7
    notification_period     24x7
    contact_groups          benchgroup
    max_check_attempts      3
    check_interval          %(svc_interval)d
    retry_interval          %(svc_interval)d
    notification_interval   0
    check_command           bench_check!svc
%(svc_customvars)s}
"""

NAEMON_CFG = """log_file={d}/var/naemon.log
cfg_dir={d}/etc/conf.d
object_cache_file={d}/var/objects.cache
precached_object_file={d}/var/objects.precache
resource_file={d}/etc/resource.cfg
status_file={d}/var/status.dat
status_update_interval=10
check_external_commands=1
command_file={d}/var/naemon.cmd
lock_file={d}/var/naemon.pid
temp_file={d}/var/naemon.tmp
temp_path={d}/var/cache
event_broker_options=-1
log_archive_path={d}/var/log/archives
check_result_path={d}/var/cache/checkresults
state_retention_file={d}/var/retention.dat
query_socket={d}/var/naemon.qh
use_syslog=0
log_notifications=0
log_service_retries=0
log_host_retries=0
log_event_handlers=0
log_initial_states=0
log_current_states=0
log_external_commands=0
log_passive_checks=0
max_concurrent_checks=0
interval_length=60
retain_state_information=1
retention_update_interval=60
use_retained_program_state=0
use_retained_scheduling_info=0
enable_notifications=0
execute_service_checks=1
execute_host_checks=1
enable_event_handlers=0
process_performance_data=0
obsess_over_services=0
obsess_over_hosts=0
check_for_orphaned_services=1
check_for_orphaned_hosts=1
check_service_freshness=0
check_host_freshness=0
enable_flap_detection=1
date_format=us
illegal_object_name_chars=`~!$%^&*|'"<>?,()=
illegal_macro_output_chars=`~$&|'"<>
use_regexp_matching=0
use_true_regexp_matching=0
service_check_timeout=60
host_check_timeout=30
cached_host_check_horizon=0
cached_service_check_horizon=0
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--hosts", type=int, default=5000)
    ap.add_argument("--services", type=int, default=20,
                    help="services per host")
    ap.add_argument("--interval", type=int, default=1,
                    help="service check_interval, in interval_length units (60s)")
    ap.add_argument("--host-interval", type=int, default=5)
    ap.add_argument("--customvars", type=int, default=0,
                    help="custom variables per object; exercises the parts of the "
                         "state file writers that walk custom variable lists")
    ap.add_argument("--check-command", default="/bin/echo OK-$HOSTNAME$-$ARG1$",
                    help="command_line for the bench_check command. Keep it free of "
                         "shell metacharacters unless you want to measure /bin/sh")
    args = ap.parse_args()

    d = os.path.abspath(args.outdir)
    for sub in ("etc/conf.d", "var/log/archives", "var/cache/checkresults"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)

    def customvars(prefix):
        return "".join("    _%s%d             value %d\n" % (prefix, i, i)
                       for i in range(args.customvars))

    with open(os.path.join(d, "etc/conf.d/objects.cfg"), "w") as out:
        out.write(OBJECTS_HEADER % {
            "check_command": args.check_command,
            "svc_interval": args.interval,
            "host_interval": args.host_interval,
            "host_customvars": customvars("HOSTVAR"),
            "svc_customvars": customvars("SVCVAR"),
        })
        for h in range(args.hosts):
            out.write("define host {\n use bench-host-tpl\n host_name host%05d\n"
                      " address 127.0.0.1\n}\n" % h)
            for s in range(args.services):
                out.write("define service {\n use bench-svc-tpl\n host_name host%05d\n"
                          " service_description svc%03d\n}\n" % (h, s))

    with open(os.path.join(d, "etc/resource.cfg"), "w") as out:
        out.write("$USER1$=/usr/lib/nagios/plugins\n")

    with open(os.path.join(d, "naemon.cfg"), "w") as out:
        out.write(NAEMON_CFG.format(d=d))

    total = args.hosts * args.services
    print("wrote %s" % d)
    print("  hosts=%d services=%d customvars/object=%d" %
          (args.hosts, total, args.customvars))
    print("  expected active service checks/s = %.1f" %
          (total / float(args.interval * 60)))


if __name__ == "__main__":
    main()
