#ifndef SCHEMA_MUSTACHE_HELPERS_H
#define SCHEMA_MUSTACHE_HELPERS_H

#include <stddef.h>
#include "mustache.h"

/**
 * @brief Mustache data-provider and renderer callbacks for Node trees.
 *
 * These callbacks bridge the generic mustache library with our Node tree
 * data model. Call schema_mustache_helpers_provider() and schema_mustache_helpers_renderer()
 * to get pre-wired structs ready for mustache_process().
 */

/** Return a MUSTACHE_DATAPROVIDER wired to operate on Node trees. */
MUSTACHE_DATAPROVIDER mustache_helpers_provider(void);

/** Return a MUSTACHE_RENDERER that writes to a FILE* (or stdout if NULL). */
MUSTACHE_RENDERER mustache_helpers_renderer(void);

#endif /* SCHEMA_MUSTACHE_HELPERS_H */
