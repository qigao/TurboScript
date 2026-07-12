/**
 * @file exprtk_map.c
 * @brief TurboUtils hash map backed map for exprtk values.
 */

#include "exprtk_module.h"
#include "turbo_hash.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  turbo_hash_map_t entries;
} exprtk_map_storage_t;

static char *exprtk_map_strdup(const char *text) {
  size_t len;
  char *copy;

  if (!text) return NULL;
  len = strlen(text);
  copy = (char *)malloc(len + 1);
  if (!copy) return NULL;
  memcpy(copy, text, len + 1);
  return copy;
}

static size_t exprtk_map_key_hash(const void *key, size_t key_size, void *ctx) {
  const char *text;
  size_t hash = 1469598103934665603ull;

  (void)key_size;
  (void)ctx;
  if (!key) return 0;
  text = *(char * const *)key;
  if (!text) return 0;
  while (*text) {
    hash ^= (unsigned char)*text++;
    hash *= 1099511628211ull;
  }
  return hash;
}

static bool exprtk_map_key_equal(const void *left, const void *right, size_t key_size,
                                 void *ctx) {
  const char *lhs;
  const char *rhs;

  (void)key_size;
  (void)ctx;
  if (!left || !right) return false;
  lhs = *(char * const *)left;
  rhs = *(char * const *)right;
  if (!lhs || !rhs) return lhs == rhs;
  return strcmp(lhs, rhs) == 0;
}

static void exprtk_map_value_free_owned_buffers(exprtk_value_t value) {
  if (value.type == EXPRTK_VAL_STRING && value.data.string.data) {
    free((void *)value.data.string.data);
  } else if (value.type == EXPRTK_VAL_BYTES && value.data.bytes.data) {
    free((void *)value.data.bytes.data);
  }
}

static exprtk_value_t exprtk_map_value_copy(exprtk_value_t value) {
  exprtk_value_t copy = value;

  if (value.type == EXPRTK_VAL_STRING && value.data.string.data) {
    char *str_copy = (char *)malloc(value.data.string.len + 1);
    if (str_copy) {
      memcpy(str_copy, value.data.string.data, value.data.string.len);
      str_copy[value.data.string.len] = '\0';
      copy.data.string.data = str_copy;
    }
  } else if (value.type == EXPRTK_VAL_BYTES && value.data.bytes.data) {
    char *bytes_copy = (char *)malloc(value.data.bytes.len);
    if (bytes_copy || value.data.bytes.len == 0) {
      if (value.data.bytes.len > 0)
        memcpy(bytes_copy, value.data.bytes.data, value.data.bytes.len);
      copy.data.bytes.data = bytes_copy;
    }
  }

  return copy;
}

static exprtk_map_storage_t *exprtk_map_storage_create(void) {
  exprtk_map_storage_t *storage = (exprtk_map_storage_t *)malloc(sizeof(*storage));
  if (!storage) return NULL;
  if (turbo_hash_map_init(&storage->entries, sizeof(char *), sizeof(exprtk_value_t),
                          exprtk_map_key_hash, exprtk_map_key_equal, NULL) != TURBO_OK) {
    free(storage);
    return NULL;
  }
  if (turbo_hash_map_reserve(&storage->entries, 8) != TURBO_OK) {
    turbo_hash_map_destroy(&storage->entries);
    free(storage);
    return NULL;
  }
  return storage;
}

static char **exprtk_map_find_key_slot(const exprtk_map_storage_t *storage, const char *key) {
  size_t capacity;

  if (!storage || !key) return NULL;
  capacity = turbo_hash_map_capacity(&storage->entries);
  for (size_t i = 0; i < capacity; ++i) {
    const void *slot = turbo_hash_map_key_at(&storage->entries, i);
    char *const *stored_key = (char *const *)slot;
    if (stored_key && *stored_key && strcmp(*stored_key, key) == 0)
      return (char **)stored_key;
  }
  return NULL;
}

exprtk_value_t exprtk_val_map(void) {
  exprtk_value_t val;
  memset(&val, 0, sizeof(val));
  val.type = EXPRTK_VAL_MAP;
  val.data.map.htab = exprtk_map_storage_create();
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
  char *lookup = (char *)key;
  exprtk_map_storage_t *storage;
  exprtk_value_t *value;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab || !key)
    return exprtk_val_num(0);
  storage = (exprtk_map_storage_t *)map->data.map.htab;
  value = (exprtk_value_t *)turbo_hash_map_get(&storage->entries, &lookup);
  return value ? *value : exprtk_val_num(0);
}

