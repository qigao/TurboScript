/**
 * @file exprtk_mod_io.c
 * @brief IO module: file operations, path utilities, date/time functions.
 *
 * All functions below are exposed in the "io" namespace (global registry).
 * They are always available in any exprtk_env_t, even outside TurboScript.
 */
#include "turbo_parser.h"
#include "exprtk_module.h"
#include "platform.h"
#include "turbo_fs.h"

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#include <fnmatch.h>
#endif

enum { IO_MAX_DIRECTORY_ENTRIES = 10000 };

/*
 * IO failure contract:
 * - Predicates return 1.0 or 0.0 and never throw for normal absence.
 * - Mutating/status operations return 0.0 on success and -1.0 on failure.
 * - Data-producing operations keep the legacy "numeric 0 sentinel" on failure.
 */
static exprtk_value_t io_fail_empty(void) { return exprtk_val_num(0); }

static exprtk_value_t io_fail_status(void) { return exprtk_val_num(-1); }

static exprtk_value_t io_bool(int value) { return exprtk_val_num(value ? 1.0 : 0.0); }

static int io_parse_fixed_u32(const char *text, size_t offset, size_t width, int *value_out) {
  size_t i = 0;
  int value = 0;

  if (!text || !value_out)
    return -1;

  for (i = 0; i < width; ++i) {
    char ch = text[offset + i];
    if (ch < '0' || ch > '9')
      return -1;
    value = (value * 10) + (ch - '0');
  }

  *value_out = value;
  return 0;
}

static int io_parse_utc_datetime(const char *text, struct tm *tm_info) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  size_t len = 0;

  if (!text || !tm_info)
    return -1;

  memset(tm_info, 0, sizeof(*tm_info));

  len = strlen(text);
  if (len != 10 && len != 19 && len != 20)
    return -1;

  if (io_parse_fixed_u32(text, 0, 4, &year) != 0 || text[4] != '-' ||
      io_parse_fixed_u32(text, 5, 2, &month) != 0 || text[7] != '-' ||
      io_parse_fixed_u32(text, 8, 2, &day) != 0)
    return -1;

  if (len > 10) {
    if ((text[10] != 'T' && text[10] != ' ') || io_parse_fixed_u32(text, 11, 2, &hour) != 0 ||
        text[13] != ':' || io_parse_fixed_u32(text, 14, 2, &minute) != 0 || text[16] != ':' ||
        io_parse_fixed_u32(text, 17, 2, &second) != 0)
      return -1;
  } else {
    hour = 0;
    minute = 0;
    second = 0;
  }

  if (len == 20 && text[19] != 'Z')
    return -1;

  if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59 || second < 0 || second > 60)
    return -1;

  tm_info->tm_year = year - 1900;
  tm_info->tm_mon = month - 1;
  tm_info->tm_mday = day;
  tm_info->tm_hour = hour;
  tm_info->tm_min = minute;
  tm_info->tm_sec = second;
  tm_info->tm_isdst = 0;
  return 0;
}

static exprtk_value_t io_make_string_value(mem_pool_t *arena, const char *data, size_t len) {
  char *buf = NULL;

  if (!arena)
    return io_fail_empty();

  buf = (char *)mem_alloc(arena, len + 1);
  if (!buf)
    return io_fail_empty();

  if (data && len > 0)
    memcpy(buf, data, len);
  buf[len] = '\0';

  return exprtk_val_str(vstr_from_buf(buf, len));
}

static exprtk_value_t io_format_date_value(size_t argc, exprtk_value_t *args, mem_pool_t *arena,
                                           int use_utc) {
  time_t t;
  char buf[128];

  if ((argc != 1 && argc != 2) ||
      (args[0].type != EXPRTK_VAL_NUMBER && args[0].type != EXPRTK_VAL_INTEGER))
    return io_fail_empty();

  t = (time_t)(args[0].type == EXPRTK_VAL_INTEGER ? args[0].data.integer : args[0].data.number);
  if (argc == 1) {
    if (turbo_datetime_format_rfc822(t, buf, sizeof(buf)) > 0) {
      size_t len = strlen(buf);
      return io_make_string_value(arena, buf, len);
    }
    return io_fail_empty();
  }

  if (args[1].type == EXPRTK_VAL_STRING) {
    char *fmt = vstr_to_arena(args[1].data.string, arena);
    if (!fmt)
      return io_fail_empty();

    if ((use_utc ? turbo_strftime_utc(t, fmt, buf, sizeof(buf))
                 : turbo_strftime_local(t, fmt, buf, sizeof(buf))) > 0) {
      size_t len = strlen(buf);
      return io_make_string_value(arena, buf, len);
    }
  }

  return io_fail_empty();
}

