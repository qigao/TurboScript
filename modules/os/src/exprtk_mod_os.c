/**
 * @file exprtk_mod_os.c
 * @brief Platform information, child processes, logging, and service status.
 */
#include "os.h"
#include "exprtk.h"
#include "platform.h"
#include "tlog.h"
#include "turbo_error.h"
#include "turbo_process.h"
#include "turbo_str.h"
#include "turbo_thread.h"
#include "cron/turbo_cron.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OS_MAX_PROCESS_HANDLES 32U
#define OS_MAX_PROCESS_ARGS 32U
#define OS_MAX_OUTPUT_READ (1024U * 1024U)
#define OS_DEFAULT_OUTPUT_READ (64U * 1024U)
#define OS_SERVICE_TIMEOUT_MS 5000U
#define OS_SERVICE_OUTPUT_SIZE (16U * 1024U)
#define OS_MAX_POWER_SCHEDULES 16U
#define OS_POWER_ACTION_SIZE 9U
#define OS_CRON_EXPRESSION_SIZE 128U

typedef struct {
    turbo_process_t *process;
} os_process_slot_t;

typedef struct {
    int success;
    int available;
    int exit_code;
    int error_code;
    int wait_code;
    char state[32];
    char output[OS_SERVICE_OUTPUT_SIZE];
} os_action_result_t;

struct os_module_s;

typedef struct {
    struct os_module_s *owner;
    turbo_cron_runner_t *runner;
    char action[OS_POWER_ACTION_SIZE];
    char expression[OS_CRON_EXPRESSION_SIZE];
    char last_state[32];
    char last_output[OS_SERVICE_OUTPUT_SIZE];
    uint64_t fire_count;
    int allocated;
    int active;
    int last_success;
    int last_error_code;
    int last_wait_code;
    time_t last_scheduled_at;
} os_power_schedule_slot_t;

typedef struct os_module_s {
    os_process_slot_t processes[OS_MAX_PROCESS_HANDLES];
    os_power_schedule_slot_t schedules[OS_MAX_POWER_SCHEDULES];
    turbo_mutex_t schedule_mutex;
} os_module_t;

static exprtk_value_t os_empty(void) { return exprtk_val_num(0); }

static exprtk_value_t os_platform_text(mem_pool_t *arena,
                                       int (*query)(char *, size_t)) {
    char *buffer;

    if (!arena || !query) return os_empty();
    buffer = (char *)mem_alloc(arena, TURBO_PLATFORM_INFO_MAX);
    if (!buffer || query(buffer, TURBO_PLATFORM_INFO_MAX) != TURBO_OK)
        return os_empty();
    return exprtk_val_str(vstr_from_cstr(buffer));
}

static int os_string_arg(const exprtk_value_t *value, const char **data,
                         size_t *length) {
    if (!value || value->type != EXPRTK_VAL_STRING || !value->data.string.data)
        return 0;
    if (data) *data = value->data.string.data;
    if (length) *length = value->data.string.len;
    return 1;
}

static int os_copy_string(mem_pool_t *arena, const exprtk_value_t *value,
                          const char **out) {
    const char *data;
    size_t length;
    char *copy;

    if (!os_string_arg(value, &data, &length) || !arena || !out) return 0;
    copy = (char *)mem_alloc(arena, length + 1U);
    if (!copy) return 0;
    memcpy(copy, data, length);
    copy[length] = '\0';
    *out = copy;
    return 1;
}

static int os_u64_arg(const exprtk_value_t *value, uint64_t *out) {
    double number;

    if (!value || !out) return 0;
    if (value->type == EXPRTK_VAL_INTEGER) {
        if (value->data.integer < 0) return 0;
        *out = (uint64_t)value->data.integer;
        return 1;
    }
    if (value->type != EXPRTK_VAL_NUMBER) return 0;
    number = value->data.number;
    if (!isfinite(number) || number < 0.0 || number > 18446744073709551615.0)
        return 0;
    *out = (uint64_t)number;
    return 1;
}

static turbo_process_t *os_process_get(os_module_t *module, int64_t id) {
    if (!module || id < 1 || id > (int64_t)OS_MAX_PROCESS_HANDLES) return NULL;
    return module->processes[(size_t)id - 1U].process;
}

static int os_process_find_free(const os_module_t *module) {
    size_t i;

    if (!module) return -1;
    for (i = 0; i < OS_MAX_PROCESS_HANDLES; ++i) {
        if (!module->processes[i].process) return (int)i;
    }
    return -1;
}

static int os_map_put(exprtk_value_t *map, const char *key, exprtk_value_t value) {
    return map && key && exprtk_map_set(map, key, value) == TURBO_OK;
}