void exprtk_map_set(exprtk_value_t *map, const char *key, exprtk_value_t value) {
  char *lookup = (char *)key;
  char *owned_key;
  exprtk_map_storage_t *storage;
  exprtk_value_t value_copy;
  exprtk_value_t *slot;

  if (!exprtk_value_is_object_like(map) || !key)
    return;
  if (!map->data.map.htab) {
    map->data.map.htab = exprtk_map_storage_create();
    if (!map->data.map.htab) return;
  }

  storage = (exprtk_map_storage_t *)map->data.map.htab;
  value_copy = exprtk_map_value_copy(value);
  slot = (exprtk_value_t *)turbo_hash_map_get(&storage->entries, &lookup);
  if (slot) {
    exprtk_map_value_free_owned_buffers(*slot);
    *slot = value_copy;
    return;
  }

  owned_key = exprtk_map_strdup(key);
  if (!owned_key) {
    exprtk_map_value_free_owned_buffers(value_copy);
    return;
  }
  if (turbo_hash_map_put(&storage->entries, &owned_key, &value_copy) != TURBO_OK) {
    free(owned_key);
    exprtk_map_value_free_owned_buffers(value_copy);
  }
}

int exprtk_map_has(const exprtk_value_t *map, const char *key) {
  char *lookup = (char *)key;
  exprtk_map_storage_t *storage;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab || !key)
    return 0;
  storage = (exprtk_map_storage_t *)map->data.map.htab;
  return turbo_hash_map_contains(&storage->entries, &lookup) ? 1 : 0;
}

int exprtk_map_delete(exprtk_value_t *map, const char *key) {
  char *lookup = (char *)key;
  char **key_slot;
  char *owned_key;
  exprtk_map_storage_t *storage;
  exprtk_value_t removed;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab || !key)
    return 0;

  storage = (exprtk_map_storage_t *)map->data.map.htab;
  key_slot = exprtk_map_find_key_slot(storage, key);
  owned_key = key_slot ? *key_slot : NULL;
  if (turbo_hash_map_remove(&storage->entries, &lookup, &removed) != TURBO_OK)
    return 0;

  free(owned_key);
  exprtk_map_value_free_owned_buffers(removed);
  return 1;
}

size_t exprtk_map_count(const exprtk_value_t *map) {
  exprtk_map_storage_t *storage;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return 0;
  storage = (exprtk_map_storage_t *)map->data.map.htab;
  return turbo_hash_map_size(&storage->entries);
}

exprtk_value_t *exprtk_map_get_ptr(const exprtk_value_t *map, const char *key) {
  char *lookup = (char *)key;
  exprtk_map_storage_t *storage;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab || !key)
    return NULL;
  storage = (exprtk_map_storage_t *)map->data.map.htab;
  return (exprtk_value_t *)turbo_hash_map_get(&storage->entries, &lookup);
}

void exprtk_map_free(exprtk_value_t *map) {
  exprtk_map_storage_t *storage;
  size_t capacity;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return;

  storage = (exprtk_map_storage_t *)map->data.map.htab;
  capacity = turbo_hash_map_capacity(&storage->entries);
  for (size_t i = 0; i < capacity; ++i) {
    const void *key_slot = turbo_hash_map_key_at(&storage->entries, i);
    const void *value_slot = turbo_hash_map_value_at_const(&storage->entries, i);
    char *const *key = (char *const *)key_slot;
    const exprtk_value_t *value = (const exprtk_value_t *)value_slot;
    if (!key || !value) continue;
    free(*key);
    exprtk_map_value_free_owned_buffers(*value);
  }

  turbo_hash_map_destroy(&storage->entries);
  free(storage);
  map->data.map.htab = NULL;
}

exprtk_map_iter_t exprtk_map_iter_begin(const exprtk_value_t *map) {
  exprtk_map_iter_t it = {NULL, 0, 0};
  exprtk_map_storage_t *storage;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return it;
  storage = (exprtk_map_storage_t *)map->data.map.htab;
  it.htab = storage;
  it.pos = 0;
  it.bound = turbo_hash_map_capacity(&storage->entries);
  return it;
}

int exprtk_map_iter_next(exprtk_map_iter_t *it, const char **key, exprtk_value_t *value) {
  exprtk_map_storage_t *storage;

  if (!it || !it->htab)
    return 0;

  storage = (exprtk_map_storage_t *)it->htab;
  while (it->pos < it->bound) {
    size_t slot = it->pos++;
    const void *key_slot = turbo_hash_map_key_at(&storage->entries, slot);
    const void *value_slot = turbo_hash_map_value_at_const(&storage->entries, slot);
    char *const *stored_key = (char *const *)key_slot;
    const exprtk_value_t *stored_value = (const exprtk_value_t *)value_slot;
    if (!stored_key || !stored_value) continue;
    if (key) *key = *stored_key;
    if (value) *value = *stored_value;
    return 1;
  }
  return 0;
}