/* ═══════════════════════════════════════════════════════════════════
 * File Read / Write
 * ═══════════════════════════════════════════════════════════════════ */

/** read_file(path) → string contents (or 0 on failure) */
static exprtk_value_t fn_read_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      turbo_fs_buf_t buf = {0};
      if (turbo_fs_read_file(path, &buf) == 0) {
        exprtk_value_t result = io_make_string_value(arena, buf.base, buf.len);
        turbo_fs_buf_free(&buf);
        if (result.type == EXPRTK_VAL_STRING)
          return result;
      }
    }
  }
  return io_fail_empty();
}

/** write_file(path, content) → 0 on success */
static exprtk_value_t fn_write_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      turbo_fs_buf_t buf;
      buf.base = (char *)args[1].data.string.data;
      buf.len = args[1].data.string.len;
      return exprtk_val_num((double)turbo_fs_write_file(path, &buf));
    }
  }
  return io_fail_status();
}

/** append_file(path, content) → 0 on success */
static exprtk_value_t fn_append_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      turbo_file_t fd = turbo_fs_open(
          path, TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_APPEND, TURBO_FS_DEFAULT_MODE);
      if (fd == TURBO_INVALID_FILE)
        return io_fail_status();
      int res = turbo_fs_write(fd, args[1].data.string.data, args[1].data.string.len);
      turbo_fs_close(fd);
      return exprtk_val_num(res >= 0 ? 0.0 : (double)res);
    }
  }
  return io_fail_status();
}

/** copy_file(src_path, dst_path) → 0 on success */
static exprtk_value_t fn_copy_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *src_path = vstr_to_arena(args[0].data.string, arena);
    char *dst_path = vstr_to_arena(args[1].data.string, arena);
    if (src_path && dst_path) {
      turbo_fs_buf_t buf = {0};
      int rc = turbo_fs_read_file(src_path, &buf);
      if (rc == 0) {
        rc = turbo_fs_write_file(dst_path, &buf);
        turbo_fs_buf_free(&buf);
      }
      return exprtk_val_num((double)rc);
    }
  }
  return io_fail_status();
}

/** file_truncate(path, length) → 0 on success */
static exprtk_value_t fn_file_truncate(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING &&
      (args[1].type == EXPRTK_VAL_NUMBER || args[1].type == EXPRTK_VAL_INTEGER)) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    int64_t length = args[1].type == EXPRTK_VAL_INTEGER
                         ? args[1].data.integer
                         : (int64_t)args[1].data.number;
    if (path && length >= 0) {
      turbo_file_t fd = turbo_fs_open(path, TURBO_FS_O_WRONLY, TURBO_FS_DEFAULT_MODE);
      int rc;
      if (fd == TURBO_INVALID_FILE)
        return io_fail_status();
      rc = turbo_fs_ftruncate(fd, length);
      turbo_fs_close(fd);
      return exprtk_val_num((double)rc);
    }
  }
  return io_fail_status();
}

/* ═══════════════════════════════════════════════════════════════════
 * File Queries
 * ═══════════════════════════════════════════════════════════════════ */

/** file_exists(path) → 1.0 or 0.0 */
static exprtk_value_t fn_file_exists(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      turbo_fs_stat_t st;
      return io_bool(turbo_fs_stat(path, &st) == 0);
    }
  }
  return io_bool(0);
}

/** file_size(path) → size in bytes (or -1 on failure) */
static exprtk_value_t fn_file_size(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      turbo_fs_stat_t st;
      if (turbo_fs_stat(path, &st) == 0)
        return exprtk_val_num((double)st.size);
    }
  }
  return io_fail_status();
}