static exprtk_value_t os_process_result_map(os_module_t *module, int64_t id,
                                            int wait_code) {
    exprtk_value_t map;
    turbo_process_t *process;
    turbo_process_result_t result;
    int poll_code;
    int64_t pid;

    process = os_process_get(module, id);
    if (!process) return os_empty();

    memset(&result, 0, sizeof(result));
    result.exit_code = -1;
    result.term_signal = -1;
    result.error_code = 0;
    poll_code = turbo_process_poll(process, &result);
    if (poll_code != TURBO_OK) {
        result.state = turbo_process_state(process);
    }
    pid = (int64_t)turbo_process_pid(process);

    map = exprtk_val_map();
    if (!os_map_put(&map, "id", exprtk_val_int(id)) ||
        !os_map_put(&map, "pid", exprtk_val_int(pid)) ||
        !os_map_put(&map, "state", exprtk_val_str(vstr_from_cstr(
                         turbo_process_state_name(result.state)))) ||
        !os_map_put(&map, "running", exprtk_val_num(
                         turbo_process_is_running(process) ? 1.0 : 0.0)) ||
        !os_map_put(&map, "exit_code", exprtk_val_int(result.exit_code)) ||
        !os_map_put(&map, "term_signal", exprtk_val_int(result.term_signal)) ||
        !os_map_put(&map, "error_code", exprtk_val_int(result.error_code)) ||
        !os_map_put(&map, "wait_code", exprtk_val_int(wait_code))) {
        exprtk_map_free(&map);
        return os_empty();
    }
    return map;
}

static exprtk_value_t os_fn_platform_name(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, void *user_data) {
    (void)args;
    (void)user_data;
    return argc == 0 && env ? os_platform_text(&env->arena, turbo_platform_os_name)
                            : os_empty();
}

static exprtk_value_t os_fn_platform_version(size_t argc, exprtk_value_t *args,
                                             exprtk_env_t *env, void *user_data) {
    (void)args;
    (void)user_data;
    return argc == 0 && env ? os_platform_text(&env->arena, turbo_platform_os_version)
                            : os_empty();
}

static exprtk_value_t os_fn_arch(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, void *user_data) {
    (void)args;
    (void)user_data;
    return argc == 0 && env ? os_platform_text(&env->arena, turbo_platform_arch) : os_empty();
}

static exprtk_value_t os_fn_hostname(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, void *user_data) {
    (void)args;
    (void)user_data;
    return argc == 0 && env ? os_platform_text(&env->arena, turbo_platform_hostname)
                            : os_empty();
}

static exprtk_value_t os_fn_username(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, void *user_data) {
    (void)args;
    (void)user_data;
    return argc == 0 && env ? os_platform_text(&env->arena, turbo_platform_username)
                            : os_empty();
}

static exprtk_value_t os_fn_pid(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, void *user_data) {
    (void)args;
    (void)env;
    (void)user_data;
    return argc == 0 ? exprtk_val_int((int64_t)turbo_getpid()) : os_empty();
}

static exprtk_value_t os_fn_log(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, void *user_data) {
    const char *level_name;
    const char *message;
    const char *component = NULL;
    turbo_log_level_t level;
    tlog_t *logger;
    size_t message_length;
    size_t level_length;
    static const char *const level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    size_t i;

    (void)env;
    (void)user_data;
    if (argc < 2 || argc > 3 ||
        !os_string_arg(&args[0], &level_name, &level_length) ||
        !os_string_arg(&args[1], &message, &message_length))
        return os_empty();
    if (argc == 3 && !os_string_arg(&args[2], &component, NULL)) return os_empty();

    level = TURBO_LOG_LEVEL_INFO;
    for (i = 0; i < sizeof(level_names) / sizeof(level_names[0]); ++i) {
        if (strlen(level_names[i]) == level_length &&
            strncmp(level_names[i], level_name, level_length) == 0) {
            level = (turbo_log_level_t)i;
            break;
        }
    }
    if (i == sizeof(level_names) / sizeof(level_names[0])) return os_empty();

    logger = tlog_get_default();
    if (!logger) return os_empty();
    turbo_log_str(logger, level, component, NULL, 0, message, message_length);
    return exprtk_val_num(1);
}

static exprtk_value_t os_fn_log_set_level(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, void *user_data) {
    const char *level_name;
    size_t level_length;
    turbo_log_level_t level;
    tlog_t *logger;
    static const char *const level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    size_t i;

    (void)env;
    (void)user_data;
    if (argc != 1 || !os_string_arg(&args[0], &level_name, &level_length)) return os_empty();
    for (i = 0; i < sizeof(level_names) / sizeof(level_names[0]); ++i) {
        if (strlen(level_names[i]) == level_length &&
            strncmp(level_names[i], level_name, level_length) == 0)
            break;
    }
    if (i == sizeof(level_names) / sizeof(level_names[0])) return os_empty();

    logger = tlog_get_default();
    if (!logger) return os_empty();
    level = (turbo_log_level_t)i;
    return exprtk_val_num(tlog_set_level_ex(logger, level) == TURBO_OK ? 1 : 0);
}

