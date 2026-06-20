#ifndef TBE_VERSION_H
#define TBE_VERSION_H

#define TBE_VERSION_MAJOR 1
#define TBE_VERSION_MINOR 0
#define TBE_VERSION_PATCH 0

#define TBE_VERSION_STRING "1.0.0"

/**
 * @brief Get the library version string.
 * @return A static string in the format "MAJOR.MINOR.PATCH".
 */
const char *tbe_version(void);

/**
 * @brief Get the library version as separate components.
 * @param major Pointer to store major version (can be NULL).
 * @param minor Pointer to store minor version (can be NULL).
 * @param patch Pointer to store patch version (can be NULL).
 */
void tbe_version_components(int *major, int *minor, int *patch);

/**
 * @brief Check if the library version is compatible with required version.
 * @param required_major Required major version.
 * @param required_minor Required minor version.
 * @param required_patch Required patch version.
 * @return 1 if compatible, 0 if not compatible.
 */
int tbe_version_compatible(int required_major, int required_minor, int required_patch);

/**
 * @brief Thread safety: This library is NOT thread-safe.
 *
 * The TBE parser maintains internal state during parsing and does not
 * use any locking mechanisms. If you need to parse schemas from multiple
 * threads, you must:
 *
 * 1. Use separate Node trees for each thread, OR
 * 2. Serialize access to parse_schema() with external locking
 *
 * Once a schema is parsed into a Node tree, the tree can be safely
 * read from multiple threads as long as no thread modifies it.
 * 
 * IMPORTANT: The node_free() function is also NOT thread-safe and
 * should only be called from one thread per Node tree.
 * 
 * For multi-threaded usage:
 * - Each thread should have its own parser instance
 * - Use external synchronization (mutexes) when sharing Node trees
 * - Consider using thread-local storage for parser contexts
 */

#endif /* TBE_VERSION_H */
