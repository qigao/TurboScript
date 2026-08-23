/**
 * @file exprtk_map.c
 * @brief TurboUtils hash map backed map for exprtk values.
 */

#include "exprtk_module.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  hash_map_t entries;
  mem_pool_t pool;
} exprtk_map_storage_t;

#define EXPRTK_MAP_ENTRY_LIMIT (SIZE_MAX / sizeof(exprtk_value_t))

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

static exprtk_map_storage_t *exprtk_map_storage_create(void) {
  exprtk_map_storage_t *storage = (exprtk_map_storage_t *)calloc(1, sizeof(*storage));
  if (!storage) return NULL;
  if (mem_init(&storage->pool, 0) != 0) {
    free(storage);
    return NULL;
  }
  if (hash_map_init_bytes(&storage->entries,
                          sizeof(char *), _Alignof(char *),
                          sizeof(exprtk_value_t), _Alignof(exprtk_value_t),
                          EXPRTK_MAP_ENTRY_LIMIT,
                          exprtk_map_key_hash, exprtk_map_key_equal, NULL) != STL_OK) {
    mem_destroy(&storage->pool);
    free(storage);
    return NULL;
  }
  if (hash_map_reserve(&storage->entries, 8) != STL_OK) {
    hash_map_destroy(&storage->entries);
    mem_destroy(&storage->pool);
    free(storage);
    return NULL;
  }
  return storage;
}

static char **exprtk_map_find_key_slot(const exprtk_map_storage_t *storage, const char *key) {
  size_t capacity;

  if (!storage || !key) return NULL;
  capacity = hash_map_capacity(&storage->entries);
  for (size_t i = 0; i < capacity; ++i) {
    const void *slot = hash_map_key_at(&storage->entries, i);
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
  val.ownership = EXPRTK_VALUE_OWNED;
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
  value = (exprtk_value_t *)hash_map_get(&storage->entries, &lookup);
  return value ? exprtk_value_borrow(*value) : exprtk_val_num(0);
}

int exprtk_map_set(exprtk_value_t *map, const char *key, exprtk_value_t value) {
  char *lookup = (char *)key;
  char *owned_key;
  exprtk_map_storage_t *storage;
  exprtk_value_t value_copy;
  exprtk_value_t *slot;

  if (!exprtk_value_is_object_like(map) || !key)
    return -1;
  if (exprtk_value_is_object_like(&value) &&
      value.data.map.htab == map->data.map.htab)
    return -1;
  if (!map->data.map.htab) {
    map->data.map.htab = exprtk_map_storage_create();
    if (!map->data.map.htab) return -1;
  }

  storage = (exprtk_map_storage_t *)map->data.map.htab;
  if (exprtk_value_copy_to_pool(value, &storage->pool, &value_copy) != 0) return -1;
  slot = (exprtk_value_t *)hash_map_get(&storage->entries, &lookup);
  if (slot) {
    exprtk_value_destroy(slot);
    *slot = value_copy;
    return 0;
  }

  owned_key = mem_strdup(&storage->pool, key);
  if (!owned_key) {
    exprtk_value_destroy(&value_copy);
    return -1;
  }
  if (hash_map_put(&storage->entries, &owned_key, &value_copy) != STL_OK) {
    mem_free(&storage->pool, owned_key);
    exprtk_value_destroy(&value_copy);
    return -1;
  }
  return 0;
}

int exprtk_map_has(const exprtk_value_t *map, const char *key) {
  char *lookup = (char *)key;
  exprtk_map_storage_t *storage;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab || !key)
    return 0;
  storage = (exprtk_map_storage_t *)map->data.map.htab;
  return hash_map_contains(&storage->entries, &lookup) ? 1 : 0;
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
  if (hash_map_remove(&storage->entries, &lookup, &removed) != STL_OK)
    return 0;

  mem_free(&storage->pool, owned_key);
  exprtk_value_destroy(&removed);
  return 1;
}

size_t exprtk_map_count(const exprtk_value_t *map) {
  exprtk_map_storage_t *storage;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return 0;
  storage = (exprtk_map_storage_t *)map->data.map.htab;
  return hash_map_size(&storage->entries);
}

exprtk_value_t *exprtk_map_get_ptr(const exprtk_value_t *map, const char *key) {
  char *lookup = (char *)key;
  exprtk_map_storage_t *storage;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab || !key)
    return NULL;
  storage = (exprtk_map_storage_t *)map->data.map.htab;
  return (exprtk_value_t *)hash_map_get(&storage->entries, &lookup);
}

void exprtk_map_free(exprtk_value_t *map) {
  exprtk_map_storage_t *storage;
  size_t capacity;

  if (!exprtk_value_is_object_like(map) || !map->data.map.htab)
    return;

  storage = (exprtk_map_storage_t *)map->data.map.htab;
  capacity = hash_map_capacity(&storage->entries);
  for (size_t i = 0; i < capacity; ++i) {
    const void *key_slot = hash_map_key_at(&storage->entries, i);
    const void *value_slot = hash_map_value_at_const(&storage->entries, i);
    char *const *key = (char *const *)key_slot;
    const exprtk_value_t *value = (const exprtk_value_t *)value_slot;
    if (!key || !value) continue;
    mem_free(&storage->pool, *key);
    exprtk_value_destroy((exprtk_value_t *)value);
  }

  hash_map_destroy(&storage->entries);
  mem_destroy(&storage->pool);
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
  it.bound = hash_map_capacity(&storage->entries);
  return it;
}

int exprtk_map_iter_next(exprtk_map_iter_t *it, const char **key, exprtk_value_t *value) {
  exprtk_map_storage_t *storage;

  if (!it || !it->htab)
    return 0;

  storage = (exprtk_map_storage_t *)it->htab;
  while (it->pos < it->bound) {
    size_t slot = it->pos++;
    const void *key_slot = hash_map_key_at(&storage->entries, slot);
    const void *value_slot = hash_map_value_at_const(&storage->entries, slot);
    char *const *stored_key = (char *const *)key_slot;
    const exprtk_value_t *stored_value = (const exprtk_value_t *)value_slot;
    if (!stored_key || !stored_value) continue;
    if (key) *key = *stored_key;
    if (value) *value = exprtk_value_borrow(*stored_value);
    return 1;
  }
  return 0;
}