static exprtk_value_t os_fn_log_level(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, void *user_data) {
    tlog_t *logger;

    (void)args;
    (void)env;
    (void)user_data;
    if (argc != 0) return os_empty();
    logger = tlog_get_default();
    return logger ? exprtk_val_str(vstr_from_cstr(turbo_log_level_name(
                           tlog_get_level(logger)))) : os_empty();
}

static exprtk_value_t os_fn_process_start(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, void *user_data) {
    os_module_t *module = (os_module_t *)user_data;
    turbo_process_options_t options;
    turbo_process_t *process = NULL;
    const char *program;
    const char **child_args;
    int slot;
    size_t i;

    if (!module || !env || argc < 1 || argc > OS_MAX_PROCESS_ARGS ||
        !os_copy_string(&env->arena, &args[0], &program))
        return os_empty();
    if (program[0] == '\0') return os_empty();
    slot = os_process_find_free(module);
    if (slot < 0) return os_empty();

    child_args = (const char **)mem_alloc(&env->arena, argc * sizeof(*child_args));
    if (!child_args) return os_empty();
    for (i = 1; i < argc; ++i) {
        if (!os_copy_string(&env->arena, &args[i], &child_args[i - 1U])) return os_empty();
    }
    child_args[argc - 1U] = NULL;

    turbo_process_options_init(&options);
    options.program = program;
    options.args = child_args;
    if (turbo_process_spawn(&options, &process) != TURBO_OK || !process) return os_empty();
    module->processes[(size_t)slot].process = process;
    return exprtk_val_int((int64_t)slot + 1);
}

static exprtk_value_t os_fn_process_poll(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, void *user_data) {
    uint64_t raw_id;
    int64_t id;

    (void)env;
    if (argc != 1 || !os_u64_arg(&args[0], &raw_id) || raw_id > INT64_MAX)
        return os_empty();
    id = (int64_t)raw_id;
    return os_process_result_map((os_module_t *)user_data, id, TURBO_OK);
}

static exprtk_value_t os_fn_process_wait(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, void *user_data) {
    os_module_t *module = (os_module_t *)user_data;
    turbo_process_t *process;
    turbo_process_result_t result;
    uint64_t timeout;
    uint64_t raw_id;
    int64_t id;
    int wait_code;

    (void)env;
    if (argc < 1 || argc > 2 || !os_u64_arg(&args[0], &raw_id) || raw_id > INT64_MAX)
        return os_empty();
    id = (int64_t)raw_id;
    process = os_process_get(module, id);
    if (!process) return os_empty();
    if (argc == 2) {
        if (!os_u64_arg(&args[1], &timeout)) return os_empty();
        wait_code = turbo_process_wait_for(process, timeout, &result);
    } else {
        wait_code = turbo_process_wait(process, &result);
    }
    return os_process_result_map(module, id, wait_code);
}

static exprtk_value_t os_fn_process_terminate(size_t argc, exprtk_value_t *args,
                                              exprtk_env_t *env, void *user_data) {
    os_module_t *module = (os_module_t *)user_data;
    turbo_process_t *process;
    uint64_t raw_id;
    int64_t id;

    (void)env;
    if (argc != 1 || !os_u64_arg(&args[0], &raw_id) || raw_id > INT64_MAX)
        return os_empty();
    id = (int64_t)raw_id;
    process = os_process_get(module, id);
    return process ? exprtk_val_num(turbo_process_terminate(process) == TURBO_OK ? 1 : 0)
                   : os_empty();
}

static exprtk_value_t os_fn_process_close(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, void *user_data) {
    os_module_t *module = (os_module_t *)user_data;
    turbo_process_t *process;
    uint64_t raw_id;
    int64_t id;

    (void)env;
    if (argc != 1 || !os_u64_arg(&args[0], &raw_id) || raw_id > INT64_MAX)
        return os_empty();
    id = (int64_t)raw_id;
    process = os_process_get(module, id);
    if (!process) return os_empty();
    turbo_process_destroy(process);
    module->processes[(size_t)id - 1U].process = NULL;
    return exprtk_val_num(1);
}

static exprtk_value_t os_process_read(os_module_t *module, size_t argc,
                                      exprtk_value_t *args, exprtk_env_t *env,
                                      int stderr_stream) {
    turbo_process_t *process;
    uint64_t requested = OS_DEFAULT_OUTPUT_READ;
    size_t read = 0;
    char *buffer;
    uint64_t raw_id;
    int64_t id;
    int code;

    if (!env || argc < 1 || argc > 2 || !os_u64_arg(&args[0], &raw_id) ||
        raw_id > INT64_MAX)
        return os_empty();
    id = (int64_t)raw_id;
    if (argc == 2 && !os_u64_arg(&args[1], &requested)) return os_empty();
    if (requested == 0 || requested > OS_MAX_OUTPUT_READ) return os_empty();
    process = os_process_get(module, id);
    if (!process) return os_empty();

    buffer = (char *)mem_alloc(&env->arena, (size_t)requested + 1U);
    if (!buffer) return os_empty();
    code = stderr_stream
                ? turbo_process_read_stderr(process, buffer, (size_t)requested, &read)
                : turbo_process_read_stdout(process, buffer, (size_t)requested, &read);
    if (code != TURBO_OK && code != TURBO_EOF) return os_empty();
    buffer[read] = '\0';
    return exprtk_val_str(vstr_from_buf(buffer, read));
}

