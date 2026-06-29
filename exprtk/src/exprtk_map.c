/**
 * @file exprtk_map.c
 * @brief O(1) hash table backed map for exprtk values.
 *
 * Replaces the old O(n) linear-scan entries array with MIR HTAB.
 * Same pattern as the variable symbol table in exprtk_eval.c.
 */

#include "exprtk_module.h"
#include "mir-htab.h"
#include <stdlib.h>
#include <string.h>


/* =========================================================================
 * HTAB type for map key-value pairs
 * ========================================================================= */

typedef struct {
  char *key;
  exprtk_value_t value;
} exprtk_map_kv_t;

DEF_HTAB(exprtk_map_kv_t)

/* MIR allocator wrapper (standard malloc/free) */
static void *map_malloc(size_t size, void *ud) {
  (void)ud;
  return malloc(size);
}
static void *map_calloc(size_t n, size_t sz, void *ud) {
  (void)ud;
  return calloc(n, sz);
}
static void *map_realloc(void *p, size_t old_sz, size_t new_sz, void *ud) {
  (void)ud;
  (void)old_sz;
  return realloc(p, new_sz);
}
static void map_free(void *p, void *ud) {
  (void)ud;
  free(p);
}

static struct MIR_alloc map_alloc_struct = {map_malloc, map_calloc, map_realloc, map_free, NULL};
static MIR_alloc_t map_alloc = &map_alloc_struct;

static htab_hash_t kv_hash(exprtk_map_kv_t e, void *arg) {
  (void)arg;
  const char *s = e.key;
  htab_hash_t h = 0;
  while (*s)
    h = h * 31 + (unsigned char)*s++;
  return h;
}

static int kv_eq(exprtk_map_kv_t a, exprtk_map_kv_t b, void *arg) {
  (void)arg;
  return strcmp(a.key, b.key) == 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

exprtk_value_t exprtk_val_map(void) {
  exprtk_value_t val;
  memset(&val, 0, sizeof(val));
  val.type = EXPRTK_VAL_MAP;

  HTAB(exprtk_map_kv_t) *htab = NULL;
  HTAB_OP(exprtk_map_kv_t, create)(&htab, map_alloc, 8, kv_hash, kv_eq, NULL, NULL);
  val.data.map.htab = htab;
  return val;
}

exprtk_value_t exprtk_val_object(void) {
  exprtk_value_t val = exprtk_val_map();
  val.type = EXPRTK_VAL_OBJECT;
  return val;
}

int exprtk_value_is_object_like(const exprtk_value_t *value) {
  return value && (value->type == EXPRTK_VAL_MAP || value->type == EXPRTK_VAL_OBJECT);
}

exprtk_value_t exprtk_map_get(const exprtk_value_t *map, const char *key) {
  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return exprtk_val_num(0);

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)map->data.map.htab;
  exprtk_map_kv_t probe = {.key = (char *)key};
  exprtk_map_kv_t result;

  if (HTAB_OP(exprtk_map_kv_t, do)(htab, probe, HTAB_FIND, &result))
    return result.value;

  return exprtk_val_num(0);
}

void exprtk_map_set(exprtk_value_t *map, const char *key, exprtk_value_t value) {
  if (!exprtk_value_is_object_like(map))
    return;

  /* Lazy init: if htab is NULL (e.g. from memset-zero'd value), create it */
  if (!map->data.map.htab) {
    HTAB(exprtk_map_kv_t) *htab = NULL;
    HTAB_OP(exprtk_map_kv_t, create)(&htab, map_alloc, 8, kv_hash, kv_eq, NULL, NULL);
    map->data.map.htab = htab;
  }

  /* Deep copy arena-backed buffers to avoid dangling pointers when arena is reset. */
  exprtk_value_t value_copy = value;
  if (value.type == EXPRTK_VAL_STRING && value.data.string.data) {
    char *str_copy = (char *)malloc(value.data.string.len + 1);
    if (str_copy) {
      memcpy(str_copy, value.data.string.data, value.data.string.len);
      str_copy[value.data.string.len] = '\0';
      value_copy.data.string.data = str_copy;
    }
  } else if (value.type == EXPRTK_VAL_BYTES && value.data.bytes.data) {
    char *bytes_copy = (char *)malloc(value.data.bytes.len);
    if (bytes_copy || value.data.bytes.len == 0) {
      if (value.data.bytes.len > 0)
        memcpy(bytes_copy, value.data.bytes.data, value.data.bytes.len);
      value_copy.data.bytes.data = bytes_copy;
    }
  }

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)map->data.map.htab;
  exprtk_map_kv_t probe = {.key = (char *)key};
  exprtk_map_kv_t result;

  if (HTAB_OP(exprtk_map_kv_t, do)(htab, probe, HTAB_FIND, &result)) {
    /* Update existing - free old copied buffers */
    if (result.value.type == EXPRTK_VAL_STRING && result.value.data.string.data) {
      free((void *)result.value.data.string.data);
    } else if (result.value.type == EXPRTK_VAL_BYTES && result.value.data.bytes.data) {
      free((void *)result.value.data.bytes.data);
    }
    result.value = value_copy;
    HTAB_OP(exprtk_map_kv_t, do)(htab, result, HTAB_REPLACE, &result);
  } else {
    /* Insert new */
    exprtk_map_kv_t entry = {.key = strdup(key), .value = value_copy};
    HTAB_OP(exprtk_map_kv_t, do)(htab, entry, HTAB_INSERT, &result);
  }
}