/** file_stat(path) → vector [size, mtime, is_file, is_dir] (or 0 on failure) */
static exprtk_value_t fn_file_stat(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      turbo_fs_stat_t st;
      if (turbo_fs_stat(path, &st) == 0) {
        double *out = MEM_ALLOC_ARRAY(arena, double, 4);
        if (out) {
          out[0] = (double)st.size;
          out[1] = (double)(st.mtime / 1000000ULL); /* µs → seconds */
          out[2] = st.is_file ? 1.0 : 0.0;
          out[3] = st.is_directory ? 1.0 : 0.0;
          return exprtk_val_vec(out, 4);
        }
      }
    }
  }
  return io_fail_empty();
}

/** is_file(path) → 1.0 if regular file, else 0.0 */
static exprtk_value_t fn_is_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      turbo_fs_stat_t st;
      if (turbo_fs_stat(path, &st) == 0)
        return io_bool(st.is_file);
    }
  }
  return io_bool(0);
}

/** is_dir(path) → 1.0 if directory, else 0.0 */
static exprtk_value_t fn_is_dir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      turbo_fs_stat_t st;
      if (turbo_fs_stat(path, &st) == 0)
        return io_bool(st.is_directory);
    }
  }
  return io_bool(0);
}

/* ═══════════════════════════════════════════════════════════════════
 * File Manipulation
 * ═══════════════════════════════════════════════════════════════════ */

/** file_remove(path) → 0 on success */
static exprtk_value_t fn_file_remove(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path)
      return exprtk_val_num((double)turbo_fs_unlink(path));
  }
  return io_fail_status();
}

/** file_rename(old_path, new_path) → 0 on success */
static exprtk_value_t fn_file_rename(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *old_path = vstr_to_arena(args[0].data.string, arena);
    char *new_path = vstr_to_arena(args[1].data.string, arena);
    if (old_path && new_path)
      return exprtk_val_num((double)turbo_fs_rename(old_path, new_path));
  }
  return io_fail_status();
}

/* ═══════════════════════════════════════════════════════════════════
 * Directory Operations
 * ═══════════════════════════════════════════════════════════════════ */

/** mkdir(path) → 0 on success */
static exprtk_value_t fn_mkdir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      int mode = (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) ? (int)args[1].data.number : 0755;
      return exprtk_val_num((double)turbo_fs_mkdir(path, mode));
    }
  }
  return io_fail_status();
}

/** rmdir(path) → 0 on success */
static exprtk_value_t fn_rmdir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path)
      return exprtk_val_num((double)turbo_fs_rmdir(path));
  }
  return io_fail_status();
}

static int io_path_sep(char ch) {
  return ch == '/' || ch == '\\';
}

static int io_dir_exists(const char *path) {
  turbo_fs_stat_t st;
  return path && turbo_fs_stat(path, &st) == 0 && st.is_directory;
}

static int io_mkdir_recursive_path(const char *path, int mode) {
  char current[TURBO_FS_MAX_PATH * 2];
  size_t len;
  size_t start = 0;

  if (!path || !*path)
    return -1;
  len = strlen(path);
  if (len >= sizeof(current))
    return -1;
  memcpy(current, path, len + 1);

  if (len >= 2 && current[1] == ':') {
    start = 2;
    if (len > 2 && io_path_sep(current[2]))
      start = 3;
  } else {
    while (start < len && io_path_sep(current[start]))
      start++;
  }

  for (size_t i = start; i <= len; ++i) {
    char saved;
    if (i < len && !io_path_sep(current[i]))
      continue;

    saved = current[i];
    current[i] = '\0';
    if (current[0] != '\0') {
      int rc = turbo_fs_mkdir(current, mode);
      if (rc != 0 && !io_dir_exists(current)) {
        current[i] = saved;
        return rc;
      }
    }
    current[i] = saved;

    while (i + 1 < len && io_path_sep(current[i + 1]))
      i++;
  }

  return io_dir_exists(path) ? 0 : -1;
}