static exprtk_value_t os_fn_process_read_stdout(size_t argc, exprtk_value_t *args,
                                                exprtk_env_t *env, void *user_data) {
    return os_process_read((os_module_t *)user_data, argc, args, env, 0);
}

static exprtk_value_t os_fn_process_read_stderr(size_t argc, exprtk_value_t *args,
                                                exprtk_env_t *env, void *user_data) {
    return os_process_read((os_module_t *)user_data, argc, args, env, 1);
}

static size_t os_read_stream(turbo_process_t *process, int stderr_stream,
                             char *buffer, size_t capacity) {
    size_t total = 0;
    size_t read = 0;
    int code;

    if (!process || !buffer || capacity == 0) return 0;
    while (total + 1U < capacity) {
        read = 0;
        code = stderr_stream
                   ? turbo_process_read_stderr(process, buffer + total, capacity - total - 1U,
                                               &read)
                   : turbo_process_read_stdout(process, buffer + total, capacity - total - 1U,
                                               &read);
        total += read;
        if (code == TURBO_EOF || code != TURBO_OK || read == 0) break;
    }
    buffer[total] = '\0';
    return total;
}

#ifdef _WIN32
static int os_contains(const char *text, const char *needle) {
    return text && needle && strstr(text, needle) != NULL;
}
#endif

#ifndef _WIN32
static int os_status_token(const char *text, const char *token) {
    size_t length;
    size_t text_length;

    if (!text || !token) return 0;
    length = strlen(token);
    text_length = strlen(text);
    return text_length >= length && strncmp(text, token, length) == 0 &&
           (text[length] == '\0' || text[length] == '\r' || text[length] == '\n' ||
            text[length] == ' ' || text[length] == '\t');
}
#endif

static exprtk_value_t os_service_map(int available, int active, const char *state,
                                     int exit_code, int error_code, const char *output) {
    exprtk_value_t map = exprtk_val_map();

    if (!os_map_put(&map, "available", exprtk_val_num(available ? 1 : 0)) ||
        !os_map_put(&map, "active", exprtk_val_num(active ? 1 : 0)) ||
        !os_map_put(&map, "state", exprtk_val_str(vstr_from_cstr(state))) ||
        !os_map_put(&map, "exit_code", exprtk_val_int(exit_code)) ||
        !os_map_put(&map, "error_code", exprtk_val_int(error_code)) ||
        !os_map_put(&map, "output", exprtk_val_str(vstr_from_cstr(output)))) {
        exprtk_map_free(&map);
        return os_empty();
    }
    return map;
}

static exprtk_value_t os_action_map(const char *action, int success, int available,
                                    const char *state, int exit_code, int error_code,
                                    int wait_code, const char *output) {
    exprtk_value_t map = exprtk_val_map();

    if (!os_map_put(&map, "action", exprtk_val_str(vstr_from_cstr(action))) ||
        !os_map_put(&map, "success", exprtk_val_num(success ? 1 : 0)) ||
        !os_map_put(&map, "available", exprtk_val_num(available ? 1 : 0)) ||
        !os_map_put(&map, "state", exprtk_val_str(vstr_from_cstr(state))) ||
        !os_map_put(&map, "exit_code", exprtk_val_int(exit_code)) ||
        !os_map_put(&map, "error_code", exprtk_val_int(error_code)) ||
        !os_map_put(&map, "wait_code", exprtk_val_int(wait_code)) ||
        !os_map_put(&map, "output", exprtk_val_str(vstr_from_cstr(output)))) {
        exprtk_map_free(&map);
        return os_empty();
    }
    return map;
}

