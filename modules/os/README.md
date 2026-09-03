# OS Module

`import("os")` exposes bounded operating-system management helpers.

Platform queries:

- `os.platform_name()`
- `os.platform_version()`
- `os.arch()`
- `os.hostname()`
- `os.username()`
- `os.pid()`

Child processes are started without a shell. Arguments are passed as separate
arguments: `os.process_start(program, arg1, arg2, ...)`. The returned numeric
handle is used with `os.process_poll`, `os.process_wait`,
`os.process_read_stdout`, `os.process_read_stderr`, `os.process_terminate`,
and `os.process_close`. Each script environment has a bounded process-handle
table, and unloading the module terminates and reaps remaining children.

`os.log(level, message, component)` writes through the Salts default
logger. Levels are uppercase `DEBUG`, `INFO`, `WARN`, `ERROR`, and `FATAL`.
`os.log_level()` and `os.log_set_level(level)` inspect and change the default
logger threshold.

`os.service_status(name)` invokes `sc.exe query` on Windows and
`systemctl is-active` on POSIX systems, without a shell, and returns a map with
`available`, `active`, `state`, `exit_code`, `error_code`, and `output`.

`os.service_start(name)` and `os.service_stop(name)` invoke the matching
service-manager command and wait for it to finish. They return a map with
`action`, `success`, `available`, `state`, `exit_code`, `error_code`,
`wait_code`, and `output`. These operations can require administrator/root
privileges and change external system state; callers must handle `success == 0`.

`os.reboot()` and `os.shutdown()` request an immediate system reboot or
shutdown through the native system power command. They return the same action
result map. These calls are destructive external side effects and normally
require the privileges of the current process user; the module does not elevate
the process or open a UAC prompt. The operating system determines whether the
request is allowed, and a denied request is reported in the result map.

`os.power_schedule(action, expression)` creates a recurring in-process Cron
schedule for `action` (`reboot` or `shutdown`). The numeric handle can be
queried with `os.power_schedule_status(id)` and removed with
`os.power_schedule_cancel(id)`. Schedules are bounded, live only while the
module is loaded, and execute under the same operating-system user as the
TurboScript process. They do not create persistent OS scheduler entries or
elevate at trigger time.