static int io_rmdir_recursive_path(const char *path) {
  turbo_fs_stat_t st;
  turbo_fs_dir_t *dir = NULL;
  int result = 0;

  if (!path || turbo_fs_lstat(path, &st) != 0 || !st.is_directory || st.is_symlink)
    return -1;

  if (turbo_fs_opendir(path, &dir) != 0)
    return -1;

  for (;;) {
    char child[TURBO_FS_MAX_PATH * 2];
    turbo_fs_stat_t child_st;
    turbo_fs_dirent_t entry;
    int read_result = turbo_fs_readdir(dir, &entry);

    if (read_result == 0)
      break;
    if (read_result < 0 ||
        turbo_fs_path_join(child, sizeof(child), path, entry.name) != 0 ||
        turbo_fs_lstat(child, &child_st) != 0) {
      result = -1;
      break;
    }
    if (child_st.is_directory && !child_st.is_symlink) {
      if (io_rmdir_recursive_path(child) != 0) {
        result = -1;
        break;
      }
    } else if (turbo_fs_unlink(child) != 0) {
      result = -1;
      break;
    }
  }

  if (turbo_fs_closedir(dir) != 0)
    result = -1;

  return result == 0 ? turbo_fs_rmdir(path) : result;
}

/** mkdir_recursive(path [, mode]) → 0 on success */
static exprtk_value_t fn_mkdir_recursive(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    int mode = (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) ? (int)args[1].data.number : 0755;
    if (path)
      return exprtk_val_num((double)io_mkdir_recursive_path(path, mode));
  }
  return io_fail_status();
}

/** rmdir_recursive(path) → 0 on success */
static exprtk_value_t fn_rmdir_recursive(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path)
      return exprtk_val_num((double)io_rmdir_recursive_path(path));
  }
  return io_fail_status();
}

/** tmpdir() → temporary directory path string */
static exprtk_value_t fn_tmpdir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  char buf[TURBO_FS_MAX_PATH];
  if (turbo_fs_get_tmpdir(buf, sizeof(buf)) == 0) {
    size_t len = strlen(buf);
    return io_make_string_value(arena, buf, len);
  }
  return io_fail_empty();
}

/** listdir(path) → list of string (directory entries) */
static exprtk_value_t fn_listdir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
    return io_fail_empty();

  char *path = vstr_to_arena(args[0].data.string, arena);
  if (!path)
    return io_fail_empty();

  turbo_fs_dir_t *dir = NULL;
  if (turbo_fs_opendir(path, &dir) != 0)
    return io_fail_empty();

  exprtk_value_t *entries = MEM_ALLOC_ARRAY(arena, exprtk_value_t, IO_MAX_DIRECTORY_ENTRIES);
  if (!entries) {
    turbo_fs_closedir(dir);
    return io_fail_empty();
  }

  size_t count = 0;
  int read_result = 0;
  while (count < IO_MAX_DIRECTORY_ENTRIES) {
    turbo_fs_dirent_t entry;
    read_result = turbo_fs_readdir(dir, &entry);
    if (read_result <= 0)
      break;
    entries[count++] = io_make_string_value(arena, entry.name, strlen(entry.name));
  }

  int close_result = turbo_fs_closedir(dir);
  if (read_result < 0 || close_result != 0)
    return io_fail_empty();

  return exprtk_val_list_ex(entries, count, 0);
}

static size_t io_glob_dir_prefix_len(const char *pattern) {
  size_t last = 0;

  if (!pattern)
    return 0;
  for (size_t i = 0; pattern[i] != '\0'; ++i) {
    if (pattern[i] == '/' || pattern[i] == '\\')
      last = i + 1;
  }
  return last;
}

static exprtk_value_t io_glob_make_path(mem_pool_t *arena, const char *prefix,
                                        size_t prefix_len, const char *name) {
  char path[TURBO_FS_MAX_PATH * 2];
  size_t name_len = name ? strlen(name) : 0;

  if (!name)
    return io_fail_empty();
  if (prefix_len == 0)
    return io_make_string_value(arena, name, name_len);
  if (prefix_len + name_len >= sizeof(path))
    return io_fail_empty();
  memcpy(path, prefix, prefix_len);
  memcpy(path + prefix_len, name, name_len + 1);
  return io_make_string_value(arena, path, prefix_len + name_len);
}