static exprtk_value_t os_fn_service_status(size_t argc, exprtk_value_t *args,
                                           exprtk_env_t *env, void *user_data) {
    const char *service_name;
    turbo_process_options_t options;
    turbo_process_result_t result;
    turbo_process_t *process = NULL;
    const char *process_args[3];
    char output[OS_SERVICE_OUTPUT_SIZE];
    size_t service_length;
    int spawn_code;
    int active = 0;
    const char *state = "unknown";

    (void)env;
    (void)user_data;
    if (argc != 1 || !os_string_arg(&args[0], &service_name, &service_length) ||
        service_length == 0 || service_length >= OS_SERVICE_OUTPUT_SIZE)
        return os_empty();

#ifdef _WIN32
    process_args[0] = "query";
    process_args[1] = service_name;
    process_args[2] = NULL;
    turbo_process_options_init(&options);
    options.program = "sc.exe";
#else
    process_args[0] = "is-active";
    process_args[1] = service_name;
    process_args[2] = NULL;
    turbo_process_options_init(&options);
    options.program = "systemctl";
#endif
    options.args = process_args;
    options.timeout_ms = OS_SERVICE_TIMEOUT_MS;
    options.max_output_bytes = OS_SERVICE_OUTPUT_SIZE;
    spawn_code = turbo_process_spawn(&options, &process);
    if (spawn_code != TURBO_OK || !process)
        return os_service_map(0, 0, "unavailable", -1, spawn_code, "");

    memset(&result, 0, sizeof(result));
    result.exit_code = -1;
    result.error_code = 0;
    (void)turbo_process_wait(process, &result);
    os_read_stream(process, 0, output, sizeof(output));
    if (output[0] == '\0') os_read_stream(process, 1, output, sizeof(output));

#ifdef _WIN32
    if (os_contains(output, "RUNNING")) {
        active = 1;
        state = "running";
    } else if (os_contains(output, "STOPPED")) {
        state = "stopped";
    }
#else
    if (os_status_token(output, "inactive")) {
        state = "inactive";
    } else if (os_status_token(output, "failed")) {
        state = "failed";
    } else if (os_status_token(output, "activating")) {
        state = "activating";
    } else if (os_status_token(output, "deactivating")) {
        state = "deactivating";
    } else if (os_status_token(output, "active")) {
        active = 1;
        state = "active";
    }
#endif
    {
        exprtk_value_t map = os_service_map(1, active, state, result.exit_code,
                                            result.error_code, output);
        turbo_process_destroy(process);
        return map;
    }
}

static exprtk_value_t os_fn_service_control(size_t argc, exprtk_value_t *args,
                                            const char *action) {
    const char *service_name;
    turbo_process_options_t options;
    turbo_process_result_t result;
    turbo_process_t *process = NULL;
    const char *process_args[3];
    char output[OS_SERVICE_OUTPUT_SIZE];
    size_t service_length;
    int spawn_code;
    int wait_code;
    int success;
    const char *state = "unavailable";

    if (!action || argc != 1 || !os_string_arg(&args[0], &service_name, &service_length) ||
        service_length == 0 || service_length >= OS_SERVICE_OUTPUT_SIZE)
        return os_empty();

    process_args[0] = action;
    process_args[1] = service_name;
    process_args[2] = NULL;
    turbo_process_options_init(&options);
#ifdef _WIN32
    options.program = "sc.exe";
#else
    options.program = "systemctl";
#endif
    options.args = process_args;
    options.timeout_ms = OS_SERVICE_TIMEOUT_MS;
    options.max_output_bytes = OS_SERVICE_OUTPUT_SIZE;

    spawn_code = turbo_process_spawn(&options, &process);
    if (spawn_code != TURBO_OK || !process)
        return os_action_map(action, 0, 0, state, -1, spawn_code, spawn_code, "");

    memset(&result, 0, sizeof(result));
    result.exit_code = -1;
    result.error_code = 0;
    wait_code = turbo_process_wait(process, &result);
    state = turbo_process_state_name(result.state);
    os_read_stream(process, 0, output, sizeof(output));
    if (output[0] == '\0') os_read_stream(process, 1, output, sizeof(output));
    success = wait_code == TURBO_OK && result.exit_code == 0 && result.error_code == 0;

    {
        exprtk_value_t map = os_action_map(
            action, success, 1, state, result.exit_code, result.error_code, wait_code, output);
        turbo_process_destroy(process);
        return map;
    }
}

static exprtk_value_t os_fn_service_start(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, void *user_data) {
    (void)env;
    (void)user_data;
    return os_fn_service_control(argc, args, "start");
}

static exprtk_value_t os_fn_service_stop(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, void *user_data) {
    (void)env;
    (void)user_data;
    return os_fn_service_control(argc, args, "stop");
}

static void os_action_result_init(os_action_result_t *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->exit_code = -1;
    result->wait_code = TURBO_EINVAL;
    (void)snprintf(result->state, sizeof(result->state), "%s", "unavailable");
}