int exprtk_map_has(const exprtk_value_t *map, const char *key) {
  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return 0;

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)map->data.map.htab;
  exprtk_map_kv_t probe = {.key = (char *)key};
  exprtk_map_kv_t result;

  return HTAB_OP(exprtk_map_kv_t, do)(htab, probe, HTAB_FIND, &result) ? 1 : 0;
}

int exprtk_map_delete(exprtk_value_t *map, const char *key) {
  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return 0;

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)map->data.map.htab;
  exprtk_map_kv_t probe = {.key = (char *)key};
  exprtk_map_kv_t result;

  if (HTAB_OP(exprtk_map_kv_t, do)(htab, probe, HTAB_FIND, &result)) {
    free(result.key);
    /* Free copied buffers */
    if (result.value.type == EXPRTK_VAL_STRING && result.value.data.string.data) {
      free((void *)result.value.data.string.data);
    } else if (result.value.type == EXPRTK_VAL_BYTES && result.value.data.bytes.data) {
      free((void *)result.value.data.bytes.data);
    }
    HTAB_OP(exprtk_map_kv_t, do)(htab, probe, HTAB_DELETE, &result);
    return 1;
  }
  return 0;
}

size_t exprtk_map_count(const exprtk_value_t *map) {
  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return 0;

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)map->data.map.htab;
  return htab->els_num;
}

exprtk_value_t *exprtk_map_get_ptr(const exprtk_value_t *map, const char *key) {
  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return NULL;

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)map->data.map.htab;
  exprtk_map_kv_t probe = {.key = (char *)key};
  exprtk_map_kv_t found;

  if (!HTAB_OP(exprtk_map_kv_t, do)(htab, probe, HTAB_FIND, &found))
    return NULL;

  /* Walk the backing array to find the live entry matching this key */
  HTAB_EL(exprtk_map_kv_t) *els = VARR_ADDR(HTAB_EL(exprtk_map_kv_t), htab->els);
  htab_size_t bound = htab->els_bound;
  for (htab_size_t i = 0; i < bound; i++) {
    if (els[i].hash != HTAB_DELETED_HASH && strcmp(els[i].el.key, key) == 0)
      return &els[i].el.value;
  }
  return NULL;
}

void exprtk_map_free(exprtk_value_t *map) {
  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return;

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)map->data.map.htab;
  HTAB_EL(exprtk_map_kv_t) *els = VARR_ADDR(HTAB_EL(exprtk_map_kv_t), htab->els);
  htab_size_t bound = htab->els_bound;

  for (htab_size_t i = 0; i < bound; i++) {
    if (els[i].hash != HTAB_DELETED_HASH) {
      free(els[i].el.key);
      /* Free copied buffers */
      if (els[i].el.value.type == EXPRTK_VAL_STRING && els[i].el.value.data.string.data) {
        free((void *)els[i].el.value.data.string.data);
      } else if (els[i].el.value.type == EXPRTK_VAL_BYTES && els[i].el.value.data.bytes.data) {
        free((void *)els[i].el.value.data.bytes.data);
      }
    }
  }

  HTAB_OP(exprtk_map_kv_t, destroy)(&htab);
  map->data.map.htab = NULL;
}

/* =========================================================================
 * Iteration
 * ========================================================================= */

exprtk_map_iter_t exprtk_map_iter_begin(const exprtk_value_t *map) {
  exprtk_map_iter_t it = {NULL, 0, 0};
  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return it;

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)map->data.map.htab;
  it.htab = htab;
  it.pos = 0;
  it.bound = htab->els_bound;
  return it;
}

int exprtk_map_iter_next(exprtk_map_iter_t *it, const char **key, exprtk_value_t *value) {
  if (!it || !it->htab)
    return 0;

  HTAB(exprtk_map_kv_t) *htab = (HTAB(exprtk_map_kv_t) *)it->htab;
  HTAB_EL(exprtk_map_kv_t) *els = VARR_ADDR(HTAB_EL(exprtk_map_kv_t), htab->els);

  while (it->pos < it->bound) {
    size_t i = it->pos++;
    if (els[i].hash != HTAB_DELETED_HASH) {
      if (key)
        *key = els[i].el.key;
      if (value)
        *value = els[i].el.value;
      return 1;
    }
  }
  return 0;
}