/** glob(pattern) → list of matching paths */
static exprtk_value_t fn_glob(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
    return io_fail_empty();

  char *pattern = vstr_to_arena(args[0].data.string, arena);
  if (!pattern)
    return io_fail_empty();

  exprtk_value_t *entries = MEM_ALLOC_ARRAY(arena, exprtk_value_t, IO_MAX_DIRECTORY_ENTRIES);
  if (!entries)
    return io_fail_empty();

  size_t count = 0;
  size_t prefix_len = io_glob_dir_prefix_len(pattern);

#ifdef _WIN32
  struct _finddata_t fileinfo;
  intptr_t handle = _findfirst(pattern, &fileinfo);
  if (handle == -1)
    return exprtk_val_list_ex(entries, 0, 0);

  do {
    if (strcmp(fileinfo.name, ".") == 0 || strcmp(fileinfo.name, "..") == 0)
      continue;
    if (count >= IO_MAX_DIRECTORY_ENTRIES)
      break;
    entries[count++] = io_glob_make_path(arena, pattern, prefix_len, fileinfo.name);
  } while (_findnext(handle, &fileinfo) == 0);

  _findclose(handle);
#else
  char dir_buf[TURBO_FS_MAX_PATH];
  const char *dir = ".";
  const char *mask = pattern;

  if (prefix_len > 0) {
    if (prefix_len >= sizeof(dir_buf))
      return io_fail_empty();
    memcpy(dir_buf, pattern, prefix_len);
    dir_buf[prefix_len] = '\0';
    dir = dir_buf;
    mask = pattern + prefix_len;
  }

  DIR *dir_handle = opendir(dir);
  if (!dir_handle)
    return exprtk_val_list_ex(entries, 0, 0);

  struct dirent *entry;
  while ((entry = readdir(dir_handle)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (fnmatch(mask, entry->d_name, 0) != 0)
      continue;
    if (count >= IO_MAX_DIRECTORY_ENTRIES)
      break;
    entries[count++] = io_glob_make_path(arena, pattern, prefix_len, entry->d_name);
  }

  closedir(dir_handle);
#endif

  return exprtk_val_list_ex(entries, count, 0);
}

/* ═══════════════════════════════════════════════════════════════════
 * Path Utilities
 * ═══════════════════════════════════════════════════════════════════ */

/** path_join(base, relative) → joined path string */
static exprtk_value_t fn_path_join(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *base = vstr_to_arena(args[0].data.string, arena);
    char *rel = vstr_to_arena(args[1].data.string, arena);
    if (base && rel) {
      char buf[TURBO_FS_MAX_PATH * 2];
      if (turbo_fs_path_join(buf, sizeof(buf), base, rel) == 0) {
        size_t len = strlen(buf);
        return io_make_string_value(arena, buf, len);
      }
    }
  }
  return io_fail_empty();
}

/** path_dirname(path) → directory component string */
static exprtk_value_t fn_path_dirname(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      char buf[TURBO_FS_MAX_PATH];
      if (turbo_fs_path_dirname(path, buf, sizeof(buf)) == 0) {
        size_t len = strlen(buf);
        return io_make_string_value(arena, buf, len);
      }
    }
  }
  return io_fail_empty();
}

/** path_basename(path) → filename component string */
static exprtk_value_t fn_path_basename(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path) {
      char buf[TURBO_FS_MAX_PATH];
      if (turbo_fs_path_basename(path, buf, sizeof(buf)) == 0) {
        size_t len = strlen(buf);
        return io_make_string_value(arena, buf, len);
      }
    }
  }
  return io_fail_empty();
}

/** path_is_absolute(path) → 1.0 or 0.0 */
static exprtk_value_t fn_path_is_absolute(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                          mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = vstr_to_arena(args[0].data.string, arena);
    if (path)
      return io_bool(turbo_fs_path_is_absolute(path));
  }
  return io_bool(0);
}

/* ═══════════════════════════════════════════════════════════════════
 * Date / Time
 * ═══════════════════════════════════════════════════════════════════ */

/** now() → Unix timestamp (seconds) */
static exprtk_value_t fn_now(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
  return exprtk_val_num((double)time(NULL));
}