static int os_run_power_action(const char *action, os_action_result_t *out) {
    turbo_process_options_t options;
    turbo_process_result_t result;
    turbo_process_t *process = NULL;
    const char *process_args[4] = {NULL, NULL, NULL, NULL};
    int spawn_code;
    const char *program;

    if (!action || !out ||
        (strcmp(action, "reboot") != 0 && strcmp(action, "shutdown") != 0))
        return TURBO_EINVAL;
    os_action_result_init(out);

#ifdef _WIN32
    program = "shutdown.exe";
    process_args[0] = strcmp(action, "reboot") == 0 ? "/r" : "/s";
    process_args[1] = "/t";
    process_args[2] = "0";
#elif defined(__linux__)
    program = "systemctl";
    process_args[0] = strcmp(action, "reboot") == 0 ? "reboot" : "poweroff";
#else
    program = "shutdown";
    process_args[0] = strcmp(action, "reboot") == 0 ? "-r" : "-h";
    process_args[1] = "now";
#endif

    turbo_process_options_init(&options);
    options.program = program;
    options.args = process_args;
    options.timeout_ms = OS_SERVICE_TIMEOUT_MS;
    options.max_output_bytes = OS_SERVICE_OUTPUT_SIZE;

    spawn_code = turbo_process_spawn(&options, &process);
    if (spawn_code != TURBO_OK || !process) {
        out->error_code = spawn_code;
        out->wait_code = spawn_code;
        return spawn_code;
    }

    memset(&result, 0, sizeof(result));
    result.exit_code = -1;
    result.error_code = 0;
    out->wait_code = turbo_process_wait(process, &result);
    out->available = 1;
    out->success = out->wait_code == TURBO_OK && result.exit_code == 0 &&
                   result.error_code == 0;
    out->exit_code = result.exit_code;
    out->error_code = result.error_code;
    (void)snprintf(out->state, sizeof(out->state), "%s",
                   turbo_process_state_name(result.state));
    os_read_stream(process, 0, out->output, sizeof(out->output));
    if (out->output[0] == '\0') os_read_stream(process, 1, out->output, sizeof(out->output));
    turbo_process_destroy(process);
    return out->success ? TURBO_OK : TURBO_EPERM;
}

static exprtk_value_t os_fn_power_control(size_t argc, const char *action) {
    os_action_result_t result;

    if (argc != 0 || !action) return os_empty();
    (void)os_run_power_action(action, &result);
    return os_action_map(action, result.success, result.available, result.state,
                         result.exit_code, result.error_code, result.wait_code,
                         result.output);
}

static exprtk_value_t os_fn_reboot(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, void *user_data) {
    (void)args;
    (void)env;
    (void)user_data;
    return os_fn_power_control(argc, "reboot");
}

static exprtk_value_t os_fn_shutdown(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, void *user_data) {
    (void)args;
    (void)env;
    (void)user_data;
    return os_fn_power_control(argc, "shutdown");
}

static void os_power_schedule_callback(const turbo_cron_expr_t *expression,
                                       time_t scheduled_at, void *user_data) {
    os_power_schedule_slot_t *slot = (os_power_schedule_slot_t *)user_data;
    os_action_result_t result;

    (void)expression;
    if (!slot || !slot->owner) return;
    (void)os_run_power_action(slot->action, &result);

    turbo_mutex_lock(&slot->owner->schedule_mutex);
    if (slot->allocated) {
        slot->fire_count++;
        slot->last_success = result.success;
        slot->last_error_code = result.error_code;
        slot->last_wait_code = result.wait_code;
        slot->last_scheduled_at = scheduled_at;
        (void)snprintf(slot->last_state, sizeof(slot->last_state), "%s", result.state);
        (void)snprintf(slot->last_output, sizeof(slot->last_output), "%s", result.output);
    }
    turbo_mutex_unlock(&slot->owner->schedule_mutex);
}

static int os_schedule_copy_arg(const exprtk_value_t *value, char *buffer,
                                size_t buffer_size) {
    const char *data;
    size_t length;

    if (!buffer || buffer_size == 0 || !os_string_arg(value, &data, &length) ||
        length == 0 || length >= buffer_size)
        return 0;
    memcpy(buffer, data, length);
    buffer[length] = '\0';
    return 1;
}

static exprtk_value_t os_power_schedule_map(const os_power_schedule_slot_t *slot,
                                            int64_t id) {
    exprtk_value_t map;

    if (!slot) return os_empty();
    map = exprtk_val_map();
    if (!os_map_put(&map, "id", exprtk_val_int(id)) ||
        !os_map_put(&map, "action", exprtk_val_str(vstr_from_cstr(slot->action))) ||
        !os_map_put(&map, "expression", exprtk_val_str(vstr_from_cstr(slot->expression))) ||
        !os_map_put(&map, "active", exprtk_val_num(slot->active ? 1 : 0)) ||
        !os_map_put(&map, "fire_count", exprtk_val_int((int64_t)slot->fire_count)) ||
        !os_map_put(&map, "last_success", exprtk_val_num(slot->last_success ? 1 : 0)) ||
        !os_map_put(&map, "last_error_code", exprtk_val_int(slot->last_error_code)) ||
        !os_map_put(&map, "last_wait_code", exprtk_val_int(slot->last_wait_code)) ||
        !os_map_put(&map, "last_scheduled_at", exprtk_val_int((int64_t)slot->last_scheduled_at)) ||
        !os_map_put(&map, "last_state", exprtk_val_str(vstr_from_cstr(slot->last_state))) ||
        !os_map_put(&map, "last_output", exprtk_val_str(vstr_from_cstr(slot->last_output)))) {
        exprtk_map_free(&map);
        return os_empty();
    }
    return map;
}