/** date(str) → Unix timestamp (seconds) */
static exprtk_value_t fn_date(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    turbo_datetime_t dt;
    char *ds = vstr_to_arena(args[0].data.string, arena);
    if (ds) {
      if (turbo_parse_datetime(ds, args[0].data.string.len, &dt) == 0)
        return exprtk_val_num((double)turbo_datetime_to_time(&dt));
    }
  }
  return io_fail_empty();
}

/** date_utc(str) → Unix timestamp (seconds, UTC parse) */
static exprtk_value_t fn_date_utc(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    struct tm tm_info;
    char *ds = vstr_to_arena(args[0].data.string, arena);
    if (ds && io_parse_utc_datetime(ds, &tm_info) == 0) {
      time_t t = turbo_timegm(&tm_info);
      if (t != (time_t)-1)
        return exprtk_val_num((double)t);
    }
  }
  return io_fail_empty();
}

/** format_date(timestamp [, fmt]) → formatted date string */
static exprtk_value_t fn_format_date(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  return io_format_date_value(argc, args, arena, 0);
}

/** format_date_utc(timestamp [, fmt]) → formatted date string using UTC */
static exprtk_value_t fn_format_date_utc(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  return io_format_date_value(argc, args, arena, 1);
}

/** date_add(timestamp, days [, hours] [, minutes] [, seconds]) → new timestamp */
static exprtk_value_t fn_date_add(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  
  if (argc < 2 || argc > 5)
    return io_fail_empty();
  
  if (args[0].type != EXPRTK_VAL_NUMBER && args[0].type != EXPRTK_VAL_INTEGER)
    return io_fail_empty();
  
  time_t timestamp = (time_t)(args[0].type == EXPRTK_VAL_INTEGER ? 
                              args[0].data.integer : args[0].data.number);
  
  double days = 0.0, hours = 0.0, minutes = 0.0, seconds = 0.0;
  
  /* Parse arguments */
  if (args[1].type == EXPRTK_VAL_NUMBER)
    days = args[1].data.number;
  else if (args[1].type == EXPRTK_VAL_INTEGER)
    days = (double)args[1].data.integer;
  else
    return io_fail_empty();
  
  if (argc >= 3) {
    if (args[2].type == EXPRTK_VAL_NUMBER)
      hours = args[2].data.number;
    else if (args[2].type == EXPRTK_VAL_INTEGER)
      hours = (double)args[2].data.integer;
    else
      return io_fail_empty();
  }
  
  if (argc >= 4) {
    if (args[3].type == EXPRTK_VAL_NUMBER)
      minutes = args[3].data.number;
    else if (args[3].type == EXPRTK_VAL_INTEGER)
      minutes = (double)args[3].data.integer;
    else
      return io_fail_empty();
  }
  
  if (argc >= 5) {
    if (args[4].type == EXPRTK_VAL_NUMBER)
      seconds = args[4].data.number;
    else if (args[4].type == EXPRTK_VAL_INTEGER)
      seconds = (double)args[4].data.integer;
    else
      return io_fail_empty();
  }
  
  /* Calculate new timestamp */
  double total_seconds = (days * 86400.0) + (hours * 3600.0) + (minutes * 60.0) + seconds;
  time_t result = timestamp + (time_t)total_seconds;
  
  return exprtk_val_num((double)result);
}

/** date_diff(timestamp1, timestamp2) → difference in seconds */
static exprtk_value_t fn_date_diff(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  (void)arena;
  
  if (argc != 2)
    return io_fail_empty();
  
  if ((args[0].type != EXPRTK_VAL_NUMBER && args[0].type != EXPRTK_VAL_INTEGER) ||
      (args[1].type != EXPRTK_VAL_NUMBER && args[1].type != EXPRTK_VAL_INTEGER))
    return io_fail_empty();
  
  time_t ts1 = (time_t)(args[0].type == EXPRTK_VAL_INTEGER ? 
                        args[0].data.integer : args[0].data.number);
  time_t ts2 = (time_t)(args[1].type == EXPRTK_VAL_INTEGER ? 
                        args[1].data.integer : args[1].data.number);
  
  return exprtk_val_num((double)(ts1 - ts2));
}

/** date_components(timestamp) → vector [year, month, day, hour, minute, second, weekday] */
static exprtk_value_t fn_date_components(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
  (void)env;
  
  if (argc != 1)
    return io_fail_empty();
  
  if (args[0].type != EXPRTK_VAL_NUMBER && args[0].type != EXPRTK_VAL_INTEGER)
    return io_fail_empty();
  
  time_t timestamp = (time_t)(args[0].type == EXPRTK_VAL_INTEGER ? 
                              args[0].data.integer : args[0].data.number);
  
  struct tm tm_info;
  if (turbo_localtime(timestamp, &tm_info) != 0)
    return io_fail_empty();
  
  double *out = MEM_ALLOC_ARRAY(arena, double, 7);
  if (!out)
    return io_fail_empty();
  
  out[0] = (double)(tm_info.tm_year + 1900);  /* year */
  out[1] = (double)(tm_info.tm_mon + 1);      /* month (1-12) */
  out[2] = (double)tm_info.tm_mday;           /* day */
  out[3] = (double)tm_info.tm_hour;           /* hour */
  out[4] = (double)tm_info.tm_min;            /* minute */
  out[5] = (double)tm_info.tm_sec;            /* second */
  out[6] = (double)tm_info.tm_wday;           /* weekday (0=Sunday) */
  
  return exprtk_val_vec(out, 7);
}

/** date_from_parts(year, month, day [, hour] [, minute] [, second]) → timestamp */
static exprtk_value_t fn_date_from_parts(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  (void)arena;
  
  if (argc < 3 || argc > 6)
    return io_fail_empty();
  
  struct tm tm_info = {0};
  
  /* Year */
  if (args[0].type == EXPRTK_VAL_NUMBER)
    tm_info.tm_year = (int)args[0].data.number - 1900;
  else if (args[0].type == EXPRTK_VAL_INTEGER)
    tm_info.tm_year = (int)args[0].data.integer - 1900;
  else
    return io_fail_empty();
  
  /* Month */
  if (args[1].type == EXPRTK_VAL_NUMBER)
    tm_info.tm_mon = (int)args[1].data.number - 1;
  else if (args[1].type == EXPRTK_VAL_INTEGER)
    tm_info.tm_mon = (int)args[1].data.integer - 1;
  else
    return io_fail_empty();
  
  /* Day */
  if (args[2].type == EXPRTK_VAL_NUMBER)
    tm_info.tm_mday = (int)args[2].data.number;
  else if (args[2].type == EXPRTK_VAL_INTEGER)
    tm_info.tm_mday = (int)args[2].data.integer;
  else
    return io_fail_empty();
  
  /* Hour (optional) */
  if (argc >= 4) {
    if (args[3].type == EXPRTK_VAL_NUMBER)
      tm_info.tm_hour = (int)args[3].data.number;
    else if (args[3].type == EXPRTK_VAL_INTEGER)
      tm_info.tm_hour = (int)args[3].data.integer;
    else
      return io_fail_empty();
  }
  
  /* Minute (optional) */
  if (argc >= 5) {
    if (args[4].type == EXPRTK_VAL_NUMBER)
      tm_info.tm_min = (int)args[4].data.number;
    else if (args[4].type == EXPRTK_VAL_INTEGER)
      tm_info.tm_min = (int)args[4].data.integer;
    else
      return io_fail_empty();
  }
  
  /* Second (optional) */
  if (argc >= 6) {
    if (args[5].type == EXPRTK_VAL_NUMBER)
      tm_info.tm_sec = (int)args[5].data.number;
    else if (args[5].type == EXPRTK_VAL_INTEGER)
      tm_info.tm_sec = (int)args[5].data.integer;
    else
      return io_fail_empty();
  }
  
  tm_info.tm_isdst = -1;  /* Auto-detect DST */
  
  time_t timestamp = turbo_mktime(&tm_info);
  if (timestamp == (time_t)-1)
    return io_fail_empty();
  
  return exprtk_val_num((double)timestamp);
}

/** weekday(timestamp) → day of week (0=Sunday, 6=Saturday) */
static exprtk_value_t fn_weekday(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  
  if (argc != 1)
    return io_fail_empty();
  
  if (args[0].type != EXPRTK_VAL_NUMBER && args[0].type != EXPRTK_VAL_INTEGER)
    return io_fail_empty();
  
  time_t timestamp = (time_t)(args[0].type == EXPRTK_VAL_INTEGER ? 
                              args[0].data.integer : args[0].data.number);
  
  struct tm tm_info;
  if (turbo_localtime(timestamp, &tm_info) != 0)
    return io_fail_empty();
  
  return exprtk_val_num((double)tm_info.tm_wday);
}