static exprtk_value_t os_fn_power_schedule(size_t argc, exprtk_value_t *args,
                                           exprtk_env_t *env, void *user_data) {
    os_module_t *module = (os_module_t *)user_data;
    char action[OS_POWER_ACTION_SIZE];
    char expression[OS_CRON_EXPRESSION_SIZE];
    os_power_schedule_slot_t *slot = NULL;
    turbo_cron_runner_t *runner;
    size_t i;
    int id = -1;

    (void)env;
    if (!module || argc != 2 || !os_schedule_copy_arg(&args[0], action, sizeof(action)) ||
        !os_schedule_copy_arg(&args[1], expression, sizeof(expression)) ||
        (strcmp(action, "reboot") != 0 && strcmp(action, "shutdown") != 0))
        return os_empty();

    turbo_mutex_lock(&module->schedule_mutex);
    for (i = 0; i < OS_MAX_POWER_SCHEDULES; ++i) {
        if (!module->schedules[i].allocated) {
            slot = &module->schedules[i];
            id = (int)i + 1;
            memset(slot, 0, sizeof(*slot));
            slot->owner = module;
            slot->allocated = 1;
            (void)snprintf(slot->action, sizeof(slot->action), "%s", action);
            (void)snprintf(slot->expression, sizeof(slot->expression), "%s", expression);
            (void)snprintf(slot->last_state, sizeof(slot->last_state), "%s", "pending");
            break;
        }
    }
    turbo_mutex_unlock(&module->schedule_mutex);
    if (!slot) return os_empty();

    runner = turbo_cron_runner_create(expression, os_power_schedule_callback, slot);
    if (!runner) {
        turbo_mutex_lock(&module->schedule_mutex);
        memset(slot, 0, sizeof(*slot));
        turbo_mutex_unlock(&module->schedule_mutex);
        return os_empty();
    }

    turbo_mutex_lock(&module->schedule_mutex);
    slot->runner = runner;
    slot->active = 1;
    turbo_mutex_unlock(&module->schedule_mutex);
    if (turbo_cron_runner_start(runner) != TURBO_CRON_OK) {
        turbo_cron_runner_destroy(runner);
        turbo_mutex_lock(&module->schedule_mutex);
        memset(slot, 0, sizeof(*slot));
        turbo_mutex_unlock(&module->schedule_mutex);
        return os_empty();
    }
    return exprtk_val_int((int64_t)id);
}

static exprtk_value_t os_fn_power_schedule_cancel(size_t argc, exprtk_value_t *args,
                                                  exprtk_env_t *env, void *user_data) {
    os_module_t *module = (os_module_t *)user_data;
    uint64_t raw_id;
    os_power_schedule_slot_t *slot;
    turbo_cron_runner_t *runner;

    (void)env;
    if (!module || argc != 1 || !os_u64_arg(&args[0], &raw_id) ||
        raw_id == 0 || raw_id > OS_MAX_POWER_SCHEDULES)
        return os_empty();

    slot = &module->schedules[(size_t)raw_id - 1U];
    turbo_mutex_lock(&module->schedule_mutex);
    if (!slot->allocated) {
        turbo_mutex_unlock(&module->schedule_mutex);
        return exprtk_val_num(0);
    }
    slot->active = 0;
    runner = slot->runner;
    turbo_mutex_unlock(&module->schedule_mutex);

    if (runner) {
        (void)turbo_cron_runner_stop(runner);
        turbo_cron_runner_destroy(runner);
    }
    turbo_mutex_lock(&module->schedule_mutex);
    memset(slot, 0, sizeof(*slot));
    turbo_mutex_unlock(&module->schedule_mutex);
    return exprtk_val_num(1);
}

static exprtk_value_t os_fn_power_schedule_status(size_t argc, exprtk_value_t *args,
                                                  exprtk_env_t *env, void *user_data) {
    os_module_t *module = (os_module_t *)user_data;
    uint64_t raw_id;
    os_power_schedule_slot_t snapshot;
    size_t index;

    (void)env;
    if (!module || argc != 1 || !os_u64_arg(&args[0], &raw_id) ||
        raw_id == 0 || raw_id > OS_MAX_POWER_SCHEDULES)
        return os_empty();
    index = (size_t)raw_id - 1U;
    turbo_mutex_lock(&module->schedule_mutex);
    if (!module->schedules[index].allocated) {
        turbo_mutex_unlock(&module->schedule_mutex);
        return os_empty();
    }
    snapshot = module->schedules[index];
    turbo_mutex_unlock(&module->schedule_mutex);
    return os_power_schedule_map(&snapshot, (int64_t)raw_id);
}

/* The descriptor exposes only stateless functions for direct static linking.
 * The plugin loader registers the same names plus stateful process functions. */
static exprtk_value_t os_static_platform_name(size_t argc, exprtk_value_t *args,
                                              exprtk_env_t *env, mem_pool_t *arena) {
    (void)args;
    (void)env;
    return argc == 0 ? os_platform_text(arena, turbo_platform_os_name) : os_empty();
}

static exprtk_value_t os_static_platform_version(size_t argc, exprtk_value_t *args,
                                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)args;
    (void)env;
    return argc == 0 ? os_platform_text(arena, turbo_platform_os_version) : os_empty();
}

static exprtk_value_t os_static_arch(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)args;
    (void)env;
    return argc == 0 ? os_platform_text(arena, turbo_platform_arch) : os_empty();
}

static exprtk_value_t os_static_hostname(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    (void)args;
    (void)env;
    return argc == 0 ? os_platform_text(arena, turbo_platform_hostname) : os_empty();
}

static exprtk_value_t os_static_username(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    (void)args;
    (void)env;
    return argc == 0 ? os_platform_text(arena, turbo_platform_username) : os_empty();
}

static exprtk_value_t os_static_pid(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)args;
    (void)env;
    (void)arena;
    return argc == 0 ? exprtk_val_int((int64_t)turbo_getpid()) : os_empty();
}

static const exprtk_func_entry_t os_entries[] = {
    {"arch", os_static_arch},
    {"hostname", os_static_hostname},
    {"platform_name", os_static_platform_name},
    {"platform_version", os_static_platform_version},
    {"pid", os_static_pid},
    {"username", os_static_username},
};

static const exprtk_module_t os_module = {
    .module_name = "os",
    .entries = os_entries,
    .count = sizeof(os_entries) / sizeof(os_entries[0]),
};

const exprtk_module_t *exprtk_module_os(void) { return &os_module; }

void *os_module_create(void) {
    os_module_t *module = (os_module_t *)calloc(1, sizeof(os_module_t));
    if (module) turbo_mutex_init(&module->schedule_mutex);
    return module;
}

void os_module_load(void *opaque_module, void *opaque_env, void *opaque_scratch) {
    os_module_t *module = (os_module_t *)opaque_module;
    exprtk_env_t *env = (exprtk_env_t *)opaque_env;

    (void)opaque_scratch;
    if (!module || !env) return;
    exprtk_env_register_func(env, "os.arch", os_fn_arch, module);
    exprtk_env_register_func(env, "os.hostname", os_fn_hostname, module);
    exprtk_env_register_func(env, "os.log", os_fn_log, module);
    exprtk_env_register_func(env, "os.log_level", os_fn_log_level, module);
    exprtk_env_register_func(env, "os.log_set_level", os_fn_log_set_level, module);
    exprtk_env_register_func(env, "os.pid", os_fn_pid, module);
    exprtk_env_register_func(env, "os.platform_name", os_fn_platform_name, module);
    exprtk_env_register_func(env, "os.platform_version", os_fn_platform_version, module);
    exprtk_env_register_func(env, "os.process_close", os_fn_process_close, module);
    exprtk_env_register_func(env, "os.process_poll", os_fn_process_poll, module);
    exprtk_env_register_func(env, "os.process_read_stderr", os_fn_process_read_stderr, module);
    exprtk_env_register_func(env, "os.process_read_stdout", os_fn_process_read_stdout, module);
    exprtk_env_register_func(env, "os.process_start", os_fn_process_start, module);
    exprtk_env_register_func(env, "os.process_terminate", os_fn_process_terminate, module);
    exprtk_env_register_func(env, "os.process_wait", os_fn_process_wait, module);
    exprtk_env_register_func(env, "os.reboot", os_fn_reboot, module);
    exprtk_env_register_func(env, "os.power_schedule", os_fn_power_schedule, module);
    exprtk_env_register_func(env, "os.power_schedule_cancel", os_fn_power_schedule_cancel,
                             module);
    exprtk_env_register_func(env, "os.power_schedule_status", os_fn_power_schedule_status,
                             module);
    exprtk_env_register_func(env, "os.service_status", os_fn_service_status, module);
    exprtk_env_register_func(env, "os.service_start", os_fn_service_start, module);
    exprtk_env_register_func(env, "os.service_stop", os_fn_service_stop, module);
    exprtk_env_register_func(env, "os.shutdown", os_fn_shutdown, module);
    exprtk_env_register_func(env, "os.username", os_fn_username, module);
}

void os_module_destroy(void *opaque_module) {
    os_module_t *module = (os_module_t *)opaque_module;
    size_t i;
    turbo_cron_runner_t *runner;

    if (!module) return;
    for (i = 0; i < OS_MAX_POWER_SCHEDULES; ++i) {
        turbo_mutex_lock(&module->schedule_mutex);
        runner = module->schedules[i].runner;
        module->schedules[i].active = 0;
        turbo_mutex_unlock(&module->schedule_mutex);
        if (runner) {
            (void)turbo_cron_runner_stop(runner);
            turbo_cron_runner_destroy(runner);
        }
        turbo_mutex_lock(&module->schedule_mutex);
        memset(&module->schedules[i], 0, sizeof(module->schedules[i]));
        turbo_mutex_unlock(&module->schedule_mutex);
    }
    for (i = 0; i < OS_MAX_PROCESS_HANDLES; ++i) {
        if (module->processes[i].process) {
            turbo_process_destroy(module->processes[i].process);
            module->processes[i].process = NULL;
        }
    }
    turbo_mutex_destroy(&module->schedule_mutex);
    free(module);
}