/** year_day(timestamp) → day of year (1-366) */
static exprtk_value_t fn_year_day(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  
  if (argc != 1)
    return io_fail_empty();
  
  if (args[0].type != EXPRTK_VAL_NUMBER && args[0].type != EXPRTK_VAL_INTEGER)
    return io_fail_empty();
  
  time_t timestamp = (time_t)(args[0].type == EXPRTK_VAL_INTEGER ? 
                              args[0].data.integer : args[0].data.number);
  
  struct tm tm_info;
  if (turbo_localtime(timestamp, &tm_info) != 0)
    return io_fail_empty();
  
  return exprtk_val_num((double)(tm_info.tm_yday + 1));
}

/* ═══════════════════════════════════════════════════════════════════
 * Platform Info
 * ═══════════════════════════════════════════════════════════════════ */

/** os_name() → "windows", "linux", "macos", or "unknown" */
static exprtk_value_t fn_os_name(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
#ifdef _WIN32
  return exprtk_val_str(vstr_from_cstr("windows"));
#elif defined(__APPLE__)
  return exprtk_val_str(vstr_from_cstr("macos"));
#elif defined(__linux__)
  return exprtk_val_str(vstr_from_cstr("linux"));
#else
  return exprtk_val_str(vstr_from_cstr("unknown"));
#endif
}

/** pid() → current process ID */
static exprtk_value_t fn_pid(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
  return exprtk_val_num((double)turbo_getpid());
}

/** uptime_ms() → milliseconds since process start */
static exprtk_value_t fn_uptime_ms(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
  return exprtk_val_num((double)turbo_uptime_ms());
}

/** monotonic_ms() → monotonic clock in milliseconds (never goes backward) */
static exprtk_value_t fn_monotonic_ms(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
  return exprtk_val_num((double)turbo_monotonic_ms());
}

/* ═══════════════════════════════════════════════════════════════════
 * Module Descriptor  (entries MUST be sorted alphabetically)
 * ═══════════════════════════════════════════════════════════════════ */

static const exprtk_func_entry_t io_entries[] = {
    {"append_file", fn_append_file},
    {"copy_file", fn_copy_file},
    {"date", fn_date},
    {"date_add", fn_date_add},
    {"date_components", fn_date_components},
    {"date_diff", fn_date_diff},
    {"date_from_parts", fn_date_from_parts},
    {"date_utc", fn_date_utc},
    {"file_exists", fn_file_exists},
    {"file_remove", fn_file_remove},
    {"file_rename", fn_file_rename},
    {"file_size", fn_file_size},
    {"file_stat", fn_file_stat},
    {"file_truncate", fn_file_truncate},
    {"format_date", fn_format_date},
    {"format_date_utc", fn_format_date_utc},
    {"glob", fn_glob},
    {"is_dir", fn_is_dir},
    {"is_file", fn_is_file},
    {"listdir", fn_listdir},
    {"mkdir", fn_mkdir},
    {"mkdir_recursive", fn_mkdir_recursive},
    {"monotonic_ms", fn_monotonic_ms},
    {"now", fn_now},
    {"os_name", fn_os_name},
    {"path_basename", fn_path_basename},
    {"path_dirname", fn_path_dirname},
    {"path_is_absolute", fn_path_is_absolute},
    {"path_join", fn_path_join},
    {"pid", fn_pid},
    {"read_file", fn_read_file},
    {"rmdir", fn_rmdir},
    {"rmdir_recursive", fn_rmdir_recursive},
    {"tmpdir", fn_tmpdir},
    {"uptime_ms", fn_uptime_ms},
    {"weekday", fn_weekday},
    {"write_file", fn_write_file},
    {"year_day", fn_year_day},
};

static const exprtk_module_t io_module = {"io", io_entries,
                                          sizeof(io_entries) / sizeof(io_entries[0])};

const exprtk_module_t *exprtk_module_io(void) { return &io_module; }
